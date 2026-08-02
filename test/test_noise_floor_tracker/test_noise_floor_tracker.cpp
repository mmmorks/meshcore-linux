#include <gtest/gtest.h>

#include <math.h>
#include <stdint.h>

#include "helpers/NoiseFloorTracker.h"

namespace {

// Deterministic Gaussian source. A fixed LCG plus Box-Muller, so every run of
// these tests sees the identical sample stream -- a statistical estimator tested
// against a seeded-at-random stream produces flaky assertions.
class Rng {
  uint32_t _s;
public:
  explicit Rng(uint32_t seed) : _s(seed) { }

  float uniform() {   // in (0,1), never exactly 0 (logf would blow up)
    _s = _s * 1664525u + 1013904223u;
    return (float)((_s >> 8) + 1u) / (float)((1u << 24) + 2u);
  }

  float normal() {
    float u1 = uniform();
    float u2 = uniform();
    return sqrtf(-2.0f * logf(u1)) * cosf(6.28318531f * u2);
  }
};

const float NOISE_MEAN  = -110.0f;
const float NOISE_SIGMA = 2.0f;

// The spread a receiver actually shows: 0.6-0.9 dB measured on a live SX1262 at
// 250 kHz. NOISE_SIGMA above is deliberately wider so the bias correction gets
// exercised, but the traffic tests want the real number -- the distance from a
// true 0.8 dB up to the 3 dB MAX_SIGMA cap is the whole span the censoring gate
// used to be able to travel, and starting at 2.0 hides most of it.
const float MEASURED_SIGMA = 0.8f;

// Samples needed to fill the ring completely: the short first sub-window plus a
// full-length one for every remaining slot.
const int FULL_WINDOW = NOISE_TRACKER_FIRST_SUB_SAMPLES
                      + (NOISE_TRACKER_SUB_WINDOWS - 1) * NOISE_TRACKER_SUB_SAMPLES;

void feedNoise(NoiseFloorTracker& nf, Rng& rng, int n,
               float mean = NOISE_MEAN, float sd = NOISE_SIGMA) {
  for (int i = 0; i < n; i++) nf.addSample(mean + sd * rng.normal());
}

// A duty-cycled interferer: `cycles` repetitions of `on` samples at
// mean + delta (or, when delta_hi > delta, at a level drawn per burst from
// [delta, delta_hi]) followed by `off` samples of plain noise. Noise of the
// same spread rides on both, since a packet does not replace the noise.
//
// Bursty rather than i.i.d. on purpose: it is the quiet gaps that keep
// windowValue() on the true floor, so this is the shape of traffic that can
// inflate the scale estimate while leaving the floor itself correct.
// Returns the widest sigma seen at any point, not the final one: the failure
// this guards against is transient by nature -- the estimate climbs while the
// traffic runs and relaxes once it stops -- so sampling only at the end would
// miss exactly the excursion that matters.
float feedBurstyTraffic(NoiseFloorTracker& nf, Rng& rng, int cycles, int on, int off,
                        float delta, float delta_hi = -1.0f, float sd = NOISE_SIGMA) {
  float worst = nf.sigma();
  for (int c = 0; c < cycles; c++) {
    float lvl = delta_hi > delta ? delta + (delta_hi - delta) * rng.uniform() : delta;
    for (int i = 0; i < on; i++) {
      nf.addSample(NOISE_MEAN + lvl + sd * rng.normal());
      if (nf.sigma() > worst) worst = nf.sigma();
    }
    for (int i = 0; i < off; i++) {
      nf.addSample(NOISE_MEAN + sd * rng.normal());
      if (nf.sigma() > worst) worst = nf.sigma();
    }
  }
  return worst;
}

// ---------------------------------------------------------------------------
// Basic contract
// ---------------------------------------------------------------------------

TEST(NoiseFloorTracker, NotReadyUntilFirstSubWindowCloses) {
  NoiseFloorTracker nf;
  Rng rng(12345);
  EXPECT_FALSE(nf.ready());
  EXPECT_EQ(0, nf.floorDbm());   // "not calibrated yet"

  feedNoise(nf, rng, NOISE_TRACKER_FIRST_SUB_SAMPLES - 1);
  EXPECT_FALSE(nf.ready()) << "must not report a floor from a partial sub-window";
  EXPECT_EQ(0, nf.floorDbm());

  nf.addSample(NOISE_MEAN);
  EXPECT_TRUE(nf.ready());
  EXPECT_NE(0, nf.floorDbm());
}

TEST(NoiseFloorTracker, ConvergesToNoiseMean) {
  NoiseFloorTracker nf;
  Rng rng(12345);
  feedNoise(nf, rng, 5000);

  // floorDbm() reports the estimated *mean* of the noise distribution, so the
  // window minimum must be bias-corrected back up by BIAS_K * sigma.
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);
}

