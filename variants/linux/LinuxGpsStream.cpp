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
  if (_transport == SERIAL_DEVICE) return openSerial(device, baud);

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
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // rare; retried below
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
  // so a `gps off` stops the reads entirely. A tty tolerates that -- undrained
  // bytes just age out of the kernel buffer -- but a socket does not: the
  // receive window fills and gpsd drops clients it cannot write to. Dropping it
  // ourselves is both the polite move and the one that stops `gps on` from
  // replaying a backlog of stale NMEA as though it were current.
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
  _state = GPSD_IDLE;
  _retry_delay_ms = 0;
  _retry_at_ms = 0;
  _last_read_ms = 0;
}

int LinuxGpsStream::rawReadByte() {
  if (_transport == GPSD_SOCKET) serviceGpsd();
  if (_fd < 0 || _state == GPSD_CONNECTING) return -1;

  uint8_t b;
  ssize_t n = ::read(_fd, &b, 1);
  if (n == 1) return b;

  // On a socket, n == 0 is the peer hanging up -- gpsd restarted, or dropped a
  // client it could not write to. A tty never reports that, so only the socket
  // path treats it as a disconnect.
  if (_transport == GPSD_SOCKET && (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))) {
    dropConnection();
  }
  return -1;  // n == 0 (no data) or -1/EAGAIN
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
