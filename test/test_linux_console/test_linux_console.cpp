#include <gtest/gtest.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include "LinuxConsole.h"
#include "LinuxEventLoop.h"

namespace {

// The console's client descriptor normally arrives from accept(). Attaching one
// directly is the only way to reach states accept() cannot produce -- chiefly a
// descriptor whose every read() fails, which is what a POLLERR client looks
// like from rawReadByte()'s side.
class TestConsole : public LinuxConsole {
public:
  using LinuxConsole::attachClient;
  using LinuxConsole::clientFd;
  using LinuxConsole::serverFd;
};

// Minimal PeekableStream over a fixed script, for the lookahead contract tests.
class ScriptedStream : public PeekableStream {
public:
  explicit ScriptedStream(const char* script) : _script(script) { }

  using PeekableStream::clearPeek;
  size_t write(uint8_t) override { return 1; }
  int    raw_calls = 0;

protected:
  int rawReadByte() override {
    raw_calls++;
    return *_script ? (uint8_t)*_script++ : -1;
  }

private:
  const char* _script;
};

int connect_client(const char* path) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr*)&addr, sizeof addr) != 0) { close(fd); return -1; }
  return fd;
}

// Everything the client end has to say, up to `idle_ms` of silence or a hangup.
std::string drain(int fd, int idle_ms = 250) {
  std::string out;
  for (;;) {
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, idle_ms) <= 0) break;
    char    buf[256];
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  return out;
}

// True once the peer has closed: recv() reports end of file rather than EAGAIN.
bool peer_hung_up(int fd, int idle_ms = 250) {
  struct pollfd p = { fd, POLLIN, 0 };
  if (poll(&p, 1, idle_ms) <= 0) return false;
  char b;
  return ::recv(fd, &b, 1, 0) == 0;
}

void remove_tree(const char* dir) {
  DIR* d = opendir(dir);
  if (d == nullptr) return;
  for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    unlink((std::string(dir) + "/" + e->d_name).c_str());
  }
  closedir(d);
  rmdir(dir);
}