TEST(NoiseFloorTracker, BiasCorrectionHoldsAcrossScales) {
  // The bias is proportional to sigma, so one coefficient must work for every
  // plausible noise spread. If BIAS_K were tuned to a single sigma this fails.
  //
  // "Plausible" is bounded above by the censoring gate: the gate is a fixed
  // width above the window statistic, so once the noise tail itself reaches
  // past it -- somewhere above 2 dB, against the 0.6-0.9 dB these receivers
  // measure -- part of the distribution is censored and the estimate reads low.
  // That is the documented trade for a gate signal cannot widen
  // (NOISE_TRACKER_SCALE_GATE_DB), and the next test pins the direction.
  const float sigmas[] = { 0.5f, 1.0f, 2.0f };
  for (int i = 0; i < 3; i++) {
    NoiseFloorTracker nf;
    Rng rng(900 + i);
    feedNoise(nf, rng, 6000, NOISE_MEAN, sigmas[i]);
    EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f + sigmas[i] * 0.5f)
        << "bias correction wrong at sigma = " << sigmas[i];
  }
}

// Beyond the gate's range the estimate must degrade in the safe direction. A
// floor that reads low fires the busy-channel check early and CSMA backoff
// absorbs it; a floor that reads high loses the detection outright, and that
// packet is not coming back.
TEST(NoiseFloorTracker, ImplausiblyWideNoiseReadsLowNotHigh) {
  const float sigmas[] = { 2.5f, 3.0f };
  for (int i = 0; i < 2; i++) {
    NoiseFloorTracker nf;
    Rng rng(910 + i);
    feedNoise(nf, rng, 6000, NOISE_MEAN, sigmas[i]);
    EXPECT_LE((float)nf.floorDbm(), NOISE_MEAN + 1.0f)
        << "floor read high at sigma = " << sigmas[i];
    EXPECT_GE((float)nf.floorDbm(), NOISE_MEAN - 2.0f * sigmas[i])
        << "floor read uselessly low at sigma = " << sigmas[i];
  }
}

TEST(NoiseFloorTracker, EstimatesScale) {
  NoiseFloorTracker nf;
  Rng rng(999);
  feedNoise(nf, rng, 5000);
  EXPECT_NEAR(NOISE_SIGMA, nf.sigma(), 0.8f);
}

// Warm-up has a direction, and only one of the two is affordable. A floor that
// reads high raises the busy-channel comparison with it, and the detection lost
// that way is lost for good; a floor that reads low only fires the check early,
// which CSMA backoff absorbs. The estimator is asymmetric by design for exactly
// this reason, and the asymmetry has to hold at every ring occupancy, not just
// at the full window -- a node that has just changed bandwidth walks through
// all twelve of them.
TEST(NoiseFloorTracker, WarmUpNeverReadsHighAtAnyRingOccupancy) {
  for (uint32_t seed = 0; seed < 8; seed++) {
    NoiseFloorTracker nf;
    Rng rng(31000 + seed * 977);
    for (int w = 1; w <= NOISE_TRACKER_SUB_WINDOWS; w++) {
      feedNoise(nf, rng, w == 1 ? NOISE_TRACKER_FIRST_SUB_SAMPLES
                                : NOISE_TRACKER_SUB_SAMPLES);
      ASSERT_TRUE(nf.ready());
      EXPECT_LE((float)nf.floorDbm(), NOISE_MEAN + 1.0f)
          << "floor read high with " << w << " sub-window(s) in the ring (seed "
          << seed << ")";
      // The other side of the same claim: low is safe, but not arbitrarily so.
      EXPECT_GE((float)nf.floorDbm(), NOISE_MEAN - 5.0f)
          << "floor read uselessly low with " << w << " sub-window(s) in the ring";
    }
  }
}

