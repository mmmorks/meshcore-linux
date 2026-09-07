#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <exception>
#ifdef ARDULINUX_HARDWARE
#include "linux/gpio/LinuxGPIOPin.h"
#endif
#include "LinuxBoard.h"
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

void ardulinuxSetup() {
}

void LinuxBoard::begin() {
  // Only NORMAL applies here -- this board never wakes from a radio IRQ or
  // deep sleep the way an MCU variant can, so nothing else ever sets this.
  startup_reason = BD_STARTUP_NORMAL;

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
    failures += initGPIOPin(config.lora_irq_pin, config.lora_gpiochip, config.lora_irq_pin);
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
}

// The ardulinux core's global ::reboot() (cores/ardulinux/main.cpp) re-execs
// this same process via execv(), with --erase stripped from argv so a reboot
// cannot re-trigger a filesystem wipe. exit(0) here would be wrong: the
// shipped systemd unit uses Restart=on-failure, not Restart=always, so a clean
// exit is a stop, not a restart -- and both the `reboot` and `clkreboot` CLI
// commands reach this from an authenticated remote admin over the mesh
// (CommonCLI::handleCommand), so that would take an unattended repeater
// off-air. Qualified as ::reboot() to resolve to the global one and not recurse
// into this member of the same name.
void LinuxBoard::reboot() {
  ::reboot();
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

// *ok reports whether `retval` is a malloc() this function made (and so is
// safe to free() later) as opposed to the literal fallback below -- assign_string()
// below needs that distinction to avoid ever handing a string literal to free().
static char *safe_copy(char *value, size_t maxlen, bool *ok) {
  char *retval;
  size_t length = strlen(value) + 1;
  if (length > maxlen) length = maxlen;

  retval = (char *)malloc(length);
  if (!retval) {
    // Cannot return NULL: every caller assigns straight into a char* the rest of
    // the daemon dereferences. An empty string is the one answer that is both
    // safe and visibly wrong. (Same char*-from-literal the field defaults use.)
    printf("ERROR: meshcored.ini: out of memory copying a value; using \"\"\n");
    *ok = false;
    return (char *) "";
  }
  strncpy(retval, value, length - 1);
  retval[length - 1] = '\0';
  *ok = true;
  return retval;
}

// Parse a GPIO pin number.
//
// Pin numbers reach the Arduino API as pin_size_t (uint8_t), so anything outside
// 0..255 wraps silently -- `lora_irq_pin = 260` would bind line 4 and then fail
// in a way that reads as a wiring fault rather than a typo. `lo` is the lowest
// acceptable value (0 for a pin that must be set). On a bad value the
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

// Parse a float config value. atof() silently stops at the first
// non-numeric character (`lora_freq = 868,5` -> 868.0) and returns 0.0 for
// anything that parses as nothing at all (`lora_freq = abc` -> 0.0) with no
// diagnostic either way -- exactly the silent-misconfiguration class
// LinuxBoard::begin()'s FATAL branch exists to rule out.
static bool parse_float(const char *key, const char *value, float *out, int *bad_values) {
  char *end = NULL;
  float v = strtof(value, &end);
  if (end == value || *end != '\0') {
    printf("ERROR: meshcored.ini: %s = '%s' is not a valid number\n", key, value);
    (*bad_values)++;
    return false;
  }
  *out = v;
  return true;
}

// Parse a bounded integer config value. Shares parse_pin()'s rationale: `lo`
// and `hi` are the field's own type width (e.g. -128..127 for an int8_t), so
// a value atoi() would otherwise truncate into something silently different
// -- `lora_tx_power = 300` becoming 44 -- is caught here instead.
static bool parse_int(const char *key, const char *value, long lo, long hi, long *out, int *bad_values) {
  char *end = NULL;
  long v = strtol(value, &end, 10);
  if (end == value || *end != '\0' || v < lo || v > hi) {
    printf("ERROR: meshcored.ini: %s = '%s' is not a valid integer (expected %ld..%ld)\n",
           key, value, lo, hi);
    (*bad_values)++;
    return false;
  }
  *out = v;
  return true;
}

// Parse a boolean config value. The bug this replaces: `atoi(value) != 0`
// treats anything not starting with a digit as false, so
// `dio2_as_rf_switch = true` -- the spelling every other bool in this file
// invites -- silently becomes false, with the DIO2 RF switch left unset and
// TX dead on boards that need it. Accept the spellings the shipped
// meshcored.ini.* templates use and reject everything else as a bad value.
static bool parse_bool(const char *key, const char *value, bool *out, int *bad_values) {
  if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "on") == 0 || strcasecmp(value, "yes") == 0) {
    *out = true;
    return true;
  }
  if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
      strcasecmp(value, "off") == 0 || strcasecmp(value, "no") == 0) {
    *out = false;
    return true;
  }
  printf("ERROR: meshcored.ini: %s = '%s' is not a valid boolean "
         "(expected 1/0, true/false, on/off, yes/no)\n", key, value);
  (*bad_values)++;
  return false;
}

// Copy `value` into a string field, freeing whatever it pointed to if this
// field has already been assigned once during this load() call. The
// compile-time default is a string literal and not ours to free; `*owned`
// tracks the transition from "still the default" to "a safe_copy()
// allocation" (and stays false across an out-of-memory copy, which is the
// literal fallback again), so a duplicate key does not leak the previous
// allocation or free() something that was never malloc()'d.
static void assign_string(const char **field, bool *owned, char *value, size_t maxlen) {
  if (*owned) free((void *) *field);
  bool ok = false;
  *field = safe_copy(value, maxlen, &ok);
  *owned = ok;
}

