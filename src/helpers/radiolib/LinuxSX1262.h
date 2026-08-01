#pragma once

#include <RadioLib.h>
#include "MeshCore.h"
#include "CustomSX1262.h"
#include "SX126xReset.h"
// For the LinuxBoard definition behind `board` below. Reached transitively via
// target.h today, but named here so this header does not depend on include
// order -- LinuxSX1262Wrapper.h calls methods on it.
#include "LinuxBoard.h"

#define SX126X_PREAMBLE_LENGTH 16

extern LinuxBoard board;

// The Linux build's SX1262.
//
// Everything that is not Linux-specific comes from CustomSX1262 unchanged --
// the RX watchdog (startReceive/isReceiving and their preamble/header
// deadlines), the millis setters, the RX-boost readback. That logic is shared
// with every SX126x board and upstream keeps fixing it, so forking it here
// would mean re-copying each fix by hand and silently missing the ones nobody
// notices.
//
// What genuinely differs is initialisation: the MCU variants pick frequency,
// regulator, RF-switch and gain per board at compile time, but one binary here
// serves every HAT, so all of it comes from meshcored.ini at runtime.
class LinuxSX1262 : public CustomSX1262 {
  public:
    LinuxSX1262(Module *mod) : CustomSX1262(mod) { }

    // Shadows (not overrides) CustomSX1262::std_init(), which is non-virtual
    // and reads LORA_*/SX126X_* build flags this variant does not define.
    bool std_init(SPIClass* spi = NULL)
    {
      const LinuxConfig& config = board.config;

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
      sx126xApplyRxSettings(this, rxSettings());
      if (config.lora_rxen_pin != RADIOLIB_NC || config.lora_txen_pin != RADIOLIB_NC) {
        setRfSwitchPins(config.lora_rxen_pin, config.lora_txen_pin);
      }

      MESH_DEBUG_PRINTLN("SX1262 status=0x%02X device_errors=0x%04X", getStatus(), getDeviceErrors());

      return true;
    }

    // The RX settings for this node, as configured in meshcored.ini. Applied at
    // init and re-applied after every AGC reset, both via sx126xApplyRxSettings()
    // -- which is where the MCU variants instead read their SX126X_* build flags.
    SX126xRxSettings rxSettings() const {
      SX126xRxSettings s;
      s.dio2_as_rf_switch = board.config.dio2_as_rf_switch;
      s.rx_boosted_gain   = board.config.rx_boosted_gain;
      s.register_patch    = board.config.rx_register_patch;
      return s;
    }
};
