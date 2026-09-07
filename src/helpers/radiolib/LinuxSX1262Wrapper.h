#pragma once

#include "CustomSX1262Wrapper.h"
#include "LinuxSX1262.h"
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
  // _radio is held as the base mesh::Radio. The inherited members downcast it to
  // CustomSX1262, which is as far as they need to see; this names the
  // LinuxSX1262 downcast for the parts only the Linux subclass has. It is always
  // a LinuxSX1262 -- the constructor takes one by reference -- so this is a
  // naming convenience, not a checked conversion.
  LinuxSX1262* r() const { return (LinuxSX1262 *)_radio; }

public:
  LinuxSX1262Wrapper(LinuxSX1262& radio, mesh::MainBoard& board) : CustomSX1262Wrapper(radio, board) { }

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