LinuxConfig::LoadResult LinuxConfig::load(const char *filename) {
  LoadResult result;

  FILE *f = fopen(filename, "r");
  if (!f) return result;   // result.opened stays false
  result.opened = true;

  // String fields start out pointing at their compile-time literal default;
  // *_owned flips true the first time this load() call replaces it with a
  // safe_copy() allocation, so a duplicate key knows there is something of
  // its own to free before overwriting it again.
  bool spidev_owned = false, lora_gpiochip_owned = false,
       advert_name_owned = false, admin_password_owned = false;

  bool first_line = true;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    // fgets() stops at sizeof(line)-1 bytes even mid-line, with no '\n' to
    // show it. Left unhandled, the unread remainder is read as its own line
    // next iteration and parsed as a fresh key=value pair -- silently
    // splitting one overlong line into a truncated setting plus a bogus
    // "unknown key". Nothing this loop writes is anywhere near this long, so
    // treat it as a line that cannot be honoured rather than guess where it
    // was meant to end.
    size_t raw_len = strlen(line);
    if (raw_len == sizeof(line) - 1 && line[raw_len - 1] != '\n' && !feof(f)) {
      int c;
      while ((c = fgetc(f)) != EOF && c != '\n') { }
      printf("ERROR: meshcored.ini: line exceeds %zu bytes; discarding it\n", sizeof(line) - 1);
      result.bad_values++;
      first_line = false;
      continue;
    }

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
    if (*p == '\0') {
      // No '=' anywhere on the line, and no key/value split possible either --
      // was silently dropped before. Inert like an unknown key (nothing here
      // consumes it), so counted the same way rather than treated as fatal.
      printf("ERROR: meshcored.ini: line '%s' has no '=' (ignored)\n", trim(key));
      result.unknown_keys++;
      continue;
    }
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

    long ival = 0;
    float fval = 0.0f;
    bool bval = false;

    if (strcmp(key, "spidev") == 0)         assign_string(&spidev, &spidev_owned, value, 32);
    else if (strcmp(key, "lora_gpiochip") == 0) assign_string(&lora_gpiochip, &lora_gpiochip_owned, value, 32);
    else if (strcmp(key, "lora_freq") == 0) { if (parse_float(key, value, &fval, &result.bad_values)) lora_freq = fval; }
    else if (strcmp(key, "lora_bw") == 0)   { if (parse_float(key, value, &fval, &result.bad_values)) lora_bw = fval; }
    else if (strcmp(key, "lora_sf") == 0)   { if (parse_int(key, value, 0, 255, &ival, &result.bad_values)) lora_sf = (uint8_t) ival; }
    else if (strcmp(key, "lora_cr") == 0)   { if (parse_int(key, value, 0, 255, &ival, &result.bad_values)) lora_cr = (uint8_t) ival; }
    else if (strcmp(key, "lora_tcxo") == 0) { if (parse_float(key, value, &fval, &result.bad_values)) lora_tcxo = fval; }
    else if (strcmp(key, "lora_tx_power") == 0) { if (parse_int(key, value, -128, 127, &ival, &result.bad_values)) lora_tx_power = (int8_t) ival; }
    else if (strcmp(key, "current_limit") == 0) { if (parse_float(key, value, &fval, &result.bad_values)) current_limit = fval; }
    else if (strcmp(key, "dio2_as_rf_switch") == 0) { if (parse_bool(key, value, &bval, &result.bad_values)) dio2_as_rf_switch = bval; }
    else if (strcmp(key, "rx_boosted_gain") == 0)   { if (parse_bool(key, value, &bval, &result.bad_values)) rx_boosted_gain = bval; }
    else if (strcmp(key, "use_regulator_ldo") == 0) { if (parse_bool(key, value, &bval, &result.bad_values)) use_regulator_ldo = bval; }
    else if (strcmp(key, "rx_register_patch") == 0) { if (parse_bool(key, value, &bval, &result.bad_values)) rx_register_patch = bval; }

    else if (strcmp(key, "lora_irq_pin") == 0)   { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_irq_pin   = (uint32_t) pin; }
    else if (strcmp(key, "lora_reset_pin") == 0) { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_reset_pin = (uint32_t) pin; }
    else if (strcmp(key, "lora_nss_pin") == 0)   { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_nss_pin   = (uint32_t) pin; }
    else if (strcmp(key, "lora_busy_pin") == 0)  { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_busy_pin  = (uint32_t) pin; }
    else if (strcmp(key, "lora_rxen_pin") == 0)  { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_rxen_pin  = (uint32_t) pin; }
    else if (strcmp(key, "lora_txen_pin") == 0)  { if (parse_pin(key, value, 0, &pin, &result.bad_values)) lora_txen_pin  = (uint32_t) pin; }

    else if (strcmp(key, "advert_name") == 0)    assign_string(&advert_name, &advert_name_owned, value, 100);
    else if (strcmp(key, "admin_password") == 0) assign_string(&admin_password, &admin_password_owned, value, 100);
    else if (strcmp(key, "lat") == 0) { if (parse_float(key, value, &fval, &result.bad_values)) lat = fval; }
    else if (strcmp(key, "lon") == 0) { if (parse_float(key, value, &fval, &result.bad_values)) lon = fval; }

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
