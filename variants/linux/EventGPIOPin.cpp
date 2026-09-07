#ifdef ARDULINUX_HARDWARE

#include "EventGPIOPin.h"

#include "AppInfo.h"
#include "logging.h"

#include <dirent.h>
#include <errno.h>
#include <stdexcept>
#include <stdlib.h>
#include <string.h>
#include <string>

#define GPIO_CONSUMER ardulinuxAppName

// ---------------------------------------------------------------------------
// Chip resolution.
//
// Turn the lora_gpiochip setting -- either a device name ("gpiochip0") or a
// kernel label ("pinctrl-rp1") -- into an open chip, using nothing but
// libgpiod's public API. How much of that libgpiod does for us differs by
// major version: v1 exports gpiod_chip_open_lookup(), which already tries a
// name, a label, a path and a bare number in turn; v2 dropped it and exports
// gpiod_is_gpiochip_device() instead, leaving the label scan to the caller --
// so on v2 we walk /dev ourselves, using that predicate as the filter.
// ---------------------------------------------------------------------------

#if EVGPIO_GPIOD_V == 2

// scandir() filter: keep exactly the /dev entries libgpiod itself recognises
// as GPIO chip character devices (symlinks included).
static int chip_dir_filter(const struct dirent* entry) {
  std::string path = "/dev/";
  path += entry->d_name;
  return gpiod_is_gpiochip_device(path.c_str()) ? 1 : 0;
}

// Open "/dev/<name>". NULL if it is absent or not a GPIO chip.
static struct gpiod_chip* open_chip_dev(const char* name) {
  std::string path = "/dev/";
  path += name;
  return gpiod_chip_open(path.c_str());
}

// Does an already-open chip report `chipLabel` as its kernel label?
static bool chip_label_matches(struct gpiod_chip* chip, const char* chipLabel) {
  struct gpiod_chip_info* info = gpiod_chip_get_info(chip);
  if (!info) return false;
  const char* label = gpiod_chip_info_get_label(info);
  bool hit = label && strcmp(label, chipLabel) == 0;
  gpiod_chip_info_free(info);
  return hit;
}

#endif  // EVGPIO_GPIOD_V == 2

// Open the chip named by `chipLabel`. The caller owns the returned chip; NULL
// means nothing matched, which is what makes the constructor throw.
static struct gpiod_chip* find_chip_by_label(const char* chipLabel) {
#if EVGPIO_GPIOD_V == 1
  struct gpiod_chip* chip = gpiod_chip_open_lookup(chipLabel);
  if (chip)
    log(SysGPIO, LogDebug, "find_chip_by_label(%s): lookup matched %s",
        chipLabel, gpiod_chip_name(chip));
  return chip;
#else
  // Device name first ("/dev/gpiochip0"), which is both the default and the
  // common case, so the scan below never runs for it.
  struct gpiod_chip* chip = open_chip_dev(chipLabel);
  if (chip) return chip;

  // Otherwise treat it as a kernel label and compare against every GPIO chip
  // in /dev.
  struct dirent** entries;
  int num_chips = scandir("/dev/", &entries, chip_dir_filter, alphasort);
  // Only a negative return leaves `entries` unset; a zero-match scan still
  // hands back an allocated (empty) array, so fall through to the free below.
  if (num_chips < 0) return NULL;

  struct gpiod_chip* match = NULL;
  for (int i = 0; i < num_chips; i++) {
    // Keep looping even once matched: every entry still has to be freed.
    if (!match) {
      struct gpiod_chip* c = open_chip_dev(entries[i]->d_name);
      if (c) {
        if (chip_label_matches(c, chipLabel)) {
          match = c;
          log(SysGPIO, LogDebug, "find_chip_by_label(%s): scan matched %s",
              chipLabel, entries[i]->d_name);
        } else {
          gpiod_chip_close(c);
        }
      }
    }
    free(entries[i]);
  }
  free(entries);
  return match;
#endif
}

// ---------------------------------------------------------------------------
// EventGPIOPin
// ---------------------------------------------------------------------------

