#include <Arduino.h>
#include "target.h"

class ArduLinuxHal : public ArduinoHal
{
public:
  ArduLinuxHal(SPIClass &spi, SPISettings spiSettings) : ArduinoHal(spi, spiSettings){};
  void spiTransfer(uint8_t *out, size_t len, uint8_t *in) {
    memcpy(in, out, len);
    spi->transfer(in, len);
  }
};

LinuxBoard board;

SPISettings spiSettings = SPISettings(2000000, MSBFIRST, SPI_MODE0);
ArduinoHal *hal = new ArduLinuxHal(SPI, spiSettings);

// A Module's pins are fixed at construction -- RadioLib keeps them private with
// only getters -- and this variant does not learn them until meshcored.ini has
// been read, which happens in board.begin(), long after static init. So the
// radio built here is a placeholder whose only job is to exist at static-init
// time, because radio_driver binds a reference to it; radio_init() rebuilds it
// with the real pins.
//
// Keeping the Module in a named pointer rather than inlining `new Module(...)`
// is what makes that rebuild releasable instead of a leak: getMod() is
// protected, so once the placeholder is overwritten there is otherwise no way
// left to reach it.
static Module *radio_module = new Module(hal, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC);
RADIO_CLASS radio = radio_module;
WRAPPER_CLASS radio_driver(radio, board);

LinuxRTCClock rtc_clock;
LinuxGpsStream gps_serial;
MicroNMEALocationProvider gps_location(gps_serial, &rtc_clock, -1, -1, NULL);
EnvironmentSensorManager sensors(gps_location);

bool linux_gps_present() {
  return gps_serial.isPresent();
}

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  rtc_clock.begin();

  // begin() dispatches on the device string, so the empty-means-disabled test
  // lives inside it rather than here.
  gps_serial.begin(board.config.gps_device, board.config.gps_baud);

  // Has to follow begin(), which is what establishes the transport. The
  // transport-derived default only catches the gpsd:// case; `defer_clock` in
  // meshcored.ini overrides it explicitly for setups the transport string
  // cannot see for itself (e.g. a serial gps_device the operator is feeding
  // to chrony some other way, alongside CAP_SYS_TIME).
  bool defer = board.config.defer_clock >= 0
             ? (board.config.defer_clock != 0)
             : (gps_serial.transport() == LinuxGpsStream::GPSD_SOCKET);
  rtc_clock.setExternallyDisciplined(defer);

  // Rebuild the radio on a Module carrying the configured pins. Assigning over
  // the object rather than replacing it is deliberate and required: radio_driver
  // holds a reference to `radio`, so the address has to stay put. Only the
  // Module underneath is swapped, and the placeholder one is freed rather than
  // orphaned.
  delete radio_module;
  radio_module = new Module(hal, board.config.lora_nss_pin, board.config.lora_irq_pin, board.config.lora_reset_pin, board.config.lora_busy_pin);
  radio = radio_module;
  return radio.std_init(&SPI);
}

uint32_t radio_get_rng_seed() {
  return radio.random(0x7FFFFFFF);
}

void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio.setFrequency(freq);
  radio.setSpreadingFactor(sf);
  radio.setBandwidth(bw);
  radio.setCodingRate(cr);
}

void radio_set_tx_power(uint8_t dbm) {
  radio.setOutputPower(dbm);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
