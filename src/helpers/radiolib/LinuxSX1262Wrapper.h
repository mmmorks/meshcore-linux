#pragma once

#include "LinuxSX1262.h"
#include "RadioLibWrappers.h"
#include "SX126xReset.h"

class LinuxSX1262Wrapper : public RadioLibWrapper {
  // _radio is held as the base mesh::Radio, so every use here needs the
  // downcast. It is always a LinuxSX1262 -- the constructor takes one by
  // reference -- so this is a naming convenience, not a checked conversion.
  LinuxSX1262* r() const { return (LinuxSX1262 *)_radio; }

public:
  LinuxSX1262Wrapper(LinuxSX1262& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    r()->setFrequency(freq);
    r()->setSpreadingFactor(sf);
    r()->setBandwidth(bw);
    r()->setCodingRate(cr);
    updatePreamble(sf);
    PacketMillis pm = calcMaxPacketMillis(sf, bw, cr, preambleLengthForSF(sf));
    r()->setPreambleMillis(pm.preambleMillis);
    r()->setMaxPayloadMillis(pm.payloadMillis);
  }

  // Full SX126x receiver reset (warm sleep, recalibrate, re-image the configured
  // band), the same one every other SX126x board gets. Without this override the
  // base class falls back to a bare sleep(), so `set agc_int` was quietly a
  // weaker knob on Linux than everywhere else.
  //
  // The settings argument is the Linux-specific part: recalibration drops
  // DIO2-as-RF-switch, RX boosted gain and the 0x8B5 patch, and the shared
  // helper restores them from the SX126X_* build flags, none of which are
  // defined here -- those come from meshcored.ini. Handing it the runtime values
  // makes it restore the right ones, rather than restoring the wrong ones for
  // this caller to redo afterwards.
  void doResetAGC() override {
    sx126xResetAGC(r(), r()->rxSettings());
  }

  // Cold sleep, matching CustomSX1262Wrapper. The only caller shuts the daemon
  // down straight afterwards, so there is no configuration worth retaining and
  // the deeper state is the better one to leave the modem in.
  void powerOff() override {
    r()->sleep(false);
  }

  bool isReceivingPacket() override {
    return r()->isReceiving();
  }
  float getCurrentRSSI() override {
    return r()->getRSSI(false);
  }
  float getLastRSSI() const override { return r()->getRSSI(); }
  float getLastSNR() const override { return r()->getSNR(); }

  float packetScore(float snr, int packet_len) override {
    return packetScoreInt(snr, r()->spreadingFactor, packet_len);
  }
  uint8_t getSpreadingFactor() const override { return r()->spreadingFactor; }

  bool setRxBoostedGainMode(bool en) override {
    return r()->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }
  bool getRxBoostedGainMode() const override {
    return r()->getRxBoostedGainMode();
  }
};
