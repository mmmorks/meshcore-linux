#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <RadioLib.h>
#include "LinuxEventSource.h"

class LinuxConfig {
public:
  float lora_freq = LORA_FREQ;
  float lora_bw = LORA_BW;
  uint8_t lora_sf = LORA_SF;
#ifdef LORA_CR
  uint8_t lora_cr = LORA_CR;
#else
  uint8_t lora_cr = 5;
#endif

  uint32_t lora_irq_pin = RADIOLIB_NC;
  uint32_t lora_reset_pin = RADIOLIB_NC;
  uint32_t lora_nss_pin = RADIOLIB_NC;
  uint32_t lora_busy_pin = RADIOLIB_NC;
  uint32_t lora_rxen_pin = RADIOLIB_NC;
  uint32_t lora_txen_pin = RADIOLIB_NC;

  int8_t lora_tx_power = 22;
  float current_limit = 140;
  bool dio2_as_rf_switch = false;
  bool rx_boosted_gain = true;

  // The MCU variants pick these per board at compile time, via the
  // SX126X_USE_REGULATOR_LDO and SX126X_REGISTER_PATCH build flags. One binary
  // here serves every HAT, so they are runtime config instead. Defaults match
  // the compile-time defaults: DC-DC, no patch.
  bool use_regulator_ldo = false;
  bool rx_register_patch = false;

  const char* spidev = "/dev/spidev0.0";
  const char* lora_gpiochip = "gpiochip0";

  float lora_tcxo = 1.8f;

  const char *advert_name = "Linux Repeater";
  const char *admin_password = "password";
  float lat = 0.0f;
  float lon = 0.0f;
  const char *gps_device = "";
  int   gps_baud   = 9600;
  int   gps_en_pin = -1;

  // Transport-derived by default (see the setExternallyDisciplined() call in
  // target.cpp): -1 leaves that default alone. An explicit `defer_clock =
  // true/false` in meshcored.ini pins it to 1/0 regardless of gps_device's
  // transport -- needed because gpsd is not the only way this process can end
  // up racing something else that owns the clock (e.g. a serial gps_device
  // plus CAP_SYS_TIME), and the transport string alone cannot see that.
  int8_t defer_clock = -1;

  // Outcome of parsing meshcored.ini. Two failure kinds, kept apart because
  // they deserve opposite responses (see LinuxBoard::begin()): a value the
  // operator wrote that could not be honoured, versus a key nothing consumes.
  struct LoadResult {
    bool opened       = false;  // false: the file could not be read at all
    int  bad_values   = 0;      // values that failed validation
    int  unknown_keys = 0;      // keys nothing consumes; ignored
  };

  LoadResult load(const char *filename);
};

class LinuxBoard : public mesh::MainBoard {
protected:
  uint8_t startup_reason;
  uint8_t btn_prev_state;

public:
  void begin();

  uint16_t getBattMilliVolts() override {
    return 0;
  }

  uint8_t getStartupReason() const override { return startup_reason; }

  const char* getManufacturerName() const override {
    return "Linux";
  }

  int buttonStateChanged() {
    return 0;
  }

  void powerOff() override {
    exit(0);
  }

  // The ardulinux core's global ::reboot() (cores/ardulinux/main.cpp) re-execs
  // this same process via execv(), with --erase stripped from argv so a
  // reboot cannot re-trigger a filesystem wipe. exit(0) here would be wrong:
  // the shipped systemd unit uses Restart=on-failure, not Restart=always, so
  // a clean exit is a stop, not a restart -- and both the `reboot` and
  // `clkreboot` CLI commands reach this from an authenticated remote admin
  // over the mesh (CommonCLI::handleCommand), so that would take an
  // unattended repeater off-air. Qualified as ::reboot() to resolve to the
  // global one and not recurse into this member of the same name.
  void reboot() override {
    ::reboot();
  }

