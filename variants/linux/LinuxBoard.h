#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <RadioLib.h>
#include <helpers/KeyValueStore.h>

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

  // Upstream lets a variant hang its own prefs off the 'custom' JSON object;
  // this target carries its runtime config in meshcored.ini instead, so this is
  // a no-op, matching ESP32Board/NRF52Board/STM32Board.
  void attachDynamicPrefs(KeyValueStore* prefs) { }

  void sleep(uint32_t secs) override {
    if (secs > 0) {
      ::sleep(secs);
    } else {
      usleep(10000); // 10ms delay to prevent busy loop
    }
  }

  // Re-exec this process image rather than exit. Defined in LinuxBoard.cpp.
  void reboot() override;

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

  // False until this object has stepped the clock once. The first step is
  // exempt from MAX_STEP_USECS below: a Pi with no RTC boots at whatever the
  // last shutdown wrote (or at the epoch) and genuinely needs a jump of years.
  bool _clock_set_once = false;

  // Latches the "refused an implausible timestamp" report, for the same reason
  // _settime_warned is latched: a receiver stuck emitting date-less fixes would
  // otherwise print this on every sync for the life of the daemon.
  bool _rejected_warned = false;

  // Oldest timestamp worth installing. MicroNMEA takes the date from RMC while
  // isValid() is satisfied by GGA alone, so a date-less fix leaves year 0 and
  // DateTime(0,...).unixtime() lands on 2000-01-01 -- a plausible-looking value
  // that is a parse artefact, not a time. Anything before this variant existed
  // is one of those. A fixed constant rather than __DATE__: a build stamp makes
  // the accepted range depend on when the binary was compiled, which is a
  // surprising thing for a clock bound to do, and reproducible builds pin it
  // anyway.
  static constexpr uint32_t CLOCK_FLOOR_SECS = 1704067200u;   // 2024-01-01T00:00:00Z

  // Largest step accepted after the first one. A node that has already been set
  // once is within seconds of correct; a jump of more than a day is a bad fix
  // or a hostile `time <epoch>` from a mesh peer, not a correction.
  static constexpr int64_t MAX_STEP_USECS = (int64_t) 24 * 60 * 60 * 1000000;

  // Below this, slew instead of stepping -- see the comment in setCurrentTime().
  static constexpr int64_t SLEW_LIMIT_USECS = 1000000;

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

    // Everything below exists because on this platform CLOCK_REALTIME is also
    // the timebase millis() is derived from: ardulinux computes millis() as
    // gettimeofday() minus a start offset captured once (cores/ardulinux/linux/
    // millis.cpp). So a settimeofday() does not merely retime the host, it
    // shifts millis() by the same delta -- and every deadline already in flight
    // in Dispatcher::millisHasNowPassed(), the CAD retry and the delayed-inbound
    // queue moves with it. A backwards step un-expires them; a step back past
    // the start offset makes `millis() - startMsec` underflow and every deadline
    // comparison in the tree garbage. This is a safety bound on that, not a
    // time discipline: chrony is the right answer, hence defer_clock above.
    struct timeval now;
    gettimeofday(&now, NULL);

    if (time < CLOCK_FLOOR_SECS) {
      // MicroNMEA's year comes from RMC while isValid() can be satisfied by GGA
      // alone, so a date-less fix yields year 0 -> 2000-01-01, and time_valid
      // gates on fix age rather than on the date. Refusing beats a 25-year step
      // backwards.
      if (!_rejected_warned) {
        _rejected_warned = true;
        printf("WARNING: refusing to set the system clock to %u -- implausibly old\n"
               "         (before %u). A GPS fix without a date reads as year 2000.\n",
               (unsigned) time, (unsigned) CLOCK_FLOOR_SECS);
      }
      return;
    }

    int64_t delta_us = ((int64_t) time - (int64_t) now.tv_sec) * 1000000 - (int64_t) now.tv_usec;
    int64_t abs_us = delta_us < 0 ? -delta_us : delta_us;

    if (_clock_set_once && abs_us > MAX_STEP_USECS) {
      // Past the first set this node is within seconds of correct, so a jump of
      // more than a day is a bad fix or a `time <epoch>` from a mesh peer.
      if (!_rejected_warned) {
        _rejected_warned = true;
        printf("WARNING: refusing to set the system clock to %u -- more than 24 h\n"
               "         from the current time. Restart meshcored if this is genuine.\n",
               (unsigned) time);
      }
      return;
    }

    if (abs_us < SLEW_LIMIT_USECS) {
      // The common case, and the one that must not step. NMEA names a second
      // that has already begun -- measured 0.193-0.384 s late on the Waveshare
      // HAT, see "Disciplining the host clock from GNSS" in README.md -- so
      // MicroNMEALocationProvider's re-sync every TIME_SYNC_INTERVAL is
      // systematically a fraction of a second behind. Stepping would drag the
      // clock, and millis() with it, backwards by that fraction 48 times a day.
      // adjtime() spreads the correction instead, so millis() only ever runs
      // slightly slow or fast.
      struct timeval adj;
      adj.tv_sec  = 0;
      adj.tv_usec = (suseconds_t) delta_us;
      if (adjtime(&adj, NULL) == 0) {
        _clock_set_once = true;
        return;
      }
      // Fall through to the CAP_SYS_TIME warning below: adjtime() needs the
      // same capability settimeofday() does, so this failing means both would.
    } else {
      struct timeval tv;
      tv.tv_sec = time;
      tv.tv_usec = 0;
      if (settimeofday(&tv, NULL) == 0) {
        _clock_set_once = true;
        // Deliberately not latched like the warnings around it: this line's
        // whole value is showing *every* time GPS/mesh sync steps the clock, so a
        // clock fighting something else outside the gpsd case above -- a
        // serial gps_device plus CAP_SYS_TIME, see defer_clock in
        // meshcored.ini -- is visible in the journal instead of silently
        // winning or losing against it.
        printf("NOTE: system clock stepped to %u by mesh/GPS time sync.\n", (unsigned) time);
        return;
      }
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
