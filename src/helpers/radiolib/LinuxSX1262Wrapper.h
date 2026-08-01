#pragma once

#include "LinuxSX1262.h"
#include "LinuxRadioWait.h"
#include "RadioLibWrappers.h"
#include "SX126xReset.h"

class LinuxSX1262Wrapper : public RadioLibWrapper {
  // How long performChannelScan() will wait for DIO1 before giving up on the
  // line and reading the result over SPI. Set from the active SF/BW by
  // setParams(); the initial value covers only the window before the first
  // call, so it is seeded from the slowest scan a MeshCore preset can produce
  // (SF12 at 62.5 kHz) rather than a hand-checked constant. CAD cannot actually
  // run in that window -- _cad_enabled stays false until Dispatcher::loop()
  // first pushes it -- so this is belt-and-braces rather than a live value.
  uint32_t _cad_timeout_ms = cadTimeoutMillis(symbolMicros(12, 62.5f));

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

    if (!board.waitForRadioIrq(_cad_timeout_ms)) {
      // Logged every time rather than latched: the rate is bounded by transmit
      // attempts, and a line that has stopped reporting should stay visible for
      // as long as it is broken.
      MESH_DEBUG_PRINTLN("LinuxSX1262Wrapper: CAD IRQ did not arrive within %ums", _cad_timeout_ms);
    }

    // Read the verdict whether or not DIO1 reported it. getChannelScanResult()
    // goes over SPI to the modem's IRQ status register, which is authoritative
    // and wholly independent of the GPIO -- so a dead line costs latency and a
    // log line, never a wrong answer, and never a hang.
    return r()->getChannelScanResult();
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
    SX126xRxSettings rx = r()->rxSettings();
    sx126xResetAGC(r(), &rx);
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
