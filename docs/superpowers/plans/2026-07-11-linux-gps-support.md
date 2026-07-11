# Linux GPS Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the `linux`/`linux_repeater` builds read location from a USB/UART serial GPS device, reusing the shared MeshCore GPS CLI, telemetry, and advert machinery.

**Architecture:** Enable `ENV_INCLUDE_GPS=1` for the Linux build so the shared `EnvironmentSensorManager` GPS surface activates. Supply the two Linux-specific pieces the shared code can't: a `termios`-backed `LinuxSerialStream` (an Arduino `Stream` over a `/dev/tty*` fd, modelled on `LinuxConsole`) and a single guarded branch in `initBasicGPS()` that opens the configured device instead of a hardware UART. `target.cpp` owns the stream and provider; the shared file learns device state only through a tiny `extern` accessor.

**Tech Stack:** C++17, ardulinux (Arduino-on-Linux), PlatformIO, `stevemarple/MicroNMEA`, POSIX `termios`.

## Global Constraints

- Target build environments: `env:linux` and `env:linux_repeater` only. No change to any other variant's behavior.
- Exactly ONE edit to shared upstream code: a `#if defined(ARDULINUX_PLATFORM)` branch inside `EnvironmentSensorManager::initBasicGPS()`. All other new code lives in `variants/linux/`.
- The Linux-build preprocessor macro is `ARDULINUX_PLATFORM` (defined in `variants/linux/platformio.ini`). Do NOT gate on `ARDULINUX_HARDWARE` — that is a separate flag meaning libgpiod is present.
- Config keys: `gps_device` (string, default `""`) and `gps_baud` (int, default `9600`), parsed in `LinuxConfig::load()` in the same flat style as `spidev`/`lora_gpiochip`.
- Empty/unset `gps_device` ⇒ GPS fully off: no device opened, `gps_detected` stays false, no `gps` setting exposed.
- A missing/unopenable GPS device is NON-FATAL: log loudly via `MESH_DEBUG_PRINTLN` (naming device + errno) but never `exit()`. The mesh node must keep running without GPS.
- `MicroNMEA` version floor: `stevemarple/MicroNMEA @ ^2.0.6` (matches `[sensor_base]`).
- Follow existing repo conventions: fd-wrapping like `LinuxConsole`; config parsing via the existing `trim`/`safe_copy` helpers.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `variants/linux/LinuxSerialStream.h` (create) | `Stream` subclass declaration: open/close a serial `/dev/tty*` fd, non-blocking read/peek/write. |
| `variants/linux/LinuxSerialStream.cpp` (create) | Implementation: `termios` raw-mode open, baud mapping, one-byte lookahead read. |
| `variants/linux/LinuxBoard.h` (modify) | Add `gps_device`/`gps_baud` fields to `LinuxConfig`. |
| `variants/linux/LinuxBoard.cpp` (modify) | Parse `gps_device`/`gps_baud` in `LinuxConfig::load()`. |
| `variants/linux/target.h` (modify) | Declare the GPS provider/stream externs and `linux_gps_available()`. |
| `variants/linux/target.cpp` (modify) | Instantiate `LinuxSerialStream` + `MicroNMEALocationProvider`, construct `sensors` with the provider, open the device from config, define `linux_gps_available()`. |
| `variants/linux/platformio.ini` (modify) | Add `-D ENV_INCLUDE_GPS=1` and the `MicroNMEA` lib dep to both linux envs. |
| `src/helpers/sensors/EnvironmentSensorManager.cpp` (modify) | ONE guarded branch in `initBasicGPS()` for the Linux device-open path. |
| `variants/linux/meshcored.ini` (modify) | Document `gps_device`/`gps_baud` (commented sample). |
| `variants/linux/README.md` (modify) | Config table rows, GPS CLI commands, device-permission note. |

**Build ordering note:** Tasks 1–2 (stream + config) are pure `variants/linux` additions that compile independently. Task 3 wires them into `target.cpp` and flips the build flag — this is the task that first requires `MicroNMEA` and the shared GPS code to compile. Task 4 is the single shared-file edit. Task 5 is docs. Do them in order.

---

### Task 1: `LinuxSerialStream` — termios-backed serial Stream

**Files:**
- Create: `variants/linux/LinuxSerialStream.h`
- Create: `variants/linux/LinuxSerialStream.cpp`

