#pragma once
#include "PeekableStream.h"

class LinuxEventLoop;

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
  LinuxConsole() = default;
  ~LinuxConsole();

  // The descriptors below are owned, not shared: a copy would close them from
  // two places and, worse, two consoles would each believe they own the single
  // control client. There is exactly one console.
  LinuxConsole(const LinuxConsole&) = delete;
  LinuxConsole& operator=(const LinuxConsole&) = delete;

  // Open the control socket and, if stdin is a TTY, put it in raw non-blocking
  // mode. Call once from setup(). Never blocks.
  void begin();

  int read() override;     // PeekableStream::read() plus newline normalisation
  int peek() override;     // same normalisation, so the two cannot disagree
  size_t write(uint8_t c) override;
  using Print::write;

  // Register the descriptors rawReadByte() will actually consume on its next
  // call, so a waiting daemon wakes on console traffic and on nothing else.
  //
  // This lives here rather than in the caller because it has to track
  // rawReadByte()'s precedence exactly: a registered descriptor that nothing
  // drains stays POLLIN forever, which turns the blocking wait back into the
  // busy loop LinuxEventLoop exists to remove. Keeping both in one class makes
  // that agreement structural instead of a comment in another file.
  void registerPollFds(LinuxEventLoop& loop) const;

protected:
  // One raw byte from the active input, or -1. Prefers a live control-socket
  // client, else accepts a pending one, else stdin -- the precedence
  // registerPollFds() mirrors.
  int rawReadByte() override;

  // Adopt an already-connected client descriptor: at most one at a time,
  // non-blocking, SIGPIPE-proof, and with no lookahead inherited from whatever
  // was being read before. tryAccept() is the only caller in the daemon; it is
  // a separate entry point so tests can reach client states accept() cannot
  // produce, such as a descriptor that fails every read().
  void attachClient(int fd);

  // The descriptor currently serving the console, or -1 when none is attached.
  int  clientFd() const { return _client_fd; }

private:
  bool tryAccept();
  void refuseExtraClients();
  void closeClient();
  void writeByte(uint8_t c);
  int  stdinFd() const;    // STDIN_FILENO if a TTY, else -1

  int  _server_fd = -1;    // listening Unix socket
  int  _client_fd = -1;    // currently connected control client, or -1
  bool _stdin_tty = false;
  char _sock_path[108] = {0};  // sockaddr_un.sun_path capacity
};

extern LinuxConsole Console;
