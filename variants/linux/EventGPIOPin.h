#pragma once

#ifdef ARDULINUX_HARDWARE

#include "Arduino.h"
#include "ArduLinuxGPIO.h"
#include "LinuxEventSource.h"

#include <gpiod.h>

// libgpiod major-version detection, mirroring ardulinux's LinuxGPIOPin.h.
// gpiod v1 defines GPIOD_LINE_BULK_MAX_LINES; v2 does not.
//
// Deliberately NOT named GPIOD_V: ardulinux's LinuxGPIOPin.h defines that
// symbol along with macro aliases (gpiod_line -> gpiod_line_request, etc.)
// that would collide if both headers reach one translation unit.
#ifndef GPIOD_LINE_BULK_MAX_LINES
  #define EVGPIO_GPIOD_V 2
#else
  #define EVGPIO_GPIOD_V 1
#endif

#if EVGPIO_GPIOD_V == 2
  typedef struct gpiod_line_request evgpio_line_t;
#else
  typedef struct gpiod_line evgpio_line_t;
#endif

// An ArduLinux GPIO pin that additionally exposes a pollable edge-event
// descriptor, so the main loop can block instead of spinning.
//
// Used only for the LoRa DIO1/IRQ line. Every other pin keeps using ardulinux's
// LinuxGPIOPin: they are outputs or synchronous reads with no events to wait on.
//
// The event descriptor is only a wake-up hint. Packet correctness still comes
// from ardulinux's gpioIdle() level-read-and-fire-ISR path, which is untouched,
// so losing edge detection degrades latency rather than dropping packets.
class EventGPIOPin : public GPIOPin, public LinuxEventSource {
public:
  // Throws std::invalid_argument if the chip or line cannot be acquired at all.
  // Failing to get *edge detection* specifically is not an error: the pin still
  // works, hasEdgeDetection() returns false, and eventFd() returns -1.
  EventGPIOPin(pin_size_t n, const char* chipLabel, int lineOffset,
               const char* pinName);
  ~EventGPIOPin() override;

  bool hasEdgeDetection() const { return _edge_ok; }

  // LinuxEventSource
  int  eventFd() const override;
  void drainEvents() override;

protected:
  // GPIOPin
  PinStatus readPinHardware() override;
  void      writePin(PinStatus s) override;
  void      setPinMode(PinMode m) override;

private:
  // Request the line as an input, with or without rising-edge detection. The
  // two differ by one setting on v2 and by which gpiod_line_request_* family is
  // called on v1, so they share one body; the named wrappers below keep the
  // call sites (and the invariants documented against them) reading the same.
  bool requestInput(PinMode m, bool with_edges);
  bool requestWithEdges(PinMode m)  { return requestInput(m, true); }
  bool requestPlainInput(PinMode m) { return requestInput(m, false); }
  bool requestOutput(PinStatus initial);

  // Release the line/chip/event-buffer (whichever are currently held) and
  // null them out. Shared by the destructor and by the constructor's failure
  // paths: a constructor that throws never runs the destructor, so every
  // throw after a partial acquisition must call this itself or leak.
  void releaseResources();

#if EVGPIO_GPIOD_V == 2
  // Shared tail of requestWithEdges/requestPlainInput/requestOutput: wraps
  // `settings` in a line_config, requests the line (first call) or
  // reconfigures it (subsequent calls), and frees both `settings` and the
  // config it builds. Takes ownership of `settings` unconditionally, on
  // every return path.
  bool applySettings(struct gpiod_line_settings* settings);
#endif

  evgpio_line_t*     _line   = NULL;
  struct gpiod_chip* _chip   = NULL;
  unsigned int       _offset = 0;
  bool               _edge_ok = false;

  // Latches the first readPinHardware() failure so the log is not flooded from
  // a path called every event-loop iteration. Per-instance rather than a
  // function-local static: a static would let one pin's failure suppress
  // another's first report entirely.
  bool               _read_warned = false;
#if EVGPIO_GPIOD_V == 2
  struct gpiod_edge_event_buffer* _evbuf = NULL;
#endif

  // errno from the most recent failing libgpiod call in
  // requestWithEdges()/requestPlainInput()/requestOutput() (and, on v2,
  // applySettings()). Callers log strerror(_last_errno) rather than
  // strerror(errno): on v2 the request*() helpers run through applySettings(),
  // whose cleanup (gpiod_line_config_free()/gpiod_line_settings_free()) happens
  // between the failing call and the log site and can clobber errno first.
  // Captured immediately after each libgpiod call, at the point closest to
  // where it can still be trusted.
  int _last_errno = 0;
};

#endif  // ARDULINUX_HARDWARE