**Interfaces:**
- Consumes: Arduino `Stream`/`Print` base (from ardulinux framework), POSIX `termios`/`unistd`/`fcntl`.
- Produces:
  - `class LinuxSerialStream : public Stream`
  - `bool begin(const char* path, int baud)` — returns `true` if the device opened, `false` otherwise (logs on failure).
  - `bool isOpen() const` — `true` when the fd is valid.
  - `void end()` — close the fd.
  - Overrides: `int available()`, `int read()`, `int peek()`, `size_t write(uint8_t)` (+ `using Print::write;`).

**Note on testing:** This class is thin POSIX I/O glue. The `[env:native]` gtest env compiles only `Utils.cpp` and cannot link Arduino `Stream`/`termios` code, so there is no unit test here — this is intentional (see spec §Testing). Verification is by build (Task 3) + runtime smoke over a `socat` PTY (Task 3, Step 8). Do not attempt to add a gtest for this file.

- [ ] **Step 1: Write the header**

Create `variants/linux/LinuxSerialStream.h`:

```cpp
#pragma once
#include <Stream.h>

// Serial-device Stream for the ArduLinux (Linux) build.
//
// ardulinux has no hardware UART / Serial1.setPath(); a serial GPS on Linux is
// a /dev/tty* character device. LinuxSerialStream opens such a device with
// termios (raw mode, configured baud, non-blocking) and presents it as an
// Arduino Stream so the shared MicroNMEALocationProvider can read/write NMEA
// through it unchanged. Modelled on LinuxConsole's fd wrapping.
class LinuxSerialStream : public Stream {
public:
  // Open `path` at `baud`. Returns true on success. On failure, logs the
  // device and errno and leaves the stream closed (isOpen() == false).
  // Non-fatal: callers proceed without GPS.
  bool begin(const char* path, int baud);
  void end();
  bool isOpen() const { return _fd >= 0; }

  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t c) override;
  using Print::write;

private:
  int rawReadByte();   // one raw byte from the fd, or -1

  int _fd   = -1;
  int _peek = -1;      // one-byte lookahead, or -1
};
```

- [ ] **Step 2: Write the implementation**

Create `variants/linux/LinuxSerialStream.cpp`:

```cpp
#include "LinuxSerialStream.h"
#include <Mesh.h>            // MESH_DEBUG_PRINTLN
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <string.h>

// Map an integer baud to the matching termios Bxxxx constant.
// Unknown values log and fall back to B9600.
static speed_t baud_to_speed(int baud) {
  switch (baud) {
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:
      MESH_DEBUG_PRINTLN("LinuxSerialStream: unsupported baud %d, using 9600", baud);
      return B9600;
  }
}

bool LinuxSerialStream::begin(const char* path, int baud) {
  end();  // idempotent

  _fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (_fd < 0) {
    MESH_DEBUG_PRINTLN("LinuxSerialStream: cannot open %s: %s", path, strerror(errno));
    return false;
  }

  struct termios tio;
  if (tcgetattr(_fd, &tio) != 0) {
    MESH_DEBUG_PRINTLN("LinuxSerialStream: tcgetattr(%s) failed: %s", path, strerror(errno));
    close(_fd);
    _fd = -1;
    return false;
  }

  cfmakeraw(&tio);
  speed_t sp = baud_to_speed(baud);
  cfsetispeed(&tio, sp);
  cfsetospeed(&tio, sp);
  tio.c_cflag |= (CLOCAL | CREAD);  // ignore modem ctrl lines, enable receiver
  tio.c_cc[VMIN]  = 0;              // non-blocking read
  tio.c_cc[VTIME] = 0;

  if (tcsetattr(_fd, TCSANOW, &tio) != 0) {
    MESH_DEBUG_PRINTLN("LinuxSerialStream: tcsetattr(%s) failed: %s", path, strerror(errno));
    close(_fd);
    _fd = -1;
    return false;
  }

  tcflush(_fd, TCIFLUSH);
  MESH_DEBUG_PRINTLN("LinuxSerialStream: opened %s @ %d", path, baud);
  return true;
}

void LinuxSerialStream::end() {
  if (_fd >= 0) { close(_fd); _fd = -1; }
  _peek = -1;
}

int LinuxSerialStream::rawReadByte() {
  if (_fd < 0) return -1;
  uint8_t b;
  ssize_t n = ::read(_fd, &b, 1);
  if (n == 1) return b;
  return -1;  // n == 0 (no data) or -1/EAGAIN
}

int LinuxSerialStream::available() {
  if (_peek >= 0) return 1;
  _peek = rawReadByte();
  return _peek >= 0 ? 1 : 0;
}

int LinuxSerialStream::read() {
  if (_peek >= 0) { int b = _peek; _peek = -1; return b; }
  return rawReadByte();
}

int LinuxSerialStream::peek() {
  if (_peek < 0) _peek = rawReadByte();
  return _peek;
}

size_t LinuxSerialStream::write(uint8_t c) {
  if (_fd < 0) return 0;
  ssize_t n = ::write(_fd, &c, 1);
  return n == 1 ? 1 : 0;
}
```

