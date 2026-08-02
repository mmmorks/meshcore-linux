#pragma once

#include <stdint.h>

// Tunables are macros rather than in-class `static constexpr float` so that
// ODR-using one (binding a reference, passing it to a function) cannot produce
// a link error on the C++11/14 toolchains some MeshCore targets build with.
// Each is overridable per-platform via build_flags.

#ifndef NOISE_TRACKER_SUB_SAMPLES
  // Samples per sub-window (15 s at a 100 ms sample interval). This is the
  // resolution at which the estimate can rise when the floor genuinely rises.
  #define NOISE_TRACKER_SUB_SAMPLES  150
#endif

#ifndef NOISE_TRACKER_FIRST_SUB_SAMPLES
  // The first sub-window is short so a fresh node has a usable floor in ~3 s
  // rather than 15 s. It lands in the ring alongside full-length sub-windows;
  // a minimum over fewer samples is biased high, and since the window estimate
  // takes the minimum across the ring, a high entry is simply never selected.
  #define NOISE_TRACKER_FIRST_SUB_SAMPLES  30
#endif

#ifndef NOISE_TRACKER_SUB_WINDOWS
  // Ring depth. Total window = SUB_WINDOWS * SUB_SAMPLES = 1800 samples = 180 s.
  // This is the estimator's robustness budget: any interference burst shorter
  // than the window leaves clean samples in it. Sized against measurement --
  // the longest excursion observed on a live repeater over 20 h was 67 s, so
  // 180 s carries ~2.7x margin. Costs one float each.
  #define NOISE_TRACKER_SUB_WINDOWS  12
#endif

#ifndef NOISE_TRACKER_MIN_SIGMA_DB
  // Scale floor. Stops a freshly seeded tracker from reporting a zero-width
  // noise distribution, which would collapse any threshold derived from sigma().
  #define NOISE_TRACKER_MIN_SIGMA_DB  0.5f
#endif

#ifndef NOISE_TRACKER_MAX_SIGMA_DB
  // Scale ceiling. Receiver noise spread is set by bandwidth, temperature and
  // LNA gain state; measured on a live SX1262 at 250 kHz the median sits at
  // 0.6-0.9 dB, so 3 dB is generous headroom and anything beyond it is not a
  // noise spread. NOISE_TRACKER_SCALE_GATE_DB is derived from this value;
  // raising one without the other leaves the gate as the binding constraint.
  #define NOISE_TRACKER_MAX_SIGMA_DB  3.0f
#endif

#ifndef NOISE_TRACKER_SIGMA_LAMBDA
  // EMA rate for the scale estimate (~50-sample time constant, 5 s at 100 ms
  // sampling). Slow on purpose: the spread physically changes far more slowly
  // than the mean.
  #define NOISE_TRACKER_SIGMA_LAMBDA  0.02f
#endif

#ifndef NOISE_TRACKER_SCALE_GATE_DB
  // Censoring gate for the scale estimate: samples more than this far above the
  // window statistic are treated as signal and take no part in it. Signal
  // energy must not widen the estimated noise spread.
  //
  // Measured from windowValue(), deliberately, and NOT from the floor estimate
  // or from any multiple of sigma. An earlier revision censored above
  // windowValue() + biasK*sigma + max(6 dB, 4*sigma): both of those terms grow
  // with the quantity the gate exists to protect, which closes a positive
  // feedback loop under sustained near-floor traffic. Packets a few dB above
  // the floor are admitted, they raise _dev, the wider gate then admits
  // stronger packets, and sigma ratchets up until MAX_SIGMA stops it. Simulated
  // against this exact code -- 80% channel occupancy, packets 8 dB above a
  // 0.8 dB floor -- sigma reached the 3.0 dB cap, which silently widens the
  // CSMA margin RadioLibWrapper derives from it (NOISE_THRESHOLD_SIGMA_K *
  // sigma) from 2.8 dB to 10.5 dB, overriding the operator's
  // interference_threshold in exactly the busy mesh where it was set. The same
  // run settles at 1.6 dB with the gate fixed.
  //
  // A fixed gate does not merely slow that loop, it removes it: admitting a
  // sample can no longer widen the set of samples admitted next, and since an
  // admitted sample raises _dev by at most the gate width, sigma is bounded
  // outright at GATE/biasK.
  //
  // The value is biasK * MAX_SIGMA (2.835 * 3.0): never look further above the
  // window statistic than the largest mean offset the estimator is allowed to
  // report, which makes the bound above exactly the MAX_SIGMA clamp -- now
  // reachable only by genuine spread. Measured from the mean rather than from
  // windowValue() the gate is still >=4 sigma for spreads up to 1.25 dB and
  // >=3 sigma up to 1.45 dB, against the 0.6-0.9 dB these receivers actually
  // produce, so censoring bias on genuine noise stays negligible across the
  // whole plausible range. Above ~2 dB the noise tail starts to overflow the
  // gate and sigma reads low (simulated: 1.98 dB at a true 2.5, 2.12 at a true
  // 3.0). That is the deliberate trade, and it is the safe direction: a low
  // sigma reads the floor low, which fires the busy check early -- absorbed by
  // CSMA backoff -- rather than losing a detection.
  #define NOISE_TRACKER_SCALE_GATE_DB  8.5f
