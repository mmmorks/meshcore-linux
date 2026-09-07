#include "LinuxGpsStream.h"
#include <Mesh.h>            // MESH_DEBUG_PRINTLN
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <Arduino.h>    // millis()

static const char GPSD_SCHEME[] = "gpsd://";
static const char GPSD_DEFAULT_HOST[] = "127.0.0.1";
static const int  GPSD_DEFAULT_PORT = 2947;

LinuxGpsStream::Target LinuxGpsStream::parseDevice(const char* device) {
  Target t;

  if (device == NULL || device[0] == '\0') {
    t.transport = NO_SOURCE;
    t.valid = true;
    return t;
  }

  if (strncmp(device, GPSD_SCHEME, sizeof(GPSD_SCHEME) - 1) != 0) {
    t.transport = SERIAL_DEVICE;
    t.valid = true;
    return t;
  }

  t.transport = GPSD_SOCKET;
  const char* rest = device + sizeof(GPSD_SCHEME) - 1;

  // Split host from port on the single permitted ':'. More than one means an
  // IPv6 literal (or a typo); either way host:port cannot be recovered
  // unambiguously, so reject rather than guess.
  const char* colon = strchr(rest, ':');
  size_t host_len = colon ? (size_t)(colon - rest) : strlen(rest);

  if (colon != NULL) {
    if (strchr(colon + 1, ':') != NULL) return t;   // invalid: multiple colons
    const char* p = colon + 1;
    if (*p == '\0') return t;                        // invalid: "host:"
    char* endp = NULL;
    long port = strtol(p, &endp, 10);
    if (endp == NULL || *endp != '\0') return t;     // invalid: non-numeric
    if (port < 1 || port > 65535) return t;          // invalid: out of range
    t.port = (int) port;
  } else {
    t.port = GPSD_DEFAULT_PORT;
  }

  if (host_len >= sizeof(t.host)) return t;          // invalid: host too long
  if (host_len == 0) {
    strcpy(t.host, GPSD_DEFAULT_HOST);
  } else {
    memcpy(t.host, rest, host_len);
    t.host[host_len] = '\0';
  }

  t.valid = true;
  return t;
}

// LinuxBoard::reboot() re-execs this process image, and execv() keeps every
// descriptor that is not close-on-exec. Neither of the ones opened here is
// reachable from the new image, so each reboot would leak a socket to gpsd or
// an open handle on the GPS tty for the life of the rebooted daemon.
//
// fcntl() rather than SOCK_CLOEXEC/O_CLOEXEC: this file also compiles for the
// native test build on macOS, and one pattern is easier to check than two. The
// window before the flag is set is harmless -- the only exec is this process's
// own reboot(), which cannot run part-way through these calls.
static void set_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFD, 0);
  if (fl != -1) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

// Map an integer baud to the matching termios Bxxxx constant.
// Unknown values log and fall back to B9600.
static speed_t baud_to_speed(int baud) {
  switch (baud) {
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:
      MESH_DEBUG_PRINTLN("LinuxGpsStream: unsupported baud %d, using 9600", baud);
      return B9600;
  }
}

// gpsd streams NMEA verbatim when asked to. Reading NMEA back out of gpsd is
// what lets MicroNMEALocationProvider stay untouched -- the alternative, gpsd's
// JSON TPV/SKY protocol, would mean teaching the shared provider a second wire
// format for no gain.
static const char WATCH_CMD[] = "?WATCH={\"enable\":true,\"nmea\":true}\n";

static const uint32_t RETRY_MIN_MS     = 1000;
static const uint32_t RETRY_MAX_MS     = 30000;
static const uint32_t CONNECT_LIMIT_MS = 5000;
static const uint32_t READ_GAP_MS      = 5000;

bool LinuxGpsStream::begin(const char* device, int baud) {
  end();  // idempotent

  Target t = parseDevice(device);
  if (!t.valid) {
    printf("ERROR: gps_device '%s' is not a usable device or gpsd:// URL\n", device);
    return false;
  }

  _transport = t.transport;
  if (_transport == NO_SOURCE) return false;
  if (_transport == SERIAL_DEVICE) {
    // Remembered so serviceSerial() can reopen the device after it goes away.
    // LinuxConfig::load() rejects anything longer than DEVICE_MAX, so this
    // cannot truncate a value that came from meshcored.ini.
    snprintf(_dev_path, sizeof _dev_path, "%s", device);
    _dev_baud = baud;
    return openSerial(device, baud);
  }

  if (!resolveGpsd(t)) {
    _transport = NO_SOURCE;
    return false;
  }
  startConnect();     // may not complete now; rawReadByte() finishes it
  return true;
}

