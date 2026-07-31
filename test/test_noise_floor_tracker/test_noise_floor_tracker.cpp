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

// Samples needed to fill the ring completely: the short first sub-window plus a
// full-length one for every remaining slot.
const int FULL_WINDOW = NOISE_TRACKER_FIRST_SUB_SAMPLES
                      + (NOISE_TRACKER_SUB_WINDOWS - 1) * NOISE_TRACKER_SUB_SAMPLES;

void feedNoise(NoiseFloorTracker& nf, Rng& rng, int n,
               float mean = NOISE_MEAN, float sd = NOISE_SIGMA) {
  for (int i = 0; i < n; i++) nf.addSample(mean + sd * rng.normal());
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
  const float sigmas[] = { 0.5f, 1.0f, 2.0f, 3.0f };
  for (int i = 0; i < 4; i++) {
    NoiseFloorTracker nf;
    Rng rng(900 + i);
    feedNoise(nf, rng, 6000, NOISE_MEAN, sigmas[i]);
    EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.0f + sigmas[i] * 0.5f)
        << "bias correction wrong at sigma = " << sigmas[i];
  }
}

TEST(NoiseFloorTracker, EstimatesScale) {
  NoiseFloorTracker nf;
  Rng rng(999);
  feedNoise(nf, rng, 5000);
  EXPECT_NEAR(NOISE_SIGMA, nf.sigma(), 0.8f);
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
