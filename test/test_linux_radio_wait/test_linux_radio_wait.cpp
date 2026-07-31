#include <gtest/gtest.h>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "LinuxEventLoop.h"
#include "LinuxRadioWait.h"

namespace {

uint64_t nowMillis() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

// Event source backed by a pipe, so a test can make it readable on demand.
// Mirrors the fake in test_linux_event_loop.
class FakePipeSource : public LinuxEventSource {
public:
  FakePipeSource() {
    if (pipe(_fds) != 0) { _fds[0] = -1; _fds[1] = -1; return; }
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

  void signal() {
    unsigned char b = 1;
    ssize_t n = ::write(_fds[1], &b, 1);
    (void)n;
  }

  // Simulate a line with no working edge detection: eventFd() returns -1.
  void disable() { _enabled = false; }

  int pendingBytes() {
    unsigned char buf[64];
    ssize_t n = ::read(_fds[0], buf, sizeof buf);
    return n > 0 ? (int)n : 0;
  }

  int drain_calls = 0;

private:
  int  _fds[2];
  bool _enabled = true;
};

// Reports LOW for the first `assert_after` samples, then HIGH.
class FakeIrqLevel : public LinuxIrqLevel {
public:
  explicit FakeIrqLevel(int assert_after = kNever) : assert_after(assert_after) { }

  bool irqAsserted() override { return ++calls > assert_after; }

  static const int kNever = 1000000;

  int assert_after;
  int calls = 0;
};

const int FakeIrqLevel::kNever;

}  // namespace

TEST(WaitForIrqAsserted, ReturnsImmediatelyWhenLineIsAlreadyHigh) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level(0);  // high on the very first sample

  uint64_t start = nowMillis();
  EXPECT_TRUE(waitForIrqAsserted(level, &source, loop, 5000));
  // Must not have polled at all: the whole point is that a scan already
  // finished costs nothing.
  EXPECT_LT(nowMillis() - start, 100u);
  EXPECT_EQ(1, level.calls);
}

TEST(WaitForIrqAsserted, WakesOnTheEventSourceAndRereadsTheLine) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level(1);  // low once, high on the second sample

  source.signal();  // edge already queued, as it is when CAD finishes fast

  uint64_t start = nowMillis();
  EXPECT_TRUE(waitForIrqAsserted(level, &source, loop, 5000));
  EXPECT_LT(nowMillis() - start, 100u);
  EXPECT_EQ(2, level.calls);
  // The source must have been drained, otherwise poll() would return
  // immediately forever on the next caller's watch.
  EXPECT_GE(source.drain_calls, 1);
  EXPECT_EQ(0, source.pendingBytes());
}

TEST(WaitForIrqAsserted, ReturnsFalseOnlyAfterTheFullTimeout) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level;  // never asserts

  const uint32_t timeout_ms = 40;
  uint64_t start = nowMillis();
  EXPECT_FALSE(waitForIrqAsserted(level, &source, loop, timeout_ms));
  uint64_t elapsed = nowMillis() - start;

  // Returning early would mean the deadline is not doing its job; returning
  // very late would mean it is not bounded.
  EXPECT_GE(elapsed + 2, timeout_ms);
  EXPECT_LT(elapsed, timeout_ms + 500u);
}

TEST(WaitForIrqAsserted, DoesNotSpinWhenTheSourceIsReadableButTheLineStaysLow) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level;  // never asserts

  source.signal();  // a spurious/stale edge

  const uint32_t timeout_ms = 40;
  uint64_t start = nowMillis();
  EXPECT_FALSE(waitForIrqAsserted(level, &source, loop, timeout_ms));
  EXPECT_GE(nowMillis() - start + 2, timeout_ms);

  // Draining is what stops the readable descriptor from turning the wait into
  // a busy loop. Without it this test would still pass on time but would have
  // spun through thousands of iterations to get there.
  EXPECT_GE(source.drain_calls, 1);
  EXPECT_EQ(0, source.pendingBytes());
}