  // Block on the LoRa IRQ edge descriptor plus whichever console descriptors
  // will actually be drained this iteration, instead of spinning. Defined in
  // LinuxBoard.cpp; see variants/linux/LinuxEventLoop.h for the poll wrapper.
  void idleUntilEvent(uint32_t max_wait_ms) override;

  // Sleep until the LoRa IRQ line goes high or timeout_ms elapses, returning
  // true if it went high. The narrow sibling of idleUntilEvent(): same event
  // source, same fallback, but it watches only the radio and is driven by a
  // caller that has just armed a specific operation and needs its completion.
  //
  // Used for hardware CAD, where RadioLib's own scanChannel() would otherwise
  // busy-spin on the line with no deadline. Returns false immediately when no
  // IRQ pin is configured; the caller reads the result over SPI either way.
  bool waitForRadioIrq(uint32_t timeout_ms);

  // Wake-up source for the Linux event loop (the LoRa IRQ line), or NULL when
  // edge detection is unavailable. Typed as the abstract interface so this
  // header stays free of any libgpiod dependency.
  LinuxEventSource* irqEventSource() const { return irq_event_source; }

protected:
  LinuxEventSource* irq_event_source = nullptr;

public:
  LinuxConfig config;
};

class LinuxRTCClock : public mesh::RTCClock {
  // Latches the first settimeofday() failure. The callers are on timers (GPS
  // time sync, the mesh clock correction), so an unlatched report would repeat
  // for the life of the daemon.
  bool _settime_warned = false;

  // Set when the host clock is disciplined by something else (chrony, fed by
  // gpsd). Then this is not merely futile but wrong to attempt.
  bool _externally_disciplined = false;

public:
  LinuxRTCClock() { }
  void begin() {
  }

  // Declare that the host clock belongs to another daemon. Both callers of
  // setCurrentTime() take their timestamp from somewhere we should not be
  // acting on in that case: GPS time sync duplicates what chrony already does
  // better, and `clock sync` / `time <epoch>` carry a value supplied by a
  // REMOTE MESH PEER. Refusing here makes "a LoRa peer cannot retime this
  // host" true by configuration rather than by the accident of the shipped
  // unit happening to lack CAP_SYS_TIME.
  void setExternallyDisciplined(bool yes) { _externally_disciplined = yes; }
  uint32_t getCurrentTime() override {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
  }
  void setCurrentTime(uint32_t time) override {
    if (_externally_disciplined) {
      if (!_settime_warned) {
        _settime_warned = true;
        printf("NOTE: not setting the system clock -- another clock source owns it\n"
               "      (gps_device names gpsd, or defer_clock = true in meshcored.ini).\n"
               "      GPS and mesh time sync are ignored here by design; getCurrentTime()\n"
               "      still reads the host clock.\n");
      }
      return;
    }

    struct timeval tv;
    tv.tv_sec = time;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) == 0) {
      // Deliberately not deduped like the messages below: this line's whole
      // value is showing *every* time GPS/mesh sync steps the clock, so a
      // clock fighting something else outside the gpsd case above -- a
      // serial gps_device plus CAP_SYS_TIME, see defer_clock in
      // meshcored.ini -- is visible in the journal instead of silently
      // winning or losing against it.
      printf("NOTE: system clock set to %u by mesh/GPS time sync.\n", (unsigned) time);
      return;
    }

    // Unlike an MCU, this is the whole host's clock, and setting it needs
    // CAP_SYS_TIME. The shipped unit runs as an unprivileged `meshcore` user
    // with NoNewPrivileges=yes, so it does not have it and this always fails --
    // `clock sync` and GPS time sync would otherwise report success and do
    // nothing at all. Not fatal: the host's clock is NTP's job on a Linux box,
    // and getCurrentTime() reads it correctly either way.
    if (!_settime_warned) {
      _settime_warned = true;
      printf("WARNING: cannot set the system clock (%s); mesh/GPS time sync will\n"
             "         not take effect. This is expected under the shipped systemd\n"
             "         unit (unprivileged, no CAP_SYS_TIME) -- keep the host on NTP.\n",
             strerror(errno));
    }
  }
};
