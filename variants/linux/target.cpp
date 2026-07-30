#include <Arduino.h>
#include "target.h"
#include "LinuxConsole.h"
#include "LinuxEventLoop.h"

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
RADIO_CLASS radio = new Module(hal, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC);
WRAPPER_CLASS radio_driver(radio, board);

LinuxRTCClock rtc_clock;
LinuxSerialStream gps_serial;
MicroNMEALocationProvider gps_location(gps_serial, &rtc_clock, -1, -1, NULL);
EnvironmentSensorManager sensors(gps_location);

bool linux_gps_present() {
  return gps_serial.isOpen();
}

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  rtc_clock.begin();

  if (board.config.gps_device && board.config.gps_device[0] != '\0') {
    gps_serial.begin(board.config.gps_device, board.config.gps_baud);
  }

  radio = new Module(hal, board.config.lora_nss_pin, board.config.lora_irq_pin, board.config.lora_reset_pin, board.config.lora_busy_pin);
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

void linux_event_wait() {
  LinuxEventSource* src = board.irqEventSource();

  // 10 ms is far coarser than any deadline that is not already delivered on
  // the IRQ descriptor: DIO1 carries both RX-done and TX-done, while CAD retry
  // is 120-480 ms and noise-floor calibration is 2 s. Without edge detection
  // there is nothing to wake us, so fall back to a tight 1 ms poll.
  const bool have_events = (src != NULL && src->eventFd() >= 0);
  const int  timeout_ms  = have_events ? 10 : 1;

  EventLoop.reset();
  EventLoop.setEventSource(src);
  EventLoop.registerFd(Console.serverFd());
  EventLoop.registerFd(Console.clientFd());
  EventLoop.registerFd(Console.stdinFd());
  EventLoop.registerFd(gps_serial.fd());
  EventLoop.wait(timeout_ms);
}