- [ ] **Step 3: Verify it parses in isolation (host syntax check)**

The full build needs ardulinux (not installed on the dev host), so do a standalone syntax check that the C++ is well-formed. The Arduino `Stream.h` and `Mesh.h` are unavailable on the host, so compile only far enough to catch syntax/typo errors in the POSIX logic:

Run:
```bash
cd /Users/john/Code/meshcore-linux
g++ -std=c++17 -fsyntax-only -x c++ - <<'EOF'
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <cstdint>
static speed_t baud_to_speed(int baud) {
  switch (baud) {
    case 4800: return B4800; case 9600: return B9600;
    case 19200: return B19200; case 38400: return B38400;
    case 57600: return B57600; case 115200: return B115200;
    default: return B9600;
  }
}
int main(){ struct termios t; (void)baud_to_speed(9600); (void)t; return 0; }
EOF
echo "syntax OK ec=$?"
```
Expected: `syntax OK ec=0` (confirms termios constants/usage compile on this host; the class body itself is validated by the real build in Task 3).

- [ ] **Step 4: Commit**

```bash
git add variants/linux/LinuxSerialStream.h variants/linux/LinuxSerialStream.cpp
git commit -m "variants/linux: add termios-backed LinuxSerialStream for serial GPS"
```

---

### Task 2: Config keys `gps_device` / `gps_baud`

**Files:**
- Modify: `variants/linux/LinuxBoard.h` (add fields to `LinuxConfig`, after line 41 `lon`)
- Modify: `variants/linux/LinuxBoard.cpp:170-173` (add parse cases in `LinuxConfig::load()`)

**Interfaces:**
- Consumes: existing `trim`/`safe_copy` helpers and `LinuxConfig::load()` parse loop.
- Produces: `LinuxConfig::gps_device` (`char*`, default `""`), `LinuxConfig::gps_baud` (`int`, default `9600`). Task 3 reads both.

- [ ] **Step 1: Add fields to `LinuxConfig`**

In `variants/linux/LinuxBoard.h`, after the `lon` field (line 41), add:

```cpp
  char *gps_device = "";
  int   gps_baud   = 9600;
```

(Placed alongside `advert_name`/`admin_password`/`lat`/`lon`, matching the string-with-default style. `gps_device` defaults to an empty literal — GPS off.)

- [ ] **Step 2: Add parse cases in `load()`**

In `variants/linux/LinuxBoard.cpp`, after the `lon` case (line 173, `else if (strcmp(key, "lon") == 0) lon = atof(value);`), add:

```cpp
    else if (strcmp(key, "gps_device") == 0)  gps_device = safe_copy(value, 64);
    else if (strcmp(key, "gps_baud") == 0)    gps_baud = atoi(value);
```

(`safe_copy(…, 64)` — device paths are short; `/dev/serial/by-id/...` symlinks can be longer than 32, so 64 gives headroom.)

- [ ] **Step 3: Host syntax check of the parse additions**

Run:
```bash
cd /Users/john/Code/meshcore-linux
grep -n "gps_device\|gps_baud" variants/linux/LinuxBoard.h variants/linux/LinuxBoard.cpp
```
Expected: two matches in `.h` (field decls) and two in `.cpp` (parse cases). Confirms both edits landed. (Real compile happens in Task 3.)

- [ ] **Step 4: Commit**

```bash
git add variants/linux/LinuxBoard.h variants/linux/LinuxBoard.cpp
git commit -m "variants/linux: add gps_device/gps_baud config keys"
```

---

### Task 3: Wire the provider into `target.cpp` and enable the build flag

