#pragma once
#include <Stream.h>

// Serial-device Stream for the ArduLinux (Linux) build.
//
// ardulinux has no hardware UART / Serial1.setPath(); a serial GPS on Linux is
// a /dev/tty* character device. LinuxSerialStream opens such a device with
// termios (raw mode, configured baud, non-blocking) and presents it as an
// Arduino Stream so the shared MicroNMEALocationProvider can read/write NMEA
// through it unchanged. Modelled on LinuxConsole's fd wrapping.
class LinuxSerialStream : public Stream {
public:
  // Open `path` at `baud`. Returns true on success. On failure, logs the
  // device and errno and leaves the stream closed (isOpen() == false).
  // Non-fatal: callers proceed without GPS.
  bool begin(const char* path, int baud);
  void end();
  bool isOpen() const { return _fd >= 0; }

  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t c) override;
  using Print::write;

private:
  int rawReadByte();   // one raw byte from the fd, or -1

  int _fd   = -1;
  int _peek = -1;      // one-byte lookahead, or -1
};