#endif

#ifndef NOISE_TRACKER_MIN_DBM
  // Same clamp the original estimator applied: below this is not physically
  // plausible for these receivers.
  #define NOISE_TRACKER_MIN_DBM  (-120)
#endif

#ifndef NOISE_TRACKER_MIN_VALID_DBM
  // Readings below this are discarded as bad reads rather than clamped. The
  // output clamp alone is not enough protection for a minimum-based estimator:
  // one spurious -200 dBm read would pin the window minimum for a full 180 s,
  // and clamping the *output* to MIN_DBM would hide that as a plausible-looking
  // stuck floor.
  #define NOISE_TRACKER_MIN_VALID_DBM  (-135.0f)
#endif

// Bias correction, indexed by how many sub-windows have completed (1..N).
//
// The k-th smallest of a window sits below the distribution mean, so it must be
// corrected back up: mean ~= window_value + k * sigma. The coefficient depends
// only on how many samples were minimised over -- it is independent of sigma,
// as it must be, since the bias is proportional to it.
//
// Determined by simulating this exact algorithm rather than from the asymptotic
// extreme-value formula, which is not accurate at these window sizes. Indexing
// by occupancy rather than using one constant matters during warm-up: a partly
// filled ring has been minimised over fewer samples, so its value is closer to
// the mean, and the full-window coefficient would over-correct there.
//
// What that costs is sigma(), not the reported floor. The coefficient cancels
// out of the floor: updateScale() divides the measured mean excess by it and
// floorEstimate() multiplies it straight back, so as long as neither sigma
// clamp binds the floor is windowValue() + _dev whatever the table says.
// Getting it wrong therefore misreports the *spread*: the full-window value
// used throughout reads 16% low at the first sub-window and 12% at the second
// (measured, at sigma = 2) while leaving the floor within 0.03 dB of correct at
// both. That still matters, because sigma sets the CSMA margin under a
// too-narrow interference_threshold (NOISE_THRESHOLD_SIGMA_K * sigma, in
// RadioLibWrapper::isChannelActive) and decides where MIN_SIGMA/MAX_SIGMA start
// binding -- and those clamps are the only route by which a wrong coefficient
// can move the reported floor at all.
#ifndef NOISE_TRACKER_BIAS_TABLE
  #define NOISE_TRACKER_BIAS_TABLE { \
    2.375f, 2.502f, 2.579f, 2.632f, 2.674f, 2.708f, \
    2.737f, 2.761f, 2.783f, 2.802f, 2.819f, 2.835f }
#endif

