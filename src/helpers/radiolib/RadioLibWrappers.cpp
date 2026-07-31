
#define RADIOLIB_STATIC_ONLY 1
#include "RadioLibWrappers.h"

#define STATE_IDLE       0
#define STATE_RX         1
#define STATE_TX_WAIT    3
#define STATE_TX_DONE    4
#define STATE_INT_READY 16

// How often the noise floor is sampled, in wall-clock terms. Sampling is rate
// limited here rather than taken once per loop() call so the estimate does not
// depend on how fast the host happens to iterate: an idle Linux daemon blocking
// on poll() and an MCU spinning flat out must characterise the channel the same
// way. It also keeps consecutive samples far enough apart to be worth taking --
// back-to-back GET_RSSI_INST reads are correlated, so they add little
// information for their SPI cost.
#ifndef NOISE_SAMPLE_INTERVAL_MS
  #define NOISE_SAMPLE_INTERVAL_MS  100
#endif

// Minimum busy-channel margin, as a multiple of the estimated noise sigma. This
// is what sets the false-alarm rate of the interference check: for normally
// distributed noise, P(sample > mean + 3.5 sigma) = 2.3e-4 per isChannelActive()
// call, so a transmit attempt (a handful of calls) defers spuriously about once
// in a thousand -- comfortably absorbed by the CSMA backoff that follows.
//
// Applied as a floor under the operator's configured dB margin, not as a
// replacement for it: interference_threshold keeps meaning dB, but a margin
// narrower than the noise itself can no longer produce continuous false busy
// and trip ERR_EVENT_CAD_TIMEOUT.
#ifndef NOISE_THRESHOLD_SIGMA_K
  #define NOISE_THRESHOLD_SIGMA_K  3.5f
#endif

// One debug line per this many samples (20 * 100 ms = 2 s), matching the log
// volume of the batch estimator this replaced.
#ifndef NOISE_LOG_EVERY_N_SAMPLES
  #define NOISE_LOG_EVERY_N_SAMPLES  20
#endif

static volatile uint8_t state = STATE_IDLE;

// this function is called when a complete packet
// is transmitted by the module
static 
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  // we sent a packet, set the flag
  state |= STATE_INT_READY;
}

void RadioLibWrapper::begin() {
  _radio->setPacketReceivedAction(setFlag);  // this is also SentComplete interrupt
  _preamble_sf = getSpreadingFactor();
  _radio->setPreambleLength(preambleLengthForSF(_preamble_sf)); // longer preamble for lower SF improves reliability
  state = STATE_IDLE;

  if (_board->getStartupReason() == BD_STARTUP_RX_PACKET) {  // received a LoRa packet (while in deep sleep)
    setFlag(); // LoRa packet is already received
  }

  _noise_floor = 0;
  _threshold = 0;
  _cad_enabled = false;

  _nf.reset();
  _next_noise_sample = millis();
}

uint32_t RadioLibWrapper::getRngSeed() {
  return _radio->random(0x7FFFFFFF);
}

void RadioLibWrapper::setTxPower(int8_t dbm) {
  _radio->setOutputPower(dbm);
}

void RadioLibWrapper::idle() {
  _radio->standby();
  state = STATE_IDLE;   // need another startReceive()
}

void RadioLibWrapper::triggerNoiseFloorCalibrate(int threshold) {
  // The estimator now runs continuously, so there is no calibration batch to
  // start and nothing here is periodic any more -- this only conveys the
  // operator's interference threshold. Kept on the mesh::Radio interface, and
  // still called on Dispatcher's 2 s timer, so that no caller has to change.
  _threshold = threshold;
}

void RadioLibWrapper::doResetAGC() {
  _radio->sleep();  // warm sleep to reset analog frontend
}

void RadioLibWrapper::resetAGC() {
  // make sure we're not mid-receive of packet!
  if ((state & STATE_INT_READY) != 0 || isReceivingPacket()) return;

  doResetAGC();
  state = STATE_IDLE;   // trigger a startReceive()

  // Re-seed the noise floor estimate: the analog frontend just changed, so
  // everything learned before it is about a different receiver.
  //
  // (This used to also work around a self-reinforcing stuck floor: the old
  // estimator only accepted samples below `floor + 14`, so a floor stuck at
  // -120 rejected every normal ~-105 sample forever. NoiseFloorTracker has no
  // estimate-dependent acceptance test, so that failure mode is gone.)
  _noise_floor = 0;
  _nf.reset();
}