TEST(NoiseFloorTracker, ConvergesWithinAFewSecondsOfSamples) {
  NoiseFloorTracker nf;
  Rng rng(4242);
  // The short first sub-window exists so a fresh node is usable quickly:
  // 30 samples == 3 s at a 100 ms sampling interval.
  feedNoise(nf, rng, NOISE_TRACKER_FIRST_SUB_SAMPLES);
  ASSERT_TRUE(nf.ready());
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 3.0f);
}

TEST(NoiseFloorTracker, ClampsAtMinDbm) {
  NoiseFloorTracker nf;
  for (int i = 0; i < 500; i++) nf.addSample(-125.0f);
  EXPECT_EQ((int16_t)NOISE_TRACKER_MIN_DBM, nf.floorDbm());
}

TEST(NoiseFloorTracker, ResetDiscardsEverything) {
  NoiseFloorTracker nf;
  Rng rng(60606);
  feedNoise(nf, rng, 5000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);

  nf.reset();
  EXPECT_FALSE(nf.ready());
  EXPECT_EQ(0, nf.floorDbm());

  // Re-warms at the new level rather than crawling there from the old one.
  feedNoise(nf, rng, NOISE_TRACKER_FIRST_SUB_SAMPLES, -80.0f, NOISE_SIGMA);
  EXPECT_NEAR(-80.0f, (float)nf.floorDbm(), 3.0f);
}

// ---------------------------------------------------------------------------
// Robustness to signal. These are the tests the previous suite was too weak to
// fail, and each one corresponds to something observed on live hardware.
// ---------------------------------------------------------------------------

TEST(NoiseFloorTracker, SingleStrongBurstDoesNotMoveEstimateAtAll) {
  NoiseFloorTracker nf;
  Rng rng(777);
  feedNoise(nf, rng, 5000);
  int16_t before = nf.floorDbm();

  nf.addSample(-50.0f);   // 60 dB above the floor

  // A minimum is not merely *resistant* to a high outlier, it is indifferent to
  // it. The previous quantile estimator allowed 1 dB of movement here.
  EXPECT_EQ(before, nf.floorDbm());
}

TEST(NoiseFloorTracker, SurvivesHeavyDispersedInterference) {
  NoiseFloorTracker nf;
  Rng rng(2026);
  feedNoise(nf, rng, 5000);

  // 20% of samples are a loud interferer. The arithmetic mean of this mixture
  // is 0.8*(-110) + 0.2*(-60) = -100 dBm, a 10 dB error.
  for (int i = 0; i < 6000; i++) {
    if (i % 5 == 0) nf.addSample(-60.0f);
    else            nf.addSample(NOISE_MEAN + NOISE_SIGMA * rng.normal());
  }

  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.5f);
}

// Regression: dispersed contamination and *runs* of contamination are different
// problems, and only the second one broke on hardware. A quantile tracker has
// essentially zero breakdown point against a run -- it just gets dragged along
// at its maximum slew rate. A minimum ignores the run entirely, as long as the
// window still holds one clean sub-window.
TEST(NoiseFloorTracker, SurvivesRunsOfInterference) {
  NoiseFloorTracker nf;
  Rng rng(8080);
  feedNoise(nf, rng, 5000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);

  int16_t worst = nf.floorDbm();
  for (int burst = 0; burst < 30; burst++) {
    for (int i = 0; i < 25; i++) {           // ~2.5 s of packet at 100 ms/sample
      nf.addSample(-70.0f);
      if (nf.floorDbm() > worst) worst = nf.floorDbm();
    }
    for (int i = 0; i < 25; i++) {           // quiet gap between packets
      nf.addSample(NOISE_MEAN + NOISE_SIGMA * rng.normal());
      if (nf.floorDbm() > worst) worst = nf.floorDbm();
    }
  }

  EXPECT_LT((float)worst, NOISE_MEAN + 1.5f)
      << "floor moved to " << worst << " dBm during runs of -70 dBm samples";
}

