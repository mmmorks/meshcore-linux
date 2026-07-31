#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <exception>
#ifdef ARDULINUX_HARDWARE
#include "linux/gpio/LinuxGPIOPin.h"
#endif
#ifdef ARDULINUX_HARDWARE
#include "EventGPIOPin.h"
#endif
#include "LinuxBoard.h"
#include "LinuxConsole.h"
#include "LinuxEventLoop.h"
#include "LinuxRadioWait.h"
#include "AppInfo.h"

const char *ardulinuxAppName        = "meshcored";
const char *ardulinuxAppDescription = "a meshcore daemon for linux";
const char *ardulinuxAppBugAddress  = "https://github.com/meshcore-dev/MeshCore";

int initGPIOPin(uint8_t pinNum, const std::string gpioChipName, uint8_t line)
{
#ifdef ARDULINUX_HARDWARE
  char gpio_name[32];
  snprintf(gpio_name, sizeof(gpio_name), "GPIO%d", pinNum);

  try {
    GPIOPin *csPin;
    csPin = new LinuxGPIOPin(pinNum, gpioChipName.c_str(), line, gpio_name);
    csPin->setSilent();
    gpioBind(csPin);
    return 0;
  } catch (const std::exception& e) {
    printf("ERROR: cannot claim GPIO line %d on %s for pin %d: %s\n",
           (int)line, gpioChipName.c_str(), (int)pinNum, e.what());
    return 1;
  } catch (...) {
    printf("ERROR: cannot claim GPIO line %d on %s for pin %d (unknown exception)\n",
           (int)line, gpioChipName.c_str(), (int)pinNum);
    return 1;
  }
#else
  return 0;
#endif
}

// Bind the LoRa IRQ line as an EventGPIOPin so the main loop can block on its
// edge-event descriptor instead of spinning. Returns 0 on success, 1 on
// failure (same convention as initGPIOPin).
//
// Falling back to a plain LinuxGPIOPin is not needed here: EventGPIOPin only
// throws when the line cannot be acquired at all, and it degrades internally
// when edge detection specifically is unavailable.
static int initEventGPIOPin(LinuxEventSource** out, uint8_t pinNum,
                            const std::string gpioChipName, uint8_t line) {
#ifdef ARDULINUX_HARDWARE
  char gpio_name[32];
  snprintf(gpio_name, sizeof(gpio_name), "GPIO%d", pinNum);

  try {
    EventGPIOPin* pin = new EventGPIOPin(pinNum, gpioChipName.c_str(), line, gpio_name);
    pin->setSilent();
    gpioBind(pin);
    *out = pin;
    printf("LoRa IRQ pin %d bound with edge detection: %s\n",
           (int)pinNum, pin->hasEdgeDetection() ? "yes" : "NO (polling fallback)");
    return 0;
  } catch (const std::exception& e) {
    printf("ERROR: cannot claim IRQ GPIO line %d on %s for pin %d: %s\n",
           (int)line, gpioChipName.c_str(), (int)pinNum, e.what());
    return 1;
  } catch (...) {
    printf("ERROR: cannot claim IRQ GPIO line %d on %s for pin %d (unknown exception)\n",
           (int)line, gpioChipName.c_str(), (int)pinNum);
    return 1;
  }
#else
  return 0;
#endif
}

void ardulinuxSetup() {
}

