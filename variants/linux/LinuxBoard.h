#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
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

  char* spidev = "/dev/spidev0.0";
  char* lora_gpiochip = "gpiochip0";

  float lora_tcxo = 1.8f;

  char *advert_name = "Linux Repeater";
  char *admin_password = "password";
  float lat = 0.0f;
  float lon = 0.0f;
  char *gps_device = "";
  int   gps_baud   = 9600;
  int   gps_en_pin = -1;

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

  void reboot() override {
    exit(0);
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
public:
  LinuxRTCClock() { }
  void begin() {
  }
  uint32_t getCurrentTime() override {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
  }
  void setCurrentTime(uint32_t time) override {
    struct timeval tv;
    tv.tv_sec = time;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
  }
};
