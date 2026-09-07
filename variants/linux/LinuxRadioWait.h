#pragma once

#include <stdint.h>

class LinuxEventLoop;
class LinuxEventSource;

// Reads the current level of the line a wait is watching.
//
// Abstract for the same reason LinuxEventSource is: it keeps waitForIrqAsserted()
// free of any GPIO dependency, so this unit compiles and is unit-tested on a host
// with no libgpiod. LinuxBoard supplies the production implementation, backed by
// digitalRead() on the configured LoRa IRQ pin.
class LinuxIrqLevel {
public:
  virtual ~LinuxIrqLevel() {}

  // True once the radio has raised its interrupt line.
  virtual bool irqAsserted() = 0;
};

// Sleep until level.irqAsserted() reports true or timeout_ms elapses, whichever
// comes first. Returns true if the line asserted before the deadline.
//
// Never spins, on any path. Each iteration blocks in poll() on src's edge
// descriptor for the whole remaining time; where src has no usable descriptor
// (including src == NULL) it waits in 1 ms slices instead, the same fallback
// (and the same value) as LinuxBoard::idleUntilEvent(). The one way an
// iteration can return with no time spent is a descriptor that stays readable
// -- stale, hung up, or failing to drain -- and LinuxEventLoop::wait() applies
// its own cool-off to every one of those before returning, which is what makes
// the guarantee hold rather than merely being the intent.
//
// This replaces the pattern RadioLib's blocking helpers use --
//   while(!hal->digitalRead(mod->getIrq())) { hal->yield(); }
// -- which burns a core for the duration and, having no deadline, converts a
// GPIO read that has started failing into an unbreakable hang of the caller.
//
// `loop` is reset and re-pointed at `src` on entry. No other descriptor is
// registered: nothing here would drain one, and a registered-but-undrained
// descriptor stays POLLIN forever, which is precisely the busy loop
// LinuxEventLoop exists to remove.
bool waitForIrqAsserted(LinuxIrqLevel& level, LinuxEventSource* src,
                        LinuxEventLoop& loop, uint32_t timeout_ms);

// How long to wait for a channel-activity-detection scan to raise DIO1, given
// the current symbol time in microseconds.
//
// RadioLib scans 4 symbols (RADIOLIB_SX126X_CAD_ON_4_SYMB, which is what
// SX126x::setCad() uses when handed RADIOLIB_SX126X_CAD_PARAM_DEFAULT). Allowing
// 8 symbol times gives twice the scan, and the fixed 20 ms covers SPI turnaround
// and Linux scheduler jitter.
//
// This is a bound on *failure*, not a budget for the normal case: a healthy scan
// returns the instant the line rises, typically in half this. Worked values:
// 52 ms at SF8/62.5 kHz, 85 ms at SF11/250 kHz, 544 ms at SF12/62.5 kHz.
uint32_t cadTimeoutMillis(uint32_t symbol_micros);
