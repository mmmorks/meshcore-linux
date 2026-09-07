#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "LinuxEventLoop.h"

namespace {

// Event source backed by a pipe, so a test can make it readable on demand.
class FakePipeSource : public LinuxEventSource {
public:
  FakePipeSource() {
    if (pipe(_fds) != 0) { _fds[0] = -1; _fds[1] = -1; return; }
    // Make both ends non-blocking for drainEvents() to work correctly
    fcntl(_fds[0], F_SETFL, fcntl(_fds[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(_fds[1], F_SETFL, fcntl(_fds[1], F_GETFL, 0) | O_NONBLOCK);
  }
  ~FakePipeSource() override {
    if (_fds[0] >= 0) close(_fds[0]);
    if (_fds[1] >= 0) close(_fds[1]);
  }

  int eventFd() const override { return _enabled ? _fds[0] : -1; }

  bool drainEvents() override {
    drain_calls++;
    // A source that cannot drain leaves its descriptor readable -- that is the
    // whole reason the failure has to reach the caller -- so read nothing here.
    if (!_drain_ok) return false;
    unsigned char buf[64];
    while (::read(_fds[0], buf, sizeof buf) > 0) { }
    return true;
  }

  // Simulate a persistent read error on the event descriptor (EIO on a wedged
  // GPIO controller, say).
  void failDrains() { _drain_ok = false; }

  // Make the source readable.
  void signal() {
    unsigned char b = 1;
    ssize_t n = ::write(_fds[1], &b, 1);
    (void)n;
  }

  void disable() { _enabled = false; }

  int pendingBytes() {
    int fl = fcntl(_fds[0], F_GETFL, 0);
    fcntl(_fds[0], F_SETFL, fl | O_NONBLOCK);
    unsigned char buf[64];
    ssize_t n = ::read(_fds[0], buf, sizeof buf);
    return n > 0 ? (int)n : 0;
  }

  int drain_calls = 0;

private:
  int  _fds[2];
  bool _enabled  = true;
  bool _drain_ok = true;
};

// A readable descriptor that is NOT the event source.
class FakePipeFd {
public:
  FakePipeFd() {
    if (pipe(_fds) != 0) { _fds[0] = -1; _fds[1] = -1; return; }
    // Make read end non-blocking
    fcntl(_fds[0], F_SETFL, fcntl(_fds[0], F_GETFL, 0) | O_NONBLOCK);
  }
  ~FakePipeFd() {
    if (_fds[0] >= 0) close(_fds[0]);
    if (_fds[1] >= 0) close(_fds[1]);
  }
  int readFd() const { return _fds[0]; }
  void signal() {
    unsigned char b = 1;
    ssize_t n = ::write(_fds[1], &b, 1);
    (void)n;
  }
private:
  int _fds[2];
};

}  // namespace

TEST(LinuxEventLoopRegister, IgnoresNegativeDescriptors) {
  LinuxEventLoop loop;
  loop.reset();
  loop.registerFd(-1);
  loop.registerFd(-42);
  EXPECT_EQ(0, loop.registeredCount());
}

TEST(LinuxEventLoopRegister, IgnoresDuplicates) {
  LinuxEventLoop loop;
  loop.reset();
  int fd = open("/dev/null", O_RDONLY);
  ASSERT_GE(fd, 0);
  loop.registerFd(fd);
  loop.registerFd(fd);
  EXPECT_EQ(1, loop.registeredCount());
  close(fd);
}

TEST(LinuxEventLoopRegister, CapsAtMaxFds) {
  LinuxEventLoop loop;
  loop.reset();
  int fds[LinuxEventLoop::MAX_FDS + 3];
  for (int i = 0; i < LinuxEventLoop::MAX_FDS + 3; i++) {
    fds[i] = open("/dev/null", O_RDONLY);
    ASSERT_GE(fds[i], 0);
    loop.registerFd(fds[i]);
  }
  EXPECT_EQ(LinuxEventLoop::MAX_FDS, loop.registeredCount());
  for (int i = 0; i < LinuxEventLoop::MAX_FDS + 3; i++) close(fds[i]);
}

TEST(LinuxEventLoopRegister, ResetClearsEverything) {
  LinuxEventLoop loop;
  FakePipeSource source;
  loop.reset();
  loop.registerFd(source.eventFd());
  loop.setEventSource(&source);
  loop.reset();
  EXPECT_EQ(0, loop.registeredCount());
  // With no source and no fds, wait() must still honour the timeout.
  EXPECT_EQ(0, loop.wait(1));
}

TEST(LinuxEventLoopWait, TimesOutWithNothingRegistered) {
  LinuxEventLoop loop;
  loop.reset();
  EXPECT_EQ(0, loop.wait(1));
}

TEST(LinuxEventLoopWait, WakesOnEventSourceAndDrainsIt) {
  LinuxEventLoop loop;
  FakePipeSource source;
  loop.reset();
  loop.setEventSource(&source);

  source.signal();
  EXPECT_GE(loop.wait(1000), 1);
  EXPECT_EQ(1, source.drain_calls);
  // Draining must have emptied the descriptor, otherwise poll() would spin.
  EXPECT_EQ(0, source.pendingBytes());
}

TEST(LinuxEventLoopWait, DisabledSourceIsNotPolled) {
  LinuxEventLoop loop;
  FakePipeSource source;
  loop.reset();
  loop.setEventSource(&source);
  source.signal();
  source.disable();  // eventFd() now returns -1

  EXPECT_EQ(0, loop.wait(1));
  EXPECT_EQ(0, source.drain_calls);
}

TEST(LinuxEventLoopWait, WakesOnRegisteredFdWithoutDraining) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakePipeFd other;
  loop.reset();
  loop.setEventSource(&source);
  loop.registerFd(other.readFd());

  other.signal();
  EXPECT_GE(loop.wait(1000), 1);
  // Only the event source gets drained; plain descriptors are the caller's job.
  EXPECT_EQ(0, source.drain_calls);
}

TEST(LinuxEventLoopWait, NullSourceIsSafe) {
  LinuxEventLoop loop;
  FakePipeFd other;
  loop.reset();
  loop.setEventSource(nullptr);
  loop.registerFd(other.readFd());
  other.signal();
  EXPECT_GE(loop.wait(1000), 1);
}

TEST(LinuxEventLoopWait, StaleDescriptorReportsNothingReadable) {
  LinuxEventLoop loop;
  loop.reset();
  int fd = open("/dev/null", O_RDONLY);
  ASSERT_GE(fd, 0);
  loop.registerFd(fd);
  close(fd);  // stale: poll() sets POLLNVAL and returns a POSITIVE count

  // Passing that count up would make the caller loop with no delay, which is
  // exactly the busy-wait this class removes.
  EXPECT_EQ(0, loop.wait(0));
}

TEST(LinuxEventLoopWait, HungUpDescriptorDoesNotSpinTheLoop) {
  // The POLLHUP sibling of the POLLNVAL case above: a pipe whose write end is
  // closed reports its hangup on every poll() and never clears, so passing that
  // count up would reinstate the busy loop.
  //
  // Platforms disagree on the revents. Linux -- which is where CI runs this
  // native build, and the only platform the production code targets -- reports
  // POLLHUP alone. macOS, where the same build is often run locally, also sets
  // POLLIN, because read() returns 0 without blocking and its poll() calls that
  // readable; there the descriptor is genuinely drainable and one wake is the
  // right answer. What must hold everywhere is that a hung-up descriptor never
  // yields a wake the caller cannot clear, so assert the platform's own view of
  // readability rather than hardcoding either one.
  LinuxEventLoop loop;
  loop.reset();

  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  close(fds[1]);            // hangup, with no data behind it
  loop.registerFd(fds[0]);

  struct pollfd probe = { fds[0], POLLIN, 0 };
  ASSERT_GE(poll(&probe, 1, 0), 0);
  int expected = (probe.revents & POLLIN) != 0 ? 1 : 0;

  EXPECT_EQ(expected, loop.wait(0));
  close(fds[0]);
}

TEST(LinuxEventLoopWait, CountsOnlyReadableDescriptors) {
  LinuxEventLoop loop;
  FakePipeFd a;
  FakePipeFd b;
  loop.reset();
  loop.registerFd(a.readFd());
  loop.registerFd(b.readFd());

  a.signal();  // only one of the two becomes readable
  EXPECT_EQ(1, loop.wait(1000));
}

TEST(LinuxEventLoopWait, BackoffSleepsOnStaleDescriptor) {
  // Discriminator: proves usleep(EVENT_LOOP_ERROR_BACKOFF_US) actually executes.
  // Removing that line would fail this test.
  LinuxEventLoop loop;
  loop.reset();

  int fd = open("/dev/null", O_RDONLY);
  ASSERT_GE(fd, 0);
  loop.registerFd(fd);
  close(fd);  // stale descriptor

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  EXPECT_EQ(0, loop.wait(0));  // zero timeout, but should sleep due to stale descriptor

  clock_gettime(CLOCK_MONOTONIC, &end);

  // Calculate elapsed time in microseconds
  long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000 +
                    (end.tv_nsec - start.tv_nsec) / 1000;

  // 100 µs, not something closer to the 1000 µs backoff: usleep() may return
  // early on EINTR, and the only thing this test needs to discriminate is
  // "slept at all" versus "returned instantly". A tighter bound would buy no
  // extra discrimination and would flake under a signal.
  EXPECT_GE(elapsed_us, 100);
}

TEST(LinuxEventLoopWait, FailedDrainBacksOffInsteadOfReportingReadable) {
  // Why drainEvents() returns bool. A source that cannot clear its descriptor
  // (a persistent read error on the gpiod event fd) leaves it readable; if
  // wait() reported the wake anyway, the caller would poll(), be woken at
  // once, fail to drain again, and burn 100% of a core with nothing further in
  // the log -- the exact busy loop this class exists to remove, and the one
  // shape of it that is invisible from the outside.
  LinuxEventLoop loop;
  FakePipeSource source;
  loop.reset();
  loop.setEventSource(&source);
  source.failDrains();

  source.signal();

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  // Not a readable count: a descriptor the caller cannot clear is not a wake.
  EXPECT_EQ(0, loop.wait(0));

  clock_gettime(CLOCK_MONOTONIC, &end);
  long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000 +
                    (end.tv_nsec - start.tv_nsec) / 1000;

  EXPECT_EQ(1, source.drain_calls);   // it did try
  EXPECT_GE(elapsed_us, 100);         // and then backed off, as above
  EXPECT_GT(source.pendingBytes(), 0);  // descriptor still readable, as in the real failure
}

TEST(LinuxEventLoopWait, FiltersPollResultsNotRaw) {
  // Discriminator: proves we count only POLLIN, not raw poll() return value.
  // An implementation returning unfiltered poll() count would return 2 here
  // (POLLIN on live fd + POLLNVAL on stale fd), failing this assertion.
  LinuxEventLoop loop;
  FakePipeFd live;

  loop.reset();

  // Register the live fd first, then create and close a stale one.
  // This ensures we can control which fd gets what number and avoid reuse.
  int live_num = live.readFd();
  loop.registerFd(live_num);

  // Create and close a stale fd. Since we're holding the live fd,
  // the stale fd number will differ and won't be immediately reused.
  int stale = open("/dev/null", O_RDONLY);
  ASSERT_GE(stale, 0);
  loop.registerFd(stale);
  close(stale);  // Now stale is closed (POLLNVAL), live is still open

  live.signal();  // Make only the live fd readable

  // poll() would return 2 (POLLIN on live + POLLNVAL on stale).
  // wait() should return 1 (only the POLLIN count, filtering out POLLNVAL).
  EXPECT_EQ(1, loop.wait(0));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
