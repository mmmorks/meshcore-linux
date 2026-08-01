#include "LinuxGpsStream.h"
#include <Mesh.h>            // MESH_DEBUG_PRINTLN
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static const char GPSD_SCHEME[] = "gpsd://";
static const char GPSD_DEFAULT_HOST[] = "127.0.0.1";
static const int  GPSD_DEFAULT_PORT = 2947;

LinuxGpsStream::Target LinuxGpsStream::parseDevice(const char* device) {
  Target t;

  if (device == NULL || device[0] == '\0') {
    t.transport = NONE;
    t.valid = true;
    return t;
  }

  if (strncmp(device, GPSD_SCHEME, sizeof(GPSD_SCHEME) - 1) != 0) {
    t.transport = SERIAL;
    t.valid = true;
    return t;
  }

  t.transport = GPSD;
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

bool LinuxGpsStream::begin(const char* path, int baud) {
  end();  // idempotent

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
}

int LinuxGpsStream::rawReadByte() {
  if (_fd < 0) return -1;
  uint8_t b;
  ssize_t n = ::read(_fd, &b, 1);
  if (n == 1) return b;
  return -1;  // n == 0 (no data) or -1/EAGAIN
}

size_t LinuxGpsStream::write(uint8_t c) {
  if (_fd < 0) return 0;
  ssize_t n = ::write(_fd, &c, 1);
  return n == 1 ? 1 : 0;
}
