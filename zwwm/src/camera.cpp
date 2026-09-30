#include "zwwm/camera.hpp"

#include <algorithm>
#include <cmath>

namespace zwwm {
namespace {

constexpr double kZoomFriction = 12.5;
constexpr double kVelocityStackLimit = 6.0;
constexpr double kVelocityEpsilon = 0.02;
constexpr double kPanDragRate = 24.0;
constexpr double kPanCoastRate = 9.0;
constexpr double kPanEpsilon = 0.05;
constexpr double kPanMomentumSeconds = 0.18;
constexpr double kPanVelocitySmoothing = 0.45;
constexpr double kPanMinimumFlingSpeed = 90.0;
constexpr double kPanMaximumSpeed = 3500.0;
constexpr std::uint64_t kPanSampleTimeoutMs = 100;

}  // namespace

Camera::Camera(const CameraConfig& config) {
  set_config(config);
}

void Camera::set_config(const CameraConfig& config) {
  config_.min_zoom = std::max(0.01, config.min_zoom);
  config_.max_zoom = std::max(config_.min_zoom, config.max_zoom);
  config_.zoom_step = std::max(1.001, config.zoom_step);
}

bool Camera::zoom_by_steps(layout::CanvasViewport& viewport, std::int32_t width,
                           std::int32_t height, int steps, std::uint64_t now_ms) {
  if (steps == 0) return false;

  if (pan_coasting_) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
    pan_coasting_ = false;
  }

  const double impulse = kZoomFriction * std::log(config_.zoom_step);
  const double limit = impulse * kVelocityStackLimit;
  if (!animating_) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
  }
  zoom_velocity_ = std::clamp(zoom_velocity_ + steps * impulse, -limit, limit);
  animating_ = true;
  if (last_update_ms_ == 0) last_update_ms_ = now_ms > 16 ? now_ms - 16 : 0;
  return tick(viewport, width, height, now_ms);
}

bool Camera::pan_by(layout::CanvasViewport& viewport, std::int32_t width,
                    std::int32_t height, double dx, double dy, std::uint64_t now_ms) {
  if (dx == 0.0 && dy == 0.0) return false;
  if (pan_coasting_) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
    pan_velocity_x_ = pan_velocity_y_ = 0.0;
    last_pan_input_ms_ = 0;
    pan_coasting_ = false;
  }
  const bool starting = !animating_;
  if (starting) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
  }
  const bool recent = last_pan_input_ms_ != 0 && now_ms > last_pan_input_ms_ &&
                      now_ms - last_pan_input_ms_ <= kPanSampleTimeoutMs;
  const auto elapsed_ms = recent ? now_ms - last_pan_input_ms_ : 16U;
  const double sample_x = -dx * 1000.0 / (viewport.scale * elapsed_ms);
  const double sample_y = -dy * 1000.0 / (viewport.scale * elapsed_ms);
  const double smoothing = recent ? kPanVelocitySmoothing : 1.0;
  pan_velocity_x_ += (sample_x - pan_velocity_x_) * smoothing;
  pan_velocity_y_ += (sample_y - pan_velocity_y_) * smoothing;
  const double speed = std::hypot(pan_velocity_x_, pan_velocity_y_) * viewport.scale;
  if (speed > kPanMaximumSpeed) {
    pan_velocity_x_ *= kPanMaximumSpeed / speed;
    pan_velocity_y_ *= kPanMaximumSpeed / speed;
  }
  last_pan_input_ms_ = now_ms;
  target_center_x_ -= dx / viewport.scale;
  target_center_y_ -= dy / viewport.scale;
  animating_ = true;
  if (!starting) return false;
  last_update_ms_ = now_ms > 16 ? now_ms - 16 : 0;
  return tick(viewport, width, height, now_ms);
}

bool Camera::finish_pan(layout::CanvasViewport& viewport, std::int32_t width,
                        std::int32_t height, std::uint64_t now_ms) {
  const bool recent = last_pan_input_ms_ != 0 && now_ms >= last_pan_input_ms_ &&
                      now_ms - last_pan_input_ms_ < kPanSampleTimeoutMs;
  if (!animating_) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
  }
  const double speed = std::hypot(pan_velocity_x_, pan_velocity_y_) * viewport.scale;
  pan_coasting_ = recent && speed >= kPanMinimumFlingSpeed;
  if (pan_coasting_) {
    const double freshness = 1.0 - static_cast<double>(now_ms - last_pan_input_ms_) / kPanSampleTimeoutMs;
    target_center_x_ += pan_velocity_x_ * kPanMomentumSeconds * freshness;
    target_center_y_ += pan_velocity_y_ * kPanMomentumSeconds * freshness;
    animating_ = true;
    if (last_update_ms_ == 0) last_update_ms_ = now_ms > 16 ? now_ms - 16 : 0;
  }
  pan_velocity_x_ = pan_velocity_y_ = 0.0;
  last_pan_input_ms_ = 0;
  return animating_;
}

bool Camera::tick(layout::CanvasViewport& viewport, std::int32_t width,
                  std::int32_t height, std::uint64_t now_ms) {
  if (!animating_) return false;

  const double elapsed = now_ms >= last_update_ms_
      ? static_cast<double>(now_ms - last_update_ms_) / 1000.0
      : 1.0 / 60.0;
  const double dt = std::clamp(elapsed, 1.0 / 240.0, 1.0 / 20.0);
  last_update_ms_ = now_ms;

  const double old_scale = std::clamp(viewport.scale, config_.min_zoom, config_.max_zoom);
  const double requested_scale = old_scale * std::exp(zoom_velocity_ * dt);
  const double new_scale = std::clamp(requested_scale, config_.min_zoom, config_.max_zoom);
  const bool hit_bound = new_scale != requested_scale;
  const double center_x = viewport.x + static_cast<double>(width) / (2.0 * old_scale);
  const double center_y = viewport.y + static_cast<double>(height) / (2.0 * old_scale);
  const double pan_alpha = 1.0 - std::exp(-(pan_coasting_ ? kPanCoastRate : kPanDragRate) * dt);
  const double next_x = center_x + (target_center_x_ - center_x) * pan_alpha;
  const double next_y = center_y + (target_center_y_ - center_y) * pan_alpha;
  const double live_x = std::abs(target_center_x_ - next_x) < kPanEpsilon ? target_center_x_ : next_x;
  const double live_y = std::abs(target_center_y_ - next_y) < kPanEpsilon ? target_center_y_ : next_y;
  const bool changed = new_scale != viewport.scale || live_x != center_x || live_y != center_y;
  viewport.scale = new_scale;
  viewport.x = live_x - static_cast<double>(width) / (2.0 * new_scale);
  viewport.y = live_y - static_cast<double>(height) / (2.0 * new_scale);

  zoom_velocity_ *= std::exp(-kZoomFriction * dt);
  if (hit_bound || std::abs(zoom_velocity_) <= kVelocityEpsilon) zoom_velocity_ = 0.0;
  if (zoom_velocity_ == 0.0 && live_x == target_center_x_ && live_y == target_center_y_) {
    animating_ = false;
    last_update_ms_ = 0;
    pan_coasting_ = false;
  }
  return changed;
}

void Camera::stop() {
  zoom_velocity_ = 0.0;
  pan_velocity_x_ = pan_velocity_y_ = 0.0;
  last_pan_input_ms_ = 0;
  last_update_ms_ = 0;
  pan_coasting_ = false;
  animating_ = false;
}

bool Camera::is_animating() const {
  return animating_;
}

}  // namespace zwwm
