#include "LinuxConsole.h"

#include "LinuxEventLoop.h"

#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

LinuxConsole Console;

// --- stdin raw-mode handling (foreground/interactive use only) ---------------
static struct termios s_orig_tty;
static bool s_raw_active = false;

static void restore_tty() {
  if (s_raw_active) {
    tcsetattr(STDIN_FILENO, TCSANOW, &s_orig_tty);
    s_raw_active = false;
  }
}

static void set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Fill `addr` with the AF_UNIX address for `path`. False if it will not fit.
static bool fill_sun_path(struct sockaddr_un* addr, const char* path) {
  if (!path || !*path) return false;
  if (strlen(path) >= sizeof(addr->sun_path)) return false;

  memset(addr, 0, sizeof *addr);
  addr->sun_family = AF_UNIX;
  strncpy(addr->sun_path, path, sizeof(addr->sun_path) - 1);
  return true;
}

// True if something is already accepting connections at `path`.
//
// try_bind() has to unlink whatever it finds before it can bind, and a socket
// file gives no way to tell a stale one -- left behind by a crashed run -- from
// a live one owned by another meshcored. Unlinking a live one is silently
// destructive in both directions: the other daemon keeps listening on an
// inode with no name, so it becomes unreachable without noticing, and this one
// takes over the path as if nothing happened. Probing with connect() is the
// only way to distinguish them, so do that first and decline the path if it
// answers.
//
// Non-blocking because connect() to a listening AF_UNIX socket whose backlog is
// full otherwise *blocks* until a slot frees -- which would hang startup behind
// the other daemon's client. Non-blocking turns that case into EAGAIN, which is
// itself proof of a live listener.
static bool socket_is_live(const char* path) {
  struct sockaddr_un addr;
  if (!fill_sun_path(&addr, path)) return false;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;   // cannot probe; treat as stale rather than refuse to start
  set_nonblock(fd);

  // "Refused" is the only answer that proves nobody is home. Anything else --
  // connected, backlog full, still connecting -- means a listener, and a false
  // positive is cheap: begin() simply moves on to the next candidate path, so
  // the daemon still comes up, just on a different socket and having said so.
  bool live = connect(fd, (struct sockaddr*)&addr, sizeof addr) == 0
              || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
  close(fd);
  return live;
}

// Create, bind and listen on a non-blocking AF_UNIX socket at `path`.
// Returns the fd, or -1 on any failure (including "someone else is there").
static int try_bind(const char* path) {
  struct sockaddr_un addr;
  if (!fill_sun_path(&addr, path)) return -1;

  if (socket_is_live(path)) {
    fprintf(stderr, "[meshcored] control socket %s is already in use "
                    "by another instance; not taking it over\n", path);
    return -1;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  unlink(path);  // clear a stale socket left by a previous run

  // Anyone who reaches this socket gets the unauthenticated admin CLI, so it
  // must never exist -- even momentarily -- at wider permissions than intended.
  // bind() applies the process umask, which is inherited and typically 022, so
  // without this the socket appears world-connectable until the chmod() below.
  // Creating it at 0600 and *widening* to 0660 afterwards leaves no window; the
  // other order leaves one. (It is small either way in /run/meshcored, whose
  // 0750 mode covers it, but not in the /tmp fallback.)
  mode_t old_umask = umask(0177);
  int bind_rv = bind(fd, (struct sockaddr*)&addr, sizeof addr);
  umask(old_umask);
  if (bind_rv != 0) { close(fd); return -1; }

  if (listen(fd, 1) != 0) { close(fd); unlink(path); return -1; }

  set_nonblock(fd);
  chmod(path, 0660);  // owner + service group may connect
  return fd;
}

void LinuxConsole::begin() {
  // Pick a control-socket path: explicit override, then the systemd runtime
  // dir, then the per-user runtime dir, then /tmp.
  const char* candidates[4];
  char        xdg_buf[108];
  int         n = 0;

  const char* override_path = getenv("MESHCORED_CONTROL_SOCKET");
  if (override_path && *override_path) candidates[n++] = override_path;
  candidates[n++] = "/run/meshcored/meshcored.sock";
  const char* xrd = getenv("XDG_RUNTIME_DIR");
  if (xrd && *xrd) {
    snprintf(xdg_buf, sizeof xdg_buf, "%s/meshcored.sock", xrd);
    candidates[n++] = xdg_buf;
  }
  candidates[n++] = "/tmp/meshcored.sock";

  for (int i = 0; i < n; i++) {
    int fd = try_bind(candidates[i]);
    if (fd >= 0) {
      _server_fd = fd;
      strncpy(_sock_path, candidates[i], sizeof(_sock_path) - 1);
      break;
    }
  }

  if (_server_fd >= 0)
    fprintf(stderr, "[meshcored] control socket listening at %s\n", _sock_path);
  else
    fprintf(stderr, "[meshcored] WARNING: no control socket; "
                    "serial CLI is available on stdin only\n");

  // Foreground use: read keystrokes one at a time, no kernel echo (we echo
  // ourselves), and never block the mesh loop waiting for input.
  _stdin_tty = isatty(STDIN_FILENO);
  if (_stdin_tty) {
    // Raw mode below clears ECHO, which makes writeByte()'s putchar() the only
    // thing putting typed characters on screen -- and stdout to a terminal is
    // line buffered, so every character of a command would sit in the buffer
    // until Enter flushed the lot. The user types blind. Unbuffering is what
    // makes the echo appear per keystroke.
    //
    // Only in the TTY case: under systemd stdout is the journal, and one write
    // syscall per character of debug output is a real cost with nobody watching
    // it arrive. (The unit sets `stdbuf -oL` for that path instead.)
    setvbuf(stdout, NULL, _IONBF, 0);
  }
  if (_stdin_tty && tcgetattr(STDIN_FILENO, &s_orig_tty) == 0) {
    struct termios raw = s_orig_tty;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~(ICRNL | INLCR);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
      s_raw_active = true;
      atexit(restore_tty);
    }
  }
  if (_stdin_tty) set_nonblock(STDIN_FILENO);
}

