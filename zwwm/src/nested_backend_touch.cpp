#include "zwwm/nested_backend.hpp"
#include "zwwm/runtime_config.hpp"

#include <algorithm>
#include <cmath>

namespace zwwm {
void NestedBackend::TouchObserver::down(zwayland::client::Proxy&, std::uint32_t,
                                       std::uint32_t time, zwayland::client::Proxy* surface,
                                       std::int32_t id, double x, double y) const {
  if (surface == backend->surface_) backend->deliver_touch_motion(time, id, x, y, true);
}
void NestedBackend::TouchObserver::up(zwayland::client::Proxy&, std::uint32_t,
                                     std::uint32_t time, std::int32_t id) const {
  if (backend->input_target_ != nullptr) backend->input_target_->touch_up(time, id);
}
void NestedBackend::TouchObserver::motion(zwayland::client::Proxy&, std::uint32_t time,
                                         std::int32_t id, double x, double y) const {
  backend->deliver_touch_motion(time, id, x, y, false);
}
void NestedBackend::TouchObserver::frame(zwayland::client::Proxy&) const {
  if (backend->input_target_ != nullptr) backend->input_target_->touch_frame();
}
void NestedBackend::TouchObserver::cancel(zwayland::client::Proxy&) const {
  if (backend->input_target_ != nullptr) backend->input_target_->touch_cancel();
}
void NestedBackend::TouchObserver::shape(zwayland::client::Proxy&, std::int32_t, double, double) const {}
void NestedBackend::TouchObserver::orientation(zwayland::client::Proxy&, std::int32_t, double) const {}

void NestedBackend::deliver_touch_motion(std::uint32_t time, std::int32_t id, double x, double y, bool down) {
  if (input_target_ == nullptr) return;
  const auto point = config_->output_for("nested-1").physical_to_logical(
      {static_cast<std::int32_t>(std::lround(x)), static_cast<std::int32_t>(std::lround(y))},
      {physical_width_, physical_height_});
  const auto px = std::clamp(point.x, 0, static_cast<std::int32_t>(logical_width_) - 1);
  const auto py = std::clamp(point.y, 0, static_cast<std::int32_t>(logical_height_) - 1);
  if (down) input_target_->touch_down(time, id, px, py);
  else input_target_->touch_motion(time, id, px, py);
}
}  // namespace zwwm