// Resolve once, at startup, where blocking is already accepted. Reconnects then
// reuse the cached address -- getaddrinfo() on a read path could stall the main
// loop for the length of a DNS timeout.
bool LinuxGpsStream::resolveGpsd(const Target& t) {
  char port[8];
  snprintf(port, sizeof port, "%d", t.port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = NULL;
  int rc = getaddrinfo(t.host, port, &hints, &res);
  if (rc != 0 || res == NULL) {
    printf("ERROR: cannot resolve gpsd host '%s': %s\n", t.host, gai_strerror(rc));
    return false;
  }
  memcpy(&_addr, res->ai_addr, res->ai_addrlen);
  _addr_len = res->ai_addrlen;
  freeaddrinfo(res);
  return true;
}

void LinuxGpsStream::startConnect() {
  // socket() + fcntl() rather than SOCK_NONBLOCK: the native test build runs on
  // the host, which may not be Linux.
  _fd = socket(_addr.ss_family, SOCK_STREAM, 0);
  if (_fd < 0) { dropConnection(); return; }
  set_cloexec(_fd);
  fcntl(_fd, F_SETFL, fcntl(_fd, F_GETFL, 0) | O_NONBLOCK);

  int one = 1;
  setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  _connect_at_ms = millis();
  if (::connect(_fd, (struct sockaddr*)&_addr, _addr_len) == 0) {
    _state = GPSD_CONNECTING;   // finishConnect() still has to send WATCH
    return;
  }
  if (errno == EINPROGRESS) { _state = GPSD_CONNECTING; return; }
  dropConnection();
}

void LinuxGpsStream::finishConnect() {
  struct pollfd p;
  p.fd = _fd; p.events = POLLOUT; p.revents = 0;
  if (poll(&p, 1, 0) <= 0) {
    // Still in flight. Give up eventually rather than sit here forever.
    if ((uint32_t)(millis() - _connect_at_ms) > CONNECT_LIMIT_MS) dropConnection();
    return;
  }

  int err = 0;
  socklen_t len = sizeof err;
  if (getsockopt(_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
    dropConnection();
    return;
  }

  size_t sent = 0;
  const size_t want = sizeof(WATCH_CMD) - 1;
  while (sent < want) {
    ssize_t n = ::write(_fd, WATCH_CMD + sent, want - sent);
    if (n > 0) { sent += (size_t) n; continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    dropConnection();
    return;
  }
  if (sent < want) {
    // A freshly-connected non-blocking socket's send buffer would not take
    // the whole WATCH command in one write(). Nothing retries the remainder
    // and the read-gap logic cannot detect the resulting silence, so this
    // must not be treated as READY -- drop and let startConnect() retry the
    // handshake from scratch.
    dropConnection();
    return;
  }

  _state = GPSD_READY;
  _retry_delay_ms = 0;
  MESH_DEBUG_PRINTLN("LinuxGpsStream: connected to gpsd");
}

// Close and arm the backoff. Doubling from 1 s to a 30 s ceiling: a gpsd that
// is simply not running yet should be picked up quickly, but a permanently
// absent one must not be retried in a tight loop for the life of the daemon.
// Shared with the serial path, which wants exactly the same schedule for a
// device node that has gone away.
void LinuxGpsStream::dropConnection() {
  if (_fd >= 0) { close(_fd); _fd = -1; }
  clearPeek();
  _state = GPSD_IDLE;
  _retry_delay_ms = _retry_delay_ms == 0 ? RETRY_MIN_MS
                  : (_retry_delay_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : _retry_delay_ms * 2);
  _retry_at_ms = millis() + _retry_delay_ms;
}

void LinuxGpsStream::serviceGpsd() {
  uint32_t now = millis();

  // EnvironmentSensorManager drains this stream only while gps_active is true,
  // so a `gps off` stops the reads entirely. A tty just queues undrained bytes
  // (serviceSerialGap() below flushes those); a socket's receive window fills
  // instead, and gpsd drops clients it cannot write to. Dropping it ourselves
  // is both the polite move and the one that stops `gps on` from replaying a
  // backlog of stale NMEA as though it were current.
  if (_state == GPSD_READY && _last_read_ms != 0 &&
      (uint32_t)(now - _last_read_ms) > READ_GAP_MS) {
    dropConnection();
    _retry_at_ms = now;      // deliberate: a fresh start, not a failure to back off from
    _retry_delay_ms = 0;
  }
  _last_read_ms = now;

  if (_state == GPSD_IDLE) {
    if ((int32_t)(now - _retry_at_ms) >= 0) startConnect();
    return;
  }
  if (_state == GPSD_CONNECTING) finishConnect();
}

// The serial path's counterpart to serviceGpsd(): reopen a device that went
// away, and flush a backlog that piled up while nothing was reading.
//
// Reopen -- a USB receiver unplugged (or a hub reset, or a CDC-ACM device
// re-enumerating) makes read() return EIO or ENXIO forever. Without this the fd
// stays open and the GPS is silently dead until meshcored restarts, which is
// reachable: the ini templates recommend /dev/ttyACM0 and /dev/ttyUSB0, and a
// replugged device comes back under the same name (or the same by-id path).
//
// Flush -- `gps off` stops EnvironmentSensorManager from draining this stream,
// so NMEA sentences pile up in the tty's kernel input queue (~4 KB) while
// nothing reads them. The gpsd path drops and re-establishes the connection;
// flushing is the tty equivalent, so `gps on` sees fresh data instead of
// replaying the backlog as though it were current.
void LinuxGpsStream::serviceSerial() {
  uint32_t now = millis();

  if (_fd < 0) {
    if ((int32_t)(now - _retry_at_ms) < 0) return;
    if (openSerial(_dev_path, _dev_baud)) {
      printf("NOTE: GPS device %s reopened.\n", _dev_path);
      _retry_delay_ms = 0;
      _last_read_ms = 0;    // nothing has been read since; do not flush on the next call
    } else {
      dropConnection();     // openSerial() left _fd at -1; this re-arms the backoff
    }
    return;
  }

  if (_last_read_ms != 0 && (uint32_t)(now - _last_read_ms) > READ_GAP_MS) {
    tcflush(_fd, TCIFLUSH);
  }
  _last_read_ms = now;
}

bool LinuxGpsStream::isPresent() const {
  if (_transport == SERIAL_DEVICE) return _fd >= 0;
  return _transport == GPSD_SOCKET;
}

bool LinuxGpsStream::openSerial(const char* path, int baud) {
  _fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (_fd < 0) {
    MESH_DEBUG_PRINTLN("LinuxGpsStream: cannot open %s: %s", path, strerror(errno));
    return false;
  }
  set_cloexec(_fd);

  struct termios tio;
  if (tcgetattr(_fd, &tio) != 0) {
    MESH_DEBUG_PRINTLN("LinuxGpsStream: tcgetattr(%s) failed: %s", path, strerror(errno));
    close(_fd);
    _fd = -1;
    return false;
  }

  cfmakeraw(&tio);
  speed_t sp = baud_to_speed(baud);
  cfsetispeed(&tio, sp);
  cfsetospeed(&tio, sp);
  tio.c_cflag |= (CLOCAL | CREAD);  // ignore modem ctrl lines, enable receiver
  tio.c_cc[VMIN]  = 0;              // non-blocking read
  tio.c_cc[VTIME] = 0;

  if (tcsetattr(_fd, TCSANOW, &tio) != 0) {
    MESH_DEBUG_PRINTLN("LinuxGpsStream: tcsetattr(%s) failed: %s", path, strerror(errno));
    close(_fd);
    _fd = -1;
    return false;
  }

  tcflush(_fd, TCIFLUSH);
  MESH_DEBUG_PRINTLN("LinuxGpsStream: opened %s @ %d", path, baud);
  return true;
}

void LinuxGpsStream::end() {
  if (_fd >= 0) { close(_fd); _fd = -1; }
  clearPeek();
  _transport = NO_SOURCE;
  _dev_path[0] = '\0';
  _dev_baud = 0;
  _state = GPSD_IDLE;
  _retry_delay_ms = 0;
  _retry_at_ms = 0;
  _last_read_ms = 0;
}

int LinuxGpsStream::rawReadByte() {
  if (_transport == GPSD_SOCKET) serviceGpsd();
  else if (_transport == SERIAL_DEVICE) serviceSerial();
  if (_fd < 0 || _state == GPSD_CONNECTING) return -1;

  uint8_t b;
  ssize_t n = ::read(_fd, &b, 1);
  if (n == 1) return b;

  // EINTR is a signal interrupting the call, not a broken source -- benign,
  // same as EAGAIN/EWOULDBLOCK. Any other error means the source is gone.
  bool fatal = n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;

  if (_transport == GPSD_SOCKET) {
    // n == 0 is the peer hanging up -- gpsd restarted, or dropped a client it
    // could not write to. On a tty n == 0 means "no data right now" instead
    // (VMIN and VTIME are both 0), so only the socket path reads it that way.
    if (n == 0 || fatal) dropConnection();
  } else if (_transport == SERIAL_DEVICE && fatal) {
    // Once per disappearance, not once per read: the fd is closed here, so the
    // next report can only come after a successful reopen and a fresh failure.
    printf("WARNING: GPS device %s read failed (%s); closing and retrying.\n",
           _dev_path, strerror(errno));
    dropConnection();
  }
  return -1;  // n == 0 (no data) or -1/EAGAIN/EINTR
}

size_t LinuxGpsStream::write(uint8_t c) {
  // gpsd's socket speaks JSON commands, not raw NMEA, so forwarding a sentence
  // there would be wrong rather than merely useless. Nothing calls this today:
  // LocationProvider::sendSentence() has no callers anywhere in the tree.
  if (_transport == GPSD_SOCKET) return 0;
  if (_fd < 0) return 0;
  ssize_t n = ::write(_fd, &c, 1);
  return n == 1 ? 1 : 0;
}
