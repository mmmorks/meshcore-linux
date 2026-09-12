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

**Container cross-build.** `build-docker.sh` builds for arm64 inside a Debian
container, which is how to build from a non-Linux machine (or without
installing the toolchain on the host). `BASE_IMAGE` picks the libgpiod major
version the binary is built against, and each base gets its own build
directory so the two cannot be mixed:

```sh
./build-docker.sh linux_repeater
# bookworm (libgpiod 1.x) -> .pio/build/linux_repeater/meshcored

BASE_IMAGE=debian:trixie ./build-docker.sh linux_repeater
# trixie (libgpiod 2.x)   -> .pio/build-trixie/linux_repeater/meshcored
```

PlatformIO's package cache lives in a Docker volume, so repeat builds skip the
download; on Apple Silicon the container is native and a warm build takes
about a minute.

## Setup

### 1. Install the binary

```sh
sudo install -m 755 .pio/build/linux_repeater/meshcored /usr/bin/meshcored
```

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
- **First-run node defaults**: `advert_name`, `admin_password`, `lat`, `lon`. On the first boot these are saved to the node's persisted prefs (`prefs.json`). After that, use the serial CLI to change them (`set name`, `set password`, etc.), the INI values are no longer consulted for these fields.

Key settings:

| Key | Default | Notes |
|-----|---------|-------|
| `spidev` | `/dev/spidev0.0` | SPI device node |
| `lora_gpiochip` | `gpiochip0` | Name of the `/dev/gpiochip*` device (or kernel label). `gpiochip0` is correct for Pi 3/4/Zero 2W; Pi 5 may need `gpiochip4` or `pinctrl-rp1` depending on kernel |
| `lora_irq_pin` | (none) | GPIO line number for IRQ |
| `lora_reset_pin` | (none) | GPIO line number for RESET |
| `lora_nss_pin` | (none) | GPIO line number for NSS/CS (if not handled by the SPI driver) |
| `lora_busy_pin` | (none) | GPIO line number for BUSY |
| `lora_rxen_pin` | (none) | GPIO line number for RX enable (RF switch); omit, or set `-1`/`none`, if unused |
| `lora_txen_pin` | (none) | GPIO line number for TX enable (RF switch); omit, or set `-1`/`none`, if unused |
| `lora_freq` | `869.618` | Frequency in MHz |
| `lora_bw` | `62.5` | Bandwidth in kHz |
| `lora_sf` | `8` | Spreading factor |
| `lora_cr` | `8` | Coding rate |
| `lora_tcxo` | `1.8` | TCXO voltage (V); set to `0.0` if your module has no TCXO |
| `lora_tx_power` | `22` | TX power in dBm |
| `current_limit` | `140` | Radio over-current protection limit in mA |
| `dio2_as_rf_switch` | `0` | `1` = use DIO2 to drive the TX/RX RF switch. **Required for the Waveshare Core1262** (without it the radio inits but TX/RX are dead); depends on module wiring |
| `rx_boosted_gain` | `1` | `1` enables the SX126x RX boosted-gain mode; `0` disables |
| `advert_name` | `"Linux Repeater"` | Node name, first-run default only |
| `admin_password` | `"password"` | Admin password, **change this**, first-run default only |
| `lat` / `lon` | `0.0` | GPS coordinates for advertisement, first-run default only |
| `gps_device` | *(empty)* | Serial path (`/dev/ttyACM0`) or `gpsd://[host][:port]` (default `127.0.0.1:2947`). Empty disables GPS. See [GPS](#gps) |
| `gps_baud` | `9600` | Serial `gps_device` only; one of 4800/9600/19200/38400/57600/115200. Ignored for `gpsd://` — gpsd owns the port |
| `gps_en_pin` | `-1` | GPIO held HIGH to wake a receiver that boots in standby (e.g. the L76K STANDBY line). `-1` = none. Leave unset with a `gpsd://` source — see [Keeping GNSS up without meshcored](#keeping-gnss-up-without-meshcored) |
| `defer_clock` | *(unset)* | Override whether meshcored sets the system clock from GPS/mesh time sync. Unset: on exactly when `gps_device` is `gpsd://` (gpsd already owns the clock). `true`: never set it — e.g. a serial `gps_device` whose NMEA is separately fed to chrony. `false`: always attempt to set it, even against a `gpsd://` device |

Comments (`#`, `;`), blank lines and `[section]` headers are ignored. Boolean
settings (`dio2_as_rf_switch`, `rx_boosted_gain`) accept `1`/`0`,
`true`/`false`, `on`/`off` or `yes`/`no`, case-insensitively; anything else is
a fatal invalid value.

#### Config validation

Every problem is reported on its own line at startup:

| Problem | Response |
|---------|----------|
| **Invalid value** — a GPIO pin outside `0..255`, a malformed or out-of-range number, an unrecognised boolean spelling, or empty | **Fatal**, the daemon refuses to start |
| **Line at or past the 511-byte limit** — the rest of it was never read, so the setting cannot be trusted. A comment, blank line or `[section]` that long is ignored instead | **Fatal**, the daemon refuses to start |
| **Unrecognised key** — e.g. `lora_frequency` for `lora_freq` | **Warning**, key ignored, startup continues |
| **File missing or unreadable** | **Warning**, built-in defaults used. The radio then fails to start, since no pins are configured |

```
WARNING: meshcored.ini: unknown key 'lora_frequency' (ignored)
WARNING: 1 unrecognised key(s) in /etc/meshcored/meshcored.ini ...

ERROR: meshcored.ini: lora_irq_pin = '260' is not a valid GPIO pin (expected 0..255)
FATAL: 1 invalid value(s) in /etc/meshcored/meshcored.ini ...
```

> **Upgrading an existing node?** This validation is stricter than what earlier
> builds did, so a file they accepted may now be rejected: values with a unit
> suffix (`lora_tcxo = 1.8V`, `lora_tx_power = 22 dBm`, `current_limit = 140mA`)
> used to be parsed loosely and are now fatal. Since the shipped unit uses
> `Restart=on-failure`, a rejected file means a restart loop and a node off the
> air, so check `journalctl -u meshcored` after upgrading. (`-1` on
> `lora_rxen_pin`/`lora_txen_pin` still works and still means "not wired".)

**Read the warnings after editing.** An ignored key does not merely fail to
apply: for a first-run default, the built-in value is persisted on the first
boot, and fixing the INI afterwards changes nothing.

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
sudo usermod -aG meshcore,dialout "$USER"   # dialout: serial GPS; log out/in afterwards
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
`spi`/`gpio` groups: `sudo usermod -aG spi,gpio,dialout $USER`; `dialout` is only
needed for a serial GPS, see [GPS](#gps).)

### 4. Run

`meshcored` takes a small set of options, parsed by the ArduLinux core:

| Flag | Description |
|------|-------------|
| `-d`, `--fsdir=DIR` | Directory to use as the VFS root, where all data is persisted. Default: `~/.local/share/meshcored/default` |
| `-e`, `--erase` | Recursively wipe the VFS root, then start. This is a **full reset**: it also removes the node identity, so the node comes back with a new Repeater ID (see step 5). Never put this in the systemd unit. |
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
sudo usermod -aG dialout meshcore                       # only for a serial GPS, see GPS below
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

### 5. Reconfiguring after first run

Node name, password, and location can be changed via the serial CLI after first boot:

```
set name <name>
set password <password>
set lat <lat>
set lon <lon>
```

There are two levels of reset:

**Prefs only**, keeps the node identity (same Repeater ID). Delete the saved prefs so the INI first-run defaults are re-applied on the next boot:

```sh
sudo rm /var/lib/meshcore/prefs.json
sudo systemctl restart meshcored
```

**Full reset**, also discards the identity, so the node returns with a **new** Repeater ID. This wipes the whole VFS root. The built-in `-e`/`--erase` flag does exactly that before starting, but for the managed service just clear the directory while it is stopped (keep `--erase` out of the unit, see the note below):

```sh
sudo systemctl stop meshcored
sudo rm -rf /var/lib/meshcore/*
sudo systemctl start meshcored
```

> When running **directly** (not under systemd), `meshcored --fsdir /var/lib/meshcore --erase` is the equivalent one-shot full reset. Do **not** add `--erase` to the service unit: systemd re-runs `ExecStart` on every restart, so it would wipe the filesystem and regenerate the identity each time. (The firmware's own `reboot()` strips `--erase` to avoid self-wiping, but that protection does not extend to a systemd restart.)

> **Note:** LoRa radio parameters (`lora_freq`, `lora_bw`, `lora_sf`, `lora_cr`, `lora_tx_power`) are also first-run defaults. After first boot they are saved in `prefs.json` and the INI values are no longer read for those fields. To apply a changed radio parameter, use the CLI (`set freq`, `set sf`, etc.) or reset prefs as above.

## GPS

With `gps_device` configured, the standard MeshCore GPS commands work through
the CLI:

| Command | Effect |
|---------|--------|
| `gps` | Status: on/off, active/deactivated, fix/no-fix, satellite count |
| `gps on` / `gps off` | Enable/disable GPS reading and location telemetry |
| `gps sync` | Force a time re-sync from GPS |
| `gps setloc` | Save the current fix as the node's advertised location |
| `gps advert none\|prefs\|share` | Control whether location is advertised |

At the 1 s read interval, the two `lat …` debug lines printed on every read
dominate the journal on a node with a fix.

### Serial GPS

The daemon opens the device directly and holds it for its whole life. It needs
read access — USB units and the Pi's own UART are usually `dialout`-owned, which
the `dialout` membership from step 3 covers (`sudo usermod -aG dialout meshcore`
for the service user).

Some receivers boot into standby and stay silent until an enable line is driven
high. The L76K on the Waveshare LoRaWAN/GNSS HAT is one: set `gps_en_pin` to its
STANDBY GPIO (4 on that HAT) and the daemon holds it high from startup.

### Disciplining the host clock from GNSS

Holding the device exclusively locks out `gpsd`, and through it `chrony`. On a
Linux node that matters more than on an MCU: the node's clock is the whole host's
clock, and a host with no network and no RTC otherwise boots with a bogus one.

It also matters to the daemon's own timers. ArduLinux derives `millis()` from
`CLOCK_REALTIME` (`gettimeofday()` minus a start offset captured once), so every
step of the system clock shifts `millis()` by the same amount and moves every
mesh deadline already in flight with it. `LinuxRTCClock` therefore refuses a
timestamp older than 2024, refuses a jump of more than 24 h once the clock has
been set, and slews sub-second corrections with `adjtime()` instead of stepping
— but letting chrony own the clock, below, is the better answer.

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
re-check it — and note that
[adding PPS](docs/gnss-pps-hardware.md#handing-pps-to-chrony) inverts how to
tune it.

Verify with `chronyc sources` (a `GPS` line that is no longer `#x`) and
the `gps` CLI command (fix and satellite count).

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

The mod itself — identifying R19, sizing the series resistor, landing the wire on
a Pi header pin, the device-tree overlay, and handing the pulse to chrony (plus
serving the resulting stratum 1 to the LAN) — is in
[docs/gnss-pps-hardware.md](docs/gnss-pps-hardware.md).

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

## Known Gaps / TODO

- **A `gpsd://` host cannot be a bare IPv6 literal**, because `host:port` cannot be split from one unambiguously. Such a value is rejected as an invalid `gps_device` rather than silently misparsed; use a hostname, an IPv4 address, or the `127.0.0.1` default that a local gpsd needs anyway.
- **Config path is hardcoded**, meshcored always loads `/etc/meshcored/meshcored.ini`; there is no flag to point it elsewhere. (The data *path* is separate and configurable: it is the ArduLinux VFS root, set with `--fsdir`.)
- **Only repeater firmware**, there is no `linux_companion` target yet; companion radio support (BLE/serial interface to a phone app) is not implemented for Linux.
- **Serial `erase` command is a no-op**, `formatFileSystem()` returns `false` on Linux, so the interactive serial `erase` command reports failure. To wipe the filesystem, use the `--erase` *startup* flag (or clear the VFS dir) instead, see step 5.
- **No power management**, `board.sleep()` is a no-op; the power-saving loop in `main.cpp` never actually sleeps.
- **Upstream-sync fragility**, the radio wrapper (`LinuxSX1262Wrapper`) implements the `RadioLibWrapper` interface by hand, so it can drift from upstream in two ways: a new **pure-virtual** method breaks the Linux build (e.g. `setParams()`), and a new **virtual-with-default** method silently no-ops on Linux until overridden (e.g. `set`/`getRxBoostedGainMode()`, which reported and applied the wrong state until added). Mirror `CustomSX1262Wrapper` when syncing.