This is the integration task: it flips `ENV_INCLUDE_GPS=1`, adds the `MicroNMEA` dep, constructs the provider, opens the device, and exposes `linux_gps_available()` for the shared branch (Task 4). It ends with the first real build + runtime smoke test.

**Files:**
- Modify: `variants/linux/platformio.ini` (both `[env:linux]` and `[env:linux_repeater]`)
- Modify: `variants/linux/target.h`
- Modify: `variants/linux/target.cpp`

**Interfaces:**
- Consumes: `LinuxSerialStream` (Task 1), `LinuxConfig::gps_device`/`gps_baud` (Task 2), shared `MicroNMEALocationProvider`, shared `EnvironmentSensorManager(LocationProvider&)` constructor (active under `ENV_INCLUDE_GPS`).
- Produces:
  - Global `EnvironmentSensorManager sensors` constructed with the Linux provider.
  - `bool linux_gps_available()` — `true` if the Linux GPS stream is open AND has bytes available. Task 4's shared branch calls this. Declared in `target.h`, defined in `target.cpp`.

- [ ] **Step 1: Enable GPS in `[env:linux]` and add the lib dep**

In `variants/linux/platformio.ini`, change `[env:linux]` to add the GPS build flag and the MicroNMEA dependency:

```ini
[env:linux]
extends = linux_base
build_flags = ${linux_base.build_flags}
  -D ENV_INCLUDE_GPS=1
  !pkg-config --cflags --libs libbsd-overlay --silence-errors || :
lib_deps =
  ${linux_base.lib_deps}
  stevemarple/MicroNMEA @ ^2.0.6
```

- [ ] **Step 2: Enable GPS in `[env:linux_repeater]`**

In the same file, add to `[env:linux_repeater]`'s `build_flags` (after `-D MESH_DEBUG=1`):

```ini
  -D ENV_INCLUDE_GPS=1
```

and add the dep to its `lib_deps`:

```ini
lib_deps =
  ${linux_base.lib_deps}
  stevemarple/MicroNMEA @ ^2.0.6
```

- [ ] **Step 3: Declare externs in `target.h`**

In `variants/linux/target.h`, add the include and externs. After the existing `#include <helpers/sensors/EnvironmentSensorManager.h>` (line 8) add:

```cpp
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include "LinuxSerialStream.h"
```

After `extern EnvironmentSensorManager sensors;` (line 21) add:

```cpp
extern LinuxSerialStream gps_serial;
extern MicroNMEALocationProvider gps_location;

// True if a serial GPS device is open and currently has bytes to read.
// Consumed by EnvironmentSensorManager::initBasicGPS() on the Linux build.
bool linux_gps_available();
```

- [ ] **Step 4: Construct the provider and open the device in `target.cpp`**

In `variants/linux/target.cpp`, replace the sensor construction line (line 22, `EnvironmentSensorManager sensors;`) with the GPS-enabled construction. Because `ENV_INCLUDE_GPS` is now defined, the `EnvironmentSensorManager` constructor requires a `LocationProvider&`.

Replace:

```cpp
EnvironmentSensorManager sensors;
```

with:

```cpp
LinuxSerialStream gps_serial;
MicroNMEALocationProvider gps_location(gps_serial, &rtc_clock, -1, -1, NULL);
EnvironmentSensorManager sensors(gps_location);

bool linux_gps_available() {
  return gps_serial.isOpen() && gps_serial.available() > 0;
}
```

Then, so the device is opened from config before `sensors.begin()` runs, add an opener. `radio_init()` (line 29) already runs early after config load. Add a dedicated helper and call it — but the cleanest hook is to open in `radio_init()` right after `rtc_clock.begin();` since that is invoked from `setup()` after `board.begin()` (which loads config). Insert after line 30 (`rtc_clock.begin();`):

```cpp
  if (board.config.gps_device && board.config.gps_device[0] != '\0') {
    gps_serial.begin(board.config.gps_device, board.config.gps_baud);
  }
```

Note: `gps_location` is constructed with `pin_reset = -1`, `pin_en = -1` so its `begin()`/`stop()`/`reset()` are hardware no-ops (correct for USB GPS). `rtc_clock` is passed so GPS fixes sync the system clock.

- [ ] **Step 5: Build `env:linux`**

