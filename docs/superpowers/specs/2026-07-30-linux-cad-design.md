# Non-blocking hardware CAD for the Linux variant

Date: 2026-07-30
Branch: `linux`

## Problem

`set cad on` enables hardware Channel Activity Detection before TX. It is off by
default and reaches the radio through `Dispatcher::loop()`, which re-pushes
`setCADEnabled(getCADEnabled())` every 2 s. When enabled,
`RadioLibWrapper::isChannelActive()` calls `performChannelScan()`, which today is
`_radio->scanChannel()` — RadioLib's blocking helper:

```cpp
int16_t SX126x::scanChannel(const ChannelScanConfig_t &cfg) {
  int state = startChannelScan(cfg);
  RADIOLIB_ASSERT(state);
  while(!this->mod->hal->digitalRead(this->mod->getIrq())) {
    this->mod->hal->yield();
  }
  return(getChannelScanResult());
}
```

All API references below were verified against **RadioLib 7.4.0**, which is what
`linux_base` pins. The other environments track a git revision via
`arduino_base`, so the two are not necessarily interchangeable.

Three problems on Linux, in ascending order of severity:

1. **It busy-spins.** A CAD is a few symbol times — tens of milliseconds at
   typical settings — burned at 100% CPU. That directly undercuts the
   `idleUntilEvent()` design this branch exists to establish.
2. **It leaves an edge queued.** The CAD-done IRQ raises an edge while the spin
   holds the loop, so the next `idleUntilEvent()` returns immediately on a stale
   event. Harmless (early return is always correct) but wasteful.
3. **The spin has no timeout.** `EventGPIOPin::readPinHardware()` deliberately
   returns `LOW` when a GPIO read fails, so a broken line degrades to "no packet"
   plus one logged error. Inside an untimed `while(!digitalRead(...))` that same
   failure becomes an unbreakable hang of the daemon: a logged degradation turns
   into a lockup.

## Approach

Keep the call synchronous, but **sleep on the DIO1 edge descriptor with a
bounded deadline** instead of spinning.

`isChannelActive()` is called synchronously from `Dispatcher::checkSend()` and
must return a boolean immediately. CAD is also destructive to reception —
`startChannelScan()` calls `standby()` first — so the radio is not receiving
during the scan and cannot be. There is nothing for the main loop to overlap
with while a CAD is in flight, which is what makes blocking the right shape
here.

Two alternatives were considered and rejected:

- **Fully async** (start the scan, harvest the result on a later loop
  iteration, cache the verdict). The loop never blocks, but "scan pending" has
  to report *busy*, which makes `checkSend()` set
  `next_tx_time = futureMillis(getCADFailRetryDelay())` — **+200 ms on every
  transmit** — and the verdict eventually acted on is 200 ms stale. That inverts
  the purpose of CAD, which exists to answer "is the channel clear *right
  now*". It also entangles with `cad_busy_start` / `ERR_EVENT_CAD_TIMEOUT`.
- **Async plus a `Dispatcher` change** so a pending scan retries fast. Removes
  the latency penalty, but puts Linux-specific structure into
  `src/Dispatcher.{h,cpp}` and the `mesh::Radio` interface — permanent merge
  friction against `meshcore-dev/dev`.

The chosen approach fixes what is actually wrong (the spin and the unbounded
wait) without paying for concurrency the daemon has no use for. Blocking in
`poll()` for tens of milliseconds is not what the recent commits were fighting;
busy-spinning was.

### Prior art in this repo

`performChannelScan()` exists precisely as an override seam: commit `4f9a0916`
("Use hardware channel activity detection for checking interference")
introduced it as a `virtual` in the same change that added CAD. Overriding it
is using the hook as designed.

`CustomSX1276::tryScanChannel()` is the only other CAD wait in the tree. It
bounds the wait — `unsigned long timeout = millis() + 16` — which is the right
instinct, but it is still a spin, the 16 ms is hardcoded (far too short above
roughly SF9), and it has **no callers**. It is dead code. Cite it as precedent
for bounding the wait, not for how to wait.

Note also that `companion_radio` overrides `getCADEnabled()` to return `true`:
CAD is on by default there, unlike the repeater, room-server and sensor
examples, which default it off behind `set cad on`. No Linux environment builds
companion_radio today, but if one is added the scan runs on every transmit out
of the box, which is a further reason the bounded path matters.

### Cost of blocking, itemised

