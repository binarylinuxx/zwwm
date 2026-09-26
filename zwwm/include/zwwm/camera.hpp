#pragma once

#include "zwwm/layout/canvas.hpp"

#include <cstdint>

namespace zwwm {

struct CameraConfig {
  double min_zoom = 0.25;
  double max_zoom = 1.35;
  double zoom_step = 1.1;
};

class Camera {
public:
  explicit Camera(const CameraConfig& config = {});

  void set_config(const CameraConfig& config);
  bool zoom_by_steps(layout::CanvasViewport& viewport, std::int32_t width,
                     std::int32_t height, int steps, std::uint64_t now_ms);
  bool pan_by(layout::CanvasViewport& viewport, std::int32_t width,
              std::int32_t height, double dx, double dy, std::uint64_t now_ms);
  bool finish_pan(layout::CanvasViewport& viewport, std::int32_t width,
                  std::int32_t height);
  bool tick(layout::CanvasViewport& viewport, std::int32_t width,
            std::int32_t height, std::uint64_t now_ms);
  void stop();

  [[nodiscard]] bool is_animating() const;

private:
  CameraConfig config_;
  double zoom_velocity_ = 0.0;
  double target_center_x_ = 0.0, target_center_y_ = 0.0;
  std::uint64_t last_update_ms_ = 0;
  bool animating_ = false;
};

}  // namespace zwwm