EventGPIOPin::EventGPIOPin(pin_size_t n, const char* chipLabel, int lineOffset,
                           const char* pinName)
    : GPIOPin(n, pinName) {
  _offset = (unsigned int)lineOffset;

  _chip = find_chip_by_label(chipLabel);
  if (!_chip)
    throw std::invalid_argument("GPIO chip not found");

#if EVGPIO_GPIOD_V == 1
  _line = gpiod_chip_get_line(_chip, lineOffset);
  if (!_line) {
    releaseResources();
    throw std::invalid_argument("GPIO line not found");
  }
#endif

  // On v2, requestWithEdges() lazily allocates _evbuf and fails if that
  // allocation fails -- see the comment there for why that (rather than a
  // check here) is what makes _edge_ok a reliable invariant.
  _edge_ok = requestWithEdges(INPUT);
  if (!_edge_ok) {
    log(SysGPIO, LogError,
        "EventGPIOPin(%s): edge detection unavailable (%s); "
        "falling back to timeout polling",
        getName(), strerror(_last_errno));
    if (!requestPlainInput(INPUT)) {
      releaseResources();
      throw std::invalid_argument("cannot request GPIO line");
    }
  }
}

EventGPIOPin::~EventGPIOPin() {
  releaseResources();
}

void EventGPIOPin::releaseResources() {
#if EVGPIO_GPIOD_V == 2
  if (_line)  { gpiod_line_request_release(_line); _line = NULL; }
  if (_evbuf) { gpiod_edge_event_buffer_free(_evbuf); _evbuf = NULL; }
#else
  if (_line) gpiod_line_release(_line);
  _line = NULL;
#endif
  if (_chip) { gpiod_chip_close(_chip); _chip = NULL; }
}

// ---------------------------------------------------------------------------
// Line (re)configuration.
//
// On v2 the request*() helpers below share everything except the
// gpiod_line_settings they build: wrap the settings in a line_config, then
// either request the line (first call, _line still NULL) or reconfigure it
// in place (every later call, e.g. from setPinMode()). applySettings() holds
// that shared tail so each helper is just its settings calls plus one call
// here. v1 has no equivalent config object -- each mode is a distinct
// gpiod_line_request_*() call -- so its branches stay separate below.
// ---------------------------------------------------------------------------

#if EVGPIO_GPIOD_V == 2
bool EventGPIOPin::applySettings(struct gpiod_line_settings* settings) {
  if (!settings) return false;

  struct gpiod_line_config* cfg = gpiod_line_config_new();
  if (!cfg) {
    _last_errno = errno;
    gpiod_line_settings_free(settings);
    return false;
  }
  gpiod_line_config_add_line_settings(cfg, &_offset, 1, settings);

  int rv;
  if (_line == NULL) {
    struct gpiod_request_config* rc = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rc, GPIO_CONSUMER);
    _line = gpiod_chip_request_lines(_chip, rc, cfg);
    // Capture errno immediately: gpiod_request_config_free() below and the
    // frees at the end of this function are not guaranteed to preserve it.
    _last_errno = errno;
    gpiod_request_config_free(rc);
    rv = (_line != NULL) ? 0 : -1;
  } else {
    // Reconfigure replaces the config wholesale, which is exactly why edge
    // detection has to be restated here on every mode change.
    rv = gpiod_line_request_reconfigure_lines(_line, cfg);
    _last_errno = errno;  // capture before the frees below can clobber it
  }

  gpiod_line_config_free(cfg);
  gpiod_line_settings_free(settings);
  return rv == 0;
}
#endif