While the CAD runs, the main loop sleeps in `poll()`. During that window:

- No packets can be received anyway — the modem is in CAD, not RX. Zero RX loss
  beyond what CAD inherently costs.
- Console/CLI latency grows by the CAD duration. The loop already idles up to
  `IDLE_MAX_WAIT_MS` (50 ms).
- The noise-floor sampler misses at most one 100 ms slot and resyncs itself
  (`RadioLibWrapper::loop()` resyncs rather than catching up).
- Signal handling is delayed by at most the CAD duration.

## When the scan actually runs

CAD is on the transmit path only, behind five gates that each skip it:

1. `checkSend()` returns immediately when `getOutboundCount() == 0` — an idle
   node never scans.
2. It returns again when the TX budget is below `est_airtime / 2`.
3. And again when `next_tx_time` has not passed.
4. `isReceiving()` short-circuits on `isReceivingPacket()`, so a packet already
   being received skips the scan.
5. `isChannelActive()` runs the RSSI-vs-noise-floor check first and returns
   early if it says busy — so enabling `int.thresh` *reduces* how often CAD
   runs.

At most one scan per transmit attempt, with attempts repeating every
`getCADFailRetryDelay()` (200 ms) for up to `getCADFailMaxDuration()` (4 s).

`Dispatcher::checkSend()` is the only core caller of `isReceiving()`.
`kiss_modem` also calls it — from its `TX_WAIT_CLEAR` state machine and from the
`HW_CMD_IS_CHANNEL_BUSY` host command — which would scan far more aggressively,
but no Linux environment builds kiss_modem.

### Scan duty at long symbol times

The CAD duration approaches the 200 ms retry delay as symbol time grows. At
SF12/62.5 kHz a 4-symbol CAD is roughly 265 ms, so a node holding a queued
packet on a contended channel cycles scan(265 ms) → busy → wait 200 ms → scan,
spending about 57% of that 4 s window inside a CAD — during which the modem is
in CAD/standby and **not receiving**. At SF8/62.5 kHz (the sample `meshcored.ini`)
the scan is about 20 ms against the same 200 ms delay, or roughly 9%.

This is inherent to CAD-before-TX and is not introduced here: the upstream
blocking implementation produces identical radio behaviour, and differs only in
spinning rather than sleeping through it. It is a genuine cost of `set cad on`
at high spreading factors and belongs in the operator documentation.

## Components

### `variants/linux/LinuxRadioWait.{h,cpp}` (new)

Depends on `LinuxEventLoop` and `LinuxEventSource` only — no RadioLib, no
libgpiod, no Arduino — so it compiles into the `native` gtest environment.

```cpp
// Predicate for "has the line asserted?", abstract for the same reason
// LinuxEventSource is: keeps the wait loop free of any GPIO dependency.
class LinuxIrqLevel {
public:
  virtual ~LinuxIrqLevel() {}
  virtual bool irqAsserted() = 0;
};

// Sleeps until level.irqAsserted() or timeout_ms elapses, whichever comes
// first. Never spins. Returns true if the line asserted before the deadline.
bool waitForIrqAsserted(LinuxIrqLevel& level, LinuxEventSource* src,
                        LinuxEventLoop& loop, uint32_t timeout_ms);

// Deadline for a 4-symbol CAD, given the current symbol time.
uint32_t cadTimeoutMillis(uint32_t symbol_micros);
```

`waitForIrqAsserted()`:

1. Calls `loop.reset()` and `loop.setEventSource(src)` once up front. It
   registers **no** other descriptors: nothing here would drain a console fd,
   and a registered-but-undrained descriptor sits `POLLIN` forever, which is
   precisely the busy loop `LinuxEventLoop` exists to remove.
2. Computes a deadline from `CLOCK_MONOTONIC`. Not Arduino `millis()`: a
   monotonic clock is the natural one for a `poll()` deadline, and the native
   test environment's `Arduino.h` mock freezes `millis()` at `g_mock_millis`,
   which would make the loop non-terminating under test.
3. Loops: sample the level; if asserted, return `true`; if the deadline has
   passed, return `false`; otherwise block in `loop.wait(remaining)`.
4. When `src` has no usable descriptor (`eventFd() < 0`), waits in 1 ms slices
   instead of one long sleep — the same fallback, and the same value, as
   `LinuxBoard::idleUntilEvent()`.

