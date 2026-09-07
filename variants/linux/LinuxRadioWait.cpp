#include "LinuxRadioWait.h"

#include "LinuxEventLoop.h"
#include "LinuxEventSource.h"

#include <limits.h>
#include <time.h>

// Slice length used when the event source has no usable edge descriptor. With
// nothing to block on, the level has to be re-read periodically; 1 ms matches
// LinuxBoard::idleUntilEvent()'s fallback, and the deadline still bounds the
// total wait.
#define IRQ_WAIT_FALLBACK_SLICE_MS 1

// Deliberately CLOCK_MONOTONIC rather than Arduino millis().
//
// A monotonic clock is the natural one for a poll() deadline -- it cannot be
// dragged by settimeofday(), which LinuxRTCClock::setCurrentTime() calls
// whenever the mesh corrects the node's time. It also keeps this unit off the
// Arduino layer, which matters for more than tidiness: the native test build's
// Arduino.h mock freezes millis() at g_mock_millis, so a deadline computed from
// it would never be reached and the wait below would not terminate under test.
static uint64_t monotonicMillis() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

bool waitForIrqAsserted(LinuxIrqLevel& level, LinuxEventSource* src,
                        LinuxEventLoop& loop, uint32_t timeout_ms) {
  loop.reset();
  loop.setEventSource(src);

  const bool have_events = (src != NULL && src->eventFd() >= 0);
  const uint64_t deadline = monotonicMillis() + timeout_ms;

  for (;;) {
    // Sampled before every wait, and once more before returning false, so a
    // line that is already high on entry costs no poll() at all and a level
    // that rises during the final slice is still seen.
    if (level.irqAsserted()) return true;

    uint64_t now = monotonicMillis();
    if (now >= deadline) return false;

    // An edge arriving between the sample above and the poll() below is not
    // lost: it leaves the descriptor readable, so the wait returns at once and
    // the next iteration reads the raised line.
    //
    // A wake that turns out not to be our edge -- EINTR, or a drained event
    // that did not correspond to a level change -- simply loops. That cannot
    // spin: wait() drains the source it reports readable, and applies its own
    // cool-off to every degenerate case that would otherwise return instantly
    // forever (POLLNVAL, POLLHUP, poll() failure, and a source that reports it
    // could not drain).
    //
    // Clamped rather than cast: timeout_ms is a uint32_t and poll() takes an
    // int, so a caller asking for more than INT_MAX ms (~24.8 days) would hand
    // poll() a negative timeout, which means "block forever" -- the unbreakable
    // hang this whole function exists to make impossible. Nothing asks for that
    // today; the clamp is here so that nothing can.
    uint64_t remaining = deadline - now;
    if (remaining > (uint64_t)INT_MAX) remaining = (uint64_t)INT_MAX;
    loop.wait(have_events ? (int)remaining : IRQ_WAIT_FALLBACK_SLICE_MS);
  }
}

uint32_t cadTimeoutMillis(uint32_t symbol_micros) {
  return (symbol_micros * 8) / 1000 + 20;
}