/**
 * \brief  Robust noise-floor estimator for a stream of RSSI readings.
 *
 * Estimates the noise floor from a low order statistic of the RSSI readings
 * over a sliding window, bias-corrected back to the distribution mean (Martin's
 * minimum statistics, IEEE Trans. Speech & Audio Processing, 2001).
 *
 * The method rests on one asymmetry that holds for any radio: signal only ever
 * ADDS power. The lowest readings in a window are therefore noise-only samples
 * by construction -- no activity gate, no outlier rejection, and no assumption
 * that interference is a minority of the samples.
 *
 * The statistic is the SECOND smallest reading of each sub-window, not the
 * smallest. That single step is what makes the method safe here: the minimum is
 * by construction the most outlier-sensitive statistic there is, so one spurious
 * low reading would pin the floor for a whole window. The second smallest is
 * indifferent to any isolated low sample while being no more reachable by signal
 * than the first. It also lets the estimator work on raw readings -- an earlier
 * revision pre-smoothed with an EMA to blunt low outliers, which cost 4.7 dB of
 * upward bias under 20% dispersed interference because the smoothed sequence
 * never settled to the true floor between bursts.
 *
 * That last point is why this replaced a quantile tracker. Quantile estimators
 * are robust to *contamination* (a minority of samples drawn from another
 * distribution) but have essentially zero breakdown point against *runs*: a
 * sustained interferer simply drags the estimate along at its maximum slew
 * rate. On a live repeater this produced hour-long stretches reporting a value
 * that was neither the noise floor nor the interference level, only how far the
 * tracker had crawled. Minimum statistics does not chase -- it selects -- so
 * its breakdown point against a run is (U-1)/U of the window length.
 *
 * The estimate falls instantly when the floor genuinely falls, and rises only
 * as contaminated sub-windows age out. That asymmetry is deliberate and is the
 * correct one for a noise floor: real increases are rare and slow, spurious
 * ones are common and fast. Reading low is also the safe direction -- it makes
 * a busy-channel check fire slightly early (absorbed by CSMA backoff) rather
 * than late (a lost detection, the expensive error).
 *
 * Deliberately free of any clock, radio, or Arduino dependency: the caller
 * decides when to sample, which keeps the estimator unit-testable on the host
 * and its behaviour independent of how often the main loop happens to run.
 */
class NoiseFloorTracker {
  float    _lo0, _lo1;                           // two smallest of current sub-window
  uint16_t _sub_count;                           // samples into current sub-window
  uint16_t _sub_target;                          // samples needed to close it
  float    _mins[NOISE_TRACKER_SUB_WINDOWS];     // ring of completed sub-window statistics
  uint8_t  _head;                                // next ring slot to write
  uint8_t  _valid;                               // completed sub-windows, saturating at ring size
  float    _dev;                                 // mean excess of noise samples over windowValue()
  float    _sigma;

  // Sentinel for "no sample yet". Any real dBm reading is far below it, so
  // comparisons need no special case.
  static float sentinel() { return 1.0e30f; }

  /** Bias coefficient for the current ring occupancy (_valid >= 1). */
  float biasK() const {
    // Unsized on purpose, so sizeof measures the table rather than the constant
    // it is supposed to match. Both tunables are documented as independently
    // overridable via build_flags, and a table shorter than the ring is the
    // dangerous mismatch: an explicit bound would zero-fill the tail in silence,
    // so biasK() would return 0 at exactly the occupancies that matter most and
    // the floor would read ~2.8 dB low -- the direction that loses detections.
    static const float k[] = NOISE_TRACKER_BIAS_TABLE;
    static_assert(sizeof(k) / sizeof(k[0]) == NOISE_TRACKER_SUB_WINDOWS,
                  "NOISE_TRACKER_BIAS_TABLE must have exactly "
                  "NOISE_TRACKER_SUB_WINDOWS entries: override both or neither");
    uint8_t i = _valid > 0 ? (uint8_t)(_valid - 1) : 0;
    if (i >= NOISE_TRACKER_SUB_WINDOWS) i = NOISE_TRACKER_SUB_WINDOWS - 1;
    return k[i];
  }

  /** Lowest sub-window statistic across the window, including the in-progress
      sub-window so a genuine drop in the floor starts showing immediately
      rather than waiting for that sub-window to close. */
  float windowValue() const {
    float m = sentinel();
    for (uint8_t i = 0; i < _valid; i++) {
      if (_mins[i] < m) m = _mins[i];
    }
    if (_lo1 < m) m = _lo1;   // sentinel until the in-progress window has 2 samples
    return m;
  }

  /** Bias-corrected floor as a float. Undefined before the first sub-window. */
  float floorEstimate() const {
    return windowValue() + biasK() * _sigma;
  }

public:
  NoiseFloorTracker() { reset(); }

  /** Discard all state; the estimator re-warms from the next sample. */
  void reset() {
    _lo0 = _lo1 = sentinel();
    _sub_count = 0;
    _sub_target = NOISE_TRACKER_FIRST_SUB_SAMPLES;
    _head = 0;
    _valid = 0;
    _sigma = NOISE_TRACKER_MIN_SIGMA_DB;
    _dev = NOISE_TRACKER_MIN_SIGMA_DB * biasK();   // consistent with _sigma; needs _valid set
    for (uint8_t i = 0; i < NOISE_TRACKER_SUB_WINDOWS; i++) _mins[i] = sentinel();
  }

  /** True once at least one sub-window has closed and floorDbm() is meaningful. */
  bool ready() const { return _valid > 0; }

