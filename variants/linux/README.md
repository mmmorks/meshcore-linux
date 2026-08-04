# MeshCore Linux Variant

Native Linux support for MeshCore, targeting Raspberry Pi (Zero, 3, 4, 5) and
similar SBCs with an SX1262 LoRa radio attached over SPI. Uses
[ArduLinux](https://github.com/l5yth/ardulinux), an Arduino API for Linux, to run
the same firmware codebase on Linux without modifying the core library.

The daemon is `meshcored`; its CLI client is `meshcorectl`.

## Contents

- [Hardware](#hardware)
- [Building](#building)
- [Installing](#installing) — [binaries](#1-install-the-binaries),
  [config](#2-create-the-config-file), [device access](#3-enable-spi-and-grant-device-access),
  [running](#4-run)
- [Deploying to a remote node](#deploying-to-a-remote-node)
- [The control CLI](#the-control-cli-meshcorectl)
- [Operation](#operation) — [idle CPU](#idle-cpu-usage),
  [CAD](#channel-activity-detection),
  [divergence from upstream](#divergence-from-upstream),
  [changing settings](#changing-settings-after-first-boot)
- [GPS](#gps) — [serial](#serial-gps), [gpsd + chrony](#disciplining-the-host-clock-from-gnss),
  [PPS](#pps-on-the-waveshare-lorawangnss-hat), [serving time](#serving-the-time-to-the-lan),
  [receiver tuning](#tuning-the-receiver),
  [identifying the receiver](#identifying-the-receiver-gnssctl-probe)
- [Resetting a node](#resetting-a-node)
- [Known gaps](#known-gaps)

## Hardware

- Raspberry Pi (any model with SPI)
- SX1262-based LoRa module on the Pi's SPI bus (e.g. Waveshare SX1262 HAT, PoW
  SX1262 HAT)
- SPI, IRQ and RESET GPIO lines; optionally BUSY, NSS and RXEN/TXEN

## Building

### Dependencies

Needed on the build machine and on the node:

```sh
# Arch Linux
sudo pacman -S pkgconf libgpiod i2c-tools bluez-libs libuv

# Debian / Raspberry Pi OS
sudo apt install pkg-config libgpiod-dev libi2c-dev libbluetooth-dev libuv1-dev
```

ArduLinux always links `bluetooth`, `uv`, `pthread` and `stdc++fs`; `gpiod` and
`i2c` are added when libgpiod is found via `pkg-config`. Two failure modes worth
recognising:

| Symptom | Cause |
|---------|-------|
| `FATAL: meshcored was built without libgpiod support` | `pkg-config` was missing at build time (DietPi's base image has no `pkgconf`), so libgpiod went undetected and the build fell back to simulated GPIO/I2C |
| `cannot find -lbluetooth` at link time | `bluez-libs` / `libbluetooth-dev` missing |

You also need **PlatformIO Core**:

```sh
sudo pacman -S platformio-core   # Arch; or: pipx install platformio
pipx install platformio          # Debian / Raspberry Pi OS
```

### Native build

`build.sh` is preferred — it embeds the version and commit hash:

```sh
FIRMWARE_VERSION=dev ./build.sh build-firmware linux_repeater
# binary: .pio/build/linux_repeater/meshcored
```

PlatformIO directly works too, without version metadata:

```sh
FIRMWARE_VERSION=dev pio run -e linux_repeater
```

### Container cross-build

`build-docker.sh` cross-builds for arm64 from any host, which is how you build
from a non-Linux machine. `BASE_IMAGE` picks the libgpiod major version:

```sh
./build-docker.sh linux_repeater
# bookworm (libgpiod 1.x) -> .pio/build/linux_repeater/meshcored

BASE_IMAGE=debian:trixie ./build-docker.sh linux_repeater
# trixie (libgpiod 2.x)   -> .pio/build-trixie/linux_repeater/meshcored
```

`EventGPIOPin` selects its libgpiod v1/v2 code path with a preprocessor check, so
each base compiles only one of them. Build against both before a release.

## Installing

### 1. Install the binaries

```sh
sudo install -m 755 .pio/build/linux_repeater/meshcored /usr/bin/meshcored
sudo install -m 755 variants/linux/meshcorectl /usr/bin/meshcorectl
```

Install `meshcorectl` now — once `meshcored` runs as a service it is the only way
to reach the CLI.

### 2. Create the config file

Two templates ship in `variants/linux/`:

| Template | Hardware |
|----------|----------|
| `meshcored.ini.pow-sx1262` | RPi Zero 2W + PoW SX1262 HAT |
| `meshcored.ini.waveshare` | RPi 3/4/5 + Waveshare SX1262 LoRa HAT, including the LoRaWAN/GNSS variant (its GNSS lines are commented out) |

```sh
sudo install -D -m 644 variants/linux/meshcored.ini.waveshare /etc/meshcored/meshcored.ini
sudo nano /etc/meshcored/meshcored.ini
```

The file has two kinds of setting:

- **Hardware config** — SPI device, GPIO lines, radio parameters. Read on every
  startup.
- **First-run node defaults** — `advert_name`, `admin_password`, `lat`, `lon`,
  and the LoRa radio parameters. Saved to `prefs.json` on the first boot; after
  that the INI values are no longer consulted for these fields and changes go
  through the CLI (`set name`, `set freq`, …) or a
  [prefs reset](#resetting-a-node).

#### Settings

| Key | Default | Notes |
|-----|---------|-------|
| `spidev` | `/dev/spidev0.0` | SPI device node |
| `lora_gpiochip` | `gpiochip0` | `/dev/gpiochip*` name or kernel label. Correct for Pi 3/4/Zero 2W; Pi 5 may need `gpiochip4` or `pinctrl-rp1` |
| `lora_irq_pin` | *(none)* | IRQ line |
| `lora_reset_pin` | *(none)* | RESET line |
| `lora_nss_pin` | *(none)* | NSS/CS line, if not handled by the SPI driver |
| `lora_busy_pin` | *(none)* | BUSY line |
| `lora_rxen_pin` | *(none)* | RF-switch RX enable; omit if unused |
| `lora_txen_pin` | *(none)* | RF-switch TX enable; omit if unused |
| `lora_freq` | `869.618` | MHz |
| `lora_bw` | `62.5` | kHz |
| `lora_sf` | `8` | Spreading factor |
| `lora_cr` | `5` | Coding rate |
| `lora_tcxo` | `1.8` | TCXO voltage; `0.0` if the module has no TCXO |
| `lora_tx_power` | `22` | dBm |
| `current_limit` | `140` | Radio over-current protection, mA |
| `dio2_as_rf_switch` | `0` | `1` = DIO2 drives the TX/RX RF switch. **Required for the Waveshare Core1262** — without it the radio inits but TX and RX are dead |
| `rx_boosted_gain` | `1` | SX126x RX boosted-gain mode |
| `use_regulator_ldo` | `0` | `1` powers the radio from the LDO instead of the DC-DC converter. Only for modules built without the DC-DC inductor |
| `rx_register_patch` | `0` | `1` applies the SX126x RX-sensitivity patch (bit 0 of register `0x8B5`). Try it if a HAT receives poorly |
| `advert_name` | `Linux Repeater` | First-run default |
| `admin_password` | `password` | **Change this.** First-run default |
| `lat` / `lon` | `0.0` | First-run default |
| `gps_device` | *(empty)* | Serial path (`/dev/ttyACM0`) or `gpsd://[host][:port]` (default `127.0.0.1:2947`). Empty disables GPS |
| `gps_baud` | `9600` | Serial `gps_device` only; one of 4800/9600/19200/38400/57600/115200. Ignored for `gpsd://` — gpsd owns the port |
| `gps_en_pin` | `-1` | GPIO held HIGH to wake a receiver that boots in standby (e.g. the L76K STANDBY line). `-1` = none. Leave unset with a `gpsd://` source — see [Keeping GNSS up without meshcored](#keeping-gnss-up-without-meshcored) |
| `defer_clock` | *(unset)* | Override whether meshcored sets the system clock from GPS/mesh time sync. Unset: on exactly when `gps_device` is `gpsd://` (gpsd already owns the clock). `true`: never set it — e.g. a serial `gps_device` whose NMEA is separately fed to chrony. `false`: always attempt to set it, even against a `gpsd://` device |

Comments (`#`, `;`), blank lines and `[section]` headers are ignored. Boolean
settings (`dio2_as_rf_switch`, `rx_boosted_gain`, `use_regulator_ldo`,
`rx_register_patch`, `defer_clock`) accept `1`/`0`, `true`/`false`, `on`/`off`
or `yes`/`no`, case-insensitively; anything else is a fatal invalid value.

#### Config validation

Every problem is reported on its own `ERROR:` line at startup:

| Problem | Response |
|---------|----------|
| **Invalid value** — a GPIO pin outside `0..255`, a malformed or out-of-range number, an unrecognised boolean spelling, an unsupported `gps_baud`, or empty | **Fatal**, the daemon refuses to start |
| **Unrecognised key** — e.g. `lora_frequency` for `lora_freq` | **Warning**, key ignored, startup continues |
| **File missing or unreadable** | **Warning**, built-in defaults used. The radio then fails to start, since no pins are configured |

```
ERROR: meshcored.ini: unknown key 'lora_frequency' (ignored)
WARNING: 1 unrecognised key(s) in /etc/meshcored/meshcored.ini ...

ERROR: meshcored.ini: lora_irq_pin = '260' is not a valid GPIO pin (expected 0..255)
FATAL: 1 invalid value(s) in /etc/meshcored/meshcored.ini ...
```

**Read the warnings after editing.** An ignored key does not merely fail to
apply: for a first-run default, the built-in value is persisted to `prefs.json` on
the first boot, and fixing the INI afterwards changes nothing.

### 3. Enable SPI and grant device access

The radio needs a `/dev/spidev*` node. Check with `ls /dev/spidev*`; if there is
none:

```sh
# Raspberry Pi OS
sudo raspi-config          # Interface Options -> SPI -> Enable, then reboot

# Arch Linux ARM (no raspi-config)
echo 'dtparam=spi=on' | sudo tee -a /boot/config.txt   # then reboot
```

The boot config is `/boot/config.txt` on most images and
`/boot/firmware/config.txt` on others. Confirm `/dev/spidev0.0` exists after the
reboot.

> **Arch kernel caveat.** `dtparam=spi=on` is only honoured by the Raspberry Pi
> vendor kernel (`linux-rpi`). The mainline `linux-aarch64` kernel boots via
> U-Boot, which loads its own device tree and ignores `config.txt` overlays, so
> `/dev/spidev*` never appears. Switch to the vendor kernel
> (`sudo pacman -S linux-rpi`, remove `linux-aarch64`) and reboot.

Then give the account that runs the daemon access to the devices. **Join the
groups the distro's own udev rules already grant** — `spi` and `gpio` for the
radio, `dialout` for a serial GPS:

```sh
sudo usermod -aG spi,gpio,dialout "$USER"    # log out and back in
```

A login shell picks the new groups up only after a re-login; to use them
immediately in one shell, prefix with `sg spi -c '…'`.

> **Distros without `spi`/`gpio` groups** (Arch, and others that grant the
> devices to root only) need a rule of their own. `variants/linux/99-meshcore.rules`
> puts `/dev/spidev*` and `/dev/gpiochip*` in a `meshcore` group:
>
> ```sh
> sudo groupadd -f -r meshcore
> sudo usermod -aG meshcore "$USER"
> sudo install -m 644 variants/linux/99-meshcore.rules /etc/udev/rules.d/
> sudo udevadm control --reload-rules && sudo udevadm trigger
> ls -l /dev/gpiochip* /dev/spidev*     # -> crw-rw---- root meshcore
> ```
>
> **Do not install this on Raspberry Pi OS.** udev sorts rule files by name and
> `GROUP=`/`MODE=` overwrite rather than merge, so a `99-meshcore` rule sorts
> after the distro's `99-com.rules` and takes the devices away from the `spi` and
> `gpio` groups — locking out the login user and anything else sharing the node.

### 4. Run

`meshcored` takes a small set of options, parsed by the ArduLinux core:

| Flag | Description |
|------|-------------|
| `-d`, `--fsdir=DIR` | VFS root, where all data is persisted. Default `~/.local/share/meshcored/default` |
| `-e`, `--erase` | Recursively wipe the VFS root, then start. A **full reset**: the node identity goes too, so it returns with a new Repeater ID. Never put this in the systemd unit |
| `--usage`, `-?` / `--help` | Short usage / full option list |
| `-V`, `--version` | Print the firmware version |

**Directly**, for testing — no `sudo` needed once the groups are active:

```sh
meshcored                                # VFS root: ~/.local/share/meshcored/default
meshcored --fsdir /var/lib/meshcore      # explicit location
sg spi -c 'meshcored'                    # before re-logging in
```

**As a systemd service**, for production. The unit runs as the `meshcore` user
and passes `--fsdir /var/lib/meshcore`:

```sh
sudo groupadd -r meshcore
sudo useradd -r -g meshcore -s /sbin/nologin meshcore
sudo usermod -aG spi,gpio,dialout meshcore
sudo chown root:meshcore /etc/meshcored/meshcored.ini
sudo chmod 640 /etc/meshcored/meshcored.ini
sudo install -m 644 variants/linux/meshcored.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now meshcored
sudo journalctl -u meshcored -f
```

The unit's `StateDirectory=meshcore` creates `/var/lib/meshcore` owned by
`meshcore:meshcore` before startup, so there is nothing to pre-create. If you
smoke-tested by running directly first, clear stale state so the service
first-boots with the INI defaults: `sudo rm -rf /var/lib/meshcore/*`.

## Deploying to a remote node

`./deploy.sh` builds and installs onto a node over ssh:

```sh
./deploy.sh                      # build + deploy to $MESHCORE_HOST (default: pimesh)
./deploy.sh othernode            # another ssh host
SKIP_BUILD=1 ./deploy.sh         # reuse the existing build artifact
```

It asks the node which Debian release it runs, picks the matching container base
so libgpiod's major version lines up, builds, copies `meshcored` and
`meshcorectl` over, restarts the service and then verifies it actually came back
up — printing the node's own journal if it did not. Setup steps (the `meshcore`
user and group, group memberships, the unit file when absent) are idempotent and
silent on an already-provisioned node.

It deliberately leaves three things alone: udev rules, `/etc/meshcored/meshcored.ini`
and an existing unit file, and gpsd/chrony configuration. Those are node-specific
and covered above.

The previous binary is kept, so a bad deploy is recoverable:

```sh
ssh <host> 'sudo mv /usr/bin/meshcored.prev /usr/bin/meshcored && sudo systemctl restart meshcored'
```

## The control CLI (`meshcorectl`)

Everything the MeshCore docs describe as the "serial CLI" — `set`, `get`,
`advert`, `neighbors`, the `gps` commands — is reached here through
`meshcorectl`.

The Arduino `Serial` object is output-only on Linux, so `meshcored` prints to
stdout and listens for commands on a **Unix-domain control socket**. The first
writable path wins, and startup logs which one it picked:

| Order | Path |
|-------|------|
| 1 | `$MESHCORED_CONTROL_SOCKET`, if set |
| 2 | `/run/meshcored/meshcored.sock` (created by the unit's `RuntimeDirectory=`) |
| 3 | `$XDG_RUNTIME_DIR/meshcored.sock` |
| 4 | `/tmp/meshcored.sock` |

The socket is mode `0660` and owned by the user running the daemon, so reaching
it means being that user, being in its group, or being root.

**The `/tmp` fallback is a predictable, shared path.** Unlike `/run/meshcored`
(a systemd `RuntimeDirectory`, mode `0750`) or `$XDG_RUNTIME_DIR` (per-user,
mode `0700`), `/tmp` is world-writable and its name never changes, so on a
multi-user host another local user can pre-create or race for
`/tmp/meshcored.sock` before the daemon starts. `meshcored` only takes over a
path it can prove is not already answering connections (see `try_bind()` in
`variants/linux/LinuxConsole.cpp`), so this is not an admin-socket takeover —
but it is still a path only a single-user development box should rely on.
Under systemd or with `$XDG_RUNTIME_DIR` set, this fallback is never reached.

Three ways to drive it:

```sh
sudo meshcorectl                             # REPL: line editing, history, Tab completion
meshcorectl set name my-repeater             # one-shot: send, print reply, exit
printf 'ver\nneighbors\n' | meshcorectl      # piped: one command per line
```

The REPL needs no `socat` or `rlwrap`: arrow-key editing, Ctrl-R search, Tab
completion of known commands, and history in `~/.meshcorectl_history`.
`MESHCORED_CONTROL_SOCKET` overrides the path for the client exactly as it does
for the daemon, which is how you reach a node that is not the packaged service.

Only **one client at a time** is served; a second connection is refused
immediately with `ERR: control socket busy, another client is connected` and
closed. Raw tools work too: `sudo socat - UNIX-CONNECT:/run/meshcored/meshcored.sock`
(also subject to the same one-client rule).

`meshcorectl` exits `0` only if every command it sent was actually run by the
daemon — that includes `reboot`, `clkreboot` and `poweroff`, whose only "reply"
is their own echo before the daemon goes away. It exits `1` if the socket is
missing or unusable, if the connection is refused as busy, if a command draws no
reply at all, or if a *later* command in a piped script finds the daemon already
gone (the case after one of those three ran earlier in the same script). A piped
script stops at the first such failure rather than sending the remaining lines
into a dead or busy connection.

Running `meshcored` in a foreground terminal with no service behind it, you can
skip the socket and type commands straight into its stdin. A connected socket
client takes priority over stdin while it is attached.

## Operation

### Idle CPU usage

`meshcored` blocks in `poll()` between events rather than spinning: expect **well
under 1% CPU at idle** with edge detection, and roughly **1–3%** in the polling
fallback. Startup logs which mode a bound LoRa IRQ pin ended up in:

```
LoRa IRQ pin 25 bound with edge detection: yes
```

`NO (polling fallback)` means the kernel or libgpiod on this device could not set
up edge events for that line, so the daemon uses a 1 ms poll timeout instead of
the normal 50 ms `IDLE_MAX_WAIT_MS`. Packet RX/TX is unaffected; it just costs
CPU.

Edge detection can also be lost later in a run:

```
EventGPIOPin(GPIO25): edge detection lost on mode change (...); falling back to timeout polling
```

The daemon stays correct but degrades to timeout polling for the rest of the run.
This is not expected in normal operation — worth reporting, with the `(...)`
errno text and your libgpiod and kernel versions.

### Channel Activity Detection

Off by default. When enabled, the radio runs a hardware CAD scan immediately
before each transmit and defers if it detects a LoRa signal. The change takes
effect within 2 seconds, no restart needed:

```
set cad on      # or: set cad off
get cad
```

CAD complements `int.thresh` rather than replacing it; either, both or neither
may be active:

- **`int.thresh`** compares RSSI against the measured noise floor. It sees any
  energy, including non-LoRa interference, but cannot see a signal below the
  noise floor.
- **`cad`** correlates against the LoRa preamble, so it detects a real LoRa
  transmission *below* the noise floor where RSSI is blind — but ignores non-LoRa
  energy entirely.

Enabling `int.thresh` alongside `cad` also reduces how often the scan runs: the
RSSI check is evaluated first, and a busy verdict there skips the scan.

**Cost at high spreading factors.** A scan takes about four symbol times: roughly
20 ms at SF8/62.5 kHz, but around 265 ms at SF12/62.5 kHz. Transmit attempts
retry every 200 ms while the channel reads busy, so at SF12 a node holding a
queued packet on a contended channel spends over half that window inside a scan —
with the modem in standby, **not listening**. That is inherent to CAD-before-TX,
but worth knowing before enabling it on a high-SF preset.

### Divergence from upstream

This fork changes the noise-floor estimator and the `int.thresh` CSMA check in
`src/helpers/radiolib/` and `src/helpers/NoiseFloorTracker.h` — shared code, not
`variants/linux/` — so it affects every target built from this tree, not only
Linux:

- The floor is a minimum-statistics estimator (a bias-corrected minimum over a
  sliding window) that re-seeds itself whenever radio parameters or RX gain
  state actually change *at runtime* (`tempradio`, `set radio.rxgain`, …) —
  not `set radio`, which only persists to `prefs.json` for the next
  [reboot](#changing-settings-after-first-boot) — instead of tracking a stale
  floor from the previous receiver state for up to 180 s.
- The estimate's scale (sigma) is protected by a fixed censoring gate so that
  busy-channel traffic can no longer ratchet sigma upward without limit. This
  removes the *unbounded* growth, not the spread itself — sustained traffic a
  few dB above the true floor can still widen sigma measurably (worst case
  ~2.35 dB against a true 0.8 dB spread). See `NOISE_TRACKER_SCALE_GATE_DB` in
  `src/helpers/NoiseFloorTracker.h` for the measured sweep and its exact
  guarantee.
- `resetAGC()` deliberately no longer re-seeds the noise floor estimate — doing
  so was a measured, live-hardware regression (see its definition in
  `src/helpers/radiolib/RadioLibWrappers.cpp`).

**This puts a floor under `int.thresh` that the operator's own setting cannot
go below.** `isChannelActive()` enforces `margin = max(int.thresh,
NOISE_THRESHOLD_SIGMA_K * sigma)` (`NOISE_THRESHOLD_SIGMA_K` = 3.5), and sigma
is clamped to at most 3.0 dB — so however tight `int.thresh` is set, the
enforced margin can be as high as **~10.5 dB**. A very tight `int.thresh` may
therefore have less effect than its number suggests.

### Changing settings after first boot

Name, password, location and the radio parameters live in `prefs.json` after the
first boot and are changed through the CLI:

```
set name <name>
password <newpwd>
set lat <lat>
set lon <lon>
set freq <mhz>
set radio <freq>,<bw>,<sf>,<cr>
```

`password` is top-level, not `set password`. There is no standalone `set sf`;
spreading factor (and bandwidth/coding rate) change only together, via
`set radio`, and take effect after a `reboot`.

## GPS

With `gps_device` configured, the standard MeshCore GPS commands work over
`meshcorectl`:

| Command | Effect |
|---------|--------|
| `gps` | Status: on/off, active/deactivated, fix/no-fix, satellite count |
| `gps on` / `gps off` | Enable/disable GPS reading and location telemetry |
| `gps sync` | Force a time re-sync from GPS |
| `gps setloc` | Save the current fix as the node's advertised location |
| `gps advert none\|prefs\|share` | Control whether location is advertised |
| `gps interval [seconds]` | Seconds between location reads (0 = default 1 s, max 86400). Bare form reports the stored value |

At the 1 s default, the two `lat …` debug lines printed on every read dominate the
journal on a node with a fix; `gps interval` throttles them too.

### Serial GPS

The daemon opens the device directly and holds it for its whole life. It needs
read access — USB units are usually root- or `dialout`-owned, which the group
memberships above cover.

Some receivers boot into standby and stay silent until an enable line is driven
high. The L76K on the Waveshare LoRaWAN/GNSS HAT is one: set `gps_en_pin` to its
STANDBY GPIO (4 on that HAT) and the daemon holds it high from startup.

### Disciplining the host clock from GNSS

Holding the device exclusively locks out `gpsd`, and through it `chrony`. On a
Linux node that matters more than on an MCU: the node's clock is the whole host's
clock, and a host with no network and no RTC otherwise boots with a bogus one.

Point `gps_device` at gpsd and the device becomes gpsd's:

```ini
gps_device = gpsd://        # 127.0.0.1:2947
# gps_en_pin stays unset -- the host holds the line, see below
```

The daemon reconnects to gpsd on its own with backoff, so start order does not
matter and restarting gpsd underneath a running node is safe. It also stops
trying to set the system clock — chrony owns it. That also means a `clock sync` /
`time <epoch>` carrying a timestamp from a remote mesh peer cannot move a
general-purpose host's clock.

On the host:

```sh
sudo apt install gpsd gpsd-clients chrony
```

`/etc/default/gpsd`:

```sh
DEVICES="/dev/ttyAMA0"      # /dev/ttyS0 on a Pi whose PL011 is not the GPS UART
GPSD_OPTIONS="-n"
```

`-n` keeps gpsd reading the receiver after its last client disconnects, which
chrony's SHM refclock needs — it is not a gpsd socket client. It says nothing
about *starting*, which is the separate trap covered in
[Keeping GNSS up without meshcored](#keeping-gnss-up-without-meshcored).

`/etc/chrony/chrony.conf`:

```
refclock SHM 0 refid GPS offset 0.307 delay 0.2
```

**The `offset` is mandatory and board-specific.** NMEA without PPS arrives some
way after the second it describes, so the refclock reads consistently late; left
uncorrected, chrony marks it a falseticker (`#x` in `chronyc sources`) and ignores
it. Start at `0.0`, read the steady-state figure, and enter it as a positive
number — a positive offset cancels a positive reported error, and getting the sign
backwards doubles it:

```sh
chronyc sources        # "#x GPS  ...  +307ms[ +307ms]"  ->  offset 0.307
```

The value is stable within a session (sd ~5 ms) but is not a constant of the
board: it tracks how long the receiver takes to start and clock out its sentence
burst, so it moves with the sentence set and the baud rate. On this HAT it has
been seen at 0.193 s, 0.307 s and 0.384 s with nothing touched. Expect to
re-check it — and note that [adding PPS](#handing-pps-to-chrony) inverts how to
tune it.

Verify with `chronyc sources` (a `GPS` line that is no longer `#x`) and
`meshcorectl gps` (fix and satellite count).

With a network present chrony will usually still prefer a good NTP server —
NMEA-only GPS is worth about ±100 ms against a stratum-1 peer's ±30 ms, so `#-`
rather than `#*` is correct. To prove GPS can hold the clock alone, take the
network sources away:

```sh
sudo chronyc offline    # refclocks are unaffected
# ~4 minutes later, once the NTP peers age out:
chronyc tracking        # Reference ID : 47505300 (GPS), Stratum : 1
sudo chronyc online
```

`gps off` stops the reads rather than closing the socket, so gpsd is left holding
a client that has stopped draining. No stale NMEA is acted on, though: the first
read after a gap of more than 5 s reconnects, so `gps on` resumes from live
sentences.

### PPS on the Waveshare LoRaWAN/GNSS HAT

Needs a hardware mod. The L76K emits a pulse-per-second signal, but the HAT
routes it only to indicator LED `L_PPS1` through R19 (510R) — no `PPS` net reaches
the Raspberry Interface block, and no unpopulated jumper or DNF resistor would
route it. One wire fixes that.

It is worth soldering: NMEA-only is worth about ±100 ms, while kernel PPS on a Pi
settles under a microsecond, bounded by interrupt latency rather than by the
receiver.

PPS is a precision layer on top of gpsd, never a replacement: it says when a
second begins, not which second it is. Keep the NMEA refclock.

#### Tapping the signal

The `PPS` net's only accessible copper is R19 and the `L_PPS1` anode. R19 is an
0402 — use 30 AWG wire and glue it down, because the pad will lift the first time
that wire is flexed.

**Option 1 — R19's Q1-side pad.** Full 3.3 V CMOS swing, LED keeps working. The
short trace is Q1 pin 4 → R19; the long one is R19 → LED. Identify the pad by
continuity to Q1 pin 4 (fourth castellated pad from the pin-1/GND corner), or with
a scope: the `PPS`-side pad swings 0→3.3 V, the LED-side pad clamps at the LED's
~1.9 V forward drop. **Do not tap the LED side** — 1.9 V is marginal against the
Pi's V_IH. Fit a 220–330R resistor in series.

**Option 2 — remove `L_PPS1` and tap its anode pad.** Mechanically easier: bigger
pad, at the board edge next to P5's through-holes for strain relief, and R19's
510R becomes the series protection, so do not add another. With the LED gone that
node swings the full 0→3.3 V; the cost is losing the indicator, which leaves the
[checks below](#verifying-the-overlay) as the only way to see pulses arriving.

**This board runs option 2**, and it works: `ppstest` shows ~±3 µs of inter-pulse
jitter and chrony settles at a single-digit-microsecond RMS offset.

#### Landing it on the Pi

Land the other end on a free P1 pad on the HAT's own underside, so the mod stays
self-contained and connects through the header when the HAT is seated.

**BCM 26 (P1 pin 37)** is the recommendation: unused by either pin set in
`meshcored.ini`, with GND adjacent at pin 39 for the return. BCM 19 (35), BCM 12
(32), BCM 25 (22) and BCM 27 (13) also work. Check the `lora_irq_pin` and
`lora_reset_pin` in your own config before committing.

Pins to stay off:

| Pin(s) | Reason |
| --- | --- |
| 1, 17 | Pi 3V3 supply. The HAT takes only 5 V and regulates its own 3V3 with AMS-1, so these pads dangle on the HAT — but the Pi still drives its rail onto them, and a tap here shorts `PPS` into it |
| 27, 28 | `ID_SD`/`ID_SC`, reserved for HAT EEPROM ID detection at boot |
| 3, 5 | BCM 2/3. Usable, but they carry the Pi's 1.8K I²C pull-ups |
| 7 | BCM 4 — GPS `STANDBY` |
| 8, 10 | BCM 14/15 — GPS UART |
| 19, 21, 23 | SPI to the SX1262 |
| 31 | BCM 6 — `DIO4`, one of the two `CTRL` inputs of the PE4259 RF switch |
| 12, 36, 38, 40 | `RST`, `DIO1`, `BUSY`, `CS` |

Both conventional `pps-gpio` pins are already taken on this HAT: GPIO 18 is `RST`
and GPIO 4 is `STANDBY`.

Since the HAT regulates its own 3V3, the PPS high level comes from the AMS1117
rather than the Pi's rail. Both are nominally 3.3 V and grounds are common, so no
level shifting is needed — but the two rails are independently derived, which is
one more reason for the series resistor.

#### Loading the overlay

`/boot/firmware/config.txt`:

```
dtoverlay=pps-gpio,gpiopin=26,pull=down
```

The pull-down matters: whenever the L76K is in standby, or simply not powered
yet, it stops driving its pin 4 and the Pi's input would float. `pull` is not in
every version of the overlay — `dtoverlay -h pps-gpio` lists what yours takes, and
an external 10K to GND does the same job. Leave `assert_falling_edge` at its
default; the L76K marks the top of the second with a **rising** edge, 100 ms wide.

#### Verifying the overlay

**The kernel prints nothing on success.** There is no `pps-gpio` line in `dmesg`,
and `lsmod` shows `pps_gpio` at refcount 0 even when bound (that column counts
dependent modules, not device bindings). Neither absence means anything is wrong.

The three checks that do carry information:

```sh
cat /sys/class/pps/pps*/name                # want one reading pps@1a  (0x1a = GPIO 26)
gpioinfo | grep -w 26                       # want:  "GPIO26"  "pps@1a"  input  [used]
ls -l /sys/bus/platform/drivers/pps-gpio/   # want a symlink named pps@1a
```

Two traps once those pass:

- **gpsd creates a second, permanently silent PPS device.** It attaches a PPS line
  discipline to the serial port, which appears as another `/dev/ppsN` named
  `serial0`. Nothing drives DCD there, so its assert counter stays at zero and
  `ppstest` on it hangs. Check the `name` files rather than assuming `/dev/pps0`
  is the GPIO one.
- **The numbering is not guaranteed.** The GPIO source is `pps0` only because the
  platform driver binds at boot, before gpsd starts. For a stable name, add
  `/etc/udev/rules.d/10-pps-gpio.rules`:

  ```
  SUBSYSTEM=="pps", ATTR{name}=="pps@1a.-1", SYMLINK+="pps-gpio"
  ```

Then watch actual pulses. `/dev/pps*` is mode 600 root:root, so this needs `sudo`
— chronyd is unaffected, as it opens refclocks before dropping privileges:

```sh
sudo apt install pps-tools
sudo ppstest /dev/pps-gpio     # want ~1.000000 s between assert events
```

Nothing appears until the receiver has a fix — the L76K gates PPS on that, so a
silent device under a cold start is expected, not a wiring fault.

#### Handing PPS to chrony

`/etc/chrony/chrony.conf`, amending the single `SHM 0` line from above:

```
refclock SHM 0 refid GPS offset 0.307 delay 0.2 noselect
refclock PPS /dev/pps-gpio refid PPS lock GPS prefer
```

Use `/dev/pps-gpio`, not `/dev/pps0` — install the udev rule above first. Pointing
this line at gpsd's silent serial PPS device fails quietly; chrony simply never
gets a sample.

`noselect` keeps NMEA labelling seconds without ever disciplining the clock. As a
side effect the NMEA line stops being reported as a falseticker (`#x`) and shows
as `#?`, which for a `noselect` source is the healthy state.

**Adding PPS inverts how to tune the `offset`.** It is no longer a calibration —
NMEA is `noselect`, so its value has no effect on the clock. Its only remaining
job is to keep NMEA inside ±0.5 s, the nearest-second rounding limit, so
`lock GPS` pairs each pulse with the right second.

For that job a **larger offset is safer than a smaller one**. The delay is
physically one-sided: NMEA can only arrive *after* the second it describes, never
before, so raising the offset costs nothing on the low side and buys headroom on
the high side where all the risk lives. On this node the raw delay was measured at
0.307 s, then 0.193 s an hour later, then 0.384 s twenty minutes after that, with
nobody touching gpsd; `offset 0.307` kept the reported error inside ±120 ms
throughout, where a "correction" to zero would have left 116 ms of margin.

So once PPS is running, leave the offset alone and treat a non-zero reading on the
`GPS` line as normal. Comment the value in `chrony.conf`, or the next person to
read it will tune it toward zero.

Give it a couple of minutes to accumulate samples. `chronyc sources` should end up
with `#* PPS` selected and the NTP servers demoted to `^-`, and `chronyc tracking`
should report `Stratum : 1` and `Reference ID : 50505300 (PPS)`.

Measured on this HAT after the mod: raw inter-pulse jitter at `ppstest` around
±3 µs, which is Pi interrupt latency rather than the receiver; chrony's filtered
result settles around **±400 ns** dispersion and a sub-microsecond RMS offset.

### Serving the time to the LAN

chronyd does not offer the clock by default and gives no hint that it is holding
back: with no `allow` directive chrony 4 never opens UDP/123, so a client sees a
silent timeout rather than a refusal and `ss -lun` on the node shows only
chronyc's `127.0.0.1:323` control socket.

Debian's `chrony.conf` starts with `confdir /etc/chrony/conf.d`, so add a file
there rather than editing the shipped config:

```
# /etc/chrony/conf.d/serve-lan.conf
allow 192.168.0.0/16
```

```sh
sudo systemctl restart chrony
ss -lun | grep :123        # want: 0.0.0.0:123
sudo chronyc serverstats   # "NTP packets received" climbs as clients appear
sudo chronyc clients       # who is asking
```

**Give the `allow` a subnet.** A bare `allow` permits everything, and the node
runs no firewall — if its interface has a globally-routable IPv6 address, an
unqualified `allow` publishes a stratum-1 server to the internet. The
address-family split matters too: `allow 192.168.0.0/16` covers v4 only, so v6
clients keep timing out until a second `allow` names their prefix. Name the
prefix; do not reach for the bare form to fix it.

The restart is not free. chronyd comes back with an empty refclock filter and
falls back to the pool — stratum 4 for a couple of minutes — before PPS
re-accumulates samples. Clients asking during that window get the pool's time,
correctly labelled.

A single-source stratum 1 is also a single point of failure: lose sky and this
node quietly becomes a stratum 3–4 relay of the Debian pool. Clients should list
it alongside a couple of internet servers rather than pointing at it alone.

### Keeping GNSS up without meshcored

Once chrony takes its time from the receiver the dependency runs the wrong way
round: the host's clock rests on a stack that by default only works while the mesh
daemon happens to be running. Two things cause that, and a node in this state
looks perfect until meshcored stops. Both matter more with PPS, because `lock GPS`
means PPS cannot number its own seconds — no gpsd is not "PPS without NMEA
labelling", it is no stratum 1 at all.

**1. gpsd does not start until something connects to it.** Debian ships gpsd
socket-activated: `gpsd.socket` is enabled and `gpsd.service` is not, so the
daemon is spawned by the first client on port 2947. Where meshcored is the only
gpsd client, the entire GNSS chain — device, SHM, chrony's stratum 1 — is
conditional on it connecting, and a meshcored held down by a bad `meshcored.ini`
takes the host's clock with it.

```sh
systemctl is-enabled gpsd.service gpsd.socket   # the trap: "disabled" / "enabled"
sudo systemctl enable --now gpsd.service        # [Install] pulls gpsd.socket in via Also=
```

Then make a crash self-healing, since there is no longer a client whose reconnect
would restart it:

```ini
# /etc/systemd/system/gpsd.service.d/resilience.conf
[Service]
Restart=on-failure
RestartSec=5
```

**2. The GPS enable pin is unowned whenever meshcored is not running.**
`gps_en_pin` is held for the daemon's lifetime and no longer. In practice the pad
keeps its last state and the receiver stays awake, so this is not an outage
waiting to happen — but what holds the L76K awake is then a pull-up before the
first run and a leftover output level after the last, neither of which is
configuration, and an unowned line is also unprotected against another consumer
claiming it and driving it low.

Hand the line to the kernel instead, in `/boot/firmware/config.txt`, and leave
`gps_en_pin` unset:

```
dtoverlay=gpio-hog,gpio=4
```

A hogged GPIO is driven for the whole boot and, in the overlay's own words, "not
available to other drivers or for gpioset/gpioget".

> **Pass `gpio=4` explicitly.** The overlay's default is **26**, which on this
> board is the PPS input — `dtoverlay=gpio-hog` bare would hog the pulse line and
> break the thing the hog was added to protect.

Not `gpio=4=op,dh`: that firmware directive sets an initial pad state before the
kernel starts, it does not take ownership, so the line stays free for anything to
claim and drop. Only the hog makes the pin state both declared and defended.

meshcored's own claim then fails and says so, which is the correct outcome rather
than a fault (setting `gps_en_pin` alongside a `gpsd://` device draws a warning
pointing here too):

```
WARNING: could not claim GPS enable pin 4; GPS may stay asleep
         (expected if the host holds this line, e.g. a GPIO hog)
```

Two different failures, so two checks. That GNSS survives the mesh daemon:

```sh
sudo systemctl stop meshcored
sleep 30
chronyc tracking      # want: Reference ID 50505300 (PPS), Stratum 1, unchanged
cgps -s               # want: still a fix -- gpsd is holding the receiver alone
sudo systemctl start meshcored
```

And that it does not need one to *begin* with, which only shows itself across a
boot — this is the one that bites, because gpsd left to socket activation looks
identical to a correct node for as long as meshcored keeps connecting:

```sh
systemctl is-enabled gpsd.service     # want: enabled (not "disabled" + an enabled socket)
sudo journalctl -b -u gpsd | head -3  # want: started at boot, before any client connected
```

### Tuning the receiver

Out of the box on the LoRaWAN/GNSS HAT, `cgps` shows an empty satellite table and
`meshcorectl gps` reports zero satellites even with a good fix. The receiver
presents a partial u-blox emulation — enough `NAV` output and `CFG-MSG` for
gpsd's u-blox driver to bind, and a NAK for nearly everything else, including
`CFG-PRT` in that framing. (What the receiver actually *is* took a separate
investigation to settle — see
[Identifying the receiver](#identifying-the-receiver-gnssctl-probe) below.) gpsd
sets its own message list on every device activation, which turns NMEA off
entirely, and without `GSV`/`GSA` it never builds a `SKY` object. The UBX
routes are dead ends here: the receiver ACKs `NAV-SVINFO` and `NAV-SAT` and
then emits neither.

The fix is the receiver's native Allystar/CASIC `$PCAS` command set, which is
reachable over plain NMEA and does work. One tool handles all of it —
`gnssctl` — plus a gpsd drop-in that wires its boot-time init in. The drop-in
sets `ExecStartPre=/usr/bin/gnssctl init`, so `gnssctl` has to be in place
*before* gpsd is restarted — get that order backwards and the restart fails
`ExecStartPre`, gpsd never starts, and the node loses its clock.

`gnssctl` itself needs no separate install step here:
[`deploy.sh`](#deploying-to-a-remote-node) already ships it to `/usr/bin/gnssctl`
on every deploy. On a node that has never run `deploy.sh`, install it by hand
first, the same way `meshcorectl` is installed above:

```sh
sudo install -m 755 gnssctl /usr/bin/gnssctl
```

Then install the drop-in and restart gpsd:

```sh
sudo mkdir -p /etc/systemd/system/gpsd.service.d
sudo install -m 644 gpsd-gnss-tuning.conf \
    /etc/systemd/system/gpsd.service.d/gnss-tuning.conf
sudo systemctl daemon-reload && sudo systemctl restart gpsd
```

The drop-in's `ExecStartPre` runs `gnssctl init` before gpsd opens the port —
which puts the port at 115200 and the fix rate at 200 ms (5 Hz) — and its
`ExecStartPost` re-enables `GSV`/`GSA` on every start (`CFG-MSG` is RAM-only,
so it cannot be saved to the receiver). Why `ExecStartPre` calls a program
rather than an inline command, and exactly what `init` proves before it sends
the fix rate, are both explained in `gpsd-gnss-tuning.conf`'s own comments —
this is also where the bandwidth measurements below come from.

**The command surface**, all of it exercised by the self-test:

```
gnssctl                          REPL (readline, history, tab completion)
gnssctl show                     current configuration and fix
gnssctl monitor                  live view; Ctrl-C to stop
gnssctl probe                    read-only capability sweep
gnssctl bandwidth                the guard's arithmetic; sends nothing
gnssctl set baud|rate|sentences|constellation
gnssctl restart hot|warm|cold|factory
gnssctl init                     boot path; always exits 0
gnssctl --selftest               235 checks, no hardware, no root
```

Exit codes: `0` means it did what was asked — including a receiver NAK, which
is an answer, not a failure; `1` means it refused to run at all or could not
get the port; `2` means it ran but recognised no framing; `3` means a guard
rail refused the change (`--force` overrides it); `130` means it was
interrupted. `init` is the one exception — it always exits `0`, because a
non-zero exit there would stop `ExecStartPre`, which stops gpsd from starting
at all, and a node with no gpsd loses its clock outright.

**Before changing anything live**, `gnssctl` computes whether the requested
configuration physically fits the link — capacity from 8N1 framing at the
current baud, demand from each sentence's spec-derived maximum width — and
refuses a change that cannot fit (`set`/`restart` exit `3`; `--force`
overrides). It distinguishes a *measured* sky, read from the receiver's own
`GSV` output, from an *assumed* one — the Table 16 worst case, substituted
when the sky has not actually been observed — and says in its output which of
the two it used. `gnssctl bandwidth` prints the same arithmetic for the
current configuration, or a hypothetical one via `--rate`/`--baud`/`--sentences`,
without sending anything at all — the way to check a change before committing
to it.

Two consequences to know about:

- **The baud rate and the sentence rates are one decision.** Everything shares a
  single UART, and the sample chrony reads from SHM 0 comes from UBX
  `NAV-TIMEGPS` in that same stream, so an epoch carrying a `GSA`/`GSV` burst
  delivers its `NAV-TIMEGPS` late. At 115200 the burst takes a twelfth as long to
  clock out and the effect collapses, which is why rate 1 is affordable. **If you
  revert to 9600, put both rates back to 5** — leaving them at 1 there costs a
  flat +523 ms. The measurements behind this are in `gpsd-gnss-tuning.conf`.
- **At 115200 the chrony residual sits near −100 ms**, so re-check that the `GPS`
  line still has room inside ±0.4 s. An `offset` calibrated at 9600 over-corrects
  by about that much.

Nothing else is worth spending the freed bandwidth on: the constellation set is
fixed at GPS + GLONASS + BeiDou (`CFG-GNSS` NAKs and `$PCAS04` has no Galileo
option), and a faster navigation rate does not reach chrony, which takes its time
from PPS.

> **Measure the receiver, not the client stream.** gpsd synthesizes NMEA for its
> clients from the binary data, so sentences the receiver never sends (`GGA`,
> `RMC`, `ZDA`, `GBS`) appear there regardless. Use `gpspipe -R` (or
> `gnssctl probe`, which reads the same raw device) for the device and
> `gpspipe -r` for the client; tuning against the latter produces conclusions
> that are exactly backwards.

### Identifying the receiver: `gnssctl probe`

**Measured on `pimesh`, 2026-08-02.** `$PCAS06` answers
`$GPTXT,01,01,02,SW=URANUS5,V5.3.0.0`. Note the field is `SW=` — **software**,
not manufacturer. The L76K spec's own TXT example is `MA=CASIC` (§2.2.7), so
`URANUS5` is a firmware string and is *not* evidence of an Allystar part, which
is how it was originally read. No `MA=` line came back, so the vendor string
remains unconfirmed.

Everything that *is* checkable says this behaves as the CASIC part the Quectel
L76K spec documents:

- Its binary command interface is CASIC — `BA CE` framing, length before
  class/ID, 32-bit word checksum, empty-payload Get semantics, and
  `CFG-PRT` / `CFG-MSG` / `CFG-RATE` all answering (spec §3).
- Its NMEA matches the spec throughout: `GN` talker for GSA and
  per-constellation `GP`/`GL`/`BD` for GSV (Table 2, which forbids `GN` on
  GSV); GSA `<SystemID>` values 1/4/2 and satellite-ID ranges 1–32 / 1–63 /
  65–88 (Table 16); the GSV `<SignalID>` trailing field.
- It ignores Allystar `f1 d9` completely — measured on `pimesh`: 13,620 bytes
  arrived across 12 `f1 d9` polls, with zero replies in any framing.

So treat `gpsd-gnss-tuning.conf`'s "Allystar URANUS5" as unverified. The
`$PCAS` command set does not identify a vendor — it is used by Allystar and
CASIC parts alike.

The important result is that **what it emits and what it accepts are different
protocols**:

| Framing | Sync | Behaviour |
|---|---|---|
| u-blox | `b5 62` | What it **emits** (NAV frames at 1 Hz). Parses commands and refuses every one |
| Allystar | `f1 d9` | **Ignored entirely** — no answer of any kind, while normal output continues |
| CASIC | `ba ce` | Never emitted, but **this is what it answers to** |

So the receiver presents a u-blox emulation on output while its real command
interface is CASIC — documented in the *Quectel L76K GNSS Protocol
Specification* §3.1, whose framing differs from u-blox in field order (length
precedes class/ID) and checksum (32-bit word sum, not an 8-bit Fletcher pair).

Two consequences worth recording:

- **gpsd 3.26's Allystar driver would not help.** Its lexer waits for `f1 d9`,
  which this chip neither sends nor answers to, so it would never bind. Do not
  plan around a gpsd upgrade.
- **`CFG-PRT` is reachable after all**, just not in the framing we were asking
  in. Over CASIC it answers `portID=1 (UART1), 8N1, 115200`. `CFG-MSG`,
  `CFG-RATE` (1000 ms) and an undocumented 44-byte `CFG-PPS` also answer.
  Everything else — `CFG-NAVSAT`, `MON-VER`, `CFG-SBAS`, `CFG-SURVEY` — returns
  a real CASIC NAK, so those really are absent.
- **The `ubxtool` calls in `gpsd-gnss-tuning.conf` do work.** Appendix C gives
  the factory default as *all eight* NMEA sentences (RMC, GGA, GSV, GSA, VTG,
  GLL, TXT, ZDA). With gpsd stopped, the receiver emits only GSA and GSV —
  precisely the two that `ExecStartPost` enables, and nothing else. gpsd turns
  NMEA off when it activates the device and those two calls put them back, so
  the u-blox numbering (`f0,03` = GSV, `f0,02` = GSA) is right on this chip.
- **GLONASS is on, and that is not the default.** Appendix C's default GNSS
  configuration is GPS + BeiDou; we see `GL` GSV sentences, so `$PCAS04` has
  been set to 7 at some point. Worth knowing because nothing in this repo sets
  it — if it turns out to be volatile, a power cycle would silently drop a
  constellation. `gnssctl set constellation gps bds glo` is how to set it back
  deliberately, and `gnssctl show` is how to check what is currently active.

`$PCAS06` itself is undocumented: the spec lists only `$PCAS01`, `02`, `03`,
`04` and `10`. `$PCAS03` is the documented way to set NMEA sentence rates, and
`$PCAS04` confirms there is no Galileo option (modes are GPS / BeiDou / GLONASS
combinations only), which settles that question independently of the binary
protocol.

Note the NMEA message class differs between the two: **`0x4E` in CASIC**,
`0xF0` in u-blox. `gpsd-gnss-tuning.conf` uses the u-blox numbering over `b5 62`
via `ubxtool`, which is a different interface from the one above.

`gnssctl probe` settles it without writing anything to the receiver:

```sh
sudo gnssctl probe                     # device from /etc/default/gpsd
sudo gnssctl probe --json probe.json   # same, plus machine-readable output
gnssctl --selftest                     # unit tests, touches no hardware
```

It stops gpsd (**and `gpsd.socket`** — gpsd is socket-activated and meshcored
reconnects with backoff, so the service alone would be respawned mid-probe),
listens passively, then sweeps a table of poll messages and reports ACK / NAK /
reply / silence for each. gpsd is restarted to whatever state it was found in,
including on Ctrl-C. Budget a couple of minutes of stratum-1 downtime afterwards
while chrony's refclock filter refills.

Reading the output:

- **`NAK` is a result, not a failure.** "This chip refuses `CFG-SURVEY`" is one
  of the answers being paid for — it says survey-in / position-hold timing mode
  is unavailable.
- **`silent` is much weaker than `NAK`**, and the distinction carries the whole
  Allystar conclusion above. A NAK is a parsed-and-refused conversation. Silence
  is no reply at all — which is why each silent line also prints how many bytes
  *did* arrive during its window, and every frame decoded in it across all three
  framings. On a wire that is never quiet, "no matching frame" and "nothing
  arrived" are different claims and only one of them supports concluding a
  protocol is unsupported.
- **It sweeps every framing, not just the one seen in phase 1**, moving on when
  a framing refuses everything. Sweeping only the observed framing is exactly
  how the CASIC interface stayed hidden.
- **Phase 1 is worth reading even if phase 2 draws blanks.** It lists the NMEA
  sentences the *receiver* emits, read from the device rather than through gpsd,
  so it is not subject to the synthesis trap warned about above. On `pimesh`
  that set is `GNGSA`, `GPGSV`, `GLGSV`, `BDGSV` and nothing else — no GGA, RMC,
  ZDA or GRS, all of which gpsd synthesizes for its clients.
- **Polls carry empty payloads**, which is the documented CASIC "Get" form but
  is *not* a valid u-blox poll for every message. `CFG-MSG` in particular needs
  a class/ID argument in u-blox, so its `b5 62` NAK is inconclusive rather than
  evidence that the `ubxtool` calls in `gpsd-gnss-tuning.conf` are refused.

Two properties are worth knowing before trusting any of this:

- **`probe` cannot write to the receiver.** That is structural, not a promise:
  `gnssctl --selftest` walks the module's own syntax tree and asserts that
  exactly six named functions in the whole file contain a `.write()` call, and
  the probe path reaches the wire only through the frozen table of polls —
  empty-payload CASIC "Get" requests, never an arbitrary send. Confirming that,
  say, enabling Galileo actually helps is a separate job — it needs writes, and
  hours of satellite counts and chrony residuals to measure.
- **Every setting change goes through the same bandwidth guard** described
  above under [Tuning the receiver](#tuning-the-receiver) — `probe` never
  triggers it, because it never writes, but `set`/`restart` always do.

### Two HAT quirks that are easy to get wrong

- **GPIO 4 is load-bearing.** R13 (marked `NC/0R`) is fitted, so driving GPIO 4
  low stops NMEA dead. The pin reads high when undriven only because of the SoC's
  default pull-up on GPIO 0–8, which is not something to rely on. Hold it
  deliberately: `gps_en_pin = 4` for as long as meshcored runs, or
  `dtoverlay=gpio-hog,gpio=4` for the whole boot — with a `gpsd://` source, use
  the hog.
- **Switch S1 drives the same transistor in parallel with GPIO 4.** If the GPS
  will not sleep, that switch is why. `FORCE_ON` is pushbutton K1, not a GPIO —
  GPIO 17 is unconnected here, despite `DEV_FORCE 17` in Waveshare's sample code
  for the standalone L76X module.

## Resetting a node

**Prefs only** — keeps the node identity and Repeater ID. Deleting the saved prefs
makes the INI first-run defaults apply again on the next boot:

```sh
sudo rm -f /var/lib/meshcore/prefs.json /var/lib/meshcore/com_prefs
sudo systemctl restart meshcored
```

(`com_prefs` is the pre-JSON prefs file; a node that has never carried one is
unaffected by the extra `rm`.)

**Full reset** — also discards the identity, so the node returns with a **new**
Repeater ID. This wipes the whole VFS root:

```sh
sudo systemctl stop meshcored
sudo rm -rf /var/lib/meshcore/*
sudo systemctl start meshcored
```

Running directly rather than under systemd, `meshcored --fsdir /var/lib/meshcore --erase`
is the one-shot equivalent. Do **not** add `--erase` to the service unit: systemd
re-runs `ExecStart` on every restart, so it would wipe the filesystem and
regenerate the identity each time. (A `reboot`/`clkreboot` CLI command re-execs
the process with `--erase` stripped from its argument list, so *that* restart
path does not repeat the erase — but a systemd-triggered restart always replays
the unit's original `ExecStart` line from scratch, so this protection does not
extend to it.)

## Known gaps

- **The config path is hardcoded.** `meshcored` always loads
  `/etc/meshcored/meshcored.ini`; there is no flag to point it elsewhere. The
  *data* path is separate and configurable — it is the ArduLinux VFS root, set
  with `--fsdir`.
- **Repeater firmware only.** There is no `linux_companion` target; companion
  radio support (BLE/serial interface to a phone app) is not implemented for
  Linux.
- **The serial `erase` command is a no-op.** `formatFileSystem()` returns `false`
  on Linux, so the interactive `erase` command reports failure. Use the `--erase`
  *startup* flag or clear the VFS directory instead.
- **No power management.** `board.sleep()` is a no-op, so the power-saving loop in
  `main.cpp` never actually sleeps.
- **Upstream-sync fragility.** `LinuxSX1262Wrapper` implements the
  `RadioLibWrapper` interface by hand, so it can drift from upstream two ways: a
  new **pure-virtual** method breaks the Linux build, and a new
  **virtual-with-default** method silently no-ops on Linux until overridden.
  Mirror `CustomSX1262Wrapper` when syncing.
- **A `gpsd://` host cannot be a bare IPv6 literal**, because `host:port` cannot
  be split from one unambiguously. Such a value is rejected as an invalid
  `gps_device` rather than silently misparsed; use a hostname, an IPv4 address, or
  the `127.0.0.1` default that a local gpsd needs anyway.
- **libgpiod v2 is compile-verified only.** `EventGPIOPin`'s v2 path (Debian
  trixie and newer) builds cleanly in CI and `build-docker.sh`, but has never been
  exercised at runtime against real hardware — all runtime verification to date is
  on libgpiod v1 (bookworm). Treat it as unproven.
