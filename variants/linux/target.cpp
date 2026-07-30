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

  // Register exactly the descriptor(s) LinuxConsole::rawReadByte() will
  // actually consume this iteration -- it accepts a new client and reads
  // stdin only when no client is connected, and otherwise reads only the
  // connected client. Registering all three unconditionally would leave the
  // listening socket (and stdin) permanently POLLIN once a client is
  // attached, since nothing here would ever drain them: that reinstates the
  // busy loop for as little as one concurrent `meshcorectl`. Keep this in
  // step with rawReadByte()'s precedence if it ever changes.
  if (Console.clientFd() < 0) {
    EventLoop.registerFd(Console.serverFd());
    EventLoop.registerFd(Console.stdinFd());
  } else {
    EventLoop.registerFd(Console.clientFd());
  }

  // Deliberately NOT registering gps_serial.fd() here. EnvironmentSensorManager
  // only drains the GPS stream when gps_active is true (initBasicGPS() leaves
  // it false until an operator runs `gps on`; PERSISTANT_GPS is not defined
  // for this variant), so a registered-but-undrained GPS descriptor would sit
  // POLLIN for as long as the module keeps streaming -- which on Linux it
  // always does: MicroNMEALocationProvider::stop() is a no-op here and
  // gps_en_pin (when configured) stays high for the whole run. That would
  // reinstate the 100% CPU spin this event loop exists to remove, for every
  // node with gps_device set and GPS not actively toggled on -- the
  // documented default. There is also nothing to gain from waking on it: at
  // 9600 baud (~960 B/s) against a ~4 KB tty input buffer, the 10 ms poll
  // ceiling drains NMEA with three orders of magnitude of margin even when
  // gps_active is true and EnvironmentSensorManager::loop() is polled from
  // the timeout alone. DO NOT add EventLoop.registerFd(gps_serial.fd()) back
  // in, even though it looks like the obviously-correct thing to do for a
  // node that streams GPS -- it is the single line that reintroduces this
  // branch's namesake bug in the default configuration.

  // Refresh the cached IRQ level immediately before blocking. Packet
  // correctness does not come from the edge-event descriptor above; it comes
  // from ArduLinux's gpioIdle(), which fires RadioLib's ISR on a LOW->HIGH
  // transition against a *cached* previous level. Nothing else in the
  // MeshCore call path refreshes that cache (no delay() calls in
  // Dispatcher.cpp/Mesh.cpp/MyMesh.cpp, and RadioLib's own
  // digitalRead(getIrq()) calls live only in blocking paths MeshCore doesn't
  // use), so the cache is stale from the moment gpioIdle() handles an
  // interrupt until the next iteration's gpioIdle() call -- today that heals
  // safely because the 10 ms timeout is comfortably shorter than any packet's
  // airtime. That margin is a coupling, not a guarantee: raising the timeout
  // further to cut CPU would silently stop RX rather than merely add latency,
  // since DIO1 would stay latched HIGH with no further rising edge to recover
  // on. This line decouples the timeout ceiling from packet airtime for good.
  // Cost is one ioctl per wake; it is latency-safe, because if the line is
  // already HIGH here an edge event is already queued and the wait below
  // returns immediately instead of blocking. Do not remove this as
  // "redundant" with gpioIdle() -- it is the only thing keeping a longer
  // timeout safe.
  if (board.config.lora_irq_pin != RADIOLIB_NC) digitalRead(board.config.lora_irq_pin);

  EventLoop.wait(timeout_ms);
}