bool EventGPIOPin::requestInput(PinMode m, bool with_edges) {
#if EVGPIO_GPIOD_V == 1
  // The flagless entry points libgpiod v1 offers -- gpiod_line_request_input()
  // and gpiod_line_request_rising_edge_events() -- are defined as their _flags
  // counterparts called with 0, so passing 0 here covers plain INPUT exactly.
  int flags = 0;
  if (m == INPUT_PULLUP)        flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
  else if (m == INPUT_PULLDOWN) flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;

  int rv = with_edges
      ? gpiod_line_request_rising_edge_events_flags(_line, GPIO_CONSUMER, flags)
      : gpiod_line_request_input_flags(_line, GPIO_CONSUMER, flags);
  _last_errno = errno;
  return rv == 0;
#else
  // The edge-detecting path is the only place _edge_ok is ever set true (see
  // both call sites), so guaranteeing _evbuf here -- and only here -- is what
  // makes "_edge_ok implies a usable _evbuf" an actual invariant rather than a
  // hopeful comment: eventFd() and drainEvents() can then both trust _edge_ok
  // alone, with no separate _evbuf check of their own to fall out of sync.
  // Lazy (rather than always allocating in the constructor) because this is
  // the only path that needs it, and it costs nothing to retry the allocation
  // here if a prior attempt failed.
  if (with_edges && !_evbuf) {
    _evbuf = gpiod_edge_event_buffer_new(16);
    if (!_evbuf) {
      _last_errno = errno;
      log(SysGPIO, LogError,
          "EventGPIOPin(%s): edge-event buffer allocation failed",
          getName());
      return false;
    }
  }

  struct gpiod_line_settings* settings = gpiod_line_settings_new();
  if (!settings) { _last_errno = errno; return false; }
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
  if (with_edges)
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING);
  if (m == INPUT_PULLUP)
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
  else if (m == INPUT_PULLDOWN)
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_DOWN);

  return applySettings(settings);
#endif
}

bool EventGPIOPin::requestOutput(PinStatus initial) {
#if EVGPIO_GPIOD_V == 1
  int rv = gpiod_line_request_output(_line, GPIO_CONSUMER, initial);
  _last_errno = errno;
  return rv == 0;
#else
  struct gpiod_line_settings* settings = gpiod_line_settings_new();
  if (!settings) { _last_errno = errno; return false; }
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_output_value(settings, (gpiod_line_value)initial);

  return applySettings(settings);
#endif
}

PinStatus EventGPIOPin::readPinHardware() {
#if EVGPIO_GPIOD_V == 1
  // Valid on an event-requested line: libgpiod v1's get-value path handles
  // LINE_REQUESTED_EVENTS by issuing the values ioctl on the event fd.
  int res = gpiod_line_get_value(_line);
#else
  int res = gpiod_line_request_get_value(_line, _offset);
#endif
  if (res < 0) {
    // An unrequested line (e.g. both requestWithEdges() and
    // requestPlainInput() failed on a mode change) reads permanently LOW
    // here with no other symptom -- silently stops all packet RX. This is
    // called every event-loop iteration, so latch rather than flood: log the
    // first occurrence only.
    if (!_read_warned) {
      log(SysGPIO, LogError,
          "EventGPIOPin(%s): read failed (%s); reading LOW until this is "
          "resolved (further occurrences suppressed)",
          getName(), strerror(errno));
      _read_warned = true;
    }
    return LOW;
  }
  return (PinStatus)res;
}

void EventGPIOPin::writePin(PinStatus s) {
  if (GPIOPin::getPinMode() != OUTPUT)
    setPinMode(OUTPUT);
  GPIOPin::writePin(s);  // update cached status

#if EVGPIO_GPIOD_V == 1
  gpiod_line_set_value(_line, s);
#else
  gpiod_line_request_set_value(_line, _offset, (gpiod_line_value)s);
#endif
}

