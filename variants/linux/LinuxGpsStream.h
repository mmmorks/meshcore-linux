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
  enum Transport { NONE, SERIAL, GPSD };

  // Parsed gps_device value. Public because both begin() and the ini
  // validation in LinuxBoard need to ask "is this string usable?" -- the
  // validator has to answer before any device is opened.
  struct Target {
    Transport transport = NONE;
    char      host[64]  = "";   // GPSD only
    int       port      = 0;    // GPSD only
    bool      valid     = false;
  };

  // Parse a gps_device value. Never touches hardware.
  //   ""                       -> NONE   (GPS disabled), valid
  //   "gpsd://[host][:port]"   -> GPSD,  defaults 127.0.0.1:2947
  //   anything else            -> SERIAL (a /dev path)
  // valid == false means the operator wrote a gpsd:// URL that cannot be
  // honoured; the caller counts it as a bad value and refuses to start.
  static Target parseDevice(const char* device);

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