Run (from a machine with the Linux toolchain + libgpiod; this is the authoritative compile that exercises Tasks 1–3 together):
```bash
cd /Users/john/Code/meshcore-linux
pio run -e linux 2>&1 | tail -40; ec=${PIPESTATUS[0]}; echo "pio ec=$ec"; test $ec -eq 0
```
Expected: `pio ec=0`. This confirms `MicroNMEA` links, `ENV_INCLUDE_GPS` compiles the shared GPS surface, and `LinuxSerialStream`/provider wiring is type-correct.

> If `pio` is unavailable in the current environment, STOP and report — do not mark this step done. The build is the primary verification for Tasks 1–4 and cannot be skipped or faked.

- [ ] **Step 6: Build `env:linux_repeater`**

Run:
```bash
cd /Users/john/Code/meshcore-linux
pio run -e linux_repeater 2>&1 | tail -40; ec=${PIPESTATUS[0]}; echo "pio ec=$ec"; test $ec -eq 0
```
Expected: `pio ec=0`.

- [ ] **Step 7: Commit the wiring (before Task 4 the shared branch is still missing, but the build must already pass)**

The shared `initBasicGPS()` still uses the `Serial1` path at this point, which will fail to compile under `ARDULINUX_PLATFORM` (undefined `Serial1`/`PIN_GPS_TX`). Therefore **Task 4 must be completed before Steps 5–6 can pass.** Reorder note for the executor: implement Task 4's code edit FIRST, then run Steps 5–6 here, then commit both together.

Commit:
```bash
git add variants/linux/platformio.ini variants/linux/target.h variants/linux/target.cpp \
        src/helpers/sensors/EnvironmentSensorManager.cpp
git commit -m "variants/linux: wire serial GPS provider and enable ENV_INCLUDE_GPS"
```

- [ ] **Step 8: Runtime smoke test with a fake NMEA feed**

Create a PTY pair, feed canned NMEA into it, point a test `meshcored.ini` at it, and drive the CLI. Run:

```bash
cd /Users/john/Code/meshcore-linux
# Terminal A: create a PTY and stream canned fixes into it
socat -d -d pty,raw,echo=0,link=/tmp/fakegps pty,raw,echo=0,link=/tmp/fakegps_host &
SOCAT_PID=$!
# Feed valid GGA/RMC sentences (San Francisco ~37.77,-122.41) once per second
( while true; do
    printf '$GPRMC,183010,A,3746.30,N,12224.60,W,0.0,0.0,110726,,*13\r\n$GPGGA,183010,3746.30,N,12224.60,W,1,08,0.9,10.0,M,,,,*47\r\n' > /tmp/fakegps_host
    sleep 1
  done ) &
FEED_PID=$!
```

Then run `meshcored` with `gps_device = /tmp/fakegps`, `gps_baud = 9600` in its ini, connect via `meshcorectl`, and run `gps on`, then `gps`. Expected: status shows `on, active, fix, N sats`, and `gps setloc` populates lat/lon near 37.77/-122.41.

Clean up: `kill $FEED_PID $SOCAT_PID`.

Use the `/verify` skill to drive this end-to-end. Record the observed CLI output in the commit/PR description. If `socat`/hardware is unavailable in the execution environment, STOP and report that the runtime smoke was not run — do not claim GPS works from a clean build alone.

---

### Task 4: Single guarded branch in shared `initBasicGPS()`

**MUST be implemented before Task 3 Steps 5–6 build** (the shared file otherwise fails to compile on Linux). Kept as a separate task because it is the one upstream-shared edit and warrants its own review gate.

**Files:**
- Modify: `src/helpers/sensors/EnvironmentSensorManager.cpp:733-772` (`initBasicGPS`)

**Interfaces:**
- Consumes: `linux_gps_available()` (declared in `variants/linux/target.h`, Task 3). On Linux, `target.h` is on the include path via the variant; declare it locally to avoid pulling variant headers into shared code (see Step 1).
- Produces: no new symbols; sets `gps_detected`/`gps_active` as the existing code does.

- [ ] **Step 1: Add the Linux branch**

In `src/helpers/sensors/EnvironmentSensorManager.cpp`, replace the body of `initBasicGPS()` (lines 733–772) so the hardware-UART setup is guarded and a Linux path is added. The shared file must not include variant headers, so forward-declare the accessor at file scope.

Near the top of the `#if ENV_INCLUDE_GPS` block that contains `initBasicGPS` (immediately before `void EnvironmentSensorManager::initBasicGPS() {` at line 733), add:

