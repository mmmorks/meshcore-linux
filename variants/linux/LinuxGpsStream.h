#pragma once
#include "PeekableStream.h"
#include <stdint.h>
#include <sys/socket.h>

// GPS byte source for the ArduLinux (Linux) build.
//
// ardulinux has no hardware UART / Serial1.setPath(); a GPS on Linux is either
// a /dev/tty* character device or, where gpsd owns the receiver, a socket. This
// opens whichever the config names and presents it as an Arduino Stream, so the
// shared MicroNMEALocationProvider reads NMEA through it unchanged.
// Modelled on LinuxConsole's fd wrapping.
class LinuxGpsStream : public PeekableStream {
public:
  enum Transport { NO_SOURCE, SERIAL_DEVICE, GPSD_SOCKET };

  // Parsed gps_device value. Public because both begin() and the ini
  // validation in LinuxBoard need to ask "is this string usable?" -- the
  // validator has to answer before any device is opened.
  struct Target {
    Transport transport = NO_SOURCE;
    char      host[64]  = "";   // GPSD_SOCKET only
    int       port      = 0;    // GPSD_SOCKET only
    bool      valid     = false;
  };

  // Parse a gps_device value. Never touches hardware.
  //   ""                     -> NO_SOURCE     (GPS disabled), valid
  //   "gpsd://[host][:port]" -> GPSD_SOCKET   defaults 127.0.0.1:2947
  //   anything else          -> SERIAL_DEVICE (a /dev path)
  // The enum members avoid the bare names NONE/SERIAL/GPSD: ArduinoCore-API's
  // Common.h defines SERIAL as a macro, which silently swallows an enumerator.
  // valid == false means the operator wrote a gpsd:// URL that cannot be
  // honoured; the caller counts it as a bad value and refuses to start.
  static Target parseDevice(const char* device);

  // Open whatever `device` names. `baud` applies to a serial device only.
  // Returns false on a device string that cannot be honoured, or a serial
  // device that would not open. A configured-but-unreachable gpsd returns
  // true: the socket is retried, and the daemon must not treat gpsd starting
  // late as "no GPS".
  bool begin(const char* device, int baud);
  void end();

  // "There is a GPS to talk to."
  //   SERIAL_DEVICE -- the fd is open. A /dev/tty* that would not open is a
  //                    permanent failure for this boot, and claiming a GPS
  //                    would be a lie.
  //   GPSD_SOCKET   -- a source is configured, connected or not. gpsd may
  //                    legitimately start after meshcored, and initBasicGPS()
  //                    only asks once.
  bool isPresent() const;

  Transport transport() const { return _transport; }

  size_t write(uint8_t c) override;
  using Print::write;

protected:
  int rawReadByte() override;   // one raw byte from the fd, or -1

private:
  // gpsd connection state. CONNECTING exists because connect() must not block
  // the main loop: it is started non-blocking and completed on a later read.
  enum GpsdState { GPSD_IDLE, GPSD_CONNECTING, GPSD_READY };

  bool openSerial(const char* path, int baud);
  bool resolveGpsd(const Target& t);
  void startConnect();
  void finishConnect();
  void dropConnection();
  void serviceGpsd();

  int       _fd = -1;
  Transport _transport = NO_SOURCE;

  struct sockaddr_storage _addr = {};
  socklen_t _addr_len = 0;

  GpsdState _state = GPSD_IDLE;
  uint32_t  _retry_at_ms    = 0;   // earliest next connect attempt
  uint32_t  _retry_delay_ms = 0;   // current backoff, 0 until the first failure
  uint32_t  _connect_at_ms  = 0;   // when the in-flight connect started
  uint32_t  _last_read_ms   = 0;   // for the read-gap reconnect
};