// The specific failure measured on a live repeater: excursions with a median
// duration of 30 s and a maximum of 67 s, during which the previous estimator
// crawled upward at exactly its own slew limit and reported a meaningless
// intermediate value. The window is sized so that an excursion of this length
// cannot contaminate every sub-window.
TEST(NoiseFloorTracker, SurvivesSustainedInterferenceShorterThanTheWindow) {
  NoiseFloorTracker nf;
  Rng rng(31337);
  feedNoise(nf, rng, 5000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);

  int16_t worst = nf.floorDbm();
  for (int i = 0; i < 670; i++) {            // 67 s at 100 ms/sample
    nf.addSample(-70.0f);
    if (nf.floorDbm() > worst) worst = nf.floorDbm();
  }

  EXPECT_LT((float)worst, NOISE_MEAN + 1.5f)
      << "a 67 s interferer moved the floor to " << worst << " dBm";

  // ...and it is still correct once the interferer stops.
  feedNoise(nf, rng, 200);
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);
}

// The other side of the same boundary: interference that outlasts the entire
// window IS the floor, and must be reported as such. Robustness must not mean
// blindness -- a node parked next to a permanent emitter needs to know.
TEST(NoiseFloorTracker, TracksInterferenceThatOutlastsTheWindow) {
  NoiseFloorTracker nf;
  Rng rng(5150);
  feedNoise(nf, rng, 5000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);

  // Every sub-window in the ring must be replaced before the estimate can rise,
  // and the scale estimate then has to re-converge on the new population, so
  // this deliberately takes longer than one window.
  for (int i = 0; i < FULL_WINDOW + 1500; i++) {
    nf.addSample(-70.0f + 0.5f * rng.normal());
  }
  EXPECT_NEAR(-70.0f, (float)nf.floorDbm(), 2.0f);
}

TEST(NoiseFloorTracker, TracksARealFloorChange) {
  NoiseFloorTracker nf;
  Rng rng(5150);
  feedNoise(nf, rng, 5000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);

  // Ambient genuinely rises by 15 dB and stays there.
  feedNoise(nf, rng, FULL_WINDOW + 500, -95.0f, NOISE_SIGMA);
  EXPECT_NEAR(-95.0f, (float)nf.floorDbm(), 1.5f);
}

// The estimator's asymmetry is deliberate: it must rise slowly (a high reading
// might be signal) but fall fast (a low reading can only be noise). Rising takes
// a full window as contaminated sub-windows age out; falling takes one
// sub-window, a 12x difference.
TEST(NoiseFloorTracker, FallsFastWhenTheFloorGenuinelyDrops) {
  NoiseFloorTracker nf;
  Rng rng(2468);
  feedNoise(nf, rng, 5000, -95.0f, NOISE_SIGMA);
  ASSERT_NEAR(-95.0f, (float)nf.floorDbm(), 1.5f);

  // 30 samples == 3 s. Most of the 15 dB drop must already be reflected, which
  // is why the in-progress sub-window counts toward the window statistic rather
  // than only being read once it closes. It is not yet exact: the estimate is
  // drawn from few samples of the new level, so the bias correction -- sized for
  // a full window -- still over-corrects slightly.
  feedNoise(nf, rng, 30, NOISE_MEAN, NOISE_SIGMA);
  EXPECT_LT((float)nf.floorDbm(), -105.0f);

  // One full sub-window in, it is converged.
  feedNoise(nf, rng, NOISE_TRACKER_SUB_SAMPLES, NOISE_MEAN, NOISE_SIGMA);
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 2.0f);
}

// ---------------------------------------------------------------------------
// Weaknesses specific to a minimum-based estimator. These bound them rather
// than pretend they are absent.
// ---------------------------------------------------------------------------

