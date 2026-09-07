#pragma once

// Abstract wake-up source for LinuxEventLoop.
//
// This interface exists so LinuxEventLoop can be compiled and unit-tested on a
// host with no libgpiod and no GPIO hardware: the loop depends only on this
// plus plain file descriptors. EventGPIOPin is the production implementation.
class LinuxEventSource {
public:
  virtual ~LinuxEventSource() {}

  // Pollable descriptor that becomes readable when an event is pending, or -1
  // when this source has no working event descriptor.
  virtual int eventFd() const = 0;

  // Consume every event queued on eventFd(). Must be called after poll()
  // reports the descriptor readable: POLLIN is level-triggered, so an
  // undrained descriptor makes every subsequent poll() return immediately.
  //
  // Returns false if the queue could not be emptied -- a read error on the
  // event descriptor, say. That has to be reported rather than swallowed:
  // the descriptor is then still readable, so a caller told the wait
  // succeeded would poll(), be woken instantly, fail to drain again, and
  // spin at 100% of a core with nothing in the log. LinuxEventLoop::wait()
  // backs off instead. Returning true with nothing drained is correct only
  // when there was nothing queued.
  virtual bool drainEvents() = 0;
};
