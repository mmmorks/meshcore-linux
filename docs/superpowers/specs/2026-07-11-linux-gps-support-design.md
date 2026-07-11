# Linux GPS support — design

**Date:** 2026-07-11
**Status:** Approved, ready for implementation plan
**Scope:** `variants/linux` (`env:linux`, `env:linux_repeater`)

## Goal

Add serial-GPS support to the Linux build so a `meshcored` node can read its
location from a USB/UART GPS device (e.g. `/dev/ttyACM0`, `/dev/ttyUSB0`),
advertise it, sync the clock from it, and expose the standard `gps` CLI
commands — reusing the existing shared MeshCore GPS machinery rather than
re-implementing it.

## Background

GPS in MeshCore is gated behind the `ENV_INCLUDE_GPS` compile flag, which is
defined only in `[sensor_base]` in `platformio.ini`. The Linux environments
extend `linux_base`, not `sensor_base`, so today `ENV_INCLUDE_GPS` is undefined
and **no GPS code is compiled into the Linux build**. The `gps*` commands that
`variants/linux/meshcorectl` advertises are therefore stripped from the daemon
and do nothing.

The shared GPS init path cannot be reused verbatim on Linux:
`EnvironmentSensorManager::initBasicGPS()` calls
`Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX)` and `Serial1.begin(GPS_BAUD_RATE)`.
Those pin/baud macros are undefined on Linux (won't compile), and `Serial1` is
a hardware-UART concept that does not map to a Linux `/dev/tty*` device.
ardulinux (this fork's Arduino-on-Linux framework) is **not** Portduino, so it
has no `Serial1.setPath()` equivalent; the serial transport must be provided by
this repo.

Reference: Meshtastic's Portduino build uses a single `GPS.SerialPath`
config key (empty = GPS off) and `Serial1.setPath(path)`. We adopt the same
*config convention* (a device path, empty = off) but supply our own serial
transport.

## Decisions

- **Source:** serial device (`/dev/tty*`) read directly. No gpsd; no
  auto-probing of device paths. (gpsd is a documented future option, not built
  now.)
- **Reuse strategy:** turn on `ENV_INCLUDE_GPS=1` for the Linux build so the
  shared CLI, telemetry, advert-location, and `start_gps`/`stop_gps`/`loop`
  machinery all activate. Replace only the two Linux-incompatible pieces: the
  serial transport and the device-open code.
- **Upstream edit:** exactly one minimal `#if defined(ARDULINUX_PLATFORM)`
  branch inside `initBasicGPS()`. Everything else in the shared
  `EnvironmentSensorManager` is reused unchanged. (Chosen over a virtual-hook
  subclass — which would alter the shared class layout — and over a fully
  standalone Linux GPS reader — which would duplicate telemetry/CLI/advert
  plumbing that already exists.)
- **Config:** two flat `meshcored.ini` keys matching the existing style
  (`spidev`, `lora_gpiochip`, …): `gps_device` (default `""`) and `gps_baud`
  (default `9600`). Empty `gps_device` ⇒ GPS fully off.

## Components

### 1. `LinuxSerialStream` (new — `variants/linux/LinuxSerialStream.{h,cpp}`)

An Arduino `Stream` subclass wrapping a `/dev/tty*` file descriptor, mirroring
the fd-wrapping pattern already used by `LinuxConsole`.

- `bool begin(const char* path, int baud)` — open the device `O_RDWR |
  O_NOCTTY | O_NONBLOCK`, put it in raw mode via `termios` (`cfmakeraw`), set
  the baud with `cfsetispeed`/`cfsetospeed`, `tcsetattr`. Returns `false` and
  logs (via `MESH_DEBUG_PRINTLN`, naming the device and `errno`) on failure,
  leaving the fd at `-1`.
- `int available()` / `int read()` / `int peek()` — drain the fd, with a
  one-byte lookahead buffer like `LinuxConsole::_peek`.
- `size_t write(uint8_t)` + `using Print::write` — write bytes to the fd
  (needed because `MicroNMEA::sendSentence` writes to the stream).
- Single responsibility: bytes in/out of one serial device. No GPS knowledge.

Baud mapping: translate the integer `gps_baud` to the matching `Bxxxx` termios
constant (support the common NMEA rates 4800/9600/19200/38400/57600/115200;
unknown value ⇒ log and fall back to `B9600`).

### 2. `MicroNMEALocationProvider` (existing — reused unchanged)

Constructed with:
- `Stream& ser` = the `LinuxSerialStream`
- `mesh::RTCClock* clock` = `&rtc_clock` (enables GPS→clock time sync)
- `pin_reset = -1`, `pin_en = -1` (no GPIO enable/reset for a USB GPS)
- `peripher_power = NULL`

Its existing `loop()` parses NMEA and, on a valid fix, updates lat/lon/alt and
syncs the clock. No source changes.

### 3. `EnvironmentSensorManager` (existing — one minimal `#ifdef`)

In `initBasicGPS()`, wrap the Linux path:

```cpp
#if defined(ARDULINUX_PLATFORM)
  // Linux: open the configured serial device instead of a hardware UART.
  if (gps_device_is_unconfigured) {   // empty gps_device
    gps_detected = false;
    return;                           // GPS off, nothing opened
  }
  // LinuxSerialStream already opened by target.cpp before begin();
  _location->begin();
  _location->reset();
  delay(1000);
  gps_detected = <serial stream saw bytes>;
  ...normal detect/disable tail (shared)...
#else
  Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX);   // existing hardware path
  ...
#endif
```

Everything downstream is reused as-is: `start_gps()`, `stop_gps()`,
`querySensors()` GPS telemetry on `TELEM_CHANNEL_SELF`, `loop()`, and
`setSettingValue("gps", …)`.

*Detail to settle in the plan:* how the branch reaches the configured device
path / open state. Options: `target.cpp` opens `LinuxSerialStream` and the
provider before `sensors.begin()`, and the branch checks
`stream.isOpen()`/`available()`; or a small `extern` accessor. The plan will
pick the least-intrusive wiring. Either way the `#ifdef` block stays ~15 lines.

### 4. `LinuxConfig` + `target.cpp` (existing — extended)

- `LinuxConfig`: add `char* gps_device = "";` and `int gps_baud = 9600;`,
  parsed in `LinuxConfig::load()` alongside the existing keys (`gps_device`
  via `safe_copy`, `gps_baud` via `atoi`).
- `target.cpp`: instantiate `LinuxSerialStream`, construct the
  `MicroNMEALocationProvider` from it + `rtc_clock`, and construct
  `EnvironmentSensorManager sensors(location)` (the `ENV_INCLUDE_GPS`
  constructor takes a `LocationProvider&`). Open the device from
  `config.gps_device`/`config.gps_baud` after config load.

### 5. Build config (`variants/linux/platformio.ini`)

- Add `-D ENV_INCLUDE_GPS=1` to the Linux build flags.
- Add `stevemarple/MicroNMEA @ ^2.0.6` to `lib_deps`.
- Add `LinuxSerialStream.cpp` to the build (it is already covered by
  `+<../variants/linux>` in `build_src_filter`).

## Data & control flow

**Startup:** `board.begin()` loads `meshcored.ini` → `gps_device`, `gps_baud`.
`target.cpp` opens the `LinuxSerialStream`. `sensors.begin()` →
`initBasicGPS()` [ARDULINUX branch]: if no device configured, `gps_detected =
false` and return; else `begin()`/`reset()` the provider, drain ~1s, set
`gps_detected` from whether bytes were seen.

**Runtime:** `sensors.loop()` (already called from the repeater/main loop) →
when `gps_active`, `_location->loop()` reads the fd, feeds `MicroNMEA`, and on a
valid fix updates `node_lat/node_lon/node_altitude` and syncs `rtc_clock`.

**CLI (through `LinuxConsole`):** `gps on/off` → `setSettingValue("gps", …)` →
`start_gps()`/`stop_gps()`; `gps` status → provider `isValid()`/
`satellitesCount()`; `gps setloc`, `gps sync`, `gps advert` reuse existing
handlers. These are already listed in `meshcorectl`.

**Telemetry/advert:** `querySensors()` emits GPS on `TELEM_CHANNEL_SELF` when
the requester has `TELEM_PERM_LOCATION` and `gps_active`. Unchanged.

**`gps on/off` semantics on Linux:** with `pin_en = -1`, `_location->begin()`/
`stop()` are hardware no-ops, but `gps_active` still gates reading and
telemetry — so the toggle is meaningful in software. This matches existing
boards that lack a GPS enable pin.

## Error handling & edge cases

- **No device configured** (default): early return, `gps_detected = false`,
  no fd opened, `getNumSettings()` returns 0 → no `gps` setting exposed;
  `gps` status reports off. Existing installs unaffected.
- **Open fails** (bad path / permissions / ENOENT): `LinuxSerialStream::begin()`
  logs the device and `errno` and leaves fd `-1`; detection yields
  `gps_detected = false`. **Non-fatal** — the daemon keeps running without GPS.
  (Loud log per the repo's "fail loud" convention, but not `exit()`, because a
  missing GPS must not take down the mesh node.)
- **Opens but no NMEA** (wrong baud / silent GPS): 1s detect drain sees no
  bytes → `gps_detected = false`, provider stopped. `PERSISTANT_GPS` is not
  defined for Linux, so the normal detect-or-disable path applies.
- **Permissions:** USB GPS devices are usually `dialout`/root-owned. Documented
  in the README: grant `meshcored` read access (systemd unit already runs
  privileged for GPIO/SPI; note a udev rule / group add for unprivileged runs).
  No code change.
- **Partial reads / EAGAIN:** non-blocking fd; `read()` returns one byte or -1;
  buffered one-byte lookahead like `LinuxConsole`. No blocking in the main loop.

## Testing

- **Build:** `pio run -e linux` and `pio run -e linux_repeater` compile cleanly
  with GPS enabled. This alone catches the `Serial1`/undefined-pin class of
  errors and confirms `MicroNMEA` links.
- **Runtime smoke (`/verify`):** feed canned `$GPGGA`/`$GPRMC` NMEA into a PTY
  with `socat`, point `gps_device` at the PTY, run `meshcored`, and drive
  `gps on` / `gps` / `gps setloc` via `meshcorectl`; confirm fix, satellite
  count, and lat/lon populate, and that `rtc_clock` syncs.
- **No unit test** is added for `LinuxSerialStream`: it is thin termios I/O
  glue, and the `[env:native]` gtest env compiles only `Utils.cpp` (it cannot
  link sensor/serial code). This gap is intentional and called out rather than
  papered over.

## Documentation

Update `variants/linux/README.md`: document `gps_device`/`gps_baud` in the
config table, the `gps*` CLI commands (now functional), and the device-access
permission note.

## Out of scope

- gpsd integration (future option).
- Device-path auto-detection.
- GPIO enable/reset pin control for GPS on Linux (USB GPS units don't need it).
- Any change to non-Linux variants beyond the single guarded `initBasicGPS()`
  branch.
