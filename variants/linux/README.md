# MeshCore Linux Variant

Native Linux support for MeshCore, targeting Raspberry Pi (Zero, 3, 4, 5) and similar SBCs with an SX1262 LoRa radio attached over SPI. Uses [ArduLinux, Arduino API for Linux](https://github.com/l5yth/ardulinux) to run the same firmware codebase on Linux without modification to the core library.

## Hardware

- Raspberry Pi (any model with SPI)
- SX1262-based LoRa module wired to the Pi's SPI bus (e.g. Waveshare SX1262 HAT, PoW SX1262 HAT)
- SPI, IRQ, RESET, and optionally BUSY/RXEN/TXEN GPIO pins

## Build

**Dependencies** (install on the build machine and on the Pi):

```sh
# Arch Linux
sudo pacman -S pkgconf libgpiod i2c-tools bluez-libs libuv

# Debian/Raspberry Pi OS
sudo apt install pkg-config libgpiod-dev libi2c-dev libbluetooth-dev libuv1-dev
```

The ArduLinux platform always links `bluetooth`, `uv`, `pthread`, and
`stdc++fs`; `gpiod`/`i2c` are added automatically when libgpiod is detected via
`pkg-config`. If `pkg-config` is missing (e.g. on DietPi, which does not ship
it in the base image), libgpiod goes undetected and the build falls back to
simulated GPIO/I2C — the resulting `meshcored` will refuse to start with a
`FATAL: meshcored was built without libgpiod support` message pointing back at
the missing dep. Missing `bluez-libs`/`libbluetooth-dev` shows up at link time
as `cannot find -lbluetooth`.

You also need **PlatformIO Core** (`pio`) to build:

```sh
# Arch Linux
sudo pacman -S platformio-core   # or: pipx install platformio

# Debian/Raspberry Pi OS
pipx install platformio          # or: pip install --user platformio
```

**Build with `build.sh`** (recommended, embeds version and commit hash):

```sh
FIRMWARE_VERSION=dev ./build.sh build-firmware linux_repeater
# binary: .pio/build/linux_repeater/meshcored
```

Alternatively, build directly with PlatformIO (no version metadata):

```sh
FIRMWARE_VERSION=dev pio run -e linux_repeater
```

For a reproducible cross-build in a container (e.g. on a non-Linux dev
machine), use `./build-docker.sh linux_repeater` from the repo root. It
defaults to `debian:bookworm`, which ships libgpiod 1.x; set `BASE_IMAGE` to
build against libgpiod 2.x instead:

```sh
BASE_IMAGE=debian:trixie ./build-docker.sh linux_repeater
# binary: .pio/build-trixie/linux_repeater/meshcored (bookworm keeps the
# default, untagged .pio/build/ path)
```

`EventGPIOPin`'s libgpiod-major-version code is selected by a preprocessor
check (`GPIOD_LINE_BULK_MAX_LINES`), so only one of the v1/v2 code paths is
ever compiled per build — building on both bases at least once before release
is the only way to catch a break in the untested one.

## Setup

### 1. Install the binary

```sh
sudo install -m 755 .pio/build/linux_repeater/meshcored /usr/bin/meshcored
sudo install -m 755 variants/linux/meshcorectl /usr/bin/meshcorectl
```

`meshcorectl` is the client for the node's CLI — see [step 5](#5-talk-to-the-running-node-meshcorectl). Install it now; once `meshcored`
is running as a service it is the only way to reach the CLI.

### 2. Create the config file

Two ready-made templates are provided in `variants/linux/`:

| Template | Hardware |
|----------|----------|
| `meshcored.ini.pow-sx1262` | RPi Zero 2W + PoW SX1262 HAT |
| `meshcored.ini.waveshare` | RPi 3/4/5 + Waveshare SX1262 LoRa HAT |

```sh
# Pick the template that matches your hardware (install -D creates /etc/meshcored):
sudo install -D -m 644 variants/linux/meshcored.ini.waveshare /etc/meshcored/meshcored.ini
sudo nano /etc/meshcored/meshcored.ini
```

The config file has two roles:

- **Hardware config** (always read on every startup): SPI device, GPIO pin numbers, LoRa radio parameters.
- **First-run node defaults**: `advert_name`, `admin_password`, `lat`, `lon`. On the first boot these are saved to the node's persisted prefs (`prefs.json`). After that, use the CLI to change them (`set name`, `set password`, etc.), the INI values are no longer consulted for these fields.

> **The config is validated.** Every problem is named on its own `ERROR:` line,
> and the two kinds are treated differently:
>
> | Problem | Response |
> |---------|----------|
> | **Invalid value** — a GPIO pin outside `0..255`, non-numeric, or empty | **Fatal.** There is no sensible fallback for which GPIO drives the radio, so the daemon refuses to start rather than run the hardware differently from how you configured it. |
> | **Unrecognised key** — e.g. `lora_frequency` for `lora_freq` | **Warning**, key ignored, startup continues. It may be a key from a newer build, so this must not take a working repeater off the air. |
> | **File unreadable / missing** | **Warning**, built-in defaults used. The radio will then fail to start, since no pins are configured. |
>
> **Read the warnings after editing the INI.** An ignored key does not merely
> fail to apply: because the radio parameters here are *first-run defaults*, the
> built-in default is persisted to `prefs.json` on the first boot and correcting
> the INI afterwards has no effect (step 6 covers resetting prefs). Comments
> (`#`, `;`), blank lines and `[section]` headers are ignored as before.
>
> ```
> ERROR: meshcored.ini: unknown key 'lora_frequency' (ignored)
> WARNING: 1 unrecognised key(s) in /etc/meshcored/meshcored.ini ...
>
> ERROR: meshcored.ini: lora_irq_pin = '260' is not a valid GPIO pin (expected 0..255)
> FATAL: 1 invalid value(s) in /etc/meshcored/meshcored.ini ...
> ```

Key settings:

| Key | Default | Notes |
|-----|---------|-------|
| `spidev` | `/dev/spidev0.0` | SPI device node |
| `lora_gpiochip` | `gpiochip0` | Name of the `/dev/gpiochip*` device (or kernel label). `gpiochip0` is correct for Pi 3/4/Zero 2W; Pi 5 may need `gpiochip4` or `pinctrl-rp1` depending on kernel |
| `lora_irq_pin` | (none) | GPIO line number for IRQ |
| `lora_reset_pin` | (none) | GPIO line number for RESET |
| `lora_nss_pin` | (none) | GPIO line number for NSS/CS (if not handled by the SPI driver) |
| `lora_busy_pin` | (none) | GPIO line number for BUSY |
| `lora_rxen_pin` | (none) | GPIO line number for RX enable (RF switch); omit if unused |
| `lora_txen_pin` | (none) | GPIO line number for TX enable (RF switch); omit if unused |
| `lora_freq` | `869.618` | Frequency in MHz |
| `lora_bw` | `62.5` | Bandwidth in kHz |
| `lora_sf` | `8` | Spreading factor |
| `lora_cr` | `8` | Coding rate |
| `lora_tcxo` | `1.8` | TCXO voltage (V); set to `0.0` if your module has no TCXO |
| `lora_tx_power` | `22` | TX power in dBm |
| `current_limit` | `140` | Radio over-current protection limit in mA |
| `dio2_as_rf_switch` | `0` | `1` = use DIO2 to drive the TX/RX RF switch. **Required for the Waveshare Core1262** (without it the radio inits but TX/RX are dead); depends on module wiring |
| `rx_boosted_gain` | `1` | `1` enables the SX126x RX boosted-gain mode; `0` disables |
| `use_regulator_ldo` | `0` | `1` powers the radio from the LDO instead of the DC-DC converter. Only for modules built without the DC-DC inductor — on a module that has one this just costs current |
| `rx_register_patch` | `0` | `1` applies the SX126x RX-sensitivity patch (sets bit 0 of register `0x8B5`). Upstream ships it for the Heltec v4; try it if a HAT receives poorly |
| `advert_name` | `"Linux Repeater"` | Node name, first-run default only |
| `admin_password` | `"password"` | Admin password, **change this**, first-run default only |
| `lat` / `lon` | `0.0` | GPS coordinates for advertisement, first-run default only |
| `gps_device` | *(empty)* | Where to read NMEA from. A serial device path (e.g. `/dev/ttyACM0`), or `gpsd://[host][:port]` to read from a gpsd instance (default `127.0.0.1:2947`). Empty disables GPS. |
| `gps_baud` | `9600` | Baud rate for a serial `gps_device`. One of 4800/9600/19200/38400/57600/115200. Ignored when `gps_device` names gpsd — gpsd owns the port and its baud rate. |
| `gps_en_pin` | `-1` | GPIO line held HIGH to wake a GPS module that boots in standby (e.g. the L76K STANDBY line on the Waveshare LoRaWAN/GNSS HAT). `-1` = none. |

### 3. Enable SPI and GPIO access

First make sure the SPI interface is actually enabled, the radio needs a
`/dev/spidev*` node. Check with `ls /dev/spidev*`; if there is none:

```sh
# Raspberry Pi OS
sudo raspi-config          # Interface Options → SPI → Enable, then reboot

# Arch Linux ARM (no raspi-config): enable the SPI device-tree overlay
echo 'dtparam=spi=on' | sudo tee -a /boot/config.txt   # then reboot
```

> The boot config path varies by image, it is `/boot/config.txt` on most
> Raspberry Pi images but `/boot/firmware/config.txt` on some. After rebooting,
> confirm `/dev/spidev0.0` exists.
>
> **Arch Linux kernel caveat:** `dtparam=spi=on` is only honored by the Raspberry
> Pi `linux-rpi` (vendor) kernel. The mainline `linux-aarch64` kernel boots via
> U-Boot, which loads its own device tree and ignores `config.txt` overlays, so
> `/dev/spidev*` never appears regardless of `config.txt`. If SPI is missing after
> enabling it and rebooting, switch to the vendor kernel
> (`sudo pacman -S linux-rpi`, remove `linux-aarch64`) and reboot.

Then grant non-root access to the SPI and GPIO devices using the provided udev
rules, which place `/dev/spidev*` and `/dev/gpiochip*` in a `meshcore` group.
Create the group, add yourself to it, and install the rules:

```sh
sudo groupadd -f -r meshcore
sudo usermod -aG meshcore "$USER"     # log out/in afterwards for this to take effect
sudo install -m 644 variants/linux/99-meshcore.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Confirm the device nodes are now group-owned by `meshcore`:

```sh
ls -l /dev/gpiochip* /dev/spidev*    # → crw-rw---- root meshcore
```

Your current login session won't pick up the new group until you log out and
back in. To use it immediately in one shell, prefix the command with
`sg meshcore -c '…'`. (On Raspberry Pi OS you can instead use the built-in
`spi`/`gpio` groups: `sudo usermod -aG spi,gpio $USER`.)

### 4. Run

`meshcored` takes a small set of options, parsed by the ArduLinux core:

| Flag | Description |
|------|-------------|
| `-d`, `--fsdir=DIR` | Directory to use as the VFS root, where all data is persisted. Default: `~/.local/share/meshcored/default` |
| `-e`, `--erase` | Recursively wipe the VFS root, then start. This is a **full reset**: it also removes the node identity, so the node comes back with a new Repeater ID (see step 6). Never put this in the systemd unit. |
| `--usage`, `-?` / `--help` | Short usage / full option list |
| `-V`, `--version` | Print the firmware version |

**Directly** (for testing). With the udev rules in place you can run as your own
user, no `sudo`. Data is persisted under the VFS root, which defaults to the XDG
data dir; pass `--fsdir` to choose another location:

```sh
meshcored                              # VFS root: ~/.local/share/meshcored/default
meshcored --fsdir /var/lib/meshcore    # explicit location
# before re-logging in (group not yet active in this shell):
sg meshcore -c 'meshcored --fsdir /var/lib/meshcore'
```

**As a systemd service** (recommended for production). The unit runs as the
`meshcore` user and passes `--fsdir /var/lib/meshcore`:

```sh
sudo install -m 644 variants/linux/meshcored.service /etc/systemd/system/
sudo install -m 644 variants/linux/99-meshcore.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo useradd -r -g meshcore -s /sbin/nologin meshcore   # -g: reuse the existing meshcore group (its udev rules grant device access)
sudo chmod 640 /etc/meshcored/meshcored.ini
sudo chown root:meshcore /etc/meshcored/meshcored.ini
sudo systemctl daemon-reload
sudo systemctl enable --now meshcored
sudo journalctl -u meshcored -f
```

> The unit's `StateDirectory=meshcore` makes systemd create `/var/lib/meshcore`
> owned by `meshcore:meshcore` before startup, so you don't need to pre-create it.
> If you smoke-tested by running directly first, clear any stale state so the
> service first-boots with the INI defaults: `sudo rm -rf /var/lib/meshcore/*`

### 5. Talk to the running node (`meshcorectl`)

Everything the MeshCore docs describe as the "serial CLI" — `set`, `get`,
`prefs`, `advert`, `neighbors`, the `gps` commands — is reached here through
`meshcorectl`.

On an MCU the CLI is a two-way UART. On Linux the Arduino `Serial` object is
output-only (its `read()` always returns -1), so there is nothing to type into:
`meshcored` prints to stdout and listens for commands on a **Unix-domain control
socket** instead. The first writable path of these wins, and startup logs which
one it picked:

| Order | Path |
|-------|------|
| 1 | `$MESHCORED_CONTROL_SOCKET`, if set |
| 2 | `/run/meshcored/meshcored.sock` (created by the unit's `RuntimeDirectory=`) |
| 3 | `$XDG_RUNTIME_DIR/meshcored.sock` |
| 4 | `/tmp/meshcored.sock` |

The socket is mode `0660`, owned by the user running the daemon, so reaching it
means being that user, being in its group (`meshcore` for the packaged service),
or being root:

```sh
sudo meshcorectl                    # interactive REPL
sg meshcore -c meshcorectl          # or via the group
```

Three ways to drive it:

```sh
meshcorectl                         # REPL: line editing, history, Tab completion
meshcorectl set name my-repeater    # one-shot: send one command, print reply, exit
printf 'prefs\nneighbors\n' | meshcorectl    # piped: one command per line
```

The REPL is self-contained (no `socat`/`rlwrap` needed): arrow-key editing,
Ctrl-R search, Tab completion of known commands, and history in
`~/.meshcorectl_history`. `MESHCORED_CONTROL_SOCKET` overrides the path for the
client exactly as it does for the daemon, which is how you reach a node that is
not the packaged service.

Only **one client at a time** is served; a second connection waits until the
first disconnects. If you prefer raw tools, `sudo socat - UNIX-CONNECT:/run/meshcored/meshcored.sock`
works too.

**Running in a foreground terminal** — `meshcored` with no service behind it —
you can skip the socket entirely and type commands straight into its stdin; the
console reads the terminal directly (raw mode, echoing as you type). A connected
socket client always takes priority over stdin while it is attached.

### Idle CPU usage

`meshcored` blocks in `poll()` between events instead of spinning: expect
**well under 1% CPU at idle with edge detection**, and roughly **1-3%** in the
polling fallback described below (both a long way from the ~100% of one core
this variant used to burn continuously). Startup logs which mode a bound LoRa
IRQ pin ended up in:

```
LoRa IRQ pin 25 bound with edge detection: yes
```

`NO (polling fallback)` instead of `yes` means the kernel/libgpiod on this
device couldn't set up edge events for that line, so the daemon falls back to
a 1 ms poll timeout rather than the normal `IDLE_MAX_WAIT_MS` (50 ms). This is still fully
functional — packet RX/TX is unaffected — it just costs more CPU because the
daemon wakes far more often with nothing to do.

If edge detection was available at startup, watch for this later in the logs:

```
EventGPIOPin(GPIO25): edge detection lost on mode change (...); falling back to timeout polling
```

Seeing this means the daemon has silently degraded from edge-driven waits to
timeout polling for the rest of the run (higher CPU, still correct). It is
not expected in normal operation; if you see it, it is worth reporting along
with the `(...)` errno text and your libgpiod/kernel version.

### Channel Activity Detection (`set cad on`)

Off by default. When enabled, the radio runs a hardware CAD scan immediately
before each transmit and defers if it detects a LoRa signal.

```
set cad on      # or: set cad off
get cad
```

The change takes effect within 2 seconds — no restart needed.

CAD complements `int.thresh` rather than replacing it, and either, both, or
neither may be active:

- **`int.thresh`** compares RSSI against the measured noise floor. It sees any
  energy, including non-LoRa interference, but cannot see a signal below the
  noise floor.
- **`cad`** correlates against the LoRa preamble, so it detects a real LoRa
  transmission *below* the noise floor where RSSI is blind — but ignores
  non-LoRa energy entirely.

Enabling `int.thresh` alongside `cad` also reduces how often the scan runs: the
RSSI check is evaluated first, and a busy verdict there skips the scan.

**On Linux the scan does not spin.** RadioLib's own `scanChannel()` busy-waits
on the DIO1 line with no timeout, which would burn a core for the length of
every scan and — because `EventGPIOPin` deliberately reads LOW when a GPIO read
fails — would turn a degraded line into a hung daemon. This variant instead
sleeps on the IRQ edge descriptor with a deadline derived from the active SF and
bandwidth, then reads the result over SPI regardless of whether the line
reported. A line that stops reporting costs latency and a log line, not
correctness.

**Cost at high spreading factors.** A CAD scan takes about four symbol times,
which grows quickly with SF: roughly 20 ms at SF8/62.5 kHz, but around 265 ms at
SF12/62.5 kHz. Transmit attempts retry every 200 ms while the channel reads
busy, so at SF12 a node holding a queued packet on a contended channel spends
over half of that window inside a scan — and the modem is in standby, **not
listening**, for the duration. That is inherent to CAD-before-TX rather than
specific to this implementation, but it is worth knowing before enabling it on a
high-SF preset.

### 6. Reconfiguring after first run

Node name, password, and location can be changed via the serial CLI after first boot:

```
set name <name>
set password <password>
set lat <lat>
set lon <lon>
```

### GPS

With `gps_device` configured, the standard MeshCore GPS commands work over the
control CLI (`meshcorectl`):

- `gps` — status: on/off, active/deactivated, fix/no-fix, satellite count
- `gps on` / `gps off` — enable/disable GPS reading and location telemetry
- `gps sync` — force a time re-sync from GPS
- `gps setloc` — save the current GPS fix as the node's advertised location
- `gps advert none|prefs|share` — control whether location is advertised
- `gps interval [seconds]` — seconds between location reads (0 = default, 1 s;
  max 86400). Bare `gps interval` reports the stored value. This also throttles
  the two `lat …` debug lines the firmware prints on every read, which at the
  1 s default dominate the journal on a node with a fix.

The daemon needs read access to `gps_device`. USB GPS units are usually owned
by root or the `dialout` group; the systemd service already runs with the
privileges it needs for SPI/GPIO. For an unprivileged run, add the user to the
device's group or install a udev rule granting access.

Some GPS modules boot into standby and stay silent until an enable/standby line
is driven high. The L76K on the Waveshare LoRaWAN/GNSS HAT is one such module —
set `gps_en_pin` to that GPIO (STANDBY is on GPIO 4 for that HAT) and the daemon
holds it high on start so the module wakes and streams NMEA.

#### Reading GPS from gpsd

By default the daemon opens the GPS device itself and holds it for its whole
life, which locks out anything else that wants the receiver — in particular
`gpsd`, and through it `chrony`. On a Linux node that matters more than it does
on an MCU, because the node's clock is the whole host's clock, and a node with
no network and no RTC otherwise boots with a bogus one.

Point `gps_device` at gpsd instead and the device becomes gpsd's:

```ini
gps_device = gpsd://        # 127.0.0.1:2947
gps_en_pin = 4              # still needed; see below
```

Then, on the host:

```sh
sudo apt install gpsd gpsd-clients chrony
```

`/etc/default/gpsd`:

```sh
DEVICES="/dev/ttyS0"
GPSD_OPTIONS="-n"
```

`-n` is required. chrony's SHM refclock is not a gpsd socket client, so without
it gpsd stops reading the receiver whenever meshcored disconnects, and chrony
sees nothing.

`/etc/chrony/chrony.conf`:

```
refclock SHM 0 refid GPS offset 0.0 delay 0.2
```

Verify with `chronyc sources` (a `GPS` line) and `meshcorectl gps` (fix and
satellite count, as before).

The daemon reconnects to gpsd on its own, with backoff, so start order does not
matter and restarting gpsd underneath a running node is safe. It also drops the
connection while GPS is switched off and reconnects on `gps on`, so gpsd is
never left holding a client that has stopped reading.

With a gpsd source the daemon no longer tries to set the system clock at all —
chrony owns it. That is deliberate beyond tidiness: the other caller of the same
path is `clock sync` / `time <epoch>`, which carries a timestamp from a remote
mesh peer, and on a general-purpose host that should never be able to move the
clock.

#### PPS is not available on the Waveshare LoRaWAN/GNSS HAT

Investigated and closed — do not re-investigate. The L76K does emit a
pulse-per-second signal, but on the SX126X XXXM LoRaWAN/GNSS HAT it is **not
routed to a Raspberry Pi header GPIO**. The schematic takes L76K pin 5 (`1PPS`,
net `PPS`) through R19 to indicator LED `L_PPS1` and no further; no `PPS` net
appears in the Raspberry Interface block. Confirmed empirically: 20 s of
`gpiomon` across all 14 free header GPIOs (2, 3, 5, 6, 12, 13, 17, 19, 22–27)
with a 21-satellite fix produced zero edges.

So there is no stratum-1 PPS refclock to be had on this board, and NMEA via
gpsd (above) is the only route to GNSS timekeeping. On a HAT that *does* route
PPS, add `dtoverlay=pps-gpio,gpiopin=<N>` and
`refclock PPS /dev/pps0 refid PPS lock NMEA` — but PPS is a precision layer on
top of gpsd, never a replacement for it, because it says when a second begins
and not which second it is.

Two related notes about this HAT, both easy to get wrong:

- `gps_en_pin = 4` is load-bearing. R13 (marked `NC/0R`) is fitted, so driving
  GPIO 4 low stops NMEA dead. The pin reads high when undriven only because of
  the SoC's own default pull-up on GPIO 0–8, which is not something to rely on.
- Switch **S1** drives the same transistor in parallel with GPIO 4. If the GPS
  will not sleep, that switch is why. `FORCE_ON` is pushbutton K1, not a GPIO —
  GPIO 17 is unconnected here, despite `DEV_FORCE 17` in Waveshare's sample code
  for the standalone L76X module.

There are two levels of reset:

**Prefs only**, keeps the node identity (same Repeater ID). Delete the saved prefs so the INI first-run defaults are re-applied on the next boot:

```sh
sudo rm -f /var/lib/meshcore/prefs.json /var/lib/meshcore/com_prefs
sudo systemctl restart meshcored
```

> Prefs used to live in a binary `com_prefs` file; upstream moved them to a JSON
> `prefs.json`. A node upgraded from an older build migrates itself on the first
> boot (`com_prefs` is read once, then rewritten as `prefs.json`) and the old
> file is left in place, so a prefs reset has to remove both.

**Full reset**, also discards the identity, so the node returns with a **new** Repeater ID. This wipes the whole VFS root. The built-in `-e`/`--erase` flag does exactly that before starting, but for the managed service just clear the directory while it is stopped (keep `--erase` out of the unit, see the note below):

```sh
sudo systemctl stop meshcored
sudo rm -rf /var/lib/meshcore/*
sudo systemctl start meshcored
```

> When running **directly** (not under systemd), `meshcored --fsdir /var/lib/meshcore --erase` is the equivalent one-shot full reset. Do **not** add `--erase` to the service unit: systemd re-runs `ExecStart` on every restart, so it would wipe the filesystem and regenerate the identity each time. (The firmware's own `reboot()` strips `--erase` to avoid self-wiping, but that protection does not extend to a systemd restart.)

> **Note:** LoRa radio parameters (`lora_freq`, `lora_bw`, `lora_sf`, `lora_cr`, `lora_tx_power`) are also first-run defaults. After first boot they are saved in `prefs.json` and the INI values are no longer read for those fields. To apply a changed radio parameter, use the CLI (`set freq`, `set sf`, etc.) or reset prefs as above.

## Known Gaps / TODO

- **Config path is hardcoded**, meshcored always loads `/etc/meshcored/meshcored.ini`; there is no flag to point it elsewhere. (The data *path* is separate and configurable: it is the ArduLinux VFS root, set with `--fsdir`.)
- **Only repeater firmware**, there is no `linux_companion` target yet; companion radio support (BLE/serial interface to a phone app) is not implemented for Linux.
- **Serial `erase` command is a no-op**, `formatFileSystem()` returns `false` on Linux, so the interactive serial `erase` command reports failure. To wipe the filesystem, use the `--erase` *startup* flag (or clear the VFS dir) instead, see step 6.
- **No power management**, `board.sleep()` is a no-op; the power-saving loop in `main.cpp` never actually sleeps.
- **Upstream-sync fragility**, the radio wrapper (`LinuxSX1262Wrapper`) implements the `RadioLibWrapper` interface by hand, so it can drift from upstream in two ways: a new **pure-virtual** method breaks the Linux build (e.g. `setParams()`), and a new **virtual-with-default** method silently no-ops on Linux until overridden (e.g. `set`/`getRxBoostedGainMode()`, which reported and applied the wrong state until added). Mirror `CustomSX1262Wrapper` when syncing.
- **libgpiod v2 is compile-verified only**, `EventGPIOPin`'s v2 code path (Debian trixie and newer) builds cleanly in CI/`build-docker.sh`, but it has never been exercised at runtime against real hardware — all runtime verification to date has been on libgpiod v1 (Debian bookworm). Treat the v2 path as unproven until someone runs it on a Pi.