void RadioLibWrapper::loop() {
  // Only sample while actually listening: during TX or standby the RSSI reading
  // describes nothing about the channel.
  if (state != STATE_RX) return;

  uint32_t now = millis();
  if ((int32_t)(now - _next_noise_sample) < 0) return;   // not due yet

  _next_noise_sample += NOISE_SAMPLE_INTERVAL_MS;
  if ((int32_t)(now - _next_noise_sample) >= 0) {
    // More than one interval elapsed -- we were transmitting, or the caller
    // stopped iterating for a while. Resync rather than catching up, otherwise
    // the next few iterations would each fire immediately and feed a burst of
    // correlated samples, which is exactly what the fixed rate exists to avoid.
    _next_noise_sample = now + NOISE_SAMPLE_INTERVAL_MS;
  }

  // Skip samples taken while the modem is demodulating: RSSI then reports the
  // strength of the signal being received, not the noise under it, and no
  // estimator can be robust to that because it is not contamination -- it is a
  // different quantity. At this sample rate a single LoRa packet is 20+
  // consecutive readings, so these arrive in runs long enough for the two
  // quantile trackers to chase them, which is precisely how a live repeater
  // ended up reporting -70 dBm against a true floor of -114.
  //
  // This does bias the sample population toward quieter moments. That bias is
  // real but second order, and far preferable to averaging in signal power.
  // Unlike the batch estimator this replaced, a skipped sample no longer
  // extends a measurement window -- the next sample simply comes 100 ms later
  // regardless -- so the bias no longer compounds into an unbounded stall.
  if (isReceivingPacket()) return;

  _nf.addSample(getCurrentRSSI());
  _noise_floor = _nf.floorDbm();

  // Rate limit the log rather than printing on every change: the estimate now
  // updates continuously and jitters by ~1 dB, so "print when it changes" would
  // emit several lines a second. One line every NOISE_LOG_EVERY_N_SAMPLES keeps
  // debug output at roughly the volume the 2 s batch estimator produced.
  if (++_noise_log_ctr >= NOISE_LOG_EVERY_N_SAMPLES) {
    _noise_log_ctr = 0;
    MESH_DEBUG_PRINTLN("RadioLibWrapper: noise_floor = %d (sigma %d/10)",
                       (int)_noise_floor, (int)(_nf.sigma() * 10.0f));
  }
}

void RadioLibWrapper::startRecv() {
  int err = _radio->startReceive();
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_RX;
  } else {
    MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
  }
}

bool RadioLibWrapper::isInRecvMode() const {
  return (state & ~STATE_INT_READY) == STATE_RX;
}

int RadioLibWrapper::recvRaw(uint8_t* bytes, int sz) {
  int len = 0;
  if (state & STATE_INT_READY) {
    len = _radio->getPacketLength();
    if (len > 0) {
      if (len > sz) { len = sz; }
      int err = _radio->readData(bytes, len);
      if (err != RADIOLIB_ERR_NONE) {
        MESH_DEBUG_PRINTLN("RadioLibWrapper: error: readData(%d)", err);
        len = 0;
        n_recv_errors++;
      } else {
      //  Serial.print("  readData() -> "); Serial.println(len);
        n_recv++;
      }
    }
    state = STATE_IDLE;   // need another startReceive()
  }

  if (state != STATE_RX) {
    int err = _radio->startReceive();
    if (err == RADIOLIB_ERR_NONE) {
      state = STATE_RX;
    } else {
      MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
    }
  }
  return len;
}

uint32_t RadioLibWrapper::getEstAirtimeFor(int len_bytes) {
  return _radio->getTimeOnAir(len_bytes) / 1000;
}

bool RadioLibWrapper::startSendRaw(const uint8_t* bytes, int len) {
  _board->onBeforeTransmit();
  int err = _radio->startTransmit((uint8_t *) bytes, len);
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_TX_WAIT;
    return true;
  }
  MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startTransmit(%d)", err);
  idle();   // trigger another startRecv()
  _board->onAfterTransmit();
  return false;
}

