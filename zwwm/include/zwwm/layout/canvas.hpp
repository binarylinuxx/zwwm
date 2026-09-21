#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace zwwm::layout {

struct CanvasBounds {
  std::int64_t x = 0, y = 0;
  std::int32_t width = 0, height = 0;
  bool initialized = false;
};

struct CanvasViewport {
  double x = 0.0, y = 0.0;
  double scale = 1.0;
  double zoom_log_velocity = 0.0;
  std::uint64_t last_zoom_tick_ms = 0;
};

struct CanvasRect {
  std::int32_t x = 0, y = 0, width = 0, height = 0;
};

struct FocusPoint {
  double x = 0.0;
  double y = 0.0;
};

enum class FocusDirection : std::uint8_t { left, right, up, down };

[[nodiscard]] CanvasRect canvas_view_bounds(const CanvasBounds& bounds,
                                            const CanvasViewport& viewport,
                                            CanvasRect work);
[[nodiscard]] CanvasRect canvas_intrinsic_bounds(const CanvasBounds& bounds,
                                                 const CanvasViewport& viewport,
                                                 CanvasRect work);
[[nodiscard]] std::int32_t canvas_world_delta(std::int32_t screen_delta,
                                              const CanvasViewport& viewport);
void initialize_canvas_bounds(CanvasBounds& bounds, CanvasRect previous,
                              CanvasRect work, const CanvasViewport& viewport,
                              std::int32_t content_width, std::int32_t content_height,
                              std::uint32_t border_width);
[[nodiscard]] std::pair<std::int64_t, std::int64_t> snap_canvas_window(
    std::int64_t x, std::int64_t y, std::int32_t width, std::int32_t height,
    std::span<const CanvasBounds> candidates, std::uint32_t gap,
    const CanvasViewport& viewport, std::int32_t capture_pixels = 16);
[[nodiscard]] std::optional<std::size_t> nearest_in_direction(
    FocusPoint origin, std::span<const FocusPoint> candidates, FocusDirection direction);

}  // namespace zwwm::layout