```cpp
#if defined(ARDULINUX_PLATFORM)
// Defined in variants/linux/target.cpp. The Linux build opens the serial GPS
// device itself (there is no hardware UART); this reports whether that device
// is open and currently has NMEA bytes to read.
bool linux_gps_available();
#endif
```

Then change the top of `initBasicGPS()` from:

```cpp
void EnvironmentSensorManager::initBasicGPS() {

  Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX);

  #ifdef GPS_BAUD_RATE
  Serial1.begin(GPS_BAUD_RATE);
  #else
  Serial1.begin(9600);
  #endif

  // Try to detect if GPS is physically connected to determine if we should expose the setting
  _location->begin();
  _location->reset();

  #ifndef PIN_GPS_EN
    MESH_DEBUG_PRINTLN("No GPS wake/reset pin found for this board. Continuing on...");
  #endif

  // Give GPS a moment to power up and send data
  delay(1000);

  // We'll consider GPS detected if we see any data on Serial1
#ifdef ENV_SKIP_GPS_DETECT
  gps_detected = true;
#else
  gps_detected = (Serial1.available() > 0);
#endif
```

to:

```cpp
void EnvironmentSensorManager::initBasicGPS() {

#if defined(ARDULINUX_PLATFORM)
  // Linux: the serial device is opened by the variant (target.cpp) from the
  // gps_device/gps_baud config. If nothing was configured/opened, GPS is off.
  _location->begin();   // no-op on Linux (pin_en/pin_reset == -1)
  _location->reset();
  delay(1000);          // let the device stream a first fix
  gps_detected = linux_gps_available();
#else
  Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX);

  #ifdef GPS_BAUD_RATE
  Serial1.begin(GPS_BAUD_RATE);
  #else
  Serial1.begin(9600);
  #endif

  // Try to detect if GPS is physically connected to determine if we should expose the setting
  _location->begin();
  _location->reset();

  #ifndef PIN_GPS_EN
    MESH_DEBUG_PRINTLN("No GPS wake/reset pin found for this board. Continuing on...");
  #endif

  // Give GPS a moment to power up and send data
  delay(1000);

  // We'll consider GPS detected if we see any data on Serial1
#ifdef ENV_SKIP_GPS_DETECT
  gps_detected = true;
#else
  gps_detected = (Serial1.available() > 0);
#endif
#endif  // ARDULINUX_PLATFORM
```

The tail of the function (lines 761–772: the `if (gps_detected)` / `_location->stop()` block) is shared and unchanged.

- [ ] **Step 2: Verify the diff is minimal and correctly bracketed**

Run:
```bash
cd /Users/john/Code/meshcore-linux
git diff src/helpers/sensors/EnvironmentSensorManager.cpp
```
Expected: only `initBasicGPS()` changed; every `#if`/`#else`/`#endif` balanced; the shared tail (`if (gps_detected) { … } _location->stop(); gps_active = false;`) untouched. No other function modified.

- [ ] **Step 3: Build both linux envs (this proves Task 3 + Task 4 together)**

This is Task 3 Steps 5–6. Run them now:
```bash
cd /Users/john/Code/meshcore-linux
pio run -e linux 2>&1 | tail -40; ec=${PIPESTATUS[0]}; echo "linux ec=$ec"; test $ec -eq 0 && \
pio run -e linux_repeater 2>&1 | tail -40; ec=${PIPESTATUS[0]}; echo "repeater ec=$ec"; test $ec -eq 0
```
Expected: both `ec=0`.

- [ ] **Step 4: Commit (with the Task 3 wiring — they are one buildable unit)**

```bash
git add src/helpers/sensors/EnvironmentSensorManager.cpp \
        variants/linux/platformio.ini variants/linux/target.h variants/linux/target.cpp
git commit -m "sensors: open serial GPS via variant accessor on ArduLinux builds"
```