TEST(NoiseFloorTracker, ImplausiblyLowReadingIsDiscarded) {
  NoiseFloorTracker nf;
  Rng rng(1357);
  feedNoise(nf, rng, 5000);
  int16_t before = nf.floorDbm();

  nf.addSample(-200.0f);   // a bad SPI read, not a quiet channel

  // Without the input guard this would pin the window minimum for a full 180 s,
  // and clamping the output at MIN_DBM would disguise it as a plausible floor.
  EXPECT_EQ(before, nf.floorDbm());
}

TEST(NoiseFloorTracker, WorstCaseValidLowReadingHasNoEffect) {
  NoiseFloorTracker nf;
  Rng rng(2469);
  feedNoise(nf, rng, 5000);
  int16_t before = nf.floorDbm();

  // The lowest reading the input guard still accepts, 25 dB below the floor.
  // Tracking the *second* smallest of each sub-window rather than the smallest
  // makes an isolated low sample irrelevant: it becomes the first smallest and
  // is never the value stored. This is the whole reason for the order
  // statistic -- a plain minimum would have taken the full 25 dB and held it
  // for a 180 s window.
  nf.addSample(NOISE_TRACKER_MIN_VALID_DBM);
  EXPECT_EQ(before, nf.floorDbm());

  // Two in the same sub-window can move it, but it still ages out.
  nf.addSample(NOISE_TRACKER_MIN_VALID_DBM);
  feedNoise(nf, rng, FULL_WINDOW + 400);
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f);
}

// ---------------------------------------------------------------------------
// Scale estimate
// ---------------------------------------------------------------------------

TEST(NoiseFloorTracker, SigmaHoldsAtFloorBeforeAnyData) {
  NoiseFloorTracker nf;
  EXPECT_FLOAT_EQ(NOISE_TRACKER_MIN_SIGMA_DB, nf.sigma());
}

// Regression: sigma used to be taken from the gap between two quantile
// trackers, which under a sustained run measured how far the two had diverged
// rather than the noise spread. Unbounded it reached 31.6 dB in test and drove
// a live repeater's reported floor from -114 dBm to -70. It is now measured
// only from samples near the floor, so signal cannot enter it at all.
TEST(NoiseFloorTracker, SigmaIsUnaffectedByASustainedInterferer) {
  NoiseFloorTracker nf;
  Rng rng(1234);
  feedNoise(nf, rng, 5000);
  float quiet_sigma = nf.sigma();
  ASSERT_NEAR(NOISE_SIGMA, quiet_sigma, 0.8f);

  float worst = quiet_sigma;
  for (int i = 0; i < 1500; i++) {
    nf.addSample(-70.0f);
    if (nf.sigma() > worst) worst = nf.sigma();
  }
  EXPECT_NEAR(quiet_sigma, worst, 0.3f)
      << "interference widened the estimated noise spread to " << worst << " dB";
}

// Regression: the censoring gate used to be placed at
// windowValue() + biasK*sigma + max(6 dB, 4*sigma). Both of those terms grow
// with sigma, so admitting a weak packet widened the gate that had admitted it
// -- positive feedback, bounded only by MAX_SIGMA. Under sustained traffic a
// few dB above the floor it ratcheted all the way there, which silently takes
// the CSMA margin RadioLibWrapper derives from sigma (NOISE_THRESHOLD_SIGMA_K *
// sigma) from ~2.8 dB to 10.5 dB, overriding the operator's
// interference_threshold in exactly the busy mesh where they set it.
//
// The gate is a fixed width above the window statistic now, so an admitted
// sample cannot widen the set of samples admitted next.
TEST(NoiseFloorTracker, SigmaIsBoundedUnderSustainedNearFloorTraffic) {
  NoiseFloorTracker nf;
  Rng rng(20260802);
  feedNoise(nf, rng, 5000, NOISE_MEAN, MEASURED_SIGMA);
  ASSERT_NEAR(MEASURED_SIGMA, nf.sigma(), 0.3f);

  // 80% occupancy, every packet 8 dB above the floor: 2 s of packet, 0.5 s of
  // gap, for ~85 minutes at a 100 ms sampling interval. This is the exact shape
  // that used to pin sigma at MAX_SIGMA.
  float worst = feedBurstyTraffic(nf, rng, 2000, 20, 5, 8.0f, -1.0f, MEASURED_SIGMA);
  EXPECT_LT(worst, 2.2f)
      << "sustained traffic 8 dB above the floor widened sigma to " << worst << " dB"
      << " (CSMA margin " << 3.5f * worst << " dB)";

  // ...and the floor itself is still the floor, which is what makes the sigma
  // reading above wrong rather than merely large. Not exact: at this occupancy
  // each sub-window holds far fewer clean samples than the bias table assumes,
  // so the window statistic sits a little high. A couple of dB of that is
  // inherent and is not what this test is about -- following the traffic to
  // -102 would be.
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 3.0f);
}