void LinuxBoard::begin() {
#ifndef ARDULINUX_HARDWARE
  printf("FATAL: meshcored was built without libgpiod support; all GPIO/I2C\n"
         "       operations would be simulated and the radio cannot be driven.\n"
         "       Install pkg-config and libgpiod-dev on the build machine, clear\n"
         "       the PlatformIO cache, and rebuild:\n"
         "         sudo apt install -y pkg-config libgpiod-dev\n"
         "         rm -rf ~/.platformio/platforms/ardulinux* .pio\n"
         "         pio run -e linux_repeater\n");
  exit(1);
#endif

  config.load("/etc/meshcored/meshcored.ini");

  printf("SPI begin %s\n", config.spidev);
  SPI.begin(config.spidev, 2000000);

  printf("LoRa pins NSS=%d BUSY=%d IRQ=%d RESET=%d TX=%d RX=%d\n",
         (int)config.lora_nss_pin,
         (int)config.lora_busy_pin,
         (int)config.lora_irq_pin,
         (int)config.lora_reset_pin,
         (int)config.lora_rxen_pin,
         (int)config.lora_txen_pin);

  int failures = 0;
  if (config.lora_nss_pin != RADIOLIB_NC) {
    failures += initGPIOPin(config.lora_nss_pin, config.lora_gpiochip, config.lora_nss_pin);
  }
  if (config.lora_busy_pin != RADIOLIB_NC) {
    failures += initGPIOPin(config.lora_busy_pin, config.lora_gpiochip, config.lora_busy_pin);
  }
  if (config.lora_irq_pin != RADIOLIB_NC) {
    failures += initEventGPIOPin(&irq_event_source, config.lora_irq_pin,
                                 config.lora_gpiochip, config.lora_irq_pin);
  }
  if (config.lora_reset_pin != RADIOLIB_NC) {
    failures += initGPIOPin(config.lora_reset_pin, config.lora_gpiochip, config.lora_reset_pin);
  }
  if (config.lora_rxen_pin != RADIOLIB_NC) {
    failures += initGPIOPin(config.lora_rxen_pin, config.lora_gpiochip, config.lora_rxen_pin);
  }
  if (config.lora_txen_pin != RADIOLIB_NC) {
    failures += initGPIOPin(config.lora_txen_pin, config.lora_gpiochip, config.lora_txen_pin);
  }

  if (failures > 0) {
    printf("FATAL: %d GPIO pin(s) failed to bind; cannot start radio.\n", failures);
    exit(1);
  }

  // GPS enable/standby pin: some modules (e.g. the L76K on the Waveshare
  // LoRaWAN/GNSS HAT) must have their STANDBY line driven HIGH to wake and
  // stream NMEA. Bind and hold it high for the daemon lifetime. Non-fatal: a
  // repeater must still run without GPS.
  if (config.gps_en_pin != -1) {
    if (initGPIOPin(config.gps_en_pin, config.lora_gpiochip, config.gps_en_pin) == 0) {
      pinMode(config.gps_en_pin, OUTPUT);
      digitalWrite(config.gps_en_pin, HIGH);
      printf("GPS enable pin %d driven HIGH\n", (int)config.gps_en_pin);
    } else {
      printf("WARNING: could not claim GPS enable pin %d; GPS may stay asleep\n",
             (int)config.gps_en_pin);
    }
  }
}

void LinuxBoard::idleUntilEvent(uint32_t max_wait_ms) {
  LinuxEventSource* src = irqEventSource();

  // Without edge detection nothing can wake us, so the caller's ceiling would
  // be pure latency: poll tightly instead. Same tradeoff (and same value) as
  // the delay(1) fallback in ESP32Board::sleep().
  const bool have_events = (src != NULL && src->eventFd() >= 0);
  const int  timeout_ms  = have_events ? (int) max_wait_ms : 1;

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
  // 9600 baud (~960 B/s) against a ~4 KB tty input buffer, a poll ceiling in
  // the tens of ms drains NMEA with three orders of magnitude of margin even
  // when gps_active is true and EnvironmentSensorManager::loop() is polled
  // from the timeout alone. DO NOT add EventLoop.registerFd(gps_serial.fd())
  // back in, even though it looks like the obviously-correct thing to do for
  // a node that streams GPS -- it is the single line that reintroduces this
  // branch's namesake bug in the default configuration.

  // Refresh the cached IRQ level immediately before blocking. Packet
  // correctness does not come from the edge-event descriptor above; it comes
  // from ArduLinux's gpioIdle(), which fires RadioLib's ISR on a LOW->HIGH
  // transition against a *cached* previous level. Nothing else in the
  // MeshCore call path refreshes that cache (no delay() calls in
  // Dispatcher.cpp/Mesh.cpp/MyMesh.cpp, and RadioLib's own
  // digitalRead(getIrq()) calls live only in blocking paths MeshCore doesn't
  // use), so the cache is stale from the moment gpioIdle() handles an
  // interrupt until the next iteration's gpioIdle() call. Without this line
  // the safe timeout ceiling would be bounded by packet airtime -- past that,
  // DIO1 stays latched HIGH with no further rising edge to recover on, and RX
  // stops silently rather than merely adding latency. This is what lets the
  // caller choose max_wait_ms freely, and it is the obligation
  // MainBoard::idleUntilEvent() documents for every implementer.
  // Cost is one ioctl per wake; it is latency-safe, because if the line is
  // already HIGH here an edge event is already queued and the wait below
  // returns immediately instead of blocking. Do not remove this as
  // "redundant" with gpioIdle() -- it is the only thing keeping a longer
  // timeout safe.
  if (config.lora_irq_pin != RADIOLIB_NC) digitalRead(config.lora_irq_pin);

  EventLoop.wait(timeout_ms);
}

