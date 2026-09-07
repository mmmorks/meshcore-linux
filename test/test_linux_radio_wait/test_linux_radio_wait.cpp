#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "LinuxEventLoop.h"
#include "LinuxRadioWait.h"

namespace {

// Upper bound used wherever a test asserts "this did not block". Those waits
// are armed with a 5 s ceiling (or none at all) and should complete in
// microseconds to a few milliseconds; the number that matters is the gap to
// 5000, not tightness. Deliberately loose: the assertion is meant to catch a
// wait that slept out its deadline, not to measure a scheduler under load, and
// a millisecond-scale bound on a busy CI host tests the host rather than the
// code. Where the exact behaviour matters -- how many times the line was
// sampled, whether the source was drained -- the tests assert that directly.
const uint64_t DID_NOT_BLOCK_MS = 2000u;

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

// Delivers SIGALRM on a repeating interval for as long as it is in scope, so a
// wait running underneath it really does take EINTR out of poll().
//
// The handler is installed WITHOUT SA_RESTART, which is the whole point: with
// it the kernel would restart poll() transparently and the test would prove
// nothing. A daemon gets signals it did not ask for -- SIGWINCH, a profiler's
// timer, whatever the supervisor sends -- and the CAD wait must survive them
// without returning early, spinning, or losing the line.
class SignalStorm {
public:
  explicit SignalStorm(useconds_t interval_us) {
    count = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = &SignalStorm::onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // no SA_RESTART: poll() must fail with EINTR
    sigaction(SIGALRM, &sa, &_old_action);

    struct itimerval it;
    it.it_interval.tv_sec  = 0;
    it.it_interval.tv_usec = interval_us;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, &_old_timer);
  }

  ~SignalStorm() {
    struct itimerval off;
    memset(&off, 0, sizeof off);
    setitimer(ITIMER_REAL, &off, NULL);
    sigaction(SIGALRM, &_old_action, NULL);
  }

  static volatile sig_atomic_t count;

private:
  static void onSignal(int) { count++; }

  struct sigaction  _old_action;
  struct itimerval  _old_timer;
};

volatile sig_atomic_t SignalStorm::count = 0;

}  // namespace

TEST(WaitForIrqAsserted, ReturnsImmediatelyWhenLineIsAlreadyHigh) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level(0);  // high on the very first sample

  uint64_t start = nowMillis();
  EXPECT_TRUE(waitForIrqAsserted(level, &source, loop, 5000));
  EXPECT_LT(nowMillis() - start, DID_NOT_BLOCK_MS);
  // The real assertion: one sample and no poll at all, which is what "a scan
  // that already finished costs nothing" actually means.
  EXPECT_EQ(1, level.calls);
}

TEST(WaitForIrqAsserted, WakesOnTheEventSourceAndRereadsTheLine) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level(1);  // low once, high on the second sample

  source.signal();  // edge already queued, as it is when CAD finishes fast

  uint64_t start = nowMillis();
  EXPECT_TRUE(waitForIrqAsserted(level, &source, loop, 5000));
  EXPECT_LT(nowMillis() - start, DID_NOT_BLOCK_MS);
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
  // time would have done. Four samples is the tight assertion; the clock only
  // has to separate 3 ms of slices from 5 s of sleeping.
  EXPECT_LT(nowMillis() - start, DID_NOT_BLOCK_MS);
  EXPECT_EQ(4, level.calls);
}

// A signal that interrupts the poll() must not be mistaken for a timeout.
// LinuxEventLoop::wait() returns -1 on EINTR and the caller loops; what has to
// hold end to end is that the deadline still governs, so a node being signalled
// does not start reporting every channel free the moment a scan is armed.
TEST(WaitForIrqAsserted, SignalsDoNotEndTheWaitEarly) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level;  // never asserts

  const uint32_t timeout_ms = 60;
  bool     asserted;
  uint64_t elapsed;
  {
    SignalStorm storm(3000);  // SIGALRM every 3 ms
    uint64_t start = nowMillis();
    asserted = waitForIrqAsserted(level, &source, loop, timeout_ms);
    elapsed  = nowMillis() - start;
  }

  EXPECT_FALSE(asserted);
  EXPECT_GE(elapsed + 2, timeout_ms);
  // Without this the test would still pass on a run where no signal happened to
  // land inside the wait, and would be testing nothing.
  EXPECT_GE((int)SignalStorm::count, 2) << "the wait was never actually interrupted";
}

// The other half: an interrupted wait must re-read the line rather than resume
// blocking on a stale sample. The IRQ may well have risen while the handler
// ran, and for CAD that edge is the entire answer.
TEST(WaitForIrqAsserted, SeesTheLineAfterAnInterruptedWait) {
  LinuxEventLoop loop;
  FakePipeSource source;
  FakeIrqLevel level(3);  // asserts on the fourth sample

  bool     asserted;
  uint64_t elapsed;
  {
    SignalStorm storm(2000);  // SIGALRM every 2 ms
    uint64_t start = nowMillis();
    // Generous ceiling on purpose: nothing will ever make the pipe readable, so
    // only the re-reads driven by EINTR can end this wait.
    asserted = waitForIrqAsserted(level, &source, loop, 5000);
    elapsed  = nowMillis() - start;
  }

  EXPECT_TRUE(asserted);
  EXPECT_EQ(4, level.calls);
  EXPECT_LT(elapsed, DID_NOT_BLOCK_MS);
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
  EXPECT_LT(nowMillis() - start, DID_NOT_BLOCK_MS);
  // Sampled once, and no poll: with no budget the deadline check must return
  // before the wait, which is what the call count proves.
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

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
