# Linux GPS enable-pin support — design

**Date:** 2026-07-11
**Status:** Approved, ready for implementation plan
**Scope:** `variants/linux` (`env:linux_repeater`)
**Builds on:** [2026-07-11-linux-gps-support-design.md](2026-07-11-linux-gps-support-design.md)

## Goal

Let the Linux build drive a GPS module's enable/standby GPIO line so modules
that boot into standby (e.g. the Quectel L76K on the Waveshare SX1262
LoRaWAN/GNSS HAT) actually stream NMEA. Configured via a new `gps_en_pin`
key in `meshcored.ini`.

## Background / root cause

The initial Linux GPS support opened the serial device and read NMEA, but on
the Waveshare HAT the L76K stayed silent: 0 bytes at every baud, no fix. The
L76K's **STANDBY line (HAT GPIO4)** must be driven HIGH to wake it — verified
on hardware (`pimesh`): holding GPIO4 high produced an immediate `fix, 19
sats` through meshcored's existing GPS CLI, and meshtasticd (which holds that
pin) reads the same module fine. Our provider was constructed with
`_pin_en = -1`, so nothing drove the line.

The shared `MicroNMEALocationProvider` already has enable-pin logic
(`_pin_en`, driven HIGH in `begin()`, LOW in `stop()`), but on Linux two
things block reusing it directly:
1. ardulinux `digitalWrite`/`pinMode` only reach a real GPIO line if that pin
   was first bound via `gpioBind(new LinuxGPIOPin(...))` (as `LinuxBoard.cpp`
   does for the LoRa pins); an unbound pin dispatches to a no-op `SimGPIOPin`.
2. The provider is a global constructed at static-init time, before
   `config.load()` runs, so the pin number isn't known at its construction.

## Decisions

- **Config:** one key `gps_en_pin` (BCM/line number, default `-1` = unset =
  current behavior). Active-high only (matches the L76K and the shared
  `PIN_GPS_EN_ACTIVE=HIGH` default). No active-low knob, no reset/force pin —
  YAGNI for the L76K.
- **Drive strategy (option 2 — hold high in the variant):** `LinuxBoard`
  binds `gps_en_pin` via the existing `initGPIOPin` helper and drives it HIGH
  for the daemon's lifetime. The provider stays constructed with `_pin_en =
  -1`. **Zero shared-upstream file changes.**
  - Consequence: the enable line is held HIGH whenever `meshcored` runs; it
    does not follow `gps on`/`gps off`. `gps off` still stops meshcored
    *reading/reporting* GPS (via `gps_active`), which is the user-facing
    behavior. The module stays powered (~mA) rather than sleeping on `gps
    off` — negligible for a Pi-powered repeater, and avoids a second
    shared-file edit or a static-init refactor.
- **Non-fatal:** a failed enable-pin bind logs and continues (unlike the LoRa
  pins, which are fatal). A missing GPS wake must not take down the repeater.

## Components (all in `variants/linux/`)

### 1. `LinuxConfig` (`LinuxBoard.h`)
Add `int gps_en_pin = -1;` alongside `gps_device`/`gps_baud`.

### 2. `LinuxConfig::load()` (`LinuxBoard.cpp`)
Parse `gps_en_pin` via `atoi` (like the LoRa pin keys).

### 3. `LinuxBoard::begin()` (`LinuxBoard.cpp`)
After the LoRa-pin binding block, if `config.gps_en_pin != -1`:
- `initGPIOPin(config.gps_en_pin, config.lora_gpiochip, config.gps_en_pin)`
  — bind the line on the same gpiochip. **Do not** add its result to the
  fatal `failures` tally; on non-zero, log a warning and continue.
- On successful bind: `pinMode(config.gps_en_pin, OUTPUT);
  digitalWrite(config.gps_en_pin, HIGH);` to wake and hold the module.

This runs after `config.load()`, so the pin number is known, and ardulinux's
`digitalWrite` reaches the bound `LinuxGPIOPin` → libgpiod.

### 4. Docs (`meshcored.ini`, `README.md`)
- `meshcored.ini`: commented sample `# gps_en_pin = 4   # GPIO to wake the
  GPS (e.g. L76K STANDBY on Waveshare LoRaWAN/GNSS HAT)`.
- `README.md`: config-table row + a note that some GPS modules (L76K on the
  Waveshare HAT) need an enable/standby pin held high, set via `gps_en_pin`.

## Data & control flow

`board.begin()` → `config.load()` (reads `gps_en_pin`) → LoRa pins bound →
**if `gps_en_pin != -1`: bind it, drive HIGH** → module wakes and streams.
Later `sensors.begin()` → `initBasicGPS()` opens `/dev/serial0`, sees NMEA,
`gps_detected = true`. `gps on` → `gps_active = true` → `loop()` parses the
now-flowing NMEA → `node_lat/lon` populate → CLI reports `fix, N sats`.

## Error handling & edge cases

- `gps_en_pin = -1` (default): nothing bound/driven; identical to current
  behavior. Existing installs unaffected.
- Bind failure (bad line, already claimed): logged, **non-fatal**; daemon
  continues as a repeater without GPS wake.
- Pin held for process lifetime; libgpiod releases it on exit (line returns to
  default, module sleeps).

## Testing

- **Build:** `./build-docker.sh linux_repeater` exits 0 (macOS host; the
  arm64 Debian container is authoritative).
- **On-hardware (definitive):** deploy to `pimesh`, set `gps_en_pin = 4` and
  `gps_device = /dev/ttyS0` in `meshcored.ini`, restart `meshcored`, run
  `meshcorectl gps on`, and confirm `gps` reports `fix, N sats` **without any
  manual GPIO holding**. We have already proven the module fixes when GPIO4 is
  held high; this proves the firmware now holds it.

## Out of scope

- Active-low enable pins; separate reset/force-on pin; the L76K FORCE_ON
  (GPIO17) pulse (not needed — STANDBY-high alone wakes it).
- Sleeping the module on `gps off` (would need a shared-file setter or
  static-init refactor; deferred).
- Non-Linux variants (unchanged).
