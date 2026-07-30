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

// Feed `n` noise-only samples.
void feedNoise(NoiseFloorTracker& nf, Rng& rng, int n,
               float mean = NOISE_MEAN, float sd = NOISE_SIGMA) {
  for (int i = 0; i < n; i++) nf.addSample(mean + sd * rng.normal());
}

TEST(NoiseFloorTracker, NotReadyUntilFirstSample) {
  NoiseFloorTracker nf;
  EXPECT_FALSE(nf.ready());
  EXPECT_EQ(0, nf.floorDbm());   // "not calibrated yet"

  nf.addSample(-110.0f);
  EXPECT_TRUE(nf.ready());
  EXPECT_NE(0, nf.floorDbm());
}

TEST(NoiseFloorTracker, SigmaHoldsAtFloorWhenSeeded) {
  NoiseFloorTracker nf;
  nf.addSample(-110.0f);
  // Both quantiles seeded equal -> zero spread -> sigma must not be 0 or
  // negative, or any threshold derived from it collapses.
  EXPECT_FLOAT_EQ(NOISE_TRACKER_MIN_SIGMA_DB, nf.sigma());
}

TEST(NoiseFloorTracker, ConvergesToNoiseMean) {
  NoiseFloorTracker nf;
  Rng rng(12345);
  feedNoise(nf, rng, 2000);

  // floorDbm() reports the estimated *mean* of the noise distribution.
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.5f);
}

TEST(NoiseFloorTracker, EstimatesScale) {
  NoiseFloorTracker nf;
  Rng rng(999);
  feedNoise(nf, rng, 2000);

  // Scale recovered from the 0.10/0.40 quantile spacing.
  EXPECT_NEAR(NOISE_SIGMA, nf.sigma(), 0.8f);
}

TEST(NoiseFloorTracker, ConvergesWithinAFewSecondsOfSamples) {
  NoiseFloorTracker nf;
  Rng rng(4242);
  // 30 samples == 3 s at a 100 ms sampling interval.
  feedNoise(nf, rng, 30);
  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 3.0f);
}

TEST(NoiseFloorTracker, SingleStrongBurstBarelyMovesEstimate) {
  NoiseFloorTracker nf;
  Rng rng(777);
  feedNoise(nf, rng, 2000);
  int16_t before = nf.floorDbm();

  nf.addSample(-50.0f);   // 60 dB above the floor

  // Bounded step: one sample can move each quantile by at most STEP * p,
  // regardless of how far away it is. An averaging estimator would take the
  // full 60 dB excursion into its sum.
  EXPECT_LE(abs((int)nf.floorDbm() - (int)before), 1);
}

TEST(NoiseFloorTracker, SurvivesHeavyInterference) {
  NoiseFloorTracker nf;
  Rng rng(2026);
  feedNoise(nf, rng, 2000);

  // 20% of samples are a loud interferer. Both tracked quantiles (0.10, 0.40)
  // remain below the 0.80 clean fraction, so neither is contaminated.
  // For contrast, the arithmetic mean of this mixture is
  // 0.8*(-110) + 0.2*(-60) = -100 dBm, a 10 dB error.
  for (int i = 0; i < 4000; i++) {
    if (i % 5 == 0) nf.addSample(-60.0f);
    else            nf.addSample(NOISE_MEAN + NOISE_SIGMA * rng.normal());
  }

  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 3.0f);
}

// Regression: dispersed contamination (above) and *runs* of contamination are
// different problems, and only the second one broke on hardware.
//
// A bounded per-sample step limits what one outlier can do, but says nothing
// about a run of them. At a 100 ms sample interval a single LoRa packet is 20+
// consecutive samples of signal power, and during such a run the 0.40 quantile
// climbs 4x faster than the 0.10 quantile (STEP*0.40 vs STEP*0.10). The gap
// between them therefore widens, so a scale estimate taken straight from that
// gap measures how far the two trackers have diverged rather than the noise
// spread -- and floorDbm() multiplies it by 1.28.
//
// Deployed to a live repeater this produced a floor of -70 dBm against a true
// floor of -114, on 27% of readings. Assert on the running maximum, not just
// the final value: the original test suite only checked where the estimate
// settled, which is exactly how this got through.
TEST(NoiseFloorTracker, SurvivesRunsOfInterference) {
  NoiseFloorTracker nf;
  Rng rng(8080);
  feedNoise(nf, rng, 2000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.5f);

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

  // The estimate may legitimately drift up somewhat -- half these samples
  // really are loud -- but it must not run away toward the burst level.
  EXPECT_LT((float)worst, NOISE_MEAN + 6.0f)
      << "floor ran away to " << worst << " dBm during runs of -70 dBm samples";
}

