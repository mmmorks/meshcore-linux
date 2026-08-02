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
| `meshcored.ini.waveshare` | RPi 3/4/5 + Waveshare SX1262 LoRa HAT (also the LoRaWAN/GNSS variant — its GNSS lines are commented in the template) |

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
| `gps_en_pin` | `-1` | GPIO line held HIGH to wake a GPS module that boots in standby (e.g. the L76K STANDBY line on the Waveshare LoRaWAN/GNSS HAT). `-1` = none. **Leave unset with a `gpsd://` source** and hold the line from the host instead — see [Keeping GNSS up without meshcored](#keeping-gnss-up-without-meshcored). |

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

With a **serial** `gps_device` the daemon needs read access to it. USB GPS units
are usually owned by root or the `dialout` group; the systemd service already
runs with the privileges it needs for SPI/GPIO. For an unprivileged run, add the
user to the device's group or install a udev rule granting access.

A **`gpsd://`** source needs none of that — the daemon talks to a local socket
and never opens the device, so it does not need `dialout` (or any other) access
to the serial port. gpsd holds it instead.

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
DEVICES="/dev/ttyAMA0"      # /dev/ttyS0 on a Pi whose PL011 is not the GPS UART
GPSD_OPTIONS="-n"
```

`-n` is required. chrony's SHM refclock is not a gpsd socket client, so without
it gpsd stops reading the receiver whenever meshcored disconnects, and chrony
sees nothing.

`-n` is necessary but **not sufficient** — Debian leaves gpsd socket-activated,
so it does not run at all until a client connects. See [Keeping GNSS up without
meshcored](#keeping-gnss-up-without-meshcored).

`/etc/chrony/chrony.conf`:

```
refclock SHM 0 refid GPS offset 0.307 delay 0.2
```

The `offset` is not optional and it is board-specific. NMEA without PPS arrives
some way after the second it describes, so the refclock reads consistently late;
left uncorrected, chrony marks it a **falseticker** (`#x` in `chronyc sources`)
and ignores it entirely. `0.307` is what this HAT measured — start at `0.0`,
read the steady-state figure from `chronyc sources`, and put it in as a positive
number:

```sh
chronyc sources        # e.g. "#x GPS  ...  +307ms[ +307ms]"  ->  offset 0.307
```

Note the sign: a *positive* offset cancels a positive reported error. Getting it
backwards doubles the error instead.

The value is systematic rather than noise *within* a session — std dev was ~5 ms
over one sample — but it is not a constant of the board. It is dominated by how
long the receiver takes to start its sentence burst plus how long that burst
takes to clock out, so it moves with the sentence set and the baud rate. On this
HAT it has been seen at 0.193 s, 0.307 s and 0.384 s at different times without
gpsd being touched. Expect to re-check it, and see the PPS section below, which
inverts this tuning advice once a pulse-per-second signal is available.

Verify with `chronyc sources` (a `GPS` line that is no longer `#x`) and
`meshcorectl gps` (fix and satellite count, as before).

With a network present chrony will usually still prefer a good NTP server —
NMEA-only GPS is worth about ±100 ms against a stratum-1 peer's ±30 ms, so being
listed as `#-` rather than `#*` is correct, not broken. To prove GPS really can
hold the clock alone, take the network sources away:

```sh
sudo chronyc offline    # refclocks are unaffected
# ~4 minutes later, once the NTP peers age out:
chronyc tracking        # Reference ID : 47505300 (GPS), Stratum : 1
sudo chronyc online
```

The daemon reconnects to gpsd on its own, with backoff, so start order does not
matter and restarting gpsd underneath a running node is safe.

`gps off` stops the reads rather than closing the socket, so gpsd is left
holding a client that has stopped draining until it drops it itself. What the
daemon does guarantee is that no stale NMEA is acted on: the first read after a
gap of more than 5 s closes the old socket and reconnects, so `gps on` starts
from live sentences instead of replaying whatever backed up while it was off.

With a gpsd source the daemon no longer tries to set the system clock at all —
chrony owns it. That is deliberate beyond tidiness: the other caller of the same
path is `clock sync` / `time <epoch>`, which carries a timestamp from a remote
mesh peer, and on a general-purpose host that should never be able to move the
clock.

#### PPS on the Waveshare LoRaWAN/GNSS HAT (needs a hardware mod)

The L76K emits a pulse-per-second signal, but the HAT does not route it to a
Raspberry Pi header GPIO. L76K pin 4 (`1PPS`, net `PPS`) goes through R19 (510R)
to indicator LED `L_PPS1` and no further; no `PPS` net reaches the Raspberry
Interface block, and no unpopulated jumper or DNF resistor would route it. One
wire fixes that.

It is worth soldering: NMEA-only is worth about ±100 ms, while kernel PPS on a
Pi settles under a microsecond — bounded by interrupt latency, not by the
receiver, whose PPS is spec'd in the tens of nanoseconds.

PPS is a precision layer on top of gpsd, never a replacement for it, because it
says when a second begins and not which second it is. Keep the NMEA refclock.

##### Pins the HAT already uses

Waveshare colour-codes the net labels in the Raspberry Interface block: maroon
nets are connected on the HAT, green ones are pass-through and connect to
nothing. Two of them are easy to misread as free:

- `DIO4` is maroon at **BCM 6** (P1 pin 31). The HAT drives it as one of the two
  `CTRL` inputs of U3 (PE4259 RF switch); the other is the SX1262's own `DIO2`.
  The SX126X module has no DIO4 pin of its own.
- `Pi_3V3` (pin 1) and `3V3_Pi` (pin 17) are green and dangle — **the HAT takes
  only 5 V from the Pi** and regulates its own 3V3 with AMS-1 (AMS1117-3.3, fed
  from P1 pins 2/4). The pads are unconnected on the HAT, but the Pi still
  drives its rail onto them.

A consequence of the second point: the PPS high level is the AMS1117 output, not
the Pi's rail. Both are nominally 3.3 V and grounds are common, so no level
shifting is needed — but the two rails are independently derived, which is one
more reason for the series resistor below.

##### Tapping the signal

The `PPS` net's only accessible copper is R19 and the `L_PPS1` anode.

**Option 1 — R19's Q1-side pad.** Full 3.3 V CMOS swing, LED keeps working. The
layout puts R19 down beside Q1 while `L_PPS1` sits at the top-right board edge,
so the short trace is Q1 pin 4 → R19 and the long one is R19 → LED. Identify the
right pad by continuity to Q1 pin 4 (fourth castellated pad from the pin-1/GND
corner), or with a scope: the `PPS`-side pad swings 0→3.3 V, the LED-side pad
clamps at the LED's ~1.9 V forward drop. **Do not tap the LED side** — 1.9 V is
marginal against the Pi's V_IH. Fit a 220–330R resistor in series with the tap.

**Option 2 — remove `L_PPS1` and tap its anode pad.** Mechanically easier:
bigger pad, at the board edge next to P5's through-holes for strain relief, and
R19's 510R becomes the series protection for free. With the LED gone that node
swings the full 0→3.3 V. Cost is losing the indicator.

**Option 2 is what this board runs**, and it works: `ppstest` shows ~±3 µs of
inter-pulse jitter and chrony settles at a single-digit-microsecond RMS offset
on it. Two consequences follow from that choice rather than from PPS in general.
R19 is already the series resistor, so do not add another. And with `L_PPS1`
gone there is no longer a blinking indicator to glance at — the checks under
"Checking whether the overlay actually loaded" below are the only way to see
whether pulses are arriving.

Either way R19 is an 0402. Use 30 AWG wire and glue it down — the pad will lift
the first time that wire is flexed.

##### Landing it on the Pi

Land the other end on a free P1 pad on the HAT's own underside, so the mod stays
self-contained and connects through the header when the HAT is seated.

**BCM 26 (P1 pin 37)** is the recommendation: green in the schematic, unused by
`meshcored.ini` in either pin set, and GND is immediately adjacent at pin 39 for
the return. BCM 19 (35), BCM 12 (32), BCM 25 (22) and BCM 27 (13) also work.

Pins to stay off, and why:

| Pin(s) | Reason |
| --- | --- |
| 1, 17 | Pi 3V3 supply. Unconnected on the HAT, still driven by the Pi — a tap here shorts `PPS` into the Pi's rail. |
| 27, 28 | `ID_SD`/`ID_SC`, reserved for HAT EEPROM ID detection at boot. |
| 3, 5 | BCM 2/3. Usable, but they carry the Pi's 1.8K I²C pull-ups. |
| 7 | BCM 4 — GPS `STANDBY`. |
| 8, 10 | BCM 14/15 — GPS UART. |
| 19, 21, 23 | SPI to the SX1262. |
| 31 | BCM 6 — `DIO4`, RF switch control. |
| 12, 36, 38, 40 | `RST`, `DIO1`, `BUSY`, `CS`. |

Note that both conventional `pps-gpio` pins are already taken on this HAT: GPIO
18 is `RST` and GPIO 4 is `STANDBY`. Check the current `lora_irq_pin` and
`lora_reset_pin` in `meshcored.ini` before committing to a pin, too — those are
free on the HAT but not necessarily free in your config.

##### Loading the overlay

`/boot/firmware/config.txt`:

```
dtoverlay=pps-gpio,gpiopin=26,pull=down
```

The pull-down matters: whenever the L76K is in standby — or simply not powered
yet — it stops driving its pin 4, and the Pi's input would otherwise float.
`pull` is supported by the overlay on current
Raspberry Pi OS but is not in every version — `dtoverlay -h pps-gpio` lists what
yours takes, and an external 10K to GND does the same job. Leave
`assert_falling_edge` at its default; the L76K marks the top of the second with
a **rising** edge, 100 ms wide.

##### Checking whether the overlay actually loaded

**The kernel prints nothing on success.** There is no `pps-gpio` line in
`dmesg` — the driver logs no probe message on a current kernel, so `dmesg | grep
pps` showing only the `pps_core` and `pps_ldisc` banners means nothing is wrong.
`lsmod` is no better: `pps_gpio` sits at refcount 0 even when bound, because
that column counts dependent modules, not device bindings. Chasing either of
those absences is a dead end.

The three checks that do carry information:

```sh
cat /sys/class/pps/pps*/name          # want one reading pps@1a  (0x1a = GPIO 26)
gpioinfo | grep -w 26                 # want:  "GPIO26"  "pps@1a"  input  [used]
ls -l /sys/bus/platform/drivers/pps-gpio/   # want a symlink named pps@1a
```

Two traps once those pass:

- **gpsd creates a second, permanently silent PPS device.** It attaches a PPS
  line discipline to the serial port, which shows up as another `/dev/ppsN`
  named `serial0`. Nothing drives DCD on that port, so its assert counter stays
  at zero forever and `ppstest` on it hangs in silence. Check the `name` files
  above rather than assuming `/dev/pps0` is the GPIO one.
- **The numbering is not guaranteed.** The GPIO source is `pps0` only because
  the platform driver binds at boot, before gpsd starts. For a stable name, drop
  a udev rule in `/etc/udev/rules.d/10-pps-gpio.rules` and use `/dev/pps-gpio`
  below:

  ```
  SUBSYSTEM=="pps", ATTR{name}=="pps@1a.-1", SYMLINK+="pps-gpio"
  ```

Then watch actual pulses. `/dev/pps*` is mode 600 root:root, so this needs
`sudo` — chronyd is unaffected, as it opens refclocks before dropping
privileges:

```sh
sudo apt install pps-tools
sudo ppstest /dev/pps0     # want ~1.000000 s between assert events
```

Nothing appears until the receiver has a fix — the L76K gates PPS on that, so a
silent `/dev/pps0` under a cold start is expected, not a wiring fault.

##### Handing it to chrony

`/etc/chrony/chrony.conf`, amending the single `SHM 0` line from above:

```
refclock SHM 0 refid GPS offset 0.307 delay 0.2 noselect
refclock PPS /dev/pps-gpio refid PPS lock GPS prefer
```

`/dev/pps-gpio` rather than `/dev/pps0`: install the udev rule from the previous
section first. The numbering is not guaranteed, and pointing this line at gpsd's
silent serial PPS device instead of the GPIO one fails quietly — chrony simply
never gets a sample.

`noselect` keeps NMEA labelling seconds without ever disciplining the clock
itself. As a side effect the NMEA line stops being reported as a falseticker
(`#x`) and starts showing as `#?`, which for a `noselect` source is the normal,
healthy state rather than a fault.

**Adding PPS changes what the `offset` is for, and inverts how to tune it.** It
is no longer a calibration — NMEA is `noselect`, so its value has no effect on
the clock at all. Its only remaining job is to keep NMEA inside ±0.5 s, the
nearest-second rounding limit, so `lock GPS` pairs each pulse with the right
second.

For that job a **larger offset is safer than a smaller one**, which is the
opposite of the tuning advice above. The delay is physically one-sided: NMEA can
only arrive *after* the second it describes, never before. So raising the offset
costs nothing on the low side and buys headroom on the high side, where all the
risk lives — the delay can grow by hundreds of milliseconds. Turning NMEA
sentences back on moves it by that much on its own: see the measured table under
"Satellite counts, and tuning what the receiver sends" below, where a `GSA` rate
change alone accounts for ~197 ms.

This is not hypothetical. On this node the raw delay was measured at 0.307 s,
then 0.193 s an hour later, then 0.384 s twenty minutes after that, without
anyone touching gpsd. With `offset 0.307` the reported error stayed inside
±120 ms throughout. Had the offset been "corrected" to zero, that last excursion
would have left only 116 ms of margin.

So once PPS is running, leave the offset where it is and treat a non-zero
reading on the `GPS` line as normal. Comment the value in `chrony.conf` — the
next person to read it will otherwise assume it is a live calibration and tune
it toward zero, which is now exactly wrong.

Give it a couple of minutes to accumulate samples. `chronyc sources` should end
up with `#* PPS` selected and the NTP servers demoted to `^-`, and
`chronyc tracking` should report `Stratum : 1` and `Reference ID : 50505300
(PPS)`.

Measured on this HAT after the mod: raw inter-pulse jitter at `ppstest` is about
±3 µs, which is Pi interrupt latency rather than the receiver; chrony's filtered
result settles around **±400 ns** dispersion and a sub-microsecond RMS offset.
Against the ±100 ms of NMEA-only, that is roughly a five-order-of-magnitude
improvement.

#### Keeping GNSS up without meshcored

Once chrony takes its time from the receiver, the dependency runs the wrong way
round: the *host's clock* now rests on a stack that, by default, only works
while the mesh daemon happens to be running. Two things cause that, and neither
announces itself — a node in this state looks perfect until meshcored stops.

Both are worth fixing even without PPS, and become more so with it, because
`lock GPS` means PPS cannot number its own seconds. No gpsd is not "PPS without
NMEA labelling", it is **no stratum 1 at all**.

**1. gpsd does not start until something connects to it.** Debian ships gpsd
socket-activated: `gpsd.socket` is enabled and `gpsd.service` is not, so the
daemon is spawned by the first client on port 2947. On a node where meshcored is
the only gpsd client, that makes the entire GNSS chain — device, SHM, chrony's
stratum 1 — conditional on the mesh daemon connecting. A meshcored held down by
a bad `meshcored.ini` takes the host's clock with it.

`-n` does not cover this. It keeps gpsd reading *after* the last client leaves;
it says nothing about starting before the first one arrives.

```sh
systemctl is-enabled gpsd.service gpsd.socket   # the trap: "disabled" / "enabled"
sudo systemctl enable --now gpsd.service        # unit's [Install] pulls gpsd.socket in via Also=
```

Then make a crash self-healing, since after this there is no longer a client
whose reconnect would restart it:

```ini
# /etc/systemd/system/gpsd.service.d/resilience.conf
[Service]
Restart=on-failure
RestartSec=5
```

**2. The GPS enable pin is left unowned when meshcored exits.** `gps_en_pin` is
held for the daemon's lifetime and no longer.

This one is weaker than it first looks, and the measurement is worth recording
because the obvious guess is wrong. Stopping meshcored does **not** drop the
receiver: on bookworm with libgpiod 1.6.3 the pad keeps its last state, and
`gpioinfo` reports the line as

```
line   4:      "GPIO4"       unused  output  active-high
```

— unowned, but still driving high, with the fix intact and chrony still at
stratum 1. So this is not an outage waiting to happen.

What it is, is undeclared. What holds the L76K awake is a pull-up before
meshcored's first run and a leftover output level after its last, neither of
which is configuration, and neither of which any layer promises to keep. An
unowned line is also unprotected — nothing stops another consumer claiming it
and driving it low. On a host whose clock rests on that receiver, the pin should
be owned on purpose.

Hand the line to the kernel instead, in `/boot/firmware/config.txt`:

```
dtoverlay=gpio-hog,gpio=4
```

and leave `gps_en_pin` unset in `meshcored.ini`. A hogged GPIO is driven for the
whole boot and, in the overlay's own words, "not available to other drivers or
for gpioset/gpioget" — so nothing can take it and nothing can release it.

> **Pass `gpio=4` explicitly.** The overlay's default is **26**, which on this
> board is the PPS input. `dtoverlay=gpio-hog` bare would hog the pulse line and
> break the thing it was added to protect.

Not `gpio=4=op,dh`. That firmware directive sets the pad before the kernel
starts, which reads like the same thing and is not: it sets an initial state, it
does not take ownership. The line stays free for anything to claim, drive and
drop — which is precisely the situation being fixed. Only the hog makes the pin
state both declared and defended.

With the hog in place, meshcored's own claim fails and it says so —

```
WARNING: could not claim GPS enable pin 4; GPS may stay asleep
         (expected if the host holds this line, e.g. a GPIO hog)
```

— which is the correct outcome, not a fault. Setting `gps_en_pin` alongside a
`gpsd://` device also draws a warning pointing back here.

##### Proving it

Two different failures, so two checks. That GNSS survives the mesh daemon:

```sh
sudo systemctl stop meshcored
sleep 30
chronyc tracking      # want: Reference ID 50505300 (PPS), Stratum 1, unchanged
cgps -s               # want: still a fix -- gpsd is holding the receiver alone
sudo systemctl start meshcored
```

And that it does not need one to *begin* with, which is the socket-activation
case and only shows itself across a boot:

```sh
systemctl is-enabled gpsd.service     # want: enabled (not "disabled" + an enabled socket)
sudo journalctl -b -u gpsd | head -3  # want: started at boot, before any client connected
```

The second is the one that bites. gpsd left to socket activation looks identical
to a correctly configured node for as long as meshcored keeps connecting to it.

#### Satellite counts, and tuning what the receiver sends

Out of the box `cgps` shows an empty satellite table and `meshcorectl gps`
reports zero satellites, even with a good fix. gpsd drives this receiver with
its **u-blox binary driver** and sets its own message list on every device
activation, which turns NMEA off entirely. Without NMEA `GSV`/`GSA` gpsd never
builds a `SKY` object, and the UBX routes to the same data are unavailable here
— the receiver ACKs `NAV-SVINFO` (01,30) and `NAV-SAT` (01,35) and then emits
neither.

**The receiver is not a u-blox.** It is an **Allystar URANUS5** (`$PCAS06,0`
answers `$GPTXT,01,01,02,SW=URANUS5,V5.3.0.0`) presenting a partial u-blox
emulation: enough NAV output and `CFG-MSG` for gpsd's driver to bind, and a NAK
for nearly everything else — `CFG-PRT`, `CFG-NAV5`, `CFG-GNSS`, `CFG-SBAS`,
`CFG-TMODE2/3`, `CFG-RATE`, `CFG-ANT`, `MON-VER`, `MON-HW`. gpsd is not
misdetecting it; the framing really is UBX (`b5 62`). This matters because the
NAKs make the receiver look unconfigurable when it is not — its **native
Allystar `$PCAS` command set is reachable over plain NMEA and works**, and that
is the only way to reach the baud rate, since `CFG-PRT` is ignored.

Install both files: [`gnss-set-baud`](gnss-set-baud), which sets the port speed
before gpsd opens it, and [`gpsd-gnss-tuning.conf`](gpsd-gnss-tuning.conf),
which wires that in and re-enables the two sentences on every start (`CFG-MSG`
is RAM-only, so it cannot be saved to the receiver):

```sh
sudo install -m 755 gnss-set-baud /usr/local/sbin/gnss-set-baud
sudo mkdir -p /etc/systemd/system/gpsd.service.d
sudo install -m 644 gpsd-gnss-tuning.conf \
    /etc/systemd/system/gpsd.service.d/gnss-tuning.conf
sudo systemctl daemon-reload && sudo systemctl restart gpsd
```

The `$PCAS01,5` speed setting is volatile — it survives a gpsd restart but not
a power cycle — so `ExecStartPre` re-applies it on every start rather than
persisting it with `$PCAS00`, whose success cannot be confirmed without
physically power-cycling the board. The script sends at both 9600 and 115200
because there is no way to ask which speed the receiver is currently listening
at; the wrong one is line noise it discards. Verified by forcing the receiver
back to 9600 and restarting gpsd: it recovers to 115200 with a fix.

Do not expect gpsd's speed hunting to cover a mismatch — it was observed sitting
at `bps=9600 driver=None` against a 115200 receiver and never converging.

> **gpsd has an Allystar driver, but you cannot use it.** `driver_allystar.c`
> arrived in gpsd **3.26** (11 May 2025); bookworm ships 3.22 and trixie 3.25,
> and there is no gpsd in trixie-backports — so a distro upgrade does not reach
> it either. It would not help regardless: its `msg_nav_svinfo()` parses the
> satellite message and then deliberately returns 0, commented *"no way to know
> if a sat used, or unhealthy"*. Binary satellite data is a dead end on this
> part from both directions, which is why NMEA `GSV`/`GSA` is the answer.

**Measure the receiver, not the client stream.** This distinction cost real
debugging time. gpsd *synthesizes* NMEA for its clients from the binary data, so
what meshcored reads is not what the receiver sent — on this node the client
stream was measured *larger* than the device stream (608 B/s vs 445 B/s), which
is only possible if gpsd is generating it. Sentences the receiver never sends
(`GGA`, `RMC`, `ZDA`, and the giveaway `GBS`) appear there regardless. Use
`gpspipe -R` for the device and `gpspipe -r` for the client:

```sh
gpspipe -R -x 20 > /tmp/dev.bin   # literal receiver bytes
gpspipe -r -x 20 > /tmp/cli.txt   # what a gpsd client sees
```

Tuning against the client stream produces conclusions that are exactly
backwards — a `CFG-MSG` disabling a sentence gets ACKed and appears to have been
ignored, when in truth the sentence was already off and the one still arriving
was gpsd's own.

**The sentence rates and the baud rate are one decision, not two.** Everything
shares a single UART, and gpsd's fix — the sample chrony reads from SHM 0 —
comes from UBX `NAV-TIMEGPS` in that same stream. NMEA therefore does not
compete with chrony for a parser, it competes for the wire: an epoch carrying a
`GSA`/`GSV` burst delivers its `NAV-TIMEGPS` late. Measured here, with chrony's
`GPS` refclock offset as the readout:

| baud | `GSA` / `GSV` | UART load | mean offset | sd | spread |
|---|---|---|---|---|---|
| 9600 | 1 / 5 | 436 B/s (45%) | +188.5 ms | 21.3 ms | 79 ms |
| 9600 | 5 / 5 | 315 B/s (33%) | −16.3 ms | 10.0 ms | 27 ms |
| 9600 | 2 / 2 | 497 B/s (52%) | +199.1 ms | 147.2 ms | 369 ms |
| 9600 | 1 / 1 | — | **+523 ms** | — | — |
| 115200 | 5 / 5 | 321 B/s (2.8%) | −88.3 ms | 6.8 ms | 20 ms |
| **115200** | **1 / 1** | **849 B/s (7.4%)** | **−29 to −97 ms** | **6.4–9.9 ms** | **16–26 ms** |

Read the 9600 rows first, because they are the trap. `GSA` at rate 1 puts a
burst in front of *every* `NAV-TIMEGPS` and adds a flat ~197 ms — easy to
mistake for a fixed calibration constant and bury in the `offset` above. But the
fix is not "make them both faster" either: what governs the result is the
**fraction of epochs carrying a burst**, because chrony's median filter is what
rejects them. At rate 5 one epoch in five is congested and is treated as an
outlier. At rate 2 half are, the filter can no longer tell which population is
real, the samples go bimodal (+310 ms and −48 ms here) and sd jumps 16×. That
369 ms spread is close to the 0.4 s `lock GPS` needs, i.e. close to losing PPS
lock. Staggering the rates is the same failure in slow motion — keep the bursts
coincident. At rate 1 every epoch is congested, so it degenerates into a flat
+523 ms offset.

Raising the baud rate dissolves the whole problem: the burst takes a twelfth as
long to clock out, and rate 1 — the *worst* setting at 9600 — becomes the best
available, with a one-second satellite view at 7% of the wire. The two 115200
figures come from runs with different settle times and n=6, so treat the sd
range as noise rather than evidence that rate 1 beats rate 5; the point is that
rate 1 is now *sustainable*, which at 9600 it was not.

The mean offset wanders by ~70 ms between runs at either speed, which is the
same effect documented under PPS above and is why the `offset` needs margin
rather than centring. **At 115200 the residual sits near −100 ms**, so if you
change the baud rate, re-check that the `GPS` line still has room inside ±0.4 s;
`offset 0.307` was calibrated at 9600 and now over-corrects by about that much.

What is *not* worth spending the freed bandwidth on: the constellation set is
fixed at GPS + GLONASS + BeiDou (`CFG-GNSS` NAKs, and Allystar's `$PCAS04` has
no Galileo option), and a faster navigation rate does not reach chrony, which
takes its time from PPS. Timing is the scarce resource here; bandwidth is not.

#### Two HAT quirks that are easy to get wrong

- GPIO 4 is load-bearing. R13 (marked `NC/0R`) is fitted, so driving GPIO 4 low
  stops NMEA dead. The pin reads high when undriven only because of the SoC's
  own default pull-up on GPIO 0–8, which is not something to rely on. Hold it
  deliberately: `gps_en_pin = 4` does it for as long as meshcored runs, and
  `dtoverlay=gpio-hog,gpio=4` does it for the whole boot — with a `gpsd://`
  source, use the hog and leave `gps_en_pin` unset ([why](#keeping-gnss-up-without-meshcored)).
- Switch **S1** drives the same transistor in parallel with GPIO 4. If the GPS
  will not sleep, that switch is why. `FORCE_ON` is pushbutton K1, not a GPIO —
  GPIO 17 is unconnected here, despite `DEV_FORCE 17` in Waveshare's sample code
  for the standalone L76X module.

### Resetting a node

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
- **A `gpsd://` host cannot be a bare IPv6 literal**, because `host:port` cannot be split from one unambiguously. Such a value is rejected as an invalid `gps_device` (the daemon refuses to start rather than silently misparsing it); use a hostname, an IPv4 address, or leave it at the `127.0.0.1` default, which is what a local gpsd needs anyway.
- **libgpiod v2 is compile-verified only**, `EventGPIOPin`'s v2 code path (Debian trixie and newer) builds cleanly in CI/`build-docker.sh`, but it has never been exercised at runtime against real hardware — all runtime verification to date has been on libgpiod v1 (Debian bookworm). Treat the v2 path as unproven until someone runs it on a Pi.