  /**
   * Feed one RSSI reading, in dBm. Call at a steady rate -- the window length
   * is expressed in samples, so a varying rate varies the time it spans.
   *
   * Safe to call during packet reception. Samples that contain signal only
   * raise the sequence, and a low order statistic ignores them; that is what
   * removed the activity gate this estimator used to need, and with it the bias
   * toward quiet moments the gate imposed.
   */
  void addSample(float rssi_dbm) {
    if (rssi_dbm < NOISE_TRACKER_MIN_VALID_DBM) return;   // bad read, not a quiet channel

    // Keep the two smallest readings of this sub-window, _lo0 <= _lo1.
    if (rssi_dbm < _lo0)      { _lo1 = _lo0; _lo0 = rssi_dbm; }
    else if (rssi_dbm < _lo1) { _lo1 = rssi_dbm; }

    if (++_sub_count >= _sub_target) {
      _mins[_head] = _lo1;
      _head = (uint8_t)((_head + 1) % NOISE_TRACKER_SUB_WINDOWS);
      if (_valid < NOISE_TRACKER_SUB_WINDOWS) _valid++;
      _lo0 = _lo1 = sentinel();
      _sub_count = 0;
      _sub_target = NOISE_TRACKER_SUB_SAMPLES;   // only the first one is short
    }

    updateScale(rssi_dbm);
  }

  /**
   * \returns  estimated standard deviation (dB) of the noise-only RSSI, always
   *           within [NOISE_TRACKER_MIN_SIGMA_DB, NOISE_TRACKER_MAX_SIGMA_DB].
   *
   * Measured as the mean amount by which noise samples exceed the window
   * statistic. That gap is exactly what the bias table predicts -- biasK() *
   * sigma -- so dividing by biasK() inverts it. Samples well above the window
   * statistic are censored out, so channel occupancy cannot widen it.
   *
   * Anchoring to windowValue() rather than to the floor estimate is deliberate,
   * and it applies to the censoring gate as much as to the deviation itself.
   * The floor is derived from sigma, so anything measured about it closes a
   * positive feedback loop: a floor reading Delta too high makes the mean
   * deviation ~Delta, which inflates sigma, which raises the floor further; a
   * gate placed relative to it widens as sigma grows and admits the very signal
   * that grew it (see NOISE_TRACKER_SCALE_GATE_DB). windowValue() is
   * sigma-independent, so neither loop exists, and sigma is bounded outright at
   * NOISE_TRACKER_SCALE_GATE_DB / biasK rather than only by MAX_SIGMA.
   */
  float sigma() const { return _sigma; }

  /**
   * \returns  estimated *mean* noise floor in whole dBm, clamped at
   *           NOISE_TRACKER_MIN_DBM, or 0 before the first sub-window closes.
   *
   * The mean rather than the raw window statistic, so the reported value stays
   * comparable with the estimators this replaced (and therefore with other
   * MeshCore nodes and historical telemetry).
   */
  int16_t floorDbm() const {
    if (_valid == 0) return 0;   // "not calibrated yet", as previous estimators reported
    float f = floorEstimate();
    // Round half away from zero without pulling in libm.
    int v = (int) (f < 0.0f ? f - 0.5f : f + 0.5f);
    return v < NOISE_TRACKER_MIN_DBM ? (int16_t) NOISE_TRACKER_MIN_DBM : (int16_t) v;
  }

private:
  /** Update the scale estimate from samples that are plausibly noise-only. */
  void updateScale(float rssi_dbm) {
    if (_valid == 0) return;    // no window statistic to measure against yet

    // One ring scan serves both uses below: the censoring gate and the
    // deviation are both anchored on the window statistic, which is the only
    // quantity here that neither signal nor the scale estimate can move.
    // Reading it once also makes the two impossible to drift apart.
    const float wv = windowValue();

    if (rssi_dbm >= wv + NOISE_TRACKER_SCALE_GATE_DB) return;   // signal or interferer, not noise

    float d = rssi_dbm - wv;
    if (d < 0.0f) d = 0.0f;     // below the window statistic only by sampling noise
    _dev += NOISE_TRACKER_SIGMA_LAMBDA * (d - _dev);

    float s = _dev / biasK();
    if (s > NOISE_TRACKER_MAX_SIGMA_DB) s = NOISE_TRACKER_MAX_SIGMA_DB;
    if (s < NOISE_TRACKER_MIN_SIGMA_DB) s = NOISE_TRACKER_MIN_SIGMA_DB;
    _sigma = s;
  }
};
