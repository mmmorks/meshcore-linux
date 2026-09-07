#pragma once

#include "LinuxEventSource.h"

// Blocking wait for the ArduLinux main loop.
//
// ArduLinux's runtime spins `while (true) { gpioIdle(); loop(); }` with no
// sleep once real hardware is bound (its 100 ms delay is skipped when
// realHardware is true), which burns 100% of a core. Calling wait() at the end
// of loop() makes that outer loop run at the event rate instead.
//
// The descriptor set is rebuilt on every call because the console's connected
// client descriptor comes and goes as clients attach and detach.
class LinuxEventLoop {
public:
  static const int MAX_FDS = 8;

  // Drop all registered descriptors and the event source.
  void reset();

  // Watch a descriptor for readability. Negative descriptors (a closed GPS
  // device, an unconnected console client) are ignored, as are duplicates and
  // anything beyond MAX_FDS.
  void registerFd(int fd);

  // Set the wake-up source, or NULL when none is available.
  void setEventSource(LinuxEventSource* source);

  // Block until a watched descriptor is readable or timeout_ms elapses, then
  // drain the event source if it was the one that fired.
  //
  // Returns the number of descriptors reporting POLLIN, or 0 if the wait timed
  // out. Returns -1 only on EINTR, where returning immediately is correct
  // because the caller loops again anyway.
  //
  // Conditions that would otherwise spin — a stale descriptor reporting
  // POLLNVAL, a hung-up peer reporting POLLHUP, or a poll() failure other than
  // EINTR — sleep 1 ms and report 0. poll() returns a positive count for
  // POLLNVAL, so without this a single closed descriptor would turn the wait
  // back into the busy loop this class exists to remove.
  int wait(int timeout_ms);

  int registeredCount() const { return _nfds; }

private:
  int               _fds[MAX_FDS];
  int               _nfds   = 0;
  LinuxEventSource* _source = nullptr;
};

extern LinuxEventLoop EventLoop;
