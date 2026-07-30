#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <LinuxBoard.h>
#include <helpers/radiolib/CustomSX1276Wrapper.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include "LinuxSerialStream.h"
#ifdef DISPLAY_CLASS
  #include <helpers/ui/SSD1306Display.h>
  #include <helpers/ui/MomentaryButton.h>
#endif

#if (USE_CUSTOM_SX1262_WRAPPER)
#include <helpers/radiolib/LinuxSX1262Wrapper.h>
#endif

extern LinuxBoard board;
extern WRAPPER_CLASS radio_driver;
extern LinuxRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;
extern LinuxSerialStream gps_serial;
extern MicroNMEALocationProvider gps_location;

// True if a serial GPS device was configured and successfully opened.
// Consumed by EnvironmentSensorManager::initBasicGPS() on the Linux build.
bool linux_gps_present();

// Block until the radio IRQ fires, console or GPS input arrives, or a short
// timeout elapses. Called at the end of loop() on Linux so the process idles
// instead of spinning. See variants/linux/LinuxEventLoop.h.
void linux_event_wait();

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
uint32_t radio_get_rng_seed();
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void radio_set_tx_power(uint8_t dbm);
mesh::LocalIdentity radio_new_identity();
