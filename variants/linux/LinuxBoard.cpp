#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
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

// Still hardcoded -- see "Known Gaps" in variants/linux/README.md -- but named,
// so the loader and the diagnostics that mention it cannot disagree.
#define CONFIG_PATH "/etc/meshcored/meshcored.ini"

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

  // Nothing about a bad config is silent any more -- that was the actual defect
  // -- but the two failure kinds get opposite responses.
  //
  // An INVALID VALUE is fatal. The operator wrote a specific setting and it
  // could not be honoured, so continuing means running hardware differently
  // from how it was asked to be run: `lora_irq_pin = 260` leaves the IRQ line
  // unconfigured, and the node then fails at RX in a way that reads as a wiring
  // fault. There is no sensible fallback for "which GPIO drives the radio".
  //
  // An UNKNOWN KEY only warns. It is inert by definition -- nothing consumes it
  // -- and it may be a key from a newer build, a leftover from an older one, or
  // a typo. Ignoring unrecognised keys is what every INI parser does, and
  // refusing to boot over one would take a working repeater off the air for a
  // line it was already ignoring. It still gets said out loud, because the
  // radio parameters here are FIRST-RUN defaults: MyMesh::begin() persists them
  // to prefs.json on the first boot, so `lora_frequency` for `lora_freq` does
  // not merely fail to apply -- correcting the INI later will not undo it.
  LinuxConfig::LoadResult cfg = config.load(CONFIG_PATH);

  if (!cfg.opened) {
    printf("WARNING: cannot read %s (%s); continuing on built-in defaults.\n"
           "         Expect the radio to fail to start -- no GPIO pins are configured.\n"
           "         Start from a template in variants/linux/ (meshcored.ini.waveshare,\n"
           "         meshcored.ini.pow-sx1262).\n",
           CONFIG_PATH, strerror(errno));
  }
  if (cfg.unknown_keys > 0) {
    printf("WARNING: %d unrecognised key(s) in %s (listed above) were ignored.\n"
           "         If one of them was meant to be a radio parameter, note that first\n"
           "         boot persists the DEFAULT to prefs.json and correcting the INI\n"
           "         afterwards will NOT change it -- check the lines above now.\n",
           cfg.unknown_keys, CONFIG_PATH);
  }
  if (cfg.bad_values > 0) {
    printf("FATAL: %d invalid value(s) in %s (listed above).\n"
           "       Refusing to start rather than drive the hardware differently from\n"
           "       how it was configured. Fix the listed line(s) and restart.\n",
           cfg.bad_values, CONFIG_PATH);
    exit(1);
  }

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

  // Exactly the descriptor(s) LinuxConsole::rawReadByte() will actually consume
  // this iteration. The console owns that choice because it owns the precedence
  // -- registering one it will not drain leaves it permanently POLLIN and
  // reinstates the busy loop for as little as one concurrent `meshcorectl`.
  Console.registerPollFds(EventLoop);

  // The GPS descriptor is deliberately absent, and LinuxGpsStream exposes no
  // accessor for it so it cannot be added here by reflex. EnvironmentSensorManager
  // drains that stream only while gps_active is true, but the module streams
  // regardless, so registering it would leave a descriptor nothing drains sitting
  // POLLIN -- the 100% CPU spin this event loop exists to remove, on every node
  // with gps_device set and GPS not toggled on. Nothing is lost: at 9600 baud
  // against a ~4 KB tty buffer, draining NMEA off the poll timeout alone has
  // three orders of magnitude of margin.

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

// Trim whitespace from both ends, returning the trimmed string.
//
// Returns rather than trimming in place because the leading trim cannot be done
// in place without moving bytes: advancing the local pointer is invisible to the
// caller, which is exactly the bug this signature prevents. Callers must use the
// return value.
//
// static, like safe_copy() below: `trim` is the kind of name another translation
// unit is entitled to define at file scope, and a variant has no business
// exporting it.
static char *trim(char *str) {
  while (isspace((unsigned char)*str)) str++;
  if (*str == 0) return str;
  char *end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end)) end--;
  end[1] = '\0';
  return str;
}

static char *safe_copy(char *value, size_t maxlen) {
  char *retval;
  size_t length = strlen(value) + 1;
  if (length > maxlen) length = maxlen;

  retval = (char *)malloc(length);
  if (!retval) {
    // Cannot return NULL: every caller assigns straight into a char* the rest of
    // the daemon dereferences. An empty string is the one answer that is both
    // safe and visibly wrong. (Same char*-from-literal the field defaults use.)
    printf("ERROR: meshcored.ini: out of memory copying a value; using \"\"\n");
    return (char *) "";
  }
  strncpy(retval, value, length - 1);
  retval[length - 1] = '\0';
  return retval;
}

