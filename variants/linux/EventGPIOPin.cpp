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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define consumer ardulinuxAppName

// ---------------------------------------------------------------------------
// Chip resolution.
//
// Copied verbatim from ardulinux's cores/ardulinux/linux/gpio/LinuxGPIOPin.cpp
// (v0.2.2). Those helpers are file-static there and LinuxGPIOPin's chip/line
// members are private, so neither reuse nor subclassing is possible without
// forking ardulinux. Keep this copy byte-identical so an upstream fix can be
// re-synced by diffing against that file.
// ---------------------------------------------------------------------------

static bool chip_is_gpiochip_device(const char *path) {
  char *realname, *sysfsp, devpath[64];
	struct stat statbuf;
	bool ret = false;
	int rv;

	rv = lstat(path, &statbuf);
	if (rv)
		goto out;

	/*
	 * Is it a symbolic link? We have to resolve it before checking
	 * the rest.
	 */
	realname = S_ISLNK(statbuf.st_mode) ? realpath(path, NULL) :
					      strdup(path);
	if (realname == NULL)
		goto out;

	rv = stat(realname, &statbuf);
	if (rv)
		goto out_free_realname;

	/* Is it a character device? */
	if (!S_ISCHR(statbuf.st_mode)) {
		errno = ENOTTY;
		goto out_free_realname;
	}

	/* Is the device associated with the GPIO subsystem? */
	snprintf(devpath, sizeof(devpath), "/sys/dev/char/%u:%u/subsystem",
		 major(statbuf.st_rdev), minor(statbuf.st_rdev));

	sysfsp = realpath(devpath, NULL);
	if (!sysfsp)
		goto out_free_realname;

	/*
	 * In glibc, if any of the underlying readlink() calls fail (which is
	 * perfectly normal when resolving paths), errno is not cleared.
	 */
	errno = 0;

	if (strcmp(sysfsp, "/sys/bus/gpio") != 0) {
		/* This is a character device but not the one we're after. */
		errno = ENODEV;
		goto out_free_sysfsp;
	}

	ret = true;

out_free_sysfsp:
	free(sysfsp);
out_free_realname:
	free(realname);
out:
	errno = 0;
	return ret;
}

static int chip_dir_filter(const struct dirent *entry)
{
	bool is_chip;
	char *path;
	int ret;

	ret = asprintf(&path, "/dev/%s", entry->d_name);
	if (ret < 0)
		return 0;

	is_chip = chip_is_gpiochip_device(path);
	free(path);
	return !!is_chip;
}

static struct gpiod_chip *chip_open_by_name(const char *name)
{
	struct gpiod_chip *chip;
	char *path;
	int ret;

	ret = asprintf(&path, "/dev/%s", name);
	if (ret < 0)
		return NULL;

	chip = gpiod_chip_open(path);
	free(path);

	return chip;
}

// Open the gpiod chip whose label matches `chipLabel`.  First tries
// "/dev/<chipLabel>" as a device basename (covers the gpiochip0 case);
// falls back to scanning /dev/ and comparing each chip's reported label
// (covers passing a kernel label like "pinctrl-rp1" or "pinctrl-bcm2835").
// Caller owns the returned chip; returns NULL if no chip matches.
static struct gpiod_chip *find_chip_by_label(const char *chipLabel)
{
	std::string path = "/dev/";
	path += chipLabel;
	if (access(path.c_str(), R_OK) == 0)
		return chip_open_by_name(chipLabel);

	struct dirent **entries;
	int num_chips = scandir("/dev/", &entries, chip_dir_filter, alphasort);
	if (num_chips <= 0)
		return NULL;

	struct gpiod_chip *match = NULL;
	for (int i = 0; i < num_chips; i++) {
		if (!match) {
			struct gpiod_chip *c = chip_open_by_name(entries[i]->d_name);
			if (c) {
#if EVGPIO_GPIOD_V == 2
				struct gpiod_chip_info *info = gpiod_chip_get_info(c);
				const char *label = info ? gpiod_chip_info_get_label(info) : NULL;
				bool hit = label && strcmp(label, chipLabel) == 0;
				if (info) gpiod_chip_info_free(info);
#else
				const char *label = gpiod_chip_label(c);
				bool hit = label && strcmp(label, chipLabel) == 0;
#endif
				if (hit) {
					match = c;
					log(SysGPIO, LogDebug,
					    "find_chip_by_label(%s): scan matched %s",
					    chipLabel, entries[i]->d_name);
				} else
					gpiod_chip_close(c);
			}
		}
		free(entries[i]);
	}
	free(entries);
	return match;
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
    gpiod_request_config_set_consumer(rc, consumer);
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
      ? gpiod_line_request_rising_edge_events_flags(_line, consumer, flags)
      : gpiod_line_request_input_flags(_line, consumer, flags);
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
  int rv = gpiod_line_request_output(_line, consumer, initial);
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

void EventGPIOPin::drainEvents() {
  // _edge_ok is the single source of truth eventFd() also uses. It can only
  // become true via requestWithEdges(), which on v2 lazily allocates _evbuf
  // and returns false if that allocation fails -- so _edge_ok true implies a
  // non-NULL _evbuf here too, not just at eventFd(). No separate _evbuf
  // check needed; see requestWithEdges() for where that invariant is made.
  if (!_edge_ok || _line == NULL) return;

#if EVGPIO_GPIOD_V == 1
  // Zero timeout keeps this non-blocking: gpiod_line_event_read() on its own
  // would block once the queue empties.
  struct timespec zero = {0, 0};
  struct gpiod_line_event ev;
  while (gpiod_line_event_wait(_line, &zero) == 1) {
    if (gpiod_line_event_read(_line, &ev) != 0) break;
  }
#else
  while (gpiod_line_request_wait_edge_events(_line, 0) == 1) {
    if (gpiod_line_request_read_edge_events(_line, _evbuf, 16) <= 0) break;
  }
#endif
}

#undef consumer

#endif  // ARDULINUX_HARDWARE
