#include <gtest/gtest.h>

#include <random>

#include "vigil/tracker/kalman.hpp"

namespace vigil::tracker {
namespace {

TEST(Kalman, StartsAtMeasuredPositionWithZeroVelocity) {
  const ConstantVelocityKf kf(Vec2(100.0, -50.0), {});
  EXPECT_EQ(kf.position(), Vec2(100.0, -50.0));
  EXPECT_EQ(kf.velocity(), Vec2::Zero());
}

TEST(Kalman, PredictMovesPositionByVelocityAndGrowsUncertainty) {
  ConstantVelocityKf kf(Vec2::Zero(), {});
  kf.update(Vec2(0.0, 0.0));
  const double var_before = kf.covariance()(0, 0);
  kf.predict(1.0);
  EXPECT_GT(kf.covariance()(0, 0), var_before);
}

TEST(Kalman, PerfectMeasurementOfPredictionHasZeroDistance) {
  const ConstantVelocityKf kf(Vec2(10.0, 20.0), {});
  const Innovation inn = kf.innovation(Vec2(10.0, 20.0));
  EXPECT_DOUBLE_EQ(inn.mahalanobis_sq, 0.0);
}

TEST(Kalman, ConvergesOnConstantVelocityTarget) {
  KalmanConfig config;
  config.measurement_sigma_m = 25.0;
  ConstantVelocityKf kf(Vec2(0.0, 0.0), config);

  std::mt19937_64 rng(1);
  std::normal_distribution<double> noise(0.0, config.measurement_sigma_m);
  const Vec2 velocity(150.0, -80.0);
  Vec2 truth = Vec2::Zero();
  for (int i = 0; i < 60; ++i) {
    truth += velocity;
    kf.predict(1.0);
    kf.update(truth + Vec2(noise(rng), noise(rng)));
  }

  EXPECT_LT((kf.position() - truth).norm(), 40.0);
  EXPECT_LT((kf.velocity() - velocity).norm(), 15.0);
}

TEST(Kalman, CovarianceStaysSymmetricAndPositiveDefinite) {
  ConstantVelocityKf kf(Vec2::Zero(), {});
  std::mt19937_64 rng(2);
  std::normal_distribution<double> noise(0.0, 25.0);
  for (int i = 0; i < 500; ++i) {
    kf.predict(0.5);
    kf.update(Vec2(noise(rng), noise(rng)));
  }
  const Mat4& P = kf.covariance();
  EXPECT_LT((P - P.transpose()).cwiseAbs().maxCoeff(), 1e-9);
  EXPECT_EQ(Eigen::LLT<Mat4>(P).info(), Eigen::Success);
}

TEST(Kalman, NonPositiveTimeStepIsANoOp) {
  ConstantVelocityKf kf(Vec2(1.0, 2.0), {});
  const Mat4 before = kf.covariance();
  kf.predict(0.0);
  kf.predict(-1.0);
  EXPECT_EQ(kf.covariance(), before);
}

}  // namespace
}  // namespace vigil::tracker