`EINTR` needs no special handling: `LinuxEventLoop::wait()` returns `-1` and the
loop simply re-samples and recomputes the remaining time. Stale/hung-up
descriptors are already absorbed by `wait()`'s `EVENT_LOOP_ERROR_BACKOFF_US`
cool-off, so no path through this loop can spin.

`cadTimeoutMillis(symbol_micros)` returns `(symbol_micros * 8) / 1000 + 20`.
RadioLib scans 4 symbols (`RADIOLIB_SX126X_CAD_ON_4_SYMB`, the value
`SX126x::setCad()` uses when passed `RADIOLIB_SX126X_CAD_PARAM_DEFAULT`); 8
symbol times is twice the scan, and the fixed 20 ms covers SPI turnaround and
Linux scheduler jitter. Worked values: 52 ms at SF8/62.5 kHz, 85 ms at
SF11/250 kHz, 544 ms at SF12/62.5 kHz. This is a bound on *failure*, not a
budget for the normal case — a healthy CAD returns the instant DIO1 rises.

### `variants/linux/LinuxBoard.{h,cpp}`

Add `bool waitForRadioIrq(uint32_t timeout_ms)` — the sibling of
`idleUntilEvent()`. It owns the pin number and the event source, and supplies a
`LinuxIrqLevel` backed by `digitalRead(config.lora_irq_pin)`.

That read is load-bearing in two ways. In ArduLinux, `digitalRead()` is a full
ISR poll — `GPIOPin::readPin()` calls `refreshState()`, which reads the hardware,
updates the cached level, and fires the attached ISR on the configured edge — so
it both samples the line and keeps the cache coherent. This is the same
obligation `idleUntilEvent()` documents.

Returns `false` immediately when `config.lora_irq_pin == RADIOLIB_NC`; the
caller then falls through to the SPI read, which is the correct answer anyway.

### `src/helpers/radiolib/LinuxSX1262Wrapper.h`

Override `performChannelScan()`:

```
startChannelScan()  →  board.waitForRadioIrq(_cad_timeout_ms)  →  getChannelScanResult()
```

- `SX126x::startChannelScan()` (no-arg) builds exactly the same
  `ChannelScanConfig_t` that `scanChannel()` did, so scan parameters are
  unchanged: 4 symbols, exit to `STDBY_RC`, DIO1 mapped to `CAD_DONE |
  CAD_DETECTED`. It also calls `clearIrqStatus()` before starting, so DIO1 is
  `LOW` on entry to the wait.
- **The whole design rests on `CAD_DONE` being in the DIO1 routing mask.** Both
  `irqFlags` and `irqMask` are `RADIOLIB_IRQ_CAD_DEFAULT_*`, i.e. `CAD_DONE |
  CAD_DETECTED`, and the modem raises `CAD_DONE` whichever way the scan
  resolves. So the line rises on *both* outcomes: a "channel free" verdict
  arrives as an edge like any other, and is never delivered as a silent
  timeout. If a future RadioLib version narrows that mask to `CAD_DETECTED`
  alone, every free channel would cost a full `_cad_timeout_ms` wait — worth
  re-checking on a RadioLib bump.
- On a non-`RADIOLIB_ERR_NONE` start, log and return the error.
  `isChannelActive()` treats anything that is not `RADIOLIB_CHANNEL_FREE` as
  busy.
- **On timeout, still call `getChannelScanResult()`.** It reads the IRQ status
  register over SPI, which is authoritative and wholly independent of the GPIO.
  A line that has stopped reporting therefore costs latency and a log line, not
  correctness — and never RadioLib's unbounded hang. Log each occurrence rather
  than latching the first: the volume is bounded by TX attempts (at most a
  handful per packet, and only while broken), and a persistent fault should stay
  visible.

Logging uses `MESH_DEBUG_PRINTLN`, matching `LinuxSX1262.h` and
`RadioLibWrappers.cpp`. `linux_repeater` builds with `-D MESH_DEBUG=1`.

`isChannelActive()` in `RadioLibWrappers.cpp` needs no change. Its existing
`state = STATE_IDLE; startRecv();` already clears the `STATE_INT_READY` that the
CAD-done IRQ sets via `setFlag()`, and `startRecv()` restores the RX IRQ mapping
and clears the modem's flags.

Add `_cad_timeout_ms`, computed in `setParams()` next to the existing
`calcMaxPacketMillis()` call, with a conservative default (550 ms, covering
SF12/62.5 kHz) for the window before the first `setParams()`. CAD cannot run in
that window anyway — `_cad_enabled` is false until `Dispatcher::loop()` pushes
it.

