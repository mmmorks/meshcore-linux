#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // ptsname_r() on glibc
#endif

#include <gtest/gtest.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <set>
#include <string>

#include "helpers/LinuxConsole.h"
#include "LinuxEventLoop.h"

namespace {

// Open the console's published path the way a serial client does. Non-blocking
// so a test that expects silence can prove it rather than hang.
int open_client(const char* path) {
  int fd = ::open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd >= 0) {
    struct termios t;
    if (tcgetattr(fd, &t) == 0) { cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); }
  }
  return fd;
}

// Everything the far end has to say, up to `idle_ms` of silence.
std::string drain(int fd, int idle_ms = 250) {
  std::string out;
  for (;;) {
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, idle_ms) <= 0) break;
    char    buf[256];
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  return out;
}

// The kernel moves bytes from slave to master asynchronously, so a byte just
// written by a client is not necessarily readable on the very next call.
template <class C> int read_within(C& c, int ms = 500) {
  for (int i = 0; i < ms; i++) {
    int b = c.read();
    if (b >= 0) return b;
    usleep(1000);
  }
  return -1;
}

template <class C> int peek_within(C& c, int ms = 500) {
  for (int i = 0; i < ms; i++) {
    int b = c.peek();
    if (b >= 0) return b;
    usleep(1000);
  }
  return -1;
}

std::set<int> open_fds() {
  std::set<int> fds;
  DIR* d = opendir("/dev/fd");
  if (d == nullptr) return fds;
  for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    int fd = atoi(e->d_name);
    if (fd != dirfd(d)) fds.insert(fd);
  }
  closedir(d);
  return fds;
}

// A pseudo-terminal to stand in for the one meshcored is started from. Returns
// the master; `slave` receives the device path of the far end.
int make_terminal(char* slave, size_t slave_len) {
  int m = posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0) return -1;
  if (grantpt(m) != 0 || unlockpt(m) != 0 || ptsname_r(m, slave, slave_len) != 0) {
    close(m);
    return -1;
  }
  return m;
}

// What a body run by run_with_controlling_tty() did, and what it left behind.
struct JobResult {
  int            status = -1;  // raw wait status: exit code, or the signal that killed it
  struct termios tty    = {};  // the terminal, read back before the session ended
};

// Exit codes the harness itself reports; a body's own are below 90.
enum JobFailure { JOB_NO_SESSION = 90, JOB_NO_TERMINAL = 91, JOB_NO_FORK = 92, JOB_STOPPED = 93 };

// Run `body` in a process whose stdin is `slave` -- its *controlling* terminal
// -- either in that terminal's foreground process group or, exactly as
// `meshcored &` leaves it, in a background one. That needs a session of its
// own, so it happens in a child; the result travels back over a pipe, along
// with the terminal's state as the body left it (read before this session ends,
// so nothing about the teardown can affect it).
//
// waitpid() uses WUNTRACED deliberately: SIGTTIN/SIGTTOU stop a process rather
// than killing it, so without it the failure being guarded against would hang
// the suite instead of failing it.
bool run_with_controlling_tty(const char* slave, bool background, int (*body)(), JobResult* out) {
  int pipefd[2];
  if (pipe(pipefd) != 0) return false;

  pid_t leader = fork();
  if (leader < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }
  if (leader == 0) {
    close(pipefd[0]);
    JobResult r;
    if (setsid() < 0) {
      r.status = JOB_NO_SESSION << 8;
    } else {
      int t = open(slave, O_RDWR);  // no O_NOCTTY: this is to become the ctty
      if (t < 0) {
        r.status = JOB_NO_TERMINAL << 8;
      } else {
        ioctl(t, TIOCSCTTY, 0);
        dup2(t, STDIN_FILENO);
        if (t != STDIN_FILENO) close(t);
        pid_t job = fork();
        if (job == 0) {
          if (background) setpgid(0, 0);  // ... and now it is a background job
          _exit(body());
        }
        if (job < 0) {
          r.status = JOB_NO_FORK << 8;
        } else if (waitpid(job, &r.status, WUNTRACED) != job) {
          r.status = -1;
        } else if (WIFSTOPPED(r.status)) {
          kill(job, SIGKILL);
          waitpid(job, nullptr, 0);
          r.status = JOB_STOPPED << 8;
        }
        tcgetattr(STDIN_FILENO, &r.tty);
      }
    }
    ssize_t w = ::write(pipefd[1], &r, sizeof r);
    (void)w;
    _exit(0);
  }

  close(pipefd[1]);
  bool ok = ::read(pipefd[0], out, sizeof *out) == (ssize_t)sizeof *out;
  close(pipefd[0]);
  waitpid(leader, nullptr, 0);
  return ok;
}