namespace {

// Samples the LoRa IRQ line for waitForIrqAsserted().
//
// digitalRead() rather than a bare level read, and that is deliberate: in
// ardulinux it runs GPIOPin::readPin() -> refreshState(), which reads the
// hardware, updates the cached level and fires the attached ISR on the
// configured edge. Sampling the line here therefore also keeps that cache
// coherent while the main loop is parked inside a scan, for exactly the reason
// idleUntilEvent() reads the pin before blocking.
class RadioIrqLevel : public LinuxIrqLevel {
public:
  explicit RadioIrqLevel(uint32_t pin) : _pin(pin) { }
  bool irqAsserted() override { return digitalRead(_pin) == HIGH; }

private:
  uint32_t _pin;
};

}  // namespace

bool LinuxBoard::waitForRadioIrq(uint32_t timeout_ms) {
  // Nothing to wait on. Callers read the operation's result over SPI anyway, so
  // this costs them the wait, not the answer.
  if (config.lora_irq_pin == RADIOLIB_NC) return false;

  RadioIrqLevel level(config.lora_irq_pin);
  return waitForIrqAsserted(level, irqEventSource(), EventLoop, timeout_ms);
}

void trim(char *str) {
  char *end;
  while (isspace((unsigned char)*str)) str++;
  if (*str == 0) { *str = 0; return; }
  end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end)) end--;
  end[1] = '\0';
}

char *safe_copy(char *value, size_t maxlen) {
  char *retval;
  size_t length = strlen(value) + 1;
  if (length > maxlen) length = maxlen;

  retval = (char *)malloc(length);
  strncpy(retval, value, length - 1);
  retval[length - 1] = '\0';
  return retval;
}

int LinuxConfig::load(const char *filename) {
  FILE *f = fopen(filename, "r");
  if (!f) return -1;

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    // skip whitespace
    while (isspace(*p)) p++;
    // skip empty lines and comments
    if (*p == '\0' || *p == '#' || *p == ';') continue;

    char *key = p;
    while (*p && !isspace(*p) && *p != '=') p++;
    if (*p == '\0') continue;
    *p++ = '\0';

    while (*p && (isspace(*p) || *p == '=')) p++;
    char *value = p;
    p = value;
    while (*p && *p != '\n' && *p != '\r' && *p != '#' && *p != ';') p++;
    *p = '\0';

    trim(key);
    trim(value);

    // strip optional surrounding quotes from string values
    {
      size_t vlen = strlen(value);
      if (vlen >= 2 && (value[0] == '"' || value[0] == '\'') && value[vlen-1] == value[0]) {
        value[vlen-1] = '\0';
        value++;
      }
    }

    if (strcmp(key, "spidev") == 0)         spidev = safe_copy(value, 32);
    else if (strcmp(key, "lora_gpiochip") == 0) lora_gpiochip = safe_copy(value, 32);
    else if (strcmp(key, "lora_freq") == 0) lora_freq = atof(value);
    else if (strcmp(key, "lora_bw") == 0)   lora_bw = atof(value);
    else if (strcmp(key, "lora_sf") == 0)   lora_sf = (uint8_t)atoi(value);
    else if (strcmp(key, "lora_cr") == 0)   lora_cr = (uint8_t)atoi(value);
    else if (strcmp(key, "lora_tcxo") == 0) lora_tcxo = atof(value);
    else if (strcmp(key, "lora_tx_power") == 0)   lora_tx_power = atoi(value);
    else if (strcmp(key, "current_limit") == 0)  current_limit = atof(value);
    else if (strcmp(key, "dio2_as_rf_switch") == 0)  dio2_as_rf_switch = atoi(value) != 0;
    else if (strcmp(key, "rx_boosted_gain") == 0)  rx_boosted_gain = atoi(value) != 0;

    else if (strcmp(key, "lora_irq_pin") == 0)   lora_irq_pin = atoi(value);
    else if (strcmp(key, "lora_reset_pin") == 0) lora_reset_pin = atoi(value);
    else if (strcmp(key, "lora_nss_pin") == 0)   lora_nss_pin = atoi(value);
    else if (strcmp(key, "lora_busy_pin") == 0)  lora_busy_pin = atoi(value);
    else if (strcmp(key, "lora_rxen_pin") == 0)  lora_rxen_pin = atoi(value);
    else if (strcmp(key, "lora_txen_pin") == 0)  lora_txen_pin = atoi(value);

    else if (strcmp(key, "advert_name") == 0)    advert_name = safe_copy(value, 100);
    else if (strcmp(key, "admin_password") == 0) admin_password = safe_copy(value, 100);
    else if (strcmp(key, "lat") == 0)            lat = atof(value);
    else if (strcmp(key, "lon") == 0)            lon = atof(value);
    else if (strcmp(key, "gps_device") == 0)  gps_device = safe_copy(value, 64);
    else if (strcmp(key, "gps_baud") == 0)    gps_baud = atoi(value);
    else if (strcmp(key, "gps_en_pin") == 0)  gps_en_pin = atoi(value);
  }
  fclose(f);
  return 0;
}