// The worst point on the curve, not the best one. The test above picks the
// packet level where the fix helps most; this one picks where it helps least.
//
// The gate sits about 6.2 dB above the mean at this spread, so traffic parked
// just under it is admitted wholesale -- no feedback needed, and none of the
// fix's benefit available. Sweeping level at 80% occupancy, worst sigma runs
// 1.45 / 1.74 / 2.04 / 2.27 / 2.35 / 2.24 / 1.51 at +2..+8 dB, against
// 1.45 / 1.74 / 2.04 / 2.33 / 2.63 / 2.92 / 3.00 for the pre-fix gate: nothing
// gained below +5, everything above +6. This pins the peak of that curve so the
// suite describes the whole bound and not just its good end.
//
// Read it as a ceiling on a known residual rather than as a regression guard.
// The pre-fix gate scores 2.63 here against this gate's 2.35, so it does trip
// the bound -- by 0.03 dB, which is seed noise, not a signal. The two tests
// either side of this one are the ones with real separation (3.00 and 2.83
// against 1.51 and 1.83); this one exists to record where the curve peaks.
TEST(NoiseFloorTracker, SigmaResidualAtTheWorstPacketLevelIsBounded) {
  NoiseFloorTracker nf;
  Rng rng(20260802);
  feedNoise(nf, rng, 5000, NOISE_MEAN, MEASURED_SIGMA);
  ASSERT_NEAR(MEASURED_SIGMA, nf.sigma(), 0.3f);

  // +6 dB at 80% occupancy: the peak.
  float worst = feedBurstyTraffic(nf, rng, 2000, 20, 5, 6.0f, -1.0f, MEASURED_SIGMA);
  EXPECT_LT(worst, 2.6f)
      << "residual grew past the level this gate is known to permit: sigma "
      << worst << " dB (CSMA margin " << 3.5f * worst << " dB)";

  // And the floor has still not followed the traffic, which is what keeps this
  // a scale-estimate problem rather than a floor problem.
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 5.0f);
}

// The same bound under the messier version: a spread of link budgets rather
// than one repeated level, which is what gave the old gate its foothold -- the
// weakest packets got in, widened it, and let the rest follow.
TEST(NoiseFloorTracker, SigmaIsBoundedUnderMixedStrengthTraffic) {
  NoiseFloorTracker nf;
  Rng rng(4711);
  feedNoise(nf, rng, 5000, NOISE_MEAN, MEASURED_SIGMA);
  ASSERT_NEAR(MEASURED_SIGMA, nf.sigma(), 0.3f);

  float worst = feedBurstyTraffic(nf, rng, 2000, 20, 20, 3.0f, 10.0f, MEASURED_SIGMA);
  EXPECT_LT(worst, 2.4f)
      << "traffic 3-10 dB above the floor widened sigma to " << worst << " dB";
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 3.0f);
}

TEST(NoiseFloorTracker, SigmaStaysWithinPhysicalBounds) {
  NoiseFloorTracker nf;
  Rng rng(4321);
  // Wildly over-dispersed input: sigma must still report something a receiver
  // could plausibly have, because the reported floor extrapolates from it.
  for (int i = 0; i < 5000; i++) {
    nf.addSample(NOISE_MEAN + 40.0f * rng.normal());
  }
  EXPECT_LE(nf.sigma(), (float)NOISE_TRACKER_MAX_SIGMA_DB + 0.01f);
  EXPECT_GE(nf.sigma(), (float)NOISE_TRACKER_MIN_SIGMA_DB - 0.01f);
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
