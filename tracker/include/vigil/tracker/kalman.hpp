#pragma once

// Linear Kalman filter for a 2-D constant-velocity target.
// State: [x, y, vx, vy] (metres, metres/second). Measurement: [x, y].

#include <Eigen/Dense>

namespace vigil::tracker {

using Vec2 = Eigen::Vector2d;
using Vec4 = Eigen::Vector4d;
using Mat2 = Eigen::Matrix2d;
using Mat4 = Eigen::Matrix4d;
using Mat24 = Eigen::Matrix<double, 2, 4>;
using Mat42 = Eigen::Matrix<double, 4, 2>;

struct KalmanConfig {
  // 1-sigma unmodelled acceleration. Larger values follow manoeuvres better at the
  // cost of noisier estimates. A 250 m/s target turning at 3 deg/s pulls ~13 m/s^2.
  double process_accel_sigma_mps2 = 10.0;
  double measurement_sigma_m = 25.0;
  // Velocity is unknown when a track starts from a single detection.
  double initial_speed_sigma_mps = 300.0;
};

struct Innovation {
  Vec2 residual;               // z - H x
  Mat2 covariance;             // S = H P H' + R
  double mahalanobis_sq = 0.0;  // residual' S^-1 residual, chi-square with 2 dof
  double log_det_covariance = 0.0;
};

// Everything needed to score measurements against one prediction. Computing it once per
// track per scan, instead of once per (track, measurement) pair, is what makes gating cheap.
struct MeasurementPrediction {
  Vec2 z;                       // predicted measurement, H x
  Mat2 inv_covariance;          // S^-1
  double log_det_covariance = 0.0;

  // Squared Mahalanobis distance of a measurement from the prediction.
  [[nodiscard]] double mahalanobis_sq(const Vec2& measurement) const {
    const Vec2 r = measurement - z;
    return r.dot(inv_covariance * r);
  }
};

class ConstantVelocityKf {
 public:
  // Starts a filter at a measured position with zero velocity and wide velocity variance.
  ConstantVelocityKf(const Vec2& position, const KalmanConfig& config);

  // Propagates state and covariance forward by dt seconds.
  void predict(double dt_s);

  // How well a measurement fits the current prediction. Does not modify the filter.
  [[nodiscard]] Innovation innovation(const Vec2& measurement) const;

  // The current prediction in measurement space, for scoring many measurements.
  [[nodiscard]] MeasurementPrediction predict_measurement() const;

  // Innovation covariance S = H P H' + R.
  [[nodiscard]] Mat2 innovation_covariance() const;

  // Folds a measurement into the estimate (Joseph form, stays symmetric and PSD).
  void update(const Vec2& measurement);

  [[nodiscard]] const Vec4& state() const { return x_; }
  [[nodiscard]] const Mat4& covariance() const { return P_; }
  [[nodiscard]] Vec2 position() const { return x_.head<2>(); }
  [[nodiscard]] Vec2 velocity() const { return x_.tail<2>(); }

 private:
  Vec4 x_;
  Mat4 P_;
  Mat2 R_;
  double accel_var_;
};

}  // namespace vigil::tracker
