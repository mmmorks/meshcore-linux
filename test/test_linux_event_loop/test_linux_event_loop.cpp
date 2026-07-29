#include <gtest/gtest.h>

#include <fcntl.h>
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

  void drainEvents() override {
    unsigned char buf[64];
    while (::read(_fds[0], buf, sizeof buf) > 0) { }
    drain_calls++;
  }

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
  bool _enabled = true;
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

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
