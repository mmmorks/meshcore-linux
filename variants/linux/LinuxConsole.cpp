#include "LinuxConsole.h"

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

// Create, bind and listen on a non-blocking AF_UNIX socket at `path`.
// Returns the fd, or -1 on any failure.
static int try_bind(const char* path) {
  struct sockaddr_un addr;
  if (!path || !*path) return -1;
  if (strlen(path) >= sizeof(addr.sun_path)) return -1;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  unlink(path);  // clear a stale socket left by a previous run
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0) { close(fd); return -1; }
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

int LinuxConsole::available() {
  if (_peek < 0) _peek = rawReadByte();
  return _peek >= 0 ? 1 : 0;
}

int LinuxConsole::peek() {
  if (_peek < 0) _peek = rawReadByte();
  return _peek;
}

int LinuxConsole::read() {
  int c;
  if (_peek >= 0) { c = _peek; _peek = -1; }
  else            { c = rawReadByte(); }
  if (c == '\n') c = '\r';  // normalise Enter so the CLI's '\r' check fires
  return c;
}

void LinuxConsole::writeByte(uint8_t c) {
  if (_client_fd >= 0) {
    // MSG_NOSIGNAL: a disconnected client must not raise SIGPIPE and kill us.
    ssize_t w = send(_client_fd, &c, 1, MSG_NOSIGNAL);
    if (w > 0) return;
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
