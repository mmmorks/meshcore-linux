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
  bool requestWithEdges(PinMode m);   // input + rising-edge detection
  bool requestPlainInput(PinMode m);  // fallback, no edge detection
  bool requestOutput(PinStatus initial);

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
#if EVGPIO_GPIOD_V == 2
  struct gpiod_edge_event_buffer* _evbuf = NULL;
#endif
};

#endif  // ARDULINUX_HARDWARE
