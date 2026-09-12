#include "LinuxEventLoop.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

// Cool-off applied to any wait that would otherwise return instantly without a
// readable descriptor, so a stale or hung-up descriptor cannot reinstate the
// busy loop.
#define EVENT_LOOP_ERROR_BACKOFF_US 1000

const int LinuxEventLoop::MAX_FDS;
LinuxEventLoop EventLoop;

void LinuxEventLoop::reset() {
  _nfds   = 0;
  _source = nullptr;
}

void LinuxEventLoop::registerFd(int fd) {
  if (fd < 0) return;
  if (_nfds >= MAX_FDS) return;
  for (int i = 0; i < _nfds; i++) {
    if (_fds[i] == fd) return;  // already watching it
  }
  _fds[_nfds++] = fd;
}

void LinuxEventLoop::setEventSource(LinuxEventSource* source) {
  _source = source;
}

int LinuxEventLoop::wait(int timeout_ms) {
  struct pollfd pfds[MAX_FDS + 1];
  int n          = 0;
  int source_idx = -1;

  if (_source != nullptr) {
    int fd = _source->eventFd();
    if (fd >= 0) {
      source_idx     = n;
      pfds[n].fd     = fd;
      pfds[n].events = POLLIN;
      pfds[n].revents = 0;
      n++;
    }
  }

  for (int i = 0; i < _nfds; i++) {
    pfds[n].fd      = _fds[i];
    pfds[n].events  = POLLIN;
    pfds[n].revents = 0;
    n++;
  }

  // poll() with zero descriptors is a portable plain sleep, which is exactly
  // the behaviour we want when nothing is available to watch.
  int rv = poll(n > 0 ? pfds : NULL, n, timeout_ms);

  if (rv < 0) {
    if (errno == EINTR) return -1;  // caller simply loops again
    usleep(EVENT_LOOP_ERROR_BACKOFF_US);
    return 0;
  }
  if (rv == 0) return 0;  // clean timeout

  // Count only genuinely readable descriptors. poll() also returns a positive
  // count for POLLNVAL (stale descriptor) and POLLHUP (peer hung up), neither
  // of which clears by itself — returning those to the caller unthrottled
  // would spin.
  int readable = 0;
  for (int i = 0; i < n; i++) {
    if ((pfds[i].revents & POLLIN) != 0) readable++;
  }

  if (readable == 0) {
    usleep(EVENT_LOOP_ERROR_BACKOFF_US);
    return 0;
  }

  if (source_idx >= 0 && (pfds[source_idx].revents & POLLIN) != 0) {
    // A drain that fails leaves the descriptor readable, which is the same
    // shape of problem as POLLNVAL above and needs the same answer. Reporting
    // the wake instead would hand the caller a descriptor it cannot clear:
    // poll() would return it again immediately, forever, at 100% of a core and
    // with nothing further in the log.
    if (!_source->drainEvents()) {
      usleep(EVENT_LOOP_ERROR_BACKOFF_US);
      return 0;
    }
  }
  return readable;
}