TEST(WaitForIrqAsserted, HonoursTheDeadlineWithoutEdgeDetection) {
  LinuxEventLoop loop;
  FakePipeSource source;
  source.disable();  // eventFd() == -1, as when the line has no edge support
  FakeIrqLevel level;

  const uint32_t timeout_ms = 30;
  uint64_t start = nowMillis();
  EXPECT_FALSE(waitForIrqAsserted(level, &source, loop, timeout_ms));
  uint64_t elapsed = nowMillis() - start;

  EXPECT_GE(elapsed + 2, timeout_ms);
  EXPECT_LT(elapsed, timeout_ms + 500u);
}

TEST(WaitForIrqAsserted, StillSeesTheLineWithoutEdgeDetection) {
  LinuxEventLoop loop;
  FakePipeSource source;
  source.disable();
  FakeIrqLevel level(3);  // asserts on the fourth sample, i.e. after 3 slices

  uint64_t start = nowMillis();
  EXPECT_TRUE(waitForIrqAsserted(level, &source, loop, 5000));
  // Polling fallback, so it costs a few milliseconds -- but nothing like the
  // 5 s ceiling, which is what a fallback that slept for the whole remaining
  // time would have done.
  EXPECT_LT(nowMillis() - start, 500u);
  EXPECT_EQ(4, level.calls);
}

TEST(WaitForIrqAsserted, NullSourceIsSafe) {
  LinuxEventLoop loop;
  FakeIrqLevel level(2);

  EXPECT_TRUE(waitForIrqAsserted(level, nullptr, loop, 5000));
  EXPECT_EQ(3, level.calls);
}

TEST(WaitForIrqAsserted, ZeroTimeoutStillReportsAnAssertedLine) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level(0);

  EXPECT_TRUE(waitForIrqAsserted(level, &source, loop, 0));
  EXPECT_EQ(1, level.calls);
}

TEST(WaitForIrqAsserted, ZeroTimeoutReturnsWithoutWaiting) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level;

  uint64_t start = nowMillis();
  EXPECT_FALSE(waitForIrqAsserted(level, &source, loop, 0));
  EXPECT_LT(nowMillis() - start, 50u);
  // Sampled once: an already-high line must be reported even with no budget.
  EXPECT_EQ(1, level.calls);
}

TEST(WaitForIrqAsserted, ClearsAnyPreviouslyRegisteredDescriptors) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level(0);

  loop.reset();
  int fd = open("/dev/null", O_RDONLY);
  ASSERT_GE(fd, 0);
  loop.registerFd(fd);

  EXPECT_TRUE(waitForIrqAsserted(level, &source, loop, 5000));
  // Console descriptors nobody drains here must not survive into the wait.
  EXPECT_EQ(0, loop.registeredCount());
  close(fd);
}

TEST(CadTimeoutMillis, CoversTheScanAtRealisticSymbolTimes) {
  // 8 symbol times (twice RadioLib's 4-symbol scan) plus 20 ms fixed slack.
  EXPECT_EQ(52u, cadTimeoutMillis(4096));   // SF8  / 62.5 kHz
  EXPECT_EQ(85u, cadTimeoutMillis(8192));   // SF11 / 250 kHz
  EXPECT_EQ(544u, cadTimeoutMillis(65536)); // SF12 / 62.5 kHz
}

TEST(CadTimeoutMillis, KeepsTheFixedSlackAtDegenerateSymbolTimes) {
  EXPECT_EQ(20u, cadTimeoutMillis(0));
  EXPECT_EQ(20u, cadTimeoutMillis(1));
}

TEST(CadTimeoutMillis, GrowsWithSymbolTime) {
  // Each SF step doubles the symbol time, so the bound must not saturate.
  uint32_t prev = cadTimeoutMillis(1024);
  for (uint32_t tsym = 2048; tsym <= 524288; tsym *= 2) {
    uint32_t next = cadTimeoutMillis(tsym);
    EXPECT_GT(next, prev);
    prev = next;
  }
  // SF12 at 7.8 kHz, the slowest setting RadioLib will accept, must not
  // overflow into a nonsense (tiny) bound.
  EXPECT_GT(cadTimeoutMillis(525128), 4000u);
}

TEST(CadTimeoutMillis, StaysUnderTheWrapperDefault) {
  // LinuxSX1262Wrapper seeds _cad_timeout_ms with 550 ms for the window before
  // the first setParams(). That default is only safe while it covers the
  // slowest scan a MeshCore preset produces (SF12 at 62.5 kHz).
  EXPECT_LE(cadTimeoutMillis(65536), 550u);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
