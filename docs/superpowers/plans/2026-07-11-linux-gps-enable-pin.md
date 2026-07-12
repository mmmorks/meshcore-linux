# Linux GPS Enable-Pin Implementation Plan

**Goal:** Drive a GPS module's enable/standby GPIO line HIGH on the Linux build so modules that boot into standby (L76K on the Waveshare HAT) stream NMEA.

**Architecture:** Add a `gps_en_pin` config key; `LinuxBoard::begin()` binds it via the existing `initGPIOPin` helper and drives it HIGH for the daemon lifetime. Reuses ardulinux's bound-pin `digitalWrite` → libgpiod path. Zero shared-upstream file changes.

## Global Constraints
- All changes in `variants/linux/` only. No shared-file edits.
- Config key: `gps_en_pin` (int, default `-1` = unset = current behavior).
- Active-high (drive HIGH to wake). Held HIGH for process lifetime.
- Enable-pin bind failure is NON-FATAL (log + continue); do NOT add it to the fatal LoRa `failures` tally.
- Build gate: `./build-docker.sh linux_repeater` exits 0 (macOS host; arm64 container authoritative).

---

### Task 1: Config key `gps_en_pin`

**Files:** Modify `variants/linux/LinuxBoard.h`, `variants/linux/LinuxBoard.cpp`.

- Add to `LinuxConfig` (after `gps_baud`): `int gps_en_pin = -1;`
- Add parse case in `LinuxConfig::load()` (after the `gps_baud` case):
  `else if (strcmp(key, "gps_en_pin") == 0)  gps_en_pin = atoi(value);`

### Task 2: Bind + drive the pin in `LinuxBoard::begin()`

**File:** Modify `variants/linux/LinuxBoard.cpp`.

After the LoRa-pin binding block and its fatal `if (failures > 0) { ... exit(1); }` check (i.e. once the radio pins are up), add:

```cpp
  // GPS enable/standby pin: some modules (e.g. L76K on the Waveshare
  // LoRaWAN/GNSS HAT) must have their STANDBY line driven HIGH to wake and
  // stream NMEA. Bind and hold it high for the daemon lifetime. Non-fatal:
  // a repeater must still run without GPS.
  if (config.gps_en_pin != -1) {
    if (initGPIOPin(config.gps_en_pin, config.lora_gpiochip, config.gps_en_pin) == 0) {
      pinMode(config.gps_en_pin, OUTPUT);
      digitalWrite(config.gps_en_pin, HIGH);
      printf("GPS enable pin %d driven HIGH\n", (int)config.gps_en_pin);
    } else {
      printf("WARNING: could not claim GPS enable pin %d; GPS may stay asleep\n",
             (int)config.gps_en_pin);
    }
  }
```

(`initGPIOPin` returns 0 on success, non-zero on failure — same convention as the LoRa binds. Note it is `#ifdef ARDULINUX_HARDWARE`-guarded internally and returns 0 in the sim build, which is fine.)

### Task 3: Build

Run:
```bash
cd /Users/john/Code/meshcore-linux
docker run --rm --platform linux/arm64 -v "$(pwd)":/src -w /src -v mc_pio_cache:/root/.platformio \
  -e FIRMWARE_VERSION=dev debian:bookworm bash -c '
    set -e
    apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      build-essential git python3 python3-venv pkg-config libgpiod-dev libi2c-dev libbluetooth-dev libuv1-dev >/dev/null 2>&1
    python3 -m venv /pio && . /pio/bin/activate && pip install --quiet --upgrade platformio
    pio run -e linux_repeater'
echo "DOCKER_EXIT=$?"
```
Expected: `DOCKER_EXIT=0`.

### Task 4: Docs

**Files:** Modify `variants/linux/meshcored.ini`, `variants/linux/README.md`.

- `meshcored.ini`, after the `gps_baud` sample line:
  `# gps_en_pin = 4               # GPIO to hold HIGH to wake the GPS (e.g. L76K STANDBY on Waveshare LoRaWAN/GNSS HAT)`
- `README.md` config table: add row
  `| \`gps_en_pin\` | \`-1\` | GPIO line held HIGH to wake a GPS module that boots in standby (e.g. L76K STANDBY on the Waveshare LoRaWAN/GNSS HAT). \`-1\` = none. |`
- `README.md` GPS section: one sentence — some modules (L76K on the Waveshare HAT) stay asleep until their enable/standby line is driven high; set `gps_en_pin` to that GPIO.

### Task 5: On-hardware verification (definitive)

Deploy to pimesh, set `gps_en_pin = 4`, restart, confirm `meshcorectl gps on` → `fix, N sats` with NO manual GPIO holding. (Details handled interactively — the module already proven to fix when GPIO4 is held high.)

## Self-Review
- Config → Task 1. Bind+drive → Task 2. Build → Task 3. Docs → Task 4. HW verify → Task 5. All spec sections covered.
- Non-fatal handling: Task 2 does not touch the fatal `failures` tally. ✓
- Zero shared-file edits: all tasks in `variants/linux/`. ✓
