#pragma once

#include "LinuxSX1262.h"
#include "LinuxRadioWait.h"
#include "RadioLibWrappers.h"

class LinuxSX1262Wrapper : public RadioLibWrapper {
  // How long performChannelScan() will wait for DIO1 before giving up on the
  // line and reading the result over SPI. Set from the active SF/BW by
  // setParams(); the default only covers the window before the first call,
  // where it must exceed the slowest scan a MeshCore preset produces (SF12 at
  // 62.5 kHz, 544 ms). CAD cannot actually run in that window -- _cad_enabled
  // stays false until Dispatcher::loop() first pushes it -- so this is a
  // belt-and-braces value rather than a live one.
  uint32_t _cad_timeout_ms = 550;

public:
  LinuxSX1262Wrapper(LinuxSX1262& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    ((LinuxSX1262 *)_radio)->setFrequency(freq);
    ((LinuxSX1262 *)_radio)->setSpreadingFactor(sf);
    ((LinuxSX1262 *)_radio)->setBandwidth(bw);
    ((LinuxSX1262 *)_radio)->setCodingRate(cr);
    updatePreamble(sf);
    PacketMillis pm = calcMaxPacketMillis(sf, bw, cr, preambleLengthForSF(sf));
    ((LinuxSX1262 *)_radio)->setPreambleMillis(pm.preambleMillis);
    ((LinuxSX1262 *)_radio)->setMaxPayloadMillis(pm.payloadMillis);
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
    LinuxSX1262* radio = (LinuxSX1262 *)_radio;

    // Same configuration scanChannel() used: 4 symbols, exit to STDBY_RC, and
    // DIO1 mapped to CAD_DONE | CAD_DETECTED. CAD_DONE being in that mask is
    // what the wait below depends on -- the line rises however the scan
    // resolves, so a free channel arrives as an edge and not as a timeout.
    // startChannelScan() also clears the IRQ status, so DIO1 is low on entry.
    int16_t state = radio->startChannelScan();
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
    return radio->getChannelScanResult();
  }

  bool isReceivingPacket() override {
    return ((LinuxSX1262 *)_radio)->isReceiving();
  }
  float getCurrentRSSI() override {
    return ((LinuxSX1262 *)_radio)->getRSSI(false);
  }
  float getLastRSSI() const override { return ((LinuxSX1262 *)_radio)->getRSSI(); }
  float getLastSNR() const override { return ((LinuxSX1262 *)_radio)->getSNR(); }

  float packetScore(float snr, int packet_len) override {
    int sf = ((LinuxSX1262 *)_radio)->spreadingFactor;
    return packetScoreInt(snr, sf, packet_len);
  }
  uint8_t getSpreadingFactor() const override { return ((LinuxSX1262 *)_radio)->spreadingFactor; }

  bool setRxBoostedGainMode(bool en) override {
    return ((LinuxSX1262 *)_radio)->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }
  bool getRxBoostedGainMode() const override {
    return ((LinuxSX1262 *)_radio)->getRxBoostedGainMode();
  }
};
