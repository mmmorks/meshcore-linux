#pragma once

#include "CustomSX1262Wrapper.h"
#include "LinuxSX1262.h"
#include "LinuxRadioWait.h"
#include "SX126xReset.h"

// LinuxSX1262 is a CustomSX1262, so its wrapper is a CustomSX1262Wrapper.
//
// setParams(), isReceivingPacket(), the RSSI/SNR readers, packetScore(),
// getSpreadingFactor(), set/getRxBoostedGainMode() and powerOff() were all
// byte-for-byte the base class's implementation with a different cast spelled
// out, so they are inherited rather than restated. That is also what keeps this
// file honest against upstream: a method added to RadioLibWrapper now arrives
// here implemented, the same way it arrives on every other SX126x board,
// instead of breaking the Linux build (pure virtual) or silently no-opping on
// it alone (virtual with a default).
class LinuxSX1262Wrapper : public CustomSX1262Wrapper {
  // How long performChannelScan() will wait for DIO1 before giving up on the
  // line and reading the result over SPI. Set from the active SF/BW by
  // setParams(); the initial value covers only the window before the first
  // call, so it is seeded from the slowest scan a MeshCore preset can produce
  // (SF12 at 62.5 kHz) rather than a hand-checked constant. CAD cannot actually
  // run in that window -- _cad_enabled stays false until Dispatcher::loop()
  // first pushes it -- so this is belt-and-braces rather than a live value.
  uint32_t _cad_timeout_ms = cadTimeoutMillis(symbolMicros(12, 62.5f));

  // _radio is held as the base mesh::Radio. The inherited members downcast it to
  // CustomSX1262, which is as far as they need to see; this names the
  // LinuxSX1262 downcast for the parts only the Linux subclass has. It is always
  // a LinuxSX1262 -- the constructor takes one by reference -- so this is a
  // naming convenience, not a checked conversion.
  LinuxSX1262* r() const { return (LinuxSX1262 *)_radio; }

  // Same for the board. waitForRadioIrq() is LinuxBoard's, not
  // mesh::MainBoard's, and this reaches it through the member the wrapper was
  // constructed with rather than through the `board` global LinuxSX1262.h
  // declares. Those are the same object today; going through the member is what
  // keeps them the same object if a second instance is ever constructed, and
  // stops this file quietly depending on a global it does not own.
  LinuxBoard* b() const { return (LinuxBoard *)_board; }

public:
  LinuxSX1262Wrapper(LinuxSX1262& radio, mesh::MainBoard& board) : CustomSX1262Wrapper(radio, board) { }

  // Re-added only to size the CAD wait from the active SF/BW. Everything else
  // the base class already does, so it does it -- this is not a reimplementation.
  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    CustomSX1262Wrapper::setParams(freq, bw, sf, cr);
    _cad_timeout_ms = cadTimeoutMillis(symbolMicros(sf, bw));
  }

  // Hardware CAD without RadioLib's busy-wait.
  //
  // The base implementation calls scanChannel(), which spins on
  // digitalRead(DIO1) until the line rises. On an MCU with nothing else to do
  // that is merely wasteful; here it burns a core for the length of every scan,
  // against an event loop built to sleep, and -- because it has no deadline --
  // it turns a GPIO read that has started failing into an unbreakable hang.
  // EventGPIOPin deliberately reads LOW on failure so a broken line degrades to
  // "no packet" plus one logged error; inside an untimed spin that same failure
  // would lock up the daemon.
  //
  // Splitting the scan into start / wait / read fixes both. Nothing is lost by
  // blocking here: startChannelScan() puts the modem in standby first, so no
  // packet can arrive during the scan and there is nothing for the loop to
  // overlap with.
  int16_t performChannelScan() override {
    // Same configuration scanChannel() used: 4 symbols, exit to STDBY_RC, and
    // DIO1 mapped to CAD_DONE | CAD_DETECTED. CAD_DONE being in that mask is
    // what the wait below depends on -- the line rises however the scan
    // resolves, so a free channel arrives as an edge and not as a timeout.
    // startChannelScan() also clears the IRQ status, so DIO1 is low on entry.
    int16_t state = r()->startChannelScan();
    if (state != RADIOLIB_ERR_NONE) {
      MESH_DEBUG_PRINTLN("LinuxSX1262Wrapper: startChannelScan() failed (%d)", state);
      return state;   // isChannelActive() reads anything but CHANNEL_FREE as busy
    }

    if (!b()->waitForRadioIrq(_cad_timeout_ms)) {
      // Logged every time rather than latched: the rate is bounded by transmit
      // attempts, and a line that has stopped reporting should stay visible for
      // as long as it is broken.
      MESH_DEBUG_PRINTLN("LinuxSX1262Wrapper: CAD IRQ did not arrive within %ums", _cad_timeout_ms);
    }

    // Read the verdict whether or not DIO1 reported it. getChannelScanResult()
    // goes over SPI to the modem's IRQ status register, which is authoritative
    // and wholly independent of the GPIO -- so a line that never reports (dead,
    // or never configured) costs latency and a log line, never a wrong answer,
    // and never a hang. What it must not do is cut the wait short: the status
    // register is only authoritative once the scan has had time to finish.
    return r()->getChannelScanResult();
  }

  // The one override that is not the inherited behaviour. Recalibration drops
  // DIO2-as-RF-switch, RX boosted gain and the 0x8B5 patch, and the inherited
  // version restores the first and third from SX126X_* build flags -- none of
  // which this variant defines, so on Linux it would silently drop both.
  // rxSettings() supplies them from meshcored.ini instead.
  //
  // The gain is read back off the chip, exactly as the inherited version does
  // it, and deliberately not taken from rxSettings(): the ini value is only the
  // starting point, and both the persisted pref and `set radio.rxgain` change
  // it afterwards. Restoring the ini value here would revert them at the first
  // agc.reset.interval tick, with `get radio.rxgain` still reporting the value
  // the radio had stopped using.
  void doResetAGC() override {
    SX126xRxSettings rx = r()->rxSettings();
    rx.rx_boosted_gain = getRxBoostedGainMode();
    sx126xResetAGC(r(), rx);
  }
};
