#pragma once

#include <RadioLib.h>
#include "MeshCore.h"
// For the LinuxBoard definition behind `board` below. Reached transitively via
// target.h today, but named here so this header does not depend on include
// order -- LinuxSX1262Wrapper.h calls methods on it.
#include "LinuxBoard.h"

#define SX126X_PREAMBLE_LENGTH 16

extern LinuxBoard board;

class LinuxSX1262 : public SX1262 {
  // RX watchdog state, mirroring CustomSX1262. A preamble- or header-detect
  // IRQ that never completes (the transmission was noise, or the sender went
  // away mid-packet) stays latched, and the old isReceiving() reported "busy"
  // for as long as it did.
  //
  // That stall bites harder on Linux than on an MCU, because isReceiving() is
  // reached through isReceivingPacket(), which gates two things: CSMA, and the
  // noise-floor sampler's in-packet skip in RadioLibWrapper::loop(). A stuck
  // flag therefore freezes the floor estimate at whatever it last held *and*
  // defers every transmit until getCADFailMaxDuration() expires.
  //
  // Deadlines come from LinuxSX1262Wrapper::setParams() via
  // calcMaxPacketMillis(); the defaults below only apply before the first
  // setParams() call.
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;

  public:
    LinuxSX1262(Module *mod) : SX1262(mod) { }

    bool std_init(SPIClass* spi = NULL)
    {
      LinuxConfig config = board.config;

      Serial.printf("Radio begin %f %f %d %d %f\n", config.lora_freq, config.lora_bw, config.lora_sf, config.lora_cr, config.lora_tcxo);
      MESH_DEBUG_PRINTLN("SX1262 regulator requested: %s", config.use_regulator_ldo ? "LDO" : "DC-DC");
      int status = begin(config.lora_freq, config.lora_bw, config.lora_sf, config.lora_cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, config.lora_tx_power, SX126X_PREAMBLE_LENGTH, config.lora_tcxo, config.use_regulator_ldo);
      // if radio init fails with -707/-706, try again with tcxo voltage set to 0.0f
      if (status == RADIOLIB_ERR_SPI_CMD_FAILED || status == RADIOLIB_ERR_SPI_CMD_INVALID) {
        MESH_DEBUG_PRINTLN("SX1262 init failed with error %d, retrying with TCXO at 0.0V", status);
        status = begin(config.lora_freq, config.lora_bw, config.lora_sf, config.lora_cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, config.lora_tx_power, SX126X_PREAMBLE_LENGTH, 0.0f, config.use_regulator_ldo);
      }
      if (status != RADIOLIB_ERR_NONE) {
        Serial.print("ERROR: radio init failed: ");
        Serial.println(status);
        return false;  // fail
      }

      setCRC(1);

      setCurrentLimit(config.current_limit);
      setDio2AsRfSwitch(config.dio2_as_rf_switch);
      setRxBoostedGainMode(config.rx_boosted_gain);
      if (config.lora_rxen_pin != RADIOLIB_NC || config.lora_txen_pin != RADIOLIB_NC) {
        setRfSwitchPins(config.lora_rxen_pin, config.lora_txen_pin);
      }
      applyRegisterPatch();

      MESH_DEBUG_PRINTLN("SX1262 status=0x%02X device_errors=0x%04X", getStatus(), getDeviceErrors());

      return true;
    }

    // The 0x8B5 RX-sensitivity patch that the MCU variants apply under
    // SX126X_REGISTER_PATCH (added there for the Heltec v4). A named method
    // rather than an inline block in std_init() because the AGC reset has to
    // re-apply it: the calibration in sx126xResetAGC() does not preserve it.
    void applyRegisterPatch() {
      if (!board.config.rx_register_patch) return;

      uint8_t r_data = 0;
      readRegister(0x8B5, &r_data, 1);
      r_data |= 0x01;
      writeRegister(0x8B5, &r_data, 1);
    }

    int16_t startReceive() override {
      // Latch PREAMBLE_DETECTED in the IRQ status register so isReceiving() can
      // see it. Only the *flags* argument gains the bit; the DIO1 routing mask
      // is left at the RX default, so this adds no new interrupt on the line
      // that EventGPIOPin/LinuxEventLoop block on -- the event loop keeps waking
      // on rx-done and tx-done exactly as before.
      return SX1262::startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF,
                                  RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED),
                                  RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    }

    bool isReceiving() {
      uint32_t irq = getIrqFlags();
      bool preamble = irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED; // bit 2
      bool header   = irq & RADIOLIB_SX126X_IRQ_HEADER_VALID;      // bit 4
      bool hdrErr   = irq & RADIOLIB_SX126X_IRQ_HEADER_ERR;        // bit 5
      uint32_t now  = millis();
      if (hdrErr) {
        clearIrqFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID);
        _activityAt = 0;
        _headerSeen = false;
        return false;
      }
      if (!header && _headerSeen) {
        // something cleared the header flag, reset our state.
        _activityAt = 0; _headerSeen = false;
        return false;
      }

      if (header) {
        if (!_headerSeen) { _headerSeen = true; _activityAt = now; };
        if (now - _activityAt > _maxPayloadMillis) {
          MESH_DEBUG_PRINTLN("Clearing header IRQ after %ums", _maxPayloadMillis);
          clearIrqFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID);
          _activityAt = 0; _headerSeen = false;
          return false;
        }
        return true;
      }
      if (preamble) {
        if (_activityAt == 0) _activityAt = now;
        if (now - _activityAt > _preambleMillis) {
          clearIrqFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED);
          _activityAt = 0;
          MESH_DEBUG_PRINTLN("Clearing preamble IRQ after %ums", _preambleMillis);

          return false;
        }
        return true;
      }
      _activityAt = 0; _headerSeen = false;
      return false;
    }

    void setPreambleMillis(uint32_t preambleMillis) {
      _preambleMillis = preambleMillis;
      MESH_DEBUG_PRINTLN("Set _preambleMillis=%u", _preambleMillis);
    }
    void setMaxPayloadMillis(uint32_t payloadMillis) {
      _maxPayloadMillis = payloadMillis;
      MESH_DEBUG_PRINTLN("Set _maxPayloadMillis=%u", _maxPayloadMillis);
    }

    bool getRxBoostedGainMode() {
      uint8_t rxGain = 0;
      readRegister(RADIOLIB_SX126X_REG_RX_GAIN, &rxGain, 1);
      return (rxGain == RADIOLIB_SX126X_RX_GAIN_BOOSTED);
    }
};
