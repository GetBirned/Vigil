#include "vigil/tracker/kalman.hpp"

#include <cmath>

namespace vigil::tracker {
namespace {

Mat24 measurement_matrix() {
  Mat24 H = Mat24::Zero();
  H(0, 0) = 1.0;
  H(1, 1) = 1.0;
  return H;
}

}  // namespace

ConstantVelocityKf::ConstantVelocityKf(const Vec2& position, const KalmanConfig& config)
    : x_(Vec4::Zero()),
      P_(Mat4::Zero()),
      R_(Mat2::Identity() * config.measurement_sigma_m * config.measurement_sigma_m),
      accel_var_(config.process_accel_sigma_mps2 * config.process_accel_sigma_mps2) {
  x_.head<2>() = position;
  const double pos_var = config.measurement_sigma_m * config.measurement_sigma_m;
  const double vel_var = config.initial_speed_sigma_mps * config.initial_speed_sigma_mps;
  P_.diagonal() << pos_var, pos_var, vel_var, vel_var;
}

void ConstantVelocityKf::predict(double dt_s) {
  if (dt_s <= 0.0) {
    return;
  }

  Mat4 F = Mat4::Identity();
  F(0, 2) = dt_s;
  F(1, 3) = dt_s;

  // Discrete white-noise acceleration model, applied independently per axis.
  const double dt2 = dt_s * dt_s;
  const double q_pp = dt2 * dt2 / 4.0 * accel_var_;
  const double q_pv = dt2 * dt_s / 2.0 * accel_var_;
  const double q_vv = dt2 * accel_var_;
  Mat4 Q = Mat4::Zero();
  Q(0, 0) = q_pp;
  Q(1, 1) = q_pp;
  Q(0, 2) = q_pv;
  Q(2, 0) = q_pv;
  Q(1, 3) = q_pv;
  Q(3, 1) = q_pv;
  Q(2, 2) = q_vv;
  Q(3, 3) = q_vv;

  x_ = F * x_;
  P_ = F * P_ * F.transpose() + Q;
}

Innovation ConstantVelocityKf::innovation(const Vec2& measurement) const {
  const Mat24 H = measurement_matrix();
  Innovation inn;
  inn.residual = measurement - H * x_;
  inn.covariance = H * P_ * H.transpose() + R_;
  const Eigen::LLT<Mat2> llt(inn.covariance);
  inn.mahalanobis_sq = inn.residual.dot(llt.solve(inn.residual));
  inn.log_det_covariance = std::log(inn.covariance.determinant());
  return inn;
}

Mat2 ConstantVelocityKf::innovation_covariance() const {
  // H just selects position, so H P H' is P's top-left 2x2 block.
  return P_.topLeftCorner<2, 2>() + R_;
}

MeasurementPrediction ConstantVelocityKf::predict_measurement() const {
  const Mat2 S = innovation_covariance();
  return {
      .z = x_.head<2>(),
      .inv_covariance = S.inverse(),  // closed form for 2x2; S is SPD, well conditioned
      .log_det_covariance = std::log(S.determinant()),
  };
}

void ConstantVelocityKf::update(const Vec2& measurement) {
  const Mat24 H = measurement_matrix();
  const Vec2 y = measurement - H * x_;
  const Mat2 S = H * P_ * H.transpose() + R_;
  // K = P H' S^-1, computed with a solve rather than an explicit inverse.
  const Mat42 K = S.llt().solve(H * P_).transpose();

  x_ += K * y;
  const Mat4 I_KH = Mat4::Identity() - K * H;
  P_ = I_KH * P_ * I_KH.transpose() + K * R_ * K.transpose();
}

}  // namespace vigil::tracker