(If Task 3's wiring was already committed in its Step 7, this commit covers only the shared file. Either grouping is fine as long as no commit is left in a non-compiling state.)

---

### Task 5: Documentation

**Files:**
- Modify: `variants/linux/meshcored.ini` (commented sample keys)
- Modify: `variants/linux/README.md` (config table + CLI + permissions)

**Interfaces:** none (docs only).

- [ ] **Step 1: Add sample config keys to `meshcored.ini`**

At the end of `variants/linux/meshcored.ini` (after line 29), add:

```ini

# GPS (serial NMEA device). Leave gps_device unset/empty to disable GPS.
# gps_device = /dev/ttyACM0    # or /dev/ttyUSB0, /dev/serial/by-id/...
# gps_baud = 9600              # 4800/9600/19200/38400/57600/115200
```

- [ ] **Step 2: Document in README config table**

In `variants/linux/README.md`, find the config table (the row block near line 106 that includes `lat`/`lon`). Add two rows:

```markdown
| `gps_device` | *(empty)* | Path to a serial NMEA GPS device (e.g. `/dev/ttyACM0`). Empty disables GPS. |
| `gps_baud` | `9600` | Baud rate for `gps_device`. One of 4800/9600/19200/38400/57600/115200. |
```

- [ ] **Step 3: Document the GPS CLI commands and permissions**

In `variants/linux/README.md`, in the CLI section (near line 198, "Node name, password, and location can be changed via the serial CLI"), add a GPS subsection:

```markdown
### GPS

With `gps_device` configured, the standard MeshCore GPS commands work over the
control CLI (`meshcorectl`):

- `gps` — status: on/off, active/deactivated, fix/no-fix, satellite count
- `gps on` / `gps off` — enable/disable GPS reading and location telemetry
- `gps sync` — force a time re-sync from GPS
- `gps setloc` — save the current GPS fix as the node's advertised location
- `gps advert none|prefs|share` — control whether location is advertised

The daemon needs read access to `gps_device`. USB GPS units are usually owned
by root or the `dialout` group; the systemd service already runs with the
privileges it needs for SPI/GPIO. For an unprivileged run, add the user to the
device's group or install a udev rule granting access.
```

- [ ] **Step 4: Commit**

```bash
git add variants/linux/meshcored.ini variants/linux/README.md
git commit -m "docs(linux): document gps_device/gps_baud config and GPS CLI"
```

---

## Self-Review

**1. Spec coverage:**
- Serial `/dev/tty*` source → Task 1 (`LinuxSerialStream`). ✓
- Reuse via `ENV_INCLUDE_GPS=1` → Task 3 Steps 1–2. ✓
- One minimal `#ifdef` in shared file → Task 4. ✓
- `MicroNMEALocationProvider` reused with `pin=-1`, clock passed → Task 3 Step 4. ✓
- Config `gps_device`/`gps_baud`, empty=off → Task 2 + Task 3 Step 4 guard. ✓
- Non-fatal open failure, loud log → Task 1 Step 2 (`begin()` returns false + logs; never exits). ✓
- Detect-or-disable, no `PERSISTANT_GPS` on Linux → Task 4 (shared tail unchanged). ✓
- CLI/telemetry/advert reuse → inherent to `ENV_INCLUDE_GPS`; documented Task 5 Step 3. ✓
- Permissions note → Task 5 Step 3. ✓
- Build + `socat` runtime smoke, no native unit test → Task 3 Steps 5–6, Step 8; Task 1 testing note. ✓
- README updates → Task 5. ✓
- Out-of-scope items (gpsd, auto-detect, GPIO enable) → not present in any task. ✓

**2. Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to Task N". All code shown in full. ✓

**3. Type consistency:**
- `linux_gps_available()` — declared identically in `target.h` (Task 3 Step 3), defined in `target.cpp` (Task 3 Step 4), forward-declared + called in shared file (Task 4 Step 1). Same signature `bool()` everywhere. ✓
- `LinuxSerialStream::begin(const char*, int)`, `isOpen()`, `available()` — used in `target.cpp` and `linux_gps_available()` consistently with the Task 1 declaration. ✓
- `MicroNMEALocationProvider(gps_serial, &rtc_clock, -1, -1, NULL)` — matches the constructor signature `(Stream&, mesh::RTCClock*, int, int, RefCountedDigitalPin*)` in `MicroNMEALocationProvider.h`. ✓
- `EnvironmentSensorManager(gps_location)` — matches the `ENV_INCLUDE_GPS` constructor `(LocationProvider&)`. ✓

**Ordering caveat (called out for the executor):** Task 4's code edit must land before Task 3's build steps (5–6) can pass, because the unguarded `Serial1` path won't compile under `ARDULINUX_PLATFORM`. Implement Task 4 Step 1 together with Task 3's edits; run the builds once both are in place. No commit should be left in a non-compiling state.
