#pragma once
#include "PeekableStream.h"

// GPS byte source for the ArduLinux (Linux) build.
//
// ardulinux has no hardware UART / Serial1.setPath(); a GPS on Linux is either
// a /dev/tty* character device or, where gpsd owns the receiver, a socket. This
// opens whichever the config names and presents it as an Arduino Stream, so the
// shared MicroNMEALocationProvider reads NMEA through it unchanged.
// Modelled on LinuxConsole's fd wrapping.
class LinuxGpsStream : public PeekableStream {
public:
  // Open `path` at `baud`. Returns true on success. On failure, logs the
  // device and errno and leaves the stream closed (isOpen() == false).
  // Non-fatal: callers proceed without GPS.
  bool begin(const char* path, int baud);
  void end();
  bool isOpen() const { return _fd >= 0; }

  size_t write(uint8_t c) override;
  using Print::write;

protected:
  int rawReadByte() override;   // one raw byte from the fd, or -1

private:
  int _fd = -1;
};
