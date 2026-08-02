
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

// Payload budget calcMaxPacketMillis() assumes when the modem cannot tell it
// how long a packet takes. Long on purpose: this deadline exists to break a
// stuck header IRQ, and one that expires early would clear the flags of a
// packet still arriving.
#ifndef MAX_PACKET_FALLBACK_PAYLOAD_US
  #define MAX_PACKET_FALLBACK_PAYLOAD_US  4000000UL
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

  _threshold = 0;
  _cad_enabled = false;

  resetNoiseFloor();          // clears _nf, _noise_floor and the log rate limiter
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

  // Deliberately does NOT reset the noise floor estimate.
  //
  // It used to, on the reasoning that the analog frontend had just changed so
  // everything learned before it was about a different receiver. Measured on a
  // live repeater that was strictly harmful: reset() re-seeds from a single
  // sample, and the guard meant to keep that sample clean cannot do its job
  // here. resetAGC() has just been through sleep() -> startReceive(), which
  // clears the modem's IRQ flags, so a packet already in the air is joined
  // mid-symbol -- its preamble and header are long past and neither will ever
  // set again for that packet. isReceivingPacket() therefore reports "idle"
  // precisely when it is most wrong, and stays wrong for the rest of the
  // packet. 20 of 257 re-seeds over 20 h landed on signal that way, throwing
  // the reported floor to -85..-95 dBm against a true floor of -114.
  //
  // Nothing is lost by keeping the estimate: an AGC reset does not move the
  // noise floor by anything like the estimator's tracking range, and if it
  // genuinely did, the estimator follows real floor changes on its own.
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

  // Sampled unconditionally, including mid-packet. NoiseFloorTracker estimates
  // the floor as a window minimum, and signal only ever adds power, so readings
  // taken during reception are discarded by construction rather than needing to
  // be gated out. Two things go away with the gate:
  //
  //  - the selection bias it imposed, by restricting the sample population to
  //    moments the modem considered quiet;
  //  - a dependency on isReceivingPacket(), which is not a pure read. It drives
  //    a timeout state machine and calls clearIrqFlags(), so polling it at
  //    10 Hz from the noise sampler would have this path participating in
  //    packet detection. Sampling the noise floor must not perturb reception.
  //
  // The gate was also a liability in its own right: before the IRQ-timeout fix
  // in CustomSX1262::isReceiving(), a latched PREAMBLE_DETECTED that never
  // completed into a packet held it true until the next startReceive(),
  // silently suspending noise sampling for seconds at a time.
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
        // Signal quality of the packet that just failed. A CRC mismatch (-7) is
        // the common case and says nothing on its own about *why*: a packet at
        // the edge of the demodulator and one lost to a collision both land
        // here. The modem's packet-status registers are written whether or not
        // the CRC passed, so this reads the same values a successful receive
        // would report, at no extra SPI cost -- enough to tell a failure
        // distribution sitting on the SF's SNR floor apart from one spread
        // across strong signals.
        //
        // SNR is scaled by 4 rather than truncated because the threshold this
        // is meant to resolve is a fraction of a dB wide, and %f is not
        // portable across every platform this file builds for. Same quarter-dB
        // convention as Packet::_snr.
        MESH_DEBUG_PRINTLN("RadioLibWrapper: error: readData(%d) len=%d rssi=%d snr4=%d",
                           err, len, (int)getLastRSSI(), (int)(getLastSNR() * 4));
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
  uint32_t tsym_us = symbolMicros(sf, bw);
  uint32_t sfCoeff1_x4 = (sf == 5 || sf == 6) ? 25 : 17; // 6.25 : 4.25, semtech magic numbers to account for sync word + sfd

  // preamble + syncword + sfd + header
  uint32_t preamble_us = (((preambleSymbols + 8) * 4 + sfCoeff1_x4) * tsym_us) / 4;
  
  // airtime for max packet at current radio settings
  uint32_t total_us   = _radio->getTimeOnAir(MAX_TRANS_UNIT);
  // airtime for payload only (no preamble, header or SOF)
  uint32_t payload_us;
  if (total_us > preamble_us) {
    payload_us = total_us - preamble_us;
  } else {
    // getTimeOnAir() gave nothing usable (it returns 0 on an unconfigured
    // modem). Fall back to the 4 s this has always claimed -- as 4 s of
    // *payload*, not as 4 s of total airtime minus the preamble.
    //
    // The value used to be 4000, i.e. 4 ms, and the subtraction underflowed for
    // any setting whose preamble exceeds that: every one of them. An underflow
    // here is not a mis-sized deadline but the absence of one, because the
    // result becomes a ~49-day payload watchdog, so CustomSX1262::isReceiving()
    // would hold a latched HEADER_VALID true forever and isReceiving() would
    // never again report the channel idle.
    payload_us = MAX_PACKET_FALLBACK_PAYLOAD_US;
  }
  // rescale payload_us for max possible CR
  if (cr >= 5 && cr < 8) { payload_us = (payload_us * 8) / cr; }

  return PacketMillis {(preamble_us + 999) / 1000, (payload_us + 999) / 1000};
}