void remove_tree(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return;
  for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    std::string p = dir + "/" + e->d_name;
    struct stat st;
    if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
    else unlink(p.c_str());
  }
  closedir(d);
  rmdir(dir.c_str());
}

class ConsoleTest : public ::testing::Test {
protected:
  void SetUp() override {
    // stdin must not be a TTY unless a test says so: begin() would otherwise
    // put the developer's terminal into raw mode and read their keystrokes.
    _saved_stdin = dup(STDIN_FILENO);
    int devnull  = open("/dev/null", O_RDONLY);
    ASSERT_GE(devnull, 0);
    ASSERT_GE(dup2(devnull, STDIN_FILENO), 0);
    close(devnull);

    char tmpl[] = "/tmp/mccon-XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(tmpl));
    _dir  = tmpl;
    _link = _dir + "/console";

    // begin() narrates to stderr. Capturing it keeps the suite's output clean
    // and makes the messages themselves assertable.
    _saved_stderr = dup(STDERR_FILENO);
    _err_path     = _dir + "/stderr";
    int errfd     = open(_err_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(errfd, 0);
    ASSERT_GE(dup2(errfd, STDERR_FILENO), 0);
    close(errfd);

    // The candidate after a declined explicit path is /run/meshcored (absent,
    // or not writable, on a development host) and then XDG_RUNTIME_DIR.
    // Pointing XDG at the temp dir keeps a declined path from landing on the
    // shared /tmp last resort.
    setenv("XDG_RUNTIME_DIR", _dir.c_str(), 1);
    _xdg_default = _dir + "/meshcore/console";
  }

  void TearDown() override {
    fflush(stderr);
    dup2(_saved_stderr, STDERR_FILENO);
    close(_saved_stderr);
    dup2(_saved_stdin, STDIN_FILENO);
    close(_saved_stdin);
    unsetenv("XDG_RUNTIME_DIR");
    remove_tree(_dir);
  }

  std::string captured_stderr() {
    fflush(stderr);
    std::string out;
    int         fd = open(_err_path.c_str(), O_RDONLY);
    if (fd < 0) return out;
    char    buf[512];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    close(fd);
    return out;
  }

  std::string _dir, _link, _xdg_default, _err_path;
  int         _saved_stdin  = -1;
  int         _saved_stderr = -1;
};

// --- PtyConsole: the PTY itself ----------------------------------------------

TEST_F(ConsoleTest, PublishesASymlinkToAnOwnerOnlySlave) {
  PtyConsole c;
  ASSERT_TRUE(c.begin(_link.c_str()));
  EXPECT_STREQ(_link.c_str(), c.path());
  EXPECT_GE(c.fd(), 0);

  struct stat st;
  ASSERT_EQ(0, lstat(_link.c_str(), &st));
  EXPECT_TRUE(S_ISLNK(st.st_mode));
  ASSERT_EQ(0, stat(_link.c_str(), &st));   // through the link, to /dev/pts/N
  EXPECT_TRUE(S_ISCHR(st.st_mode));
  EXPECT_EQ(0600u, st.st_mode & 0777) << "the mode is the access gate";

  c.end();
  EXPECT_NE(0, lstat(_link.c_str(), &st)) << "end() must unpublish";
  EXPECT_EQ(-1, c.fd());
  EXPECT_FALSE(c.isOpen());
}

TEST_F(ConsoleTest, CarriesBothDirectionsAndMapsNewline) {
  PtyConsole c;
  ASSERT_TRUE(c.begin(_link.c_str()));
  int client = open_client(c.path());
  ASSERT_GE(client, 0);

  ASSERT_EQ(4, ::write(client, "ver\n", 4));
  EXPECT_EQ('v', read_within(c));
  EXPECT_EQ('e', read_within(c));
  EXPECT_EQ('r', read_within(c));
  EXPECT_EQ('\r', read_within(c)) << "tools send '\\n'; the CLI ends a line on '\\r'";
  EXPECT_EQ(-1, c.read());

  c.write('O');
  c.write('K');
  EXPECT_EQ("OK", drain(client));

  close(client);
}

TEST_F(ConsoleTest, PeekDoesNotConsume) {
  PtyConsole c;
  ASSERT_TRUE(c.begin(_link.c_str()));
  int client = open_client(c.path());
  ASSERT_GE(client, 0);

  ASSERT_EQ(1, ::write(client, "Z", 1));
  EXPECT_EQ('Z', peek_within(c));
  EXPECT_EQ('Z', c.peek());
  EXPECT_EQ(1, c.available());
  EXPECT_EQ('Z', c.read());
  EXPECT_EQ(-1, c.read());
  EXPECT_EQ(0, c.available());

  close(client);
}

// The reason the console holds a descriptor on its own slave. Without one, the
// master reports POLLHUP and read() fails with EIO from the moment the last
// client closes until the next one opens -- a level condition the event loop
// can only throttle, not clear.
TEST_F(ConsoleTest, ADetachedClientLeavesTheConsoleIdleNotHungUp) {
  PtyConsole c;
  ASSERT_TRUE(c.begin(_link.c_str()));

  int first = open_client(c.path());
  ASSERT_GE(first, 0);
  ASSERT_EQ(1, ::write(first, "a", 1));
  EXPECT_EQ('a', read_within(c));
  close(first);
  usleep(20 * 1000);

  struct pollfd p = { c.fd(), POLLIN, 0 };
  EXPECT_EQ(0, poll(&p, 1, 50)) << "revents=" << p.revents;
  EXPECT_EQ(-1, c.read());
  for (int i = 0; i < 100; i++) c.write('x');   // nobody reading: dropped, not fatal

  // And through the event loop: a clean timeout, not the 1 ms error backoff.
  LinuxEventLoop loop;
  loop.reset();
  loop.registerFd(c.fd());
  auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(0, loop.wait(40));
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
  EXPECT_GE(ms, 35) << "wait() returned early: the master is reporting a condition";

  int second = open_client(c.path());
  ASSERT_GE(second, 0);
  ASSERT_EQ(1, ::write(second, "b", 1));
  EXPECT_EQ('b', read_within(c)) << "the next client attaches as if the first had never left";
  close(second);
}

// LinuxBoard::reboot() re-execs this process image, and execv() keeps every
// descriptor that is not close-on-exec. A master that reached the new image
// would keep the old /dev/pts/N alive with nobody reading it.
TEST_F(ConsoleTest, EveryDescriptorItOpensIsCloseOnExec) {
  std::set<int> before = open_fds();
  PtyConsole    c;
  ASSERT_TRUE(c.begin(_link.c_str()));

  int opened = 0;
  for (int fd : open_fds()) {
    if (before.count(fd)) continue;
    opened++;
    EXPECT_TRUE(fcntl(fd, F_GETFD) & FD_CLOEXEC) << "descriptor " << fd;
  }
  EXPECT_EQ(2, opened) << "the master and the console's own slave descriptor";

  c.end();
  EXPECT_EQ(before, open_fds()) << "end() must close everything begin() opened";
}

TEST_F(ConsoleTest, DeclinesAPathAnotherLiveConsoleHolds) {
  PtyConsole first;
  ASSERT_TRUE(first.begin(_link.c_str()));

  PtyConsole second;
  ASSERT_TRUE(second.begin(_link.c_str()));   // same path
  EXPECT_STREQ(_xdg_default.c_str(), second.path()) << "fell through to the next candidate";
  EXPECT_NE(std::string::npos, captured_stderr().find("in use"));

  // The first instance kept the path, and it still works.
  int client = open_client(_link.c_str());
  ASSERT_GE(client, 0);
  ASSERT_EQ(1, ::write(client, "m", 1));
  EXPECT_EQ('m', read_within(first));
  EXPECT_EQ(-1, second.read());
  close(client);
}

// What a crashed daemon leaves behind: a symlink to a /dev/pts/N that no longer
// exists. That is nobody's console, so it is reclaimed.
TEST_F(ConsoleTest, ReclaimsAStaleSymlink) {
  ASSERT_EQ(0, symlink("/dev/pts/no-such-console", _link.c_str()));

  PtyConsole c;
  ASSERT_TRUE(c.begin(_link.c_str()));
  EXPECT_STREQ(_link.c_str(), c.path());

  char target[128] = {0};
  ASSERT_GT(readlink(_link.c_str(), target, sizeof target - 1), 0);
  EXPECT_STRNE("/dev/pts/no-such-console", target);
  struct stat st;
  EXPECT_EQ(0, stat(_link.c_str(), &st)) << "the link now resolves to a live device";
}

TEST_F(ConsoleTest, RefusesToReplaceARegularFile) {
  const char* content = "not a console\n";
  int         f       = open(_link.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  ASSERT_GE(f, 0);
  ASSERT_EQ((ssize_t)strlen(content), ::write(f, content, strlen(content)));
  close(f);

  PtyConsole c;
  ASSERT_TRUE(c.begin(_link.c_str()));
  EXPECT_STREQ(_xdg_default.c_str(), c.path());
  EXPECT_NE(std::string::npos, captured_stderr().find("is not a symlink"));

  // An operator who points console_path at the wrong file gets a refusal, not
  // a deletion.
  struct stat st;
  ASSERT_EQ(0, lstat(_link.c_str(), &st));
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ((off_t)strlen(content), st.st_size);
}

// Whoever can write the directory owns the console: they can replace the
// symlink with a PTY of their own and read whatever the operator types at it,
// including `get prv.key`. Pre-creating the directory is not a privilege, so
// the check cannot be "did mkdir() succeed".
TEST_F(ConsoleTest, DeclinesAParentDirectoryOthersCanWrite) {
  std::string open_dir = _dir + "/open";
  ASSERT_EQ(0, mkdir(open_dir.c_str(), 0700));
  ASSERT_EQ(0, chmod(open_dir.c_str(), 0777));  // mkdir()'s mode goes through umask
  std::string link = open_dir + "/console";

  PtyConsole c;
  ASSERT_TRUE(c.begin(link.c_str()));
  EXPECT_STREQ(_xdg_default.c_str(), c.path()) << "fell through to the next candidate";
  EXPECT_NE(std::string::npos, captured_stderr().find("not a private directory"));

  struct stat st;
  EXPECT_NE(0, lstat(link.c_str(), &st)) << "nothing published where anyone can rewrite it";
}

TEST_F(ConsoleTest, DefaultsToAPrivateDirectoryUnderXdgRuntimeDir) {
  PtyConsole c;
  ASSERT_TRUE(c.begin(""));
  EXPECT_STREQ(_xdg_default.c_str(), c.path());

  struct stat st;
  ASSERT_EQ(0, stat((_dir + "/meshcore").c_str(), &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(0700u, st.st_mode & 0777);
}

// --- LinuxConsole: the Stream the CLI sees -----------------------------------

TEST_F(ConsoleTest, NormalisesNewlineIdenticallyForPeekAndRead) {
  LinuxConsole console;
  ASSERT_TRUE(console.begin(_link.c_str()));
  int client = open_client(console.path());
  ASSERT_GE(client, 0);

  ASSERT_EQ(2, ::write(client, "a\n", 2));
  EXPECT_EQ('a', peek_within(console));
  EXPECT_EQ('a', console.read());
  // The CLI ends a command on '\r'; peek() reporting '\n' would tell a caller
  // the line is unfinished when read() is about to say it is finished.
  EXPECT_EQ('\r', peek_within(console));
  EXPECT_EQ('\r', console.read());
  EXPECT_EQ(0, console.available());

  close(client);
}

TEST_F(ConsoleTest, WatchesStdinOnlyWhenItIsATerminal) {
  LinuxConsole console;
  ASSERT_TRUE(console.begin(_link.c_str()));
  EXPECT_GE(console.ptyFd(), 0);
  EXPECT_EQ(-1, console.stdinFd()) << "stdin is /dev/null here";
}

// The body of the test below, in the background job itself.
int begin_in_background() {
  if (tcgetpgrp(STDIN_FILENO) < 0) return 80;                // harness: stdin is not the ctty
  if (tcgetpgrp(STDIN_FILENO) == getpgrp()) return 81;       // harness: this is the foreground
  struct termios before;
  if (tcgetattr(STDIN_FILENO, &before) != 0) return 82;

  LinuxConsole console;
  console.begin("");  // XDG_RUNTIME_DIR points at the test's temp dir
  int fd = console.stdinFd();

  struct termios after;
  if (tcgetattr(STDIN_FILENO, &after) != 0) return 83;
  bool changed = (before.c_lflag & (ICANON | ECHO)) != (after.c_lflag & (ICANON | ECHO));
  console.end();
  if (fd != -1) return 1;
  if (changed) return 2;
  return 0;
}

// `meshcored &`. isatty() is just as true for a background job, but tcsetattr()
// from one sends SIGTTOU to its whole process group -- unconditionally, not
// gated on TOSTOP -- and the default action is to *stop* it. begin() runs
// inside setup(), so the daemon would print "Stopped" and never boot; reading
// stdin would do the same via SIGTTIN. So a background job's terminal is left
// entirely alone and the PTY is the only door.
TEST_F(ConsoleTest, LeavesTheTerminalAloneWhenStartedInTheBackground) {
  char term_slave[128];
  int  term = make_terminal(term_slave, sizeof term_slave);
  ASSERT_GE(term, 0);

  JobResult r;
  ASSERT_TRUE(run_with_controlling_tty(term_slave, true, begin_in_background, &r));
  close(term);

  ASSERT_TRUE(WIFEXITED(r.status)) << "raw wait status " << r.status;
  ASSERT_NE(JOB_STOPPED, WEXITSTATUS(r.status)) << "it was stopped: `meshcored &` never boots";
  EXPECT_EQ(0, WEXITSTATUS(r.status)) << "1: stdin taken over; 2: the terminal was changed; "
                                         ">= 80: the harness itself (see begin_in_background)";
  EXPECT_NE(0u, r.tty.c_lflag & (ICANON | ECHO)) << "the shell's terminal must be untouched";
}

// One CLI, two doors. Output goes to stdout always (journald's copy) and to the
// PTY only for a command that arrived there.
TEST_F(ConsoleTest, RepliesFollowTheCommandToItsSource) {
  // A terminal on stdin: the slave of a second PTY pair, driven from its master.
  char term_slave[128];
  int  term = make_terminal(term_slave, sizeof term_slave);
  ASSERT_GE(term, 0);
  int keyboard = open(term_slave, O_RDWR | O_NOCTTY);
  ASSERT_GE(keyboard, 0);
  ASSERT_GE(dup2(keyboard, STDIN_FILENO), 0);
  close(keyboard);
  struct termios before;
  ASSERT_EQ(0, tcgetattr(STDIN_FILENO, &before));

  // Capture stdout. gtest prints nothing during a test body, so the redirect
  // is invisible to it as long as it is undone before the body ends.
  std::string out_path = _dir + "/stdout";
  fflush(stdout);
  int saved_stdout = dup(STDOUT_FILENO);
  int out_fd       = open(out_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  ASSERT_GE(out_fd, 0);
  ASSERT_GE(dup2(out_fd, STDOUT_FILENO), 0);
  close(out_fd);

  LinuxConsole console;
  bool         began = console.begin(_link.c_str());
  int          stdin_fd = console.stdinFd();
  struct termios raw;
  tcgetattr(STDIN_FILENO, &raw);

  // Over the PTY: the reply reaches the client (and stdout).
  int client = open_client(console.path());
  int a1 = -1, a2 = -1, b1 = -1, b2 = -1;
  std::string a_reply, b_leak, term_echo;
  if (client >= 0 && ::write(client, "a\r", 2) == 2) {
    a1 = read_within(console);
    a2 = read_within(console);
    console.print("  -> A\n");
    a_reply = drain(client);
  }

  // From the terminal: the reply reaches stdout and stays out of the PTY.
  if (::write(term, "b\r", 2) == 2) {
    b1 = read_within(console);
    b2 = read_within(console);
    console.print("  -> B\n");
    b_leak    = drain(client, 100);
    term_echo = drain(term, 50);
  }

  console.end();
  struct termios after;
  tcgetattr(STDIN_FILENO, &after);

  // Everything is asserted only once stdout is back: a failure message printed
  // into the capture file would otherwise be invisible.
  fflush(stdout);
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
  if (client >= 0) close(client);
  close(term);

  ASSERT_TRUE(began);
  EXPECT_EQ(STDIN_FILENO, stdin_fd);
  EXPECT_EQ(0u, raw.c_lflag & (ICANON | ECHO)) << "keystrokes, not lines, and no double echo";
  EXPECT_NE(0u, raw.c_lflag & ISIG) << "Ctrl-C must still stop the daemon";
  ASSERT_GE(client, 0);
  EXPECT_EQ('a', a1);
  EXPECT_EQ('\r', a2);
  EXPECT_EQ("  -> A\n", a_reply);
  EXPECT_EQ('b', b1);
  EXPECT_EQ('\r', b2);
  EXPECT_EQ("", b_leak) << "a foreground session's output must not queue for the next client";
  EXPECT_EQ("", term_echo) << "the terminal echoes nothing on its own";
  // Only the bits begin() changed are compared: the kernel keeps transient
  // state flags (PENDIN) in c_lflag as well.
  EXPECT_EQ(before.c_lflag & (ICANON | ECHO), after.c_lflag & (ICANON | ECHO)) << "end() restores the terminal";
  EXPECT_EQ(before.c_iflag & (ICRNL | INLCR), after.c_iflag & (ICRNL | INLCR));

  std::string out;
  int         fd = open(out_path.c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);
  char    buf[256];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
  close(fd);
  EXPECT_EQ("  -> A\n  -> B\n", out) << "stdout carries both, in order";
}

// `nohup ./meshcored &` over an ssh session that then drops (nohup redirects
// stdout, not stdin): fd 0 stays open and stays hung up for the rest of the
// run. Nothing can drain a hung-up descriptor, and a level condition is the one
// thing the event loop can only throttle -- a thousand wake-ups a second, the
// busy loop this console exists to remove. So stdin is dropped instead.
TEST_F(ConsoleTest, StopsWatchingStdinOnceTheTerminalHangsUp) {
  char term_slave[128];
  int  term = make_terminal(term_slave, sizeof term_slave);
  ASSERT_GE(term, 0);
  int keyboard = open(term_slave, O_RDWR | O_NOCTTY);
  ASSERT_GE(keyboard, 0);
  ASSERT_GE(dup2(keyboard, STDIN_FILENO), 0);
  close(keyboard);

  LinuxConsole console;
  ASSERT_TRUE(console.begin(_link.c_str()));
  ASSERT_EQ(STDIN_FILENO, console.stdinFd());

  // Nothing typed yet. In the VMIN=0 mode begin() sets, that is read() == 0 --
  // the same value a hangup gives -- and it must not be mistaken for one.
  EXPECT_EQ(-1, console.read());
  EXPECT_EQ(STDIN_FILENO, console.stdinFd()) << "an idle terminal is not a gone one";

  close(term);  // the far end goes away
  struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
  ASSERT_GT(poll(&p, 1, 1000), 0) << "the slave should be reporting the hangup by now";

  EXPECT_EQ(-1, console.read());
  EXPECT_EQ(-1, console.stdinFd()) << "idleUntilEvent() must stop registering fd 0";
}

// The body of the test below, in a foreground job of its own terminal.
int begin_then_interrupt() {
  if (tcgetpgrp(STDIN_FILENO) != getpgrp()) return 80;  // harness: not the foreground
  // Whatever started the suite may have left SIGINT ignored (a shell does that
  // for its background jobs); the daemon's case is a plain Ctrl-C.
  signal(SIGINT, SIG_DFL);

  LinuxConsole console;
  if (!console.begin("")) return 81;
  if (console.stdinFd() != STDIN_FILENO) return 82;
  struct termios raw;
  if (tcgetattr(STDIN_FILENO, &raw) != 0) return 83;
  if ((raw.c_lflag & (ICANON | ECHO)) != 0) return 84;  // must be raw before we test the undo

  raise(SIGINT);
  return 85;  // the handler must re-raise, not return
}

// Ctrl-C is how a foreground meshcored actually ends: loop() never returns and
// nothing calls exit(), so the destructor never runs. Without a handler the
// operator gets their shell back with ECHO and ICANON off, typing blind until
// they think to run `reset`.
TEST_F(ConsoleTest, RestoresTheTerminalWhenInterrupted) {
  char term_slave[128];
  int  term = make_terminal(term_slave, sizeof term_slave);
  ASSERT_GE(term, 0);
  int probe = open(term_slave, O_RDWR | O_NOCTTY);
  ASSERT_GE(probe, 0);
  struct termios before;
  ASSERT_EQ(0, tcgetattr(probe, &before));
  close(probe);

  JobResult r;
  ASSERT_TRUE(run_with_controlling_tty(term_slave, false, begin_then_interrupt, &r));
  close(term);

  ASSERT_FALSE(WIFEXITED(r.status)) << "exited " << WEXITSTATUS(r.status)
                                    << " instead of dying on SIGINT (see begin_then_interrupt)";
  ASSERT_TRUE(WIFSIGNALED(r.status)) << "raw wait status " << r.status;
  EXPECT_EQ(SIGINT, WTERMSIG(r.status)) << "the handler must re-raise, so the exit status still "
                                           "says what killed it";
  EXPECT_EQ(before.c_lflag & (ICANON | ECHO), r.tty.c_lflag & (ICANON | ECHO))
      << "Ctrl-C left the terminal in raw mode";
  EXPECT_EQ(before.c_iflag & (ICRNL | INLCR), r.tty.c_iflag & (ICRNL | INLCR));
}

// What LinuxBoard::reboot() does, and what the re-exec'd image must then find.
TEST_F(ConsoleTest, EndThenBeginRestartsCleanly) {
  std::set<int> baseline = open_fds();
  LinuxConsole  console;
  ASSERT_TRUE(console.begin(_link.c_str()));
  console.end();

  struct stat st;
  EXPECT_NE(0, lstat(_link.c_str(), &st)) << "nothing left for the next image to decline";
  EXPECT_EQ(baseline, open_fds());
  EXPECT_FALSE(console.hasPty());
  EXPECT_EQ(-1, console.ptyFd());
  EXPECT_EQ(-1, console.read());

  ASSERT_TRUE(console.begin(_link.c_str()));
  EXPECT_STREQ(_link.c_str(), console.path());
  int client = open_client(console.path());
  ASSERT_GE(client, 0);
  ASSERT_EQ(1, ::write(client, "r", 1));
  EXPECT_EQ('r', read_within(console));
  close(client);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
