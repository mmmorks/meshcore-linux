#pragma once

#include <Mesh.h>
#include <RadioLib.h>
#include <helpers/NoiseFloorTracker.h>

struct PacketMillis {
  uint32_t preambleMillis;  // preamble-detect -> header-valid deadline
  uint32_t payloadMillis;   // header-valid   -> rx-done deadline
};

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent, n_recv_errors;
  int16_t _noise_floor, _threshold;
  bool _cad_enabled;
  // _num_floor_samples/_floor_sample_sum (the upstream batch estimator) are gone:
  // NoiseFloorTracker replaces them with a continuous, fixed-rate estimate.
  NoiseFloorTracker _nf;
  uint32_t _next_noise_sample;   // millis() deadline for the next RSSI read
  uint8_t _noise_log_ctr;        // rate limiter for the noise-floor debug line
  uint8_t _preamble_sf;

  void idle();
  void startRecv();
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;
  virtual void doResetAGC();

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board)
    : _radio(&radio), _board(&board), _next_noise_sample(0), _noise_log_ctr(0), _preamble_sf(0)
  { n_recv = n_sent = 0; }

  void begin() override;
  virtual void powerOff() { _radio->sleep(); }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override {
    if (isReceivingPacket()) return true;

    return isChannelActive();
  }

  virtual void setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
  uint32_t getRngSeed();
  void setTxPower(int8_t dbm);

  virtual float getCurrentRSSI() =0;
  virtual uint8_t getSpreadingFactor() const { return LORA_SF; }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  // LoRa symbol time in microseconds, for a spreading factor and a bandwidth in
  // kHz. Every airtime and timeout derived from the modem's rate starts here.
  static uint32_t symbolMicros(uint8_t sf, float bw) { return ((uint32_t)10000 << sf) / (bw * 10); }

  // Discard the noise-floor estimate because the receiver it characterises has
  // changed. Bandwidth is the big one -- the thermal floor moves ~6 dB going
  // from 62.5 to 250 kHz -- but frequency and LNA gain move it too.
  //
  // Without this the estimate can only *rise* as contaminated sub-windows age
  // out of the ring, so a floor that has genuinely jumped up takes the full
  // NOISE_TRACKER_SUB_WINDOWS * NOISE_TRACKER_SUB_SAMPLES window (180 s at the
  // default sampling rate) to be reported. That asymmetry is right for a floor
  // that drifts and wrong for one the operator has just moved: with
  // interference_threshold set, isChannelActive() would read busy against a
  // stale floor and defer every transmit to getCADFailMaxDuration() for three
  // minutes. The batch estimator this replaced re-converged in ~2 s, so the
  // reset is what keeps a runtime `set bw` as cheap as it used to be.
  //
  // _noise_floor goes with it: getNoiseFloor() reports the cached value, and
  // leaving it behind would keep serving the old floor to isChannelActive() and
  // to telemetry until the first sub-window of the new configuration closes.
  //
  // The cost, which is the whole reason this is a judgement call rather than an
  // obvious win: for the ~3 s until the first sub-window closes, _nf.ready() is
  // false, so isChannelActive() skips the interference-threshold branch
  // entirely and getNoiseFloor() reports 0. The node transmits over the top of
  // anything that check would have caught, and telemetry shows an uncalibrated
  // floor. That is accepted deliberately -- 3 s of no RSSI check beats 180 s of
  // a wrong one, and the CAD check is unaffected throughout -- but a caller
  // adding a new reset site should know it is spending that, not nothing.
  void resetNoiseFloor() { _nf.reset(); _noise_floor = 0; _noise_log_ctr = 0; }

  // Called by every setParams() override, which is why the reset above lives
  // here: it is the one hook every radio family already routes a parameter
  // change through.
  void updatePreamble(uint8_t sf) {
    _preamble_sf = sf;
    _radio->setPreambleLength(preambleLengthForSF(sf));
    resetNoiseFloor();
  }
  PacketMillis calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols);
  virtual int16_t performChannelScan();

  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getPacketsSent() const { return n_sent; }
  void resetStats() { n_recv = n_sent = n_recv_errors = 0; }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10

  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
  }
};