class LinuxConsoleTest : public ::testing::Test {
protected:
  void SetUp() override {
    // stdin must not be a TTY for these tests: begin() would otherwise put the
    // developer's terminal into raw mode, and rawReadByte() would fall back to
    // reading keystrokes. /dev/null makes the answer the same either way.
    _saved_stdin = dup(STDIN_FILENO);
    int devnull  = open("/dev/null", O_RDONLY);
    ASSERT_GE(devnull, 0);
    ASSERT_GE(dup2(devnull, STDIN_FILENO), 0);
    close(devnull);

    strcpy(_dir, "/tmp/mccon-XXXXXX");
    ASSERT_NE(nullptr, mkdtemp(_dir));
    snprintf(_sock, sizeof _sock, "%s/ctl.sock", _dir);
    snprintf(_err_path, sizeof _err_path, "%s/stderr", _dir);

    // begin() narrates to stderr. Capturing it keeps the suite's output clean
    // and makes the messages themselves assertable.
    _saved_stderr = dup(STDERR_FILENO);
    int errfd     = open(_err_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(errfd, 0);
    ASSERT_GE(dup2(errfd, STDERR_FILENO), 0);
    close(errfd);

    setenv("MESHCORED_CONTROL_SOCKET", _sock, 1);
    // The candidates after the override are /run/meshcored (absent here) and
    // XDG_RUNTIME_DIR, then /tmp/meshcored.sock. Pointing XDG at the temp dir
    // keeps a refused override from landing on that shared last resort.
    setenv("XDG_RUNTIME_DIR", _dir, 1);
  }

  void TearDown() override {
    fflush(stderr);
    dup2(_saved_stderr, STDERR_FILENO);
    close(_saved_stderr);
    dup2(_saved_stdin, STDIN_FILENO);
    close(_saved_stdin);
    unsetenv("MESHCORED_CONTROL_SOCKET");
    unsetenv("XDG_RUNTIME_DIR");
    remove_tree(_dir);
  }

  std::string captured_stderr() {
    fflush(stderr);
    std::string out;
    int         fd = open(_err_path, O_RDONLY);
    if (fd < 0) return out;
    char    buf[512];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    close(fd);
    return out;
  }

  std::string path_in_dir(const char* name) const {
    return std::string(_dir) + "/" + name;
  }

  // rawReadByte() runs once per available() call; that is how the daemon's
  // loop drives the accept/refuse/read state machine.
  static void pump(LinuxConsole& c) { c.available(); }

  char _dir[64];
  char _sock[128];
  char _err_path[128];
  int  _saved_stdin  = -1;
  int  _saved_stderr = -1;
};

TEST_F(LinuxConsoleTest, AcceptsAClientAndCarriesBothDirections) {
  LinuxConsole console;
  console.begin();

  int client = connect_client(_sock);
  ASSERT_GE(client, 0);
  ASSERT_EQ(3, ::write(client, "ver", 3));

  EXPECT_EQ('v', console.read());
  EXPECT_EQ('e', console.read());
  EXPECT_EQ('r', console.read());
  EXPECT_EQ(-1, console.read());   // nothing more pending

  console.print("  -> OK\n");
  EXPECT_EQ("  -> OK\n", drain(client));

  close(client);
}

TEST_F(LinuxConsoleTest, NormalisesNewlineIdenticallyForPeekAndRead) {
  LinuxConsole console;
  console.begin();

  int client = connect_client(_sock);
  ASSERT_GE(client, 0);
  ASSERT_EQ(2, ::write(client, "a\n", 2));

  EXPECT_EQ('a', console.peek());
  EXPECT_EQ('a', console.read());
  // The CLI ends a command on '\r'; peek() reporting '\n' would tell a caller
  // the line is unfinished when read() is about to say it is finished.
  EXPECT_EQ('\r', console.peek());
  EXPECT_EQ('\r', console.read());

  close(client);
}

TEST_F(LinuxConsoleTest, ClientHangupFreesTheConsoleForTheNextClient) {
  LinuxConsole   console;
  LinuxEventLoop loop;
  console.begin();

  int first = connect_client(_sock);
  ASSERT_GE(first, 0);
  ASSERT_EQ(1, ::write(first, "a", 1));
  EXPECT_EQ('a', console.read());
  close(first);

  EXPECT_EQ(-1, console.read());   // sees EOF and drops the client

  // Back to the idle descriptor set: the listener only (stdin is not a TTY).
  loop.reset();
  console.registerPollFds(loop);
  EXPECT_EQ(1, loop.registeredCount());

  int second = connect_client(_sock);
  ASSERT_GE(second, 0);
  ASSERT_EQ(1, ::write(second, "b", 1));
  EXPECT_EQ('b', console.read());

  close(second);
}

TEST_F(LinuxConsoleTest, PersistentReadErrorTearsTheClientDown) {
  TestConsole    console;
  LinuxEventLoop loop;
  console.begin();

  // A write-only descriptor fails every read() with EBADF: the persistent error
  // state of a client that reports POLLERR instead of POLLIN. Treating it as
  // transient wedges the console -- it is the only descriptor read while a
  // client is attached, so stdin and every later client stay locked out.
  int wronly = open(path_in_dir("sink").c_str(), O_WRONLY | O_CREAT, 0600);
  ASSERT_GE(wronly, 0);
  console.attachClient(wronly);
  ASSERT_EQ(wronly, console.clientFd());

  EXPECT_EQ(-1, console.read());
  EXPECT_LT(console.clientFd(), 0);

  loop.reset();
  console.registerPollFds(loop);
  EXPECT_EQ(1, loop.registeredCount());

  // And the console is genuinely usable again, not merely reset.
  int client = connect_client(_sock);
  ASSERT_GE(client, 0);
  ASSERT_EQ(1, ::write(client, "z", 1));
  EXPECT_EQ('z', console.read());
  close(client);
}

TEST_F(LinuxConsoleTest, TransientReadErrorKeepsTheClient) {
  // The counterweight to the test above: EAGAIN is what an idle non-blocking
  // client returns on every loop iteration, and dropping the session for that
  // would make the console unusable rather than merely wedged.
  LinuxConsole console;
  console.begin();

  int client = connect_client(_sock);
  ASSERT_GE(client, 0);
  pump(console);                    // accepts, then reads EAGAIN

  ASSERT_EQ(1, ::write(client, "q", 1));
  EXPECT_EQ('q', console.read());   // same session, still attached

  close(client);
}

TEST_F(LinuxConsoleTest, SecondClientIsRefusedWhileOneIsAttached) {
  LinuxConsole   console;
  LinuxEventLoop loop;
  console.begin();

  int first = connect_client(_sock);
  ASSERT_GE(first, 0);
  ASSERT_EQ(1, ::write(first, "a", 1));
  EXPECT_EQ('a', console.read());

  // The listener stays in the poll set alongside the client, so the daemon
  // wakes for the second connection instead of leaving it in the backlog.
  loop.reset();
  console.registerPollFds(loop);
  EXPECT_EQ(2, loop.registeredCount());

  int second = connect_client(_sock);
  ASSERT_GE(second, 0);
  ASSERT_EQ(4, ::write(second, "ver\r", 4));

  pump(console);   // one iteration of the daemon loop

  std::string refusal = drain(second);
  EXPECT_NE(std::string::npos, refusal.find("busy"))
      << "second client got: [" << refusal << "]";
  EXPECT_TRUE(peer_hung_up(second)) << "a refused client must be closed, not parked";

  // Its command must not reach the CLI -- not now, and not later when the
  // first client detaches.
  EXPECT_EQ(-1, console.read());
  close(first);
  EXPECT_EQ(-1, console.read());   // notices the hangup
  EXPECT_EQ(-1, console.read());   // and nothing is waiting behind it
}

TEST_F(LinuxConsoleTest, RefusesToUnlinkANonSocketPath) {
  const char* content = "not a socket\n";
  int         f       = open(_sock, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  ASSERT_GE(f, 0);
  ASSERT_EQ((ssize_t)strlen(content), ::write(f, content, strlen(content)));
  close(f);

  LinuxConsole console;
  console.begin();

  // An operator who points MESHCORED_CONTROL_SOCKET at the wrong file gets a
  // refusal, not a deletion.
  struct stat st;
  ASSERT_EQ(0, lstat(_sock, &st));
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ((off_t)strlen(content), st.st_size);
  EXPECT_NE(std::string::npos, captured_stderr().find("is not a socket"));

  // ...and the daemon still comes up, on the next usable candidate.
  int client = connect_client(path_in_dir("meshcored.sock").c_str());
  ASSERT_GE(client, 0);
  ASSERT_EQ(1, ::write(client, "k", 1));
  EXPECT_EQ('k', console.read());
  close(client);
}

TEST_F(LinuxConsoleTest, ReportsAnOverlongSocketPath) {
  std::string too_long = std::string(_dir) + "/" + std::string(200, 'x');
  setenv("MESHCORED_CONTROL_SOCKET", too_long.c_str(), 1);

  LinuxConsole console;
  console.begin();

  EXPECT_NE(std::string::npos,
            captured_stderr().find("control socket path is too long"));

  int client = connect_client(path_in_dir("meshcored.sock").c_str());
  ASSERT_GE(client, 0);   // fell through to XDG_RUNTIME_DIR
  close(client);
}

TEST_F(LinuxConsoleTest, ReportsAnOverlongXdgRuntimeDir) {
  setenv("XDG_RUNTIME_DIR", std::string(200, 'y').c_str(), 1);

  LinuxConsole console;
  console.begin();

  // Truncating the path silently would bind the socket at a name nobody is
  // looking for.
  EXPECT_NE(std::string::npos,
            captured_stderr().find("XDG_RUNTIME_DIR is too long"));

  int client = connect_client(_sock);
  ASSERT_GE(client, 0);   // the override still worked
  close(client);
}

TEST_F(LinuxConsoleTest, DoesNotStealASocketAnotherInstanceIsUsing) {
  LinuxConsole first;
  first.begin();

  LinuxConsole second;
  second.begin();   // same MESHCORED_CONTROL_SOCKET

  EXPECT_NE(std::string::npos, captured_stderr().find("already in use"));

  // second.begin()'s liveness probe connected and hung up; one loop iteration
  // retires that connection. (It has to happen before the next connect(): the
  // backlog is one deep, and a blocking connect() to a full one waits.)
  EXPECT_EQ(-1, first.read());

  int client = connect_client(_sock);
  ASSERT_GE(client, 0);
  ASSERT_EQ(1, ::write(client, "m", 1));
  EXPECT_EQ('m', first.read());   // the first instance kept the path
  close(client);
}

// --- close-on-exec across LinuxBoard::reboot() -------------------------------
//
// reboot() re-execs this process image, and execv() keeps every descriptor that
// is not close-on-exec. An inherited listener is the damaging one: the new
// image's begin() decides whether another instance owns the control-socket path
// by connecting to it, and the leftover listener answers -- so the daemon
// declines its own path, falls through to /tmp, and after a second reboot has no
// control socket at all.

TEST_F(LinuxConsoleTest, ControlSocketDescriptorsAreCloseOnExec) {
  TestConsole console;
  console.begin();

  ASSERT_GE(console.serverFd(), 0);
  EXPECT_TRUE(fcntl(console.serverFd(), F_GETFD) & FD_CLOEXEC)
      << "an inherited listener answers the next image's liveness probe";

  int client = connect_client(_sock);
  ASSERT_GE(client, 0);
  ASSERT_EQ(1, ::write(client, "a", 1));
  EXPECT_EQ('a', console.read());   // one call accepts and reads
  ASSERT_GE(console.clientFd(), 0);

  // accept() does not carry the listener's descriptor flags across, so this is
  // a separate guarantee rather than a consequence of the one above.
  EXPECT_TRUE(fcntl(console.clientFd(), F_GETFD) & FD_CLOEXEC)
      << "an inherited client fd holds a dead session open at the far end";

  close(client);
}

// What the re-exec'd image must find: the socket file still on disk, but nothing
// listening on it, so try_bind()'s probe is refused and the stale-socket path
// unlinks and rebinds the same candidate.
TEST_F(LinuxConsoleTest, AReleasedListenerLeavesOnlyAStaleSocketToRebind) {
  {
    LinuxConsole console;
    console.begin();
    console.end();   // what LinuxBoard::reboot() does before execv()

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, _sock, sizeof(addr.sun_path) - 1);
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(probe, 0);
    EXPECT_NE(0, connect(probe, (struct sockaddr*)&addr, sizeof addr));
    EXPECT_EQ(ECONNREFUSED, errno) << "something is still listening after end()";
    close(probe);

    struct stat st;
    ASSERT_EQ(0, lstat(_sock, &st));   // the file outlives the descriptor
    EXPECT_TRUE(S_ISSOCK(st.st_mode));
  }   // and end() runs again from the destructor, harmlessly

  LinuxConsole restarted;
  restarted.begin();
  EXPECT_EQ(std::string::npos, captured_stderr().find("already in use"))
      << "the restarted daemon refused the path it had just released";

  int client = connect_client(_sock);
  ASSERT_GE(client, 0);
  ASSERT_EQ(1, ::write(client, "r", 1));
  EXPECT_EQ('r', restarted.read());
  close(client);
}

// --- PeekableStream contract -------------------------------------------------

TEST(PeekableStreamContract, PeekDoesNotConsume) {
  ScriptedStream s("ab");
  EXPECT_EQ(1, s.available());
  EXPECT_EQ('a', s.peek());
  EXPECT_EQ('a', s.peek());
  EXPECT_EQ(1, s.raw_calls) << "the lookahead must be filled once, not per peek";
  EXPECT_EQ('a', s.read());
  EXPECT_EQ('b', s.read());
  EXPECT_EQ(-1, s.read());
  EXPECT_EQ(0, s.available());
}

TEST(PeekableStreamContract, ClearPeekDropsTheLookahead) {
  ScriptedStream s("ab");
  EXPECT_EQ('a', s.peek());
  s.clearPeek();   // source closed or replaced
  EXPECT_EQ('b', s.read()) << "a byte from the old source must not survive it";
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