// Parse a GPIO pin number.
//
// Pin numbers reach the Arduino API as pin_size_t (uint8_t), so anything outside
// 0..255 wraps silently -- `lora_irq_pin = 260` would bind line 4 and then fail
// in a way that reads as a wiring fault rather than a typo. `lo` is 0 for the
// LoRa pins and -1 for gps_en_pin, whose -1 means "unset". On a bad value the
// caller's field is left at its default and *bad_values is bumped, which
// LinuxBoard::begin() treats as fatal -- there is no sensible fallback for
// "which GPIO drives the radio".
static bool parse_pin(const char *key, const char *value, long lo, long *out, int *bad_values) {
  char *end = NULL;
  long v = strtol(value, &end, 10);
  if (end == value || *end != '\0' || v < lo || v > 255) {
    printf("ERROR: meshcored.ini: %s = '%s' is not a valid GPIO pin (expected %ld..255)\n",
           key, value, lo);
    (*bad_values)++;
    return false;
  }
  *out = v;
  return true;
}

LinuxConfig::LoadResult LinuxConfig::load(const char *filename) {
  LoadResult result;

  FILE *f = fopen(filename, "r");
  if (!f) return result;   // result.opened stays false
  result.opened = true;

  bool first_line = true;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *p = line;

    // Strip a UTF-8 BOM. An editor that writes one would otherwise glue it to
    // the first key, which both drops that setting and reports it as "unknown
    // key 'advert_name'" against a line that reads exactly right -- the least
    // actionable message this loop could produce.
    if (first_line && (unsigned char)p[0] == 0xEF
                   && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
      p += 3;
    }
    first_line = false;

    // skip whitespace. isspace() is undefined for negative char values, which a
    // UTF-8 advert_name supplies, so every call here casts.
    while (isspace((unsigned char)*p)) p++;
    // skip empty lines, comments, and INI section headers. Nothing here is
    // sectioned, but the format invites them, and a "[general]" reported as an
    // unknown key is noise that trains the operator to ignore the warning.
    if (*p == '\0' || *p == '#' || *p == ';' || *p == '[') continue;

    char *key = p;
    while (*p && !isspace((unsigned char)*p) && *p != '=') p++;
    if (*p == '\0') continue;
    *p++ = '\0';

    while (*p && (isspace((unsigned char)*p) || *p == '=')) p++;
    char *value = p;
    p = value;
    while (*p && *p != '\n' && *p != '\r' && *p != '#' && *p != ';') p++;
    *p = '\0';

    key   = trim(key);
    value = trim(value);
    long pin = 0;

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
    else if (strcmp(key, "use_regulator_ldo") == 0)  use_regulator_ldo = atoi(value) != 0;
    else if (strcmp(key, "rx_register_patch") == 0)  rx_register_patch = atoi(value) != 0;

    else if (strcmp(key, "lora_irq_pin") == 0)   { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_irq_pin   = (uint32_t) pin; }
    else if (strcmp(key, "lora_reset_pin") == 0) { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_reset_pin = (uint32_t) pin; }
    else if (strcmp(key, "lora_nss_pin") == 0)   { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_nss_pin   = (uint32_t) pin; }
    else if (strcmp(key, "lora_busy_pin") == 0)  { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_busy_pin  = (uint32_t) pin; }
    else if (strcmp(key, "lora_rxen_pin") == 0)  { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_rxen_pin  = (uint32_t) pin; }
    else if (strcmp(key, "lora_txen_pin") == 0)  { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_txen_pin  = (uint32_t) pin; }

    else if (strcmp(key, "advert_name") == 0)    advert_name = safe_copy(value, 100);
    else if (strcmp(key, "admin_password") == 0) admin_password = safe_copy(value, 100);
    else if (strcmp(key, "lat") == 0)            lat = atof(value);
    else if (strcmp(key, "lon") == 0)            lon = atof(value);
    else if (strcmp(key, "gps_device") == 0)  gps_device = safe_copy(value, 64);
    else if (strcmp(key, "gps_baud") == 0)    gps_baud = atoi(value);
    else if (strcmp(key, "gps_en_pin") == 0)  { if (parse_pin(key, value, -1, &pin, &result.bad_values)) gps_en_pin = (int) pin; }

    else {
      // Nothing below this chain consumes leftovers, so an unrecognised key is
      // a key that does nothing -- inert, and possibly just a key from a newer
      // build. Counted separately from a bad value for that reason: the caller
      // warns about these and refuses to start over those.
      printf("ERROR: meshcored.ini: unknown key '%s' (ignored)\n", key);
      result.unknown_keys++;
    }
  }
  fclose(f);
  return result;
}