TEST(NoiseFloorTracker, ScaleEstimateStaysPhysicallyPlausible) {
  NoiseFloorTracker nf;
  Rng rng(1234);
  feedNoise(nf, rng, 500);

  // A sustained run of strong samples makes the two quantiles diverge, since the
  // 0.40 tracker chases 4x faster than the 0.10 tracker. sigma() must stay
  // within a plausible receiver noise spread throughout, because floorDbm()
  // extrapolates from it -- unbounded, this reached 31.6 dB and dragged the
  // reported floor up by ~40 dB.
  float worst_sigma = nf.sigma();
  int16_t worst_floor = nf.floorDbm();
  for (int i = 0; i < 1500; i++) {
    nf.addSample(-70.0f);
    if (nf.sigma() > worst_sigma) worst_sigma = nf.sigma();
    if (nf.floorDbm() > worst_floor) worst_floor = nf.floorDbm();
  }
  EXPECT_LE(worst_sigma, (float)NOISE_TRACKER_MAX_SIGMA_DB + 0.01f);

  // The 0.10 quantile does legitimately climb toward a genuinely louder ambient,
  // but bounding sigma bounds how far above it the reported floor can be thrown.
  EXPECT_LE((float)worst_floor,
            -70.0f + 1.2816f * (float)NOISE_TRACKER_MAX_SIGMA_DB + 1.0f)
      << "reported floor overshot the sample level itself";
}

TEST(NoiseFloorTracker, RecoversAfterInterferenceStops) {
  NoiseFloorTracker nf;
  Rng rng(31337);
  feedNoise(nf, rng, 500);

  for (int i = 0; i < 2000; i++) {           // heavy interference
    if (i % 5 == 0) nf.addSample(-60.0f);
    else            nf.addSample(NOISE_MEAN + NOISE_SIGMA * rng.normal());
  }
  feedNoise(nf, rng, 2000);                  // clean again

  EXPECT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.5f);
}

TEST(NoiseFloorTracker, TracksARealFloorChange) {
  NoiseFloorTracker nf;
  Rng rng(5150);
  feedNoise(nf, rng, 2000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.5f);

  // Ambient genuinely rises by 15 dB and stays there. The estimator must follow
  // it -- being robust to bursts must not mean being blind to the environment.
  feedNoise(nf, rng, 4000, -95.0f, NOISE_SIGMA);

  EXPECT_NEAR(-95.0f, (float)nf.floorDbm(), 1.5f);
}

TEST(NoiseFloorTracker, ClampsAtMinDbm) {
  NoiseFloorTracker nf;
  for (int i = 0; i < 500; i++) nf.addSample(-140.0f);
  EXPECT_EQ((int16_t)NOISE_TRACKER_MIN_DBM, nf.floorDbm());
}

TEST(NoiseFloorTracker, KeepsQuantilesOrdered) {
  NoiseFloorTracker nf;
  nf.addSample(-110.0f);          // seeds q10 == q40 == -110

  // One low sample opens a gap: q10 -= 0.18, q40 -= 0.12.
  nf.addSample(-120.0f);
  // A sample landing *between* the two quantiles closes the gap from both
  // sides (q10 += 0.02, q40 -= 0.12) and can invert their order. sigma() would
  // go negative without the clamp in addSample().
  for (int i = 0; i < 10; i++) nf.addSample(-110.15f);

  EXPECT_GE(nf.sigma(), (float)NOISE_TRACKER_MIN_SIGMA_DB);
  EXPECT_LE(nf.floorDbm(), 0);    // still a plausible dBm, not NaN-derived
}

TEST(NoiseFloorTracker, ResetReseedsAtNewLevel) {
  NoiseFloorTracker nf;
  Rng rng(60606);
  feedNoise(nf, rng, 2000);
  ASSERT_NEAR((float)NOISE_MEAN, (float)nf.floorDbm(), 1.5f);

  nf.reset();
  EXPECT_FALSE(nf.ready());
  EXPECT_EQ(0, nf.floorDbm());

  // Re-seeds at the new level rather than crawling there from the old one.
  feedNoise(nf, rng, 30, -80.0f, NOISE_SIGMA);
  EXPECT_NEAR(-80.0f, (float)nf.floorDbm(), 3.0f);
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