### `src/helpers/radiolib/RadioLibWrappers.{h,cpp}`

The only touch to shared code: lift the symbol-time expression already inside
`calcMaxPacketMillis()` into

```cpp
// Symbol time in microseconds. bw is in kHz.
static uint32_t symbolMicros(uint8_t sf, float bw) {
  return ((uint32_t)10000 << sf) / (bw * 10);
}
```

— the expression verbatim, so the float division and its truncation are
unchanged — and call it from both `calcMaxPacketMillis()` and
`LinuxSX1262Wrapper::setParams()`, so the formula has one home.

## Data flow

```
Dispatcher::checkSend()
  └─ Radio::isReceiving()
       ├─ isReceivingPacket()          — preamble/header watchdog, unchanged
       └─ isChannelActive()
            ├─ RSSI vs noise floor + margin   — unchanged
            └─ if (_cad_enabled) performChannelScan()      ← LinuxSX1262Wrapper
                 ├─ SX126x::startChannelScan()             — standby, map DIO1, set CAD
                 ├─ LinuxBoard::waitForRadioIrq(_cad_timeout_ms)
                 │    └─ waitForIrqAsserted() → LinuxEventLoop::wait() → poll()
                 └─ SX126x::getChannelScanResult()         — SPI read of IRQ flags
            └─ state = STATE_IDLE; startRecv()             — unchanged
```

## Error handling

| Condition | Behaviour |
|---|---|
| `startChannelScan()` returns an error | Log; return the error; `isChannelActive()` reads it as busy. |
| DIO1 never rises before the deadline | Log; read the result over SPI anyway; act on whatever it says. |
| `getChannelScanResult()` returns `RADIOLIB_ERR_UNKNOWN` | Treated as busy. `getCADFailMaxDuration()` (4 s) forces the transmit and raises `ERR_EVENT_CAD_TIMEOUT`, the existing escape hatch. |
| `lora_irq_pin == RADIOLIB_NC` | `waitForRadioIrq()` returns `false` at once; the SPI read still produces the right answer. |
| No edge detection on the line | 1 ms polling slices, bounded by the same deadline. |
| `poll()` interrupted (`EINTR`) | `wait()` returns `-1`; the loop re-samples and continues to the deadline. |

## Testing

`test/test_linux_radio_wait/` in the `native` gtest environment, following the
fake-pipe-source pattern of `test_linux_event_loop`. Add
`+<../variants/linux/LinuxRadioWait.cpp>` to that environment's
`build_src_filter`.

- Returns `true` without polling when the level is already asserted.
- Returns `true` when the source becomes readable and the level then asserts.
- Returns `false` only after the full timeout has genuinely elapsed (assert
  measured elapsed time ≥ timeout, so an early return cannot pass).
- Honours the deadline when `eventFd()` is `-1` (no edge detection), and still
  returns `true` if the level asserts during that fallback.
- `timeout_ms == 0` returns immediately.
- A null `LinuxEventSource*` is safe.
- `cadTimeoutMillis()` over several symbol times, including the SF12/62.5 kHz
  extreme.

Compile check: `./build-docker.sh linux_repeater`.

On-air validation requires the Raspberry Pi and is out of scope for this
change: enable with `set cad on`, confirm transmits still happen, confirm CPU
stays at idle during CAD, and confirm `ERR_EVENT_CAD_TIMEOUT` is not being
raised in normal operation.

## Documentation

`variants/linux/README.md`: a short section on `set cad on` — what CAD adds over
`int.thresh` (correlates against the LoRa preamble, so it sees a transmission
below the noise floor where RSSI is blind; it is blind in turn to non-LoRa
energy, which is what `int.thresh` covers), that it is opt-in and off by
default, and that on Linux the scan sleeps on the IRQ descriptor with a
bounded deadline rather than spinning.

Also document the scan-duty cost from "Scan duty at long symbol times" above,
so an operator running a high spreading factor knows that `set cad on` trades
receive time for collision avoidance, and that enabling `int.thresh` alongside
it reduces how often the scan runs.

## Out of scope

- Making the CAD symbol count configurable. RadioLib's 4-symbol default stays.
- Any change to `Dispatcher`, `mesh::Radio`, or the `cad` CLI/pref plumbing,
  all of which arrived with the upstream merge and work as-is.
- The `int.thresh` / noise-floor path, which is independent and already sound.
