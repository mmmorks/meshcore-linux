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
- **First-run node defaults**: `advert_name`, `admin_password`, `lat`, `lon`. On the first boot these are saved to the node's persisted prefs (`prefs.json`). After that, use the console CLI to change them (`set name`, `set password`, etc.; see [§5](#5-reconfiguring-after-first-run)), the INI values are no longer consulted for these fields.

Key settings:

| Key | Default | Notes |
|-----|---------|-------|
| `spidev` | `/dev/spidev0.0` | SPI device node |
| `lora_gpiochip` | `gpiochip0` | Name of the `/dev/gpiochip*` device (or kernel label). `gpiochip0` is correct for Pi 3/4/Zero 2W; Pi 5 may need `gpiochip4` or `pinctrl-rp1` depending on kernel |
| `console_path` | (auto) | Where to publish the CLI console. Unset, the daemon picks `/run/meshcored/console` under the systemd unit, else a per-user path; see [The control CLI](#the-control-cli) |
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
| `use_regulator_ldo` | `0` | `1` powers the radio from the LDO instead of the DC-DC converter. Only for modules built without the DC-DC inductor |
| `rx_register_patch` | `0` | `1` applies the SX126x RX-sensitivity patch (bit 0 of register `0x8B5`). Try it if a HAT receives poorly |
| `advert_name` | `"Linux Repeater"` | Node name, first-run default only |
| `admin_password` | `"password"` | Admin password, **change this**, first-run default only |
| `lat` / `lon` | `0.0` | GPS coordinates for advertisement, first-run default only |

Comments (`#`, `;`), blank lines and `[section]` headers are ignored. Boolean
settings (`dio2_as_rf_switch`, `rx_boosted_gain`, `use_regulator_ldo`,
`rx_register_patch`) accept `1`/`0`, `true`/`false`, `on`/`off` or `yes`/`no`,
case-insensitively; anything else is a fatal invalid value.

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

> Under the unit the CLI console appears at `/run/meshcored/console` with no
> INI change: the daemon finds the unit's `RuntimeDirectory` on its own. See
> [§5](#5-reconfiguring-after-first-run).

### 5. Reconfiguring after first run

`meshcored` exposes a local CLI console, kept separate from the logs (which go
to stdout / journald). Under the systemd unit it is `/run/meshcored/console`.
Connect with [`meshcore-cli`](https://github.com/fdlamotte/meshcore-cli), or any
serial terminal (see [The control CLI](#the-control-cli)):

```sh
sudo meshcore-cli -r -s /run/meshcored/console
```

```
set name <name>
set password <password>
set lat <lat>
set lon <lon>
set freq 910.525
set sf 7
```

> **Logs are separate from the console.** Debug logs (`MESH_DEBUG`, on in this
> experimental build) go to stdout → journald, *not* to the console, so it shows
> only your commands and their replies. Those commands/replies are also copied to
> the log, so `journalctl -u meshcored -f` still records what was run.

> **Security:** connecting to the console grants the privileged, *unauthenticated*
> local CLI — it can change the radio, read the private key (`get prv.key`), erase
> state, etc. The console is owner-only (mode `0600`), so keep the daemon's user
> (`root`/`meshcore`) trusted. Where the console is published is described in
> [The control CLI](#the-control-cli).

Logs stream to journald (`sudo journalctl -u meshcored -f`); the daemon
line-buffers stdout itself, so no `stdbuf` wrapper is needed.

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

> **Note:** LoRa radio parameters (`lora_freq`, `lora_bw`, `lora_sf`, `lora_cr`, `lora_tx_power`) are also first-run defaults. After first boot they are saved in `prefs.json` and the INI values are no longer read for those fields. To apply a changed radio parameter on a running node, use the console CLI (`set freq`, `set sf`, etc.; see [§5](#5-reconfiguring-after-first-run)) or reset prefs as above.

## The control CLI

Everything the MeshCore docs describe as the "serial CLI" — `set`, `get`,
`advert`, `neighbors`, and the rest — is reached here through the console.

The Arduino `Serial` object is output-only on Linux, so `meshcored` prints its
logs to stdout and serves the CLI on a **pseudo-terminal**, published as a
symlink at a stable path. `console_path` in `meshcored.ini` sets it; unset, the
first of these that can be published wins, and startup logs which one it was:

| Order | Path | When |
|-------|------|------|
| 1 | `console_path`, if set | |
| 2 | `/run/meshcored/console` | the directory exists and is writable: the unit's `RuntimeDirectory=` |
| 3 | `$XDG_RUNTIME_DIR/meshcore/console` | running directly as a logged-in user |
| 4 | `/tmp/meshcore-<uid>/console` | neither of the above |

The device behind the symlink is mode `0600` and owned by the user running the
daemon, so reaching it means being that user or root. A path is skipped, with a
line on stderr, if another live `meshcored` already holds it, if something that
is not a symlink sits there, or if its parent directory is not owned by the
daemon's own user with no group or other permissions — anyone who can write that
directory can point `console` at a terminal of their own and read what the
operator types at it. (The unit's `RuntimeDirectoryMode=0700` satisfies that;
so do the `0700` directories the daemon creates for the per-user paths.) A
symlink left by a crashed daemon is reclaimed. `reboot` unpublishes the console
before re-executing, so the new process starts from a clean path.

Because the console is a terminal device, any serial tool can attach:
`meshcore-cli -r -s <path>`, `screen`, `minicom`, `picocom`. There is no
single-client rule: two tools attached at once share one CLI and see each
other's traffic.

Running `meshcored` in a **foreground** terminal, you can also type commands
straight into its stdin. Replies are printed to stdout either way; a command
typed at the terminal is not copied to the console, and a command sent over the
console is answered there. The terminal is put in raw mode for this and handed
back as it was, on `Ctrl-C` and `SIGTERM` as well as on a clean exit. Started in
the background (`meshcored &`, or under systemd) it leaves stdin alone
altogether — reading a terminal from a background job would only stop the job —
so there the console is the way in.

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

## Known Gaps / TODO

- **Config path is hardcoded**, meshcored always loads `/etc/meshcored/meshcored.ini`; there is no flag to point it elsewhere. (The data *path* is separate and configurable: it is the ArduLinux VFS root, set with `--fsdir`.)
- **Only repeater firmware**, there is no `linux_companion` target yet; companion radio support (BLE/serial interface to a phone app) is not implemented for Linux.
- **Serial `erase` command is a no-op**, `formatFileSystem()` returns `false` on Linux, so the interactive serial `erase` command reports failure. To wipe the filesystem, use the `--erase` *startup* flag (or clear the VFS dir) instead, see step 5.
- **No suspend or wake-on-radio**, `board.sleep()` is an ordinary `sleep(3)`: the process idles, but the host is not suspended and nothing arms a wake source, so `powersaving_enabled` saves no more power than the normal idle described under [Idle CPU usage](#idle-cpu-usage). It also cannot be interrupted by an incoming packet, so leaving it off is the better default on Linux.
- **libgpiod v2 is compile-verified only.** `EventGPIOPin`'s v2 path (Debian
  trixie and newer) builds cleanly in the trixie `build-docker.sh` container, but has never been
  exercised at runtime against real hardware — all runtime verification to date is
  on libgpiod v1 (bookworm). Treat it as unproven.
