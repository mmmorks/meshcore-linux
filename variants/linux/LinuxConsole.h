#pragma once
#include "PeekableStream.h"

// Control console for the ArduLinux (Linux) build.
//
// On Linux the Arduino `Serial` object is an output-only stub (its read() always
// returns -1), so the firmware's serial CLI can print but never receives typed
// commands. LinuxConsole fills that gap: it is a Stream the CLI reads from and
// writes to, sourcing bytes from either
//
//   * a Unix-domain control socket  -> for the systemd daemon case, or
//   * stdin                         -> when meshcored runs in a foreground TTY.
//
// Command echo and replies are written back to the connected socket client, or
// to stdout when driven from stdin. A socket client always takes priority over
// stdin. This class is Linux-only; other platforms keep using hardware Serial.
class LinuxConsole : public PeekableStream {
public:
  // Open the control socket and, if stdin is a TTY, put it in raw non-blocking
  // mode. Call once from setup(). Never blocks.
  void begin();

  int read() override;     // PeekableStream::read() plus newline normalisation
  size_t write(uint8_t c) override;
  using Print::write;

  // Absolute path of the control socket that was opened (empty if none).
  const char* socketPath() const { return _sock_path; }

  // Descriptors the Linux event loop polls so a waiting daemon wakes on
  // console traffic. Any of these may be -1, which the event loop ignores.
  int serverFd() const { return _server_fd; }   // listening control socket
  int clientFd() const { return _client_fd; }   // connected client, -1 if none
  int stdinFd()  const;                         // STDIN_FILENO if a TTY, else -1

protected:
  // One raw byte from the active input, or -1. Prefers a live control-socket
  // client, else accepts a pending one, else stdin.
  int rawReadByte() override;

private:
  bool tryAccept();
  void writeByte(uint8_t c);

  int  _server_fd = -1;    // listening Unix socket
  int  _client_fd = -1;    // currently connected control client, or -1
  bool _stdin_tty = false;
  char _sock_path[108] = {0};  // sockaddr_un.sun_path capacity
};

extern LinuxConsole Console;