bool RadioLibWrapper::isSendComplete() {
  if (state & STATE_INT_READY) {
    state = STATE_IDLE;
    n_sent++;
    return true;
  }
  return false;
}

void RadioLibWrapper::onSendFinished() {
  _radio->finishTransmit();
  _board->onAfterTransmit();
  state = STATE_IDLE;
}

int16_t RadioLibWrapper::performChannelScan() {
  return _radio->scanChannel();
}

bool RadioLibWrapper::isChannelActive() {
  // int.thresh: RSSI-based interference detection (relative to noise floor).
  // Skipped when the check is disabled (_threshold == 0), and while the
  // estimator has no floor yet -- neither is a reason to skip the CAD check
  // below, so these are a guard rather than an early return.
  if (_threshold != 0 && _nf.ready()) {
    // The operator's configured dB margin still means dB, so existing
    // interference_threshold settings behave as before. What is new is the floor
    // under it: a margin narrower than NOISE_THRESHOLD_SIGMA_K sigma would fire on
    // noise alone, and a channel that reads busy continuously does not protect
    // anything -- it just delays every packet until getCADFailMaxDuration()
    // expires and the node transmits regardless.
    float margin = (float) _threshold;
    float min_margin = NOISE_THRESHOLD_SIGMA_K * _nf.sigma();
    if (margin < min_margin) margin = min_margin;

    if (getCurrentRSSI() > (float)_noise_floor + margin) return true;
  }

  // cad: hardware channel activity detection
  if (_cad_enabled) {
    int16_t result = performChannelScan();
    // scanChannel() triggers DIO interrupt (CAD done) which sets STATE_INT_READY
    // via setFlag() ISR. Clear it before restarting RX so recvRaw() doesn't
    // try to read a non-existent packet and count a spurious recv error.
    state = STATE_IDLE;
    startRecv();
    if (result != RADIOLIB_CHANNEL_FREE) return true;
  }

  return false;
}

float RadioLibWrapper::getLastRSSI() const {
  return _radio->getRSSI();
}
float RadioLibWrapper::getLastSNR() const {
  return _radio->getSNR();
}

// Approximate SNR threshold per SF for successful reception (based on Semtech datasheets)
static float snr_threshold[] = {
    -7.5,  // SF7 needs at least -7.5 dB SNR
    -10,   // SF8 needs at least -10 dB SNR
    -12.5, // SF9 needs at least -12.5 dB SNR
    -15,  // SF10 needs at least -15 dB SNR
    -17.5,// SF11 needs at least -17.5 dB SNR
    -20   // SF12 needs at least -20 dB SNR
};
  
float RadioLibWrapper::packetScoreInt(float snr, int sf, int packet_len) {
  if (sf < 7) return 0.0f;
  
  if (snr < snr_threshold[sf - 7]) return 0.0f;    // Below threshold, no chance of success

  auto success_rate_based_on_snr = (snr - snr_threshold[sf - 7]) / 10.0;
  auto collision_penalty = 1 - (packet_len / 256.0);   // Assuming max packet of 256 bytes

  return max(0.0, min(1.0, success_rate_based_on_snr * collision_penalty));
}

PacketMillis RadioLibWrapper::calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols) {
  // based on RadioLib's calculateTimeOnAir()
  uint32_t tsym_us = ((uint32_t)10000 << sf) / (bw * 10);
  uint32_t sfCoeff1_x4 = (sf == 5 || sf == 6) ? 25 : 17; // 6.25 : 4.25, semtech magic numbers to account for sync word + sfd

  // preamble + syncword + sfd + header
  uint32_t preamble_us = (((preambleSymbols + 8) * 4 + sfCoeff1_x4) * tsym_us) / 4;
  
  // airtime for max packet at current radio settings
  uint32_t total_us   = _radio->getTimeOnAir(MAX_TRANS_UNIT);
  // airtime for payload only (no preamble, header or SOF)
  uint32_t payload_us = total_us > preamble_us ? total_us - preamble_us : 4000 - preamble_us; // fallback to 4 secs at worst case
  // rescale payload_us for max possible CR
  if (cr >= 5 && cr < 8) { payload_us = (payload_us * 8) / cr; }

  return PacketMillis {(preamble_us + 999) / 1000, (payload_us + 999) / 1000};
}