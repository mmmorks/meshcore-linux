#pragma once

#include <RadioLib.h>

// RX settings that Calibrate(0x7F) does not preserve, so every reset has to
// restate them. Boards that fix these at build time (every MCU variant) do not
// need this -- see sx126xResetAGC() below, which reads their SX126X_* flags
// directly. It exists for targets configured at runtime instead: the Linux
// daemon serves every HAT from one binary, so its values come from
// meshcored.ini and cannot be macros.
struct SX126xRxSettings {
  bool dio2_as_rf_switch = false;
  bool rx_boosted_gain   = false;
  bool register_patch    = false;   // 0x8B5 RX-sensitivity patch
};

// Full receiver reset for all SX126x-family chips (SX1262, SX1268, LLCC68, STM32WLx).
// Warm sleep powers down analog, Calibrate(0x7F) refreshes ADC/PLL/image calibration,
// then re-applies RX settings that calibration may reset.
//
// `rx` overrides where those settings come from. Left NULL -- every MCU variant
// -- the SX126X_* build flags are used exactly as before. Passing settings makes
// this the single place they are applied, rather than having the caller re-apply
// them afterwards: a setting added here then reaches runtime-configured targets
// too, instead of being silently dropped on them until someone notices.
inline void sx126xResetAGC(SX126x* radio, const SX126xRxSettings* rx = NULL) {
  radio->sleep(true);
  radio->standby(RADIOLIB_SX126X_STANDBY_RC, true);

  uint8_t calData = RADIOLIB_SX126X_CALIBRATE_ALL;
  radio->mod->SPIwriteStream(RADIOLIB_SX126X_CMD_CALIBRATE, &calData, 1, true, false);
  radio->mod->hal->delay(5);
  uint32_t start = millis();
  while (radio->mod->hal->digitalRead(radio->mod->getGpio())) {
    if (millis() - start > 50) break;
    radio->mod->hal->yield();
  }

  // Calibrate(0x7F) defaults image calibration to 902-928MHz band.
  // Re-calibrate for the actual operating frequency.
  radio->calibrateImage(radio->freqMHz);

  if (rx) {
    radio->setDio2AsRfSwitch(rx->dio2_as_rf_switch);
    radio->setRxBoostedGainMode(rx->rx_boosted_gain);
    if (rx->register_patch) {
      uint8_t r_data = 0;
      radio->readRegister(0x8B5, &r_data, 1);
      r_data |= 0x01;
      radio->writeRegister(0x8B5, &r_data, 1);
    }
    return;
  }

#ifdef SX126X_DIO2_AS_RF_SWITCH
  radio->setDio2AsRfSwitch(SX126X_DIO2_AS_RF_SWITCH);
#endif
#ifdef SX126X_RX_BOOSTED_GAIN
  radio->setRxBoostedGainMode(SX126X_RX_BOOSTED_GAIN);
#endif
#ifdef SX126X_REGISTER_PATCH
  uint8_t r_data = 0;
  radio->readRegister(0x8B5, &r_data, 1);
  r_data |= 0x01;
  radio->writeRegister(0x8B5, &r_data, 1);
#endif
}