void EventGPIOPin::setPinMode(PinMode m) {
  GPIOPin::setPinMode(m);  // update cached mode + log

  if (m == OUTPUT) {
    // Should never happen for DIO1. An output line has no edges to report, so
    // say so rather than silently keeping a stale descriptor.
    if (_edge_ok) {
      log(SysGPIO, LogError,
          "EventGPIOPin(%s): OUTPUT requested, edge detection disabled",
          getName());
      _edge_ok = false;
    }
    // readPin() returns the cached status without touching hardware: mode is
    // already OUTPUT (GPIOPin::setPinMode(m) above already updated it), so
    // refreshState() short-circuits and skips readPinHardware(). Must be read
    // *before* the v1 release below -- reading after release hits EPERM,
    // which readPinHardware() maps to LOW, silently discarding the pin's
    // prior level on every mode change. Mirrors upstream LinuxGPIOPin.cpp's
    // gpiod_line_request_output(line, consumer, readPin()).
    PinStatus initial = readPin();
#if EVGPIO_GPIOD_V == 1
    gpiod_line_release(_line);
#endif
    if (!requestOutput(initial)) {
      log(SysGPIO, LogError,
          "EventGPIOPin(%s): failed to request line as OUTPUT (%s)",
          getName(), strerror(_last_errno));
    }
    return;
  }

  // INPUT / INPUT_PULLUP / INPUT_PULLDOWN.
  //
  // RadioLib calls pinMode(irq, INPUT) from SX126x::begin() *after* this pin is
  // bound. v1 cannot reconfigure in place, and v2's reconfigure replaces the
  // config wholesale, so edge detection must be restated here or every wake-up
  // silently degrades to the poll timeout.
#if EVGPIO_GPIOD_V == 1
  gpiod_line_release(_line);  // v1 cannot reconfigure in place
#endif

  _edge_ok = requestWithEdges(m);
  if (!_edge_ok) {
    log(SysGPIO, LogError,
        "EventGPIOPin(%s): edge detection lost on mode change (%s); "
        "falling back to timeout polling",
        getName(), strerror(_last_errno));
    if (!requestPlainInput(m)) {
      // Both the edge-detecting and plain-input requests failed: the line is
      // now unrequested. readPinHardware() maps that to a permanent LOW,
      // GPIOPin::callISR() never fires, and packets are silently dropped
      // rather than merely delayed -- this must not be quiet.
      log(SysGPIO, LogError,
          "EventGPIOPin(%s): failed to request line as plain INPUT (%s); "
          "line is unrequested, reads will return LOW until the next "
          "setPinMode() call",
          getName(), strerror(_last_errno));
    }
  }
}

int EventGPIOPin::eventFd() const {
  if (!_edge_ok || _line == NULL) return -1;
#if EVGPIO_GPIOD_V == 1
  return gpiod_line_event_get_fd(_line);
#else
  return gpiod_line_request_get_fd(_line);
#endif
}

bool EventGPIOPin::drainEvents() {
  // _edge_ok is the single source of truth eventFd() also uses. It can only
  // become true via requestWithEdges(), which on v2 lazily allocates _evbuf
  // and returns false if that allocation fails -- so _edge_ok true implies a
  // non-NULL _evbuf here too, not just at eventFd(). No separate _evbuf
  // check needed; see requestWithEdges() for where that invariant is made.
  //
  // True, not false: with no edge detection eventFd() is -1, so the loop never
  // polled this source and there is nothing it failed to drain. Failure here
  // means "the descriptor is still readable and I could not clear it", which
  // is not the case.
  if (!_edge_ok || _line == NULL) return true;

  // Read until the queue is empty, reporting anything that is not an empty
  // queue as a failure. Both the wait and the read can fail persistently (EIO
  // on a wedged controller is the same class of fault readPinHardware()
  // already guards against), and either one leaves the descriptor readable --
  // so they have to be told apart from "drained", not merged into a bare
  // `break` that looks like success to the caller.
#if EVGPIO_GPIOD_V == 1
  // Zero timeout keeps the wait non-blocking: gpiod_line_event_read() on its
  // own would block once the queue empties.
  struct timespec zero = {0, 0};
  struct gpiod_line_event ev;
  for (;;) {
    int rv = gpiod_line_event_wait(_line, &zero);
    if (rv == 0) return true;   // queue empty: fully drained
    if (rv < 0) return false;   // the wait itself failed
    if (gpiod_line_event_read(_line, &ev) != 0) return false;
  }
#else
  for (;;) {
    int rv = gpiod_line_request_wait_edge_events(_line, 0);
    if (rv == 0) return true;
    if (rv < 0) return false;
    if (gpiod_line_request_read_edge_events(_line, _evbuf, 16) <= 0) return false;
  }
#endif
}

#undef GPIO_CONSUMER

#endif  // ARDULINUX_HARDWARE
