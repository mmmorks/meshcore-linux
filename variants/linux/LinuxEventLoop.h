#pragma once

#include "LinuxEventSource.h"

// Blocking wait for the ArduLinux main loop.
//
// ArduLinux's runtime spins `while (true) { gpioIdle(); loop(); }` with no
// sleep once real hardware is bound (its 100 ms delay is skipped when
// realHardware is true), which burns 100% of a core. Calling wait() at the end
// of loop() makes that outer loop run at the event rate instead.
//
// Besides the single wake-up source, registerFd() takes any other pollable
// descriptor the caller wants included in the same wait. reset() drops the
// whole set, so the intended usage is to rebuild it each iteration and let
// descriptors that only exist part of the time (a device opened on demand, an
// accepted client socket) simply not be registered on the iterations they are
// absent -- there is nothing to deregister.
class LinuxEventLoop {
public:
  static const int MAX_FDS = 8;

  // Drop all registered descriptors and the event source.
  void reset();

  // Watch a descriptor for readability until the next reset(). Negative
  // descriptors are ignored rather than rejected, so a caller can hand over
  // "the fd if I have one" unconditionally; duplicates and anything beyond
  // MAX_FDS are dropped too.
  //
  // Only register a descriptor the caller will actually read this iteration.
  // POLLIN is level-triggered: one that nothing drains stays readable forever
  // and turns every wait() into an immediate return.
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
  // POLLNVAL, a hung-up peer reporting POLLHUP, a poll() failure other than
  // EINTR, or an event source that reports it could not drain — sleep 1 ms and
  // report 0. poll() returns a positive count for POLLNVAL, and a descriptor
  // that failed to drain stays readable, so without this either one would turn
  // the wait back into the busy loop this class exists to remove.
  int wait(int timeout_ms);

  int registeredCount() const { return _nfds; }

private:
  int               _fds[MAX_FDS];
  int               _nfds   = 0;
  LinuxEventSource* _source = nullptr;
};

extern LinuxEventLoop EventLoop;