int LinuxConsole::stdinFd() const {
  return _stdin_tty ? STDIN_FILENO : -1;
}

bool LinuxConsole::tryAccept() {
  if (_server_fd < 0) return false;
  int fd = accept(_server_fd, nullptr, nullptr);
  if (fd < 0) return false;
  set_nonblock(fd);
  _client_fd = fd;
  return true;
}

int LinuxConsole::rawReadByte() {
  // Prefer a live control-socket client; otherwise accept a pending one.
  if (_client_fd < 0) tryAccept();
  if (_client_fd >= 0) {
    uint8_t b;
    ssize_t r = ::read(_client_fd, &b, 1);
    if (r == 1) return b;
    if (r == 0) { close(_client_fd); _client_fd = -1; }  // client hung up
    return -1;  // EAGAIN, or just closed; retry on the next loop
  }
  if (_stdin_tty) {
    uint8_t b;
    if (::read(STDIN_FILENO, &b, 1) == 1) return b;
  }
  return -1;
}

int LinuxConsole::read() {
  int c = PeekableStream::read();
  if (c == '\n') c = '\r';  // normalise Enter so the CLI's '\r' check fires
  return c;
}

void LinuxConsole::registerPollFds(LinuxEventLoop& loop) const {
  // Mirrors rawReadByte()'s precedence exactly: with a client attached it reads
  // only that client, so registering the listening socket and stdin as well
  // would leave both permanently POLLIN with nothing to drain them.
  if (_client_fd < 0) {
    loop.registerFd(_server_fd);
    loop.registerFd(stdinFd());
  } else {
    loop.registerFd(_client_fd);
  }
}

void LinuxConsole::writeByte(uint8_t c) {
  if (_client_fd >= 0) {
    // MSG_NOSIGNAL: a disconnected client must not raise SIGPIPE and kill us.
    ssize_t w = send(_client_fd, &c, 1, MSG_NOSIGNAL);
    if (w > 0) return;

    // A full send buffer (a client that has stopped reading) or an interrupted
    // call says nothing about whether the client is still there. Dropping the
    // byte costs one character of a reply; tearing the session down costs the
    // rest of it, and dumps the remainder onto stdout where nobody asked for
    // it. Only a real error means the client is gone.
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;

    close(_client_fd);
    _client_fd = -1;  // fall through and echo to stdout instead
  }
  putchar(c);
  if (c == '\n') fflush(stdout);
}

size_t LinuxConsole::write(uint8_t c) {
  writeByte(c);
  return 1;
}
