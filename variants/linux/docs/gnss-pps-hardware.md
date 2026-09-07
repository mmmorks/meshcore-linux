# PPS and NTP on the Waveshare LoRaWAN/GNSS HAT

Hardware notes split out of [../README.md](../README.md), where the `## GPS`
section introduces the mod and links here. None of it is reachable from
meshcored: it is a soldering, device-tree and chrony guide for one specific
board, kept because getting any of it wrong is expensive and the details are
hard to find.

Read `## GPS` in the README first — it covers `gps_device`, gpsd and the
NMEA-only chrony setup, which is the part that works without touching the
hardware. PPS is a precision layer on top of that, never a replacement.

## Tapping the signal

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

## Landing it on the Pi

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

## Loading the overlay

`/boot/firmware/config.txt`:

```
dtoverlay=pps-gpio,gpiopin=26,pull=down
```

The pull-down matters: whenever the L76K is in standby, or simply not powered
yet, it stops driving its pin 4 and the Pi's input would float. `pull` is not in
every version of the overlay — `dtoverlay -h pps-gpio` lists what yours takes, and
an external 10K to GND does the same job. Leave `assert_falling_edge` at its
default; the L76K marks the top of the second with a **rising** edge, 100 ms wide.

## Verifying the overlay

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

## Handing PPS to chrony

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

## Serving the time to the LAN

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
