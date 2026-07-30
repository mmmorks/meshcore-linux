#pragma once

#include <stdint.h>

// Tunables are macros rather than in-class `static constexpr float` so that
// ODR-using one (binding a reference, passing it to a function) cannot produce
// a link error on the C++11/14 toolchains some MeshCore targets build with.
// Each is overridable per-platform via build_flags.

#ifndef NOISE_TRACKER_STEP_DB
  // dB the estimate moves per sample. Larger tracks faster and jitters more.
  // At 0.2, a freshly seeded estimate descends toward the true 0.10 quantile at
  // ~0.08 dB/sample, so it is usable within ~3 s at a 100 ms sample interval --
  // comparable to the ~2.5 s the 64-sample batch estimator took.
  #define NOISE_TRACKER_STEP_DB  0.2f
#endif

#ifndef NOISE_TRACKER_MIN_SIGMA_DB
  // Scale floor. Stops a freshly seeded tracker (where both quantiles start
  // equal) from reporting a zero-width noise distribution, which would collapse
  // any threshold derived from sigma().
  #define NOISE_TRACKER_MIN_SIGMA_DB  0.5f
#endif

#ifndef NOISE_TRACKER_MIN_DBM
  // Same clamp the previous estimator applied: below this is not physically
  // plausible for these receivers and indicates a bad RSSI read.
  #define NOISE_TRACKER_MIN_DBM  (-120)
#endif

/**
 * \brief  Robust noise-floor estimator for a stream of RSSI readings.
 *
 * Tracks two low quantiles of the observed RSSI distribution by stochastic
 * approximation (Robbins-Monro on the pinball loss), yielding both a location
 * and a scale estimate for the noise-only part of the signal.
 *
 * Why quantiles rather than an average: every sample that contains signal is an
 * outlier, and outliers bias an average upward -- which raises any
 * busy-channel threshold derived from it and loses detections, the expensive
 * error. An average has a breakdown point of 1/N, so a single strong burst
 * corrupts the estimate. Here each update moves the estimate by at most
 * NOISE_TRACKER_STEP_DB no matter how distant the sample, so bursts cannot drag
 * it. That bounded step IS the robustness argument -- do not "improve" it into
 * a step proportional to the residual.
 *
 * Both tracked quantiles (0.10 and 0.40) sit below any plausible channel
 * occupancy, so neither is reachable by signal energy. That is what removes the
 * need for a transmit/receive activity gate to keep the samples clean, and with
 * it the selection bias such a gate imposes.
 *
 * Deliberately free of any clock, radio, or Arduino dependency: the caller
 * decides when to sample, which keeps the estimator unit-testable on the host
 * and keeps its behaviour independent of how often the main loop happens to
 * run.
 */
class NoiseFloorTracker {
  float _q10;
  float _q40;
  bool  _init;

public:
  NoiseFloorTracker() : _q10(0.0f), _q40(0.0f), _init(false) { }

  /** Discard all state; the next sample re-seeds. */
  void reset() { _init = false; }

  /** True once at least one sample has been taken since construction/reset. */
  bool ready() const { return _init; }

  /**
   * Feed one RSSI reading, in dBm. Call at a steady rate -- the estimator's
   * time constants are expressed in samples, so a varying rate varies them.
   */
  void addSample(float rssi_dbm) {
    if (!_init) {
      // Seed both quantiles at the first reading. sigma() holds at its floor
      // until they separate, so the reported floor is up to ~0.6 dB high for
      // the first few samples.
      _q10 = _q40 = rssi_dbm;
      _init = true;
      return;
    }

    // Robbins-Monro quantile update: theta += step * (p - 1{x <= theta}).
    // Equilibrium is where P(x <= theta) == p.
    _q10 += (rssi_dbm > _q10) ?  NOISE_TRACKER_STEP_DB * 0.10f
                              : -NOISE_TRACKER_STEP_DB * 0.90f;
    _q40 += (rssi_dbm > _q40) ?  NOISE_TRACKER_STEP_DB * 0.40f
                              : -NOISE_TRACKER_STEP_DB * 0.60f;

    // The two trackers are independent, so a transient can momentarily invert
    // them. sigma() would go negative; keep them ordered instead.
    if (_q40 < _q10) _q40 = _q10;
  }

  /**
   * \returns  estimated standard deviation (dB) of the noise-only RSSI, never
   *           below NOISE_TRACKER_MIN_SIGMA_DB.
   *
   * Derived from the spacing of two *lower* quantiles, so that a busy channel
   * cannot inflate it: for a normal distribution,
   * Phi^-1(0.40) - Phi^-1(0.10) = -0.2533 - (-1.2816) = 1.0283 sigma.
   */
  float sigma() const {
    float s = (_q40 - _q10) / 1.0283f;
    return s < NOISE_TRACKER_MIN_SIGMA_DB ? (float) NOISE_TRACKER_MIN_SIGMA_DB : s;
  }

  /**
   * \returns  estimated *mean* noise floor in whole dBm, clamped at
   *           NOISE_TRACKER_MIN_DBM, or 0 before the first sample.
   *
   * The mean rather than the raw 0.10 quantile, so the reported value stays
   * comparable with the trimmed-mean estimator this replaced (and therefore
   * with other MeshCore nodes and historical telemetry). Since
   * q10 == mean - 1.2816 sigma, the mean is recovered by adding it back.
   */
  int16_t floorDbm() const {
    if (!_init) return 0;   // "not calibrated yet", as the previous estimator reported
    float mean = _q10 + 1.2816f * sigma();
    // Round half away from zero without pulling in libm.
    int v = (int) (mean < 0.0f ? mean - 0.5f : mean + 0.5f);
    return v < NOISE_TRACKER_MIN_DBM ? (int16_t) NOISE_TRACKER_MIN_DBM : (int16_t) v;
  }
};
