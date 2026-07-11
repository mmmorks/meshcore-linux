#include "LinuxSerialStream.h"
#include <Mesh.h>            // MESH_DEBUG_PRINTLN
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <string.h>

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
      MESH_DEBUG_PRINTLN("LinuxSerialStream: unsupported baud %d, using 9600", baud);
      return B9600;
  }
}

bool LinuxSerialStream::begin(const char* path, int baud) {
  end();  // idempotent

  _fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (_fd < 0) {
    MESH_DEBUG_PRINTLN("LinuxSerialStream: cannot open %s: %s", path, strerror(errno));
    return false;
  }

  struct termios tio;
  if (tcgetattr(_fd, &tio) != 0) {
    MESH_DEBUG_PRINTLN("LinuxSerialStream: tcgetattr(%s) failed: %s", path, strerror(errno));
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
    MESH_DEBUG_PRINTLN("LinuxSerialStream: tcsetattr(%s) failed: %s", path, strerror(errno));
    close(_fd);
    _fd = -1;
    return false;
  }

  tcflush(_fd, TCIFLUSH);
  MESH_DEBUG_PRINTLN("LinuxSerialStream: opened %s @ %d", path, baud);
  return true;
}

void LinuxSerialStream::end() {
  if (_fd >= 0) { close(_fd); _fd = -1; }
  _peek = -1;
}

int LinuxSerialStream::rawReadByte() {
  if (_fd < 0) return -1;
  uint8_t b;
  ssize_t n = ::read(_fd, &b, 1);
  if (n == 1) return b;
  return -1;  // n == 0 (no data) or -1/EAGAIN
}

int LinuxSerialStream::available() {
  if (_peek >= 0) return 1;
  _peek = rawReadByte();
  return _peek >= 0 ? 1 : 0;
}

int LinuxSerialStream::read() {
  if (_peek >= 0) { int b = _peek; _peek = -1; return b; }
  return rawReadByte();
}

int LinuxSerialStream::peek() {
  if (_peek < 0) _peek = rawReadByte();
  return _peek;
}

size_t LinuxSerialStream::write(uint8_t c) {
  if (_fd < 0) return 0;
  ssize_t n = ::write(_fd, &c, 1);
  return n == 1 ? 1 : 0;
}
