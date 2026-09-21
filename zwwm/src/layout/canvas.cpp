#include "zwwm/layout/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zwwm::layout {

CanvasRect canvas_view_bounds(const CanvasBounds& bounds, const CanvasViewport& viewport,
                              CanvasRect work) {
  const auto coordinate = [](double value) {
    return static_cast<std::int32_t>(std::clamp(
        std::round(value), static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())));
  };
  const auto left = coordinate(work.x + (bounds.x - viewport.x) * viewport.scale);
  const auto top = coordinate(work.y + (bounds.y - viewport.y) * viewport.scale);
  const auto right = coordinate(work.x +
      (bounds.x + bounds.width - viewport.x) * viewport.scale);
  const auto bottom = coordinate(work.y +
      (bounds.y + bounds.height - viewport.y) * viewport.scale);
  return {left, top,
          static_cast<std::int32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(right) - left)),
          static_cast<std::int32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(bottom) - top))};
}

CanvasRect canvas_intrinsic_bounds(const CanvasBounds& bounds,
                                   const CanvasViewport& viewport,
                                   CanvasRect work) {
  const double center_x = viewport.x + work.width / (2.0 * viewport.scale);
  const double center_y = viewport.y + work.height / (2.0 * viewport.scale);
  return {static_cast<std::int32_t>(std::lround(
              work.x + work.width / 2.0 + bounds.x - center_x)),
          static_cast<std::int32_t>(std::lround(
              work.y + work.height / 2.0 + bounds.y - center_y)),
          std::max(1, bounds.width), std::max(1, bounds.height)};
}

std::int32_t canvas_world_delta(std::int32_t screen_delta, const CanvasViewport& viewport) {
  return static_cast<std::int32_t>(std::clamp(
      std::llround(static_cast<double>(screen_delta) / viewport.scale),
      static_cast<long long>(std::numeric_limits<std::int32_t>::min()),
      static_cast<long long>(std::numeric_limits<std::int32_t>::max())));
}

void initialize_canvas_bounds(CanvasBounds& bounds, CanvasRect previous,
                              CanvasRect work, const CanvasViewport& viewport,
                              std::int32_t content_width, std::int32_t content_height,
                              std::uint32_t border_width) {
  if (bounds.initialized) return;
  if (previous.width > 0 && previous.height > 0) {
    bounds = {.x = static_cast<std::int64_t>(std::llround(
                  viewport.x + (previous.x - work.x) / viewport.scale)),
              .y = static_cast<std::int64_t>(std::llround(
                  viewport.y + (previous.y - work.y) / viewport.scale)),
              .width = previous.width, .height = previous.height, .initialized = true};
    return;
  }
  const auto border = static_cast<std::int32_t>(std::min<std::uint32_t>(
      border_width, static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max() / 2)));
  const auto width = content_width > 0
      ? std::max(1, content_width + std::min(border * 2,
          std::numeric_limits<std::int32_t>::max() - content_width))
      : std::max(1, work.width / 2);
  const auto height = content_height > 0
      ? std::max(1, content_height + std::min(border * 2,
          std::numeric_limits<std::int32_t>::max() - content_height))
      : std::max(1, work.height / 2);
  bounds = {.x = static_cast<std::int64_t>(std::llround(
                viewport.x + (work.width / viewport.scale - width) / 2.0)),
            .y = static_cast<std::int64_t>(std::llround(
                viewport.y + (work.height / viewport.scale - height) / 2.0)),
            .width = width, .height = height, .initialized = true};
}

std::pair<std::int64_t, std::int64_t> snap_canvas_window(
    std::int64_t x, std::int64_t y, std::int32_t width, std::int32_t height,
    std::span<const CanvasBounds> candidates, std::uint32_t configured_gap,
    const CanvasViewport& viewport, std::int32_t capture_pixels) {
  const auto gap = static_cast<std::int64_t>(configured_gap);
  const auto threshold = static_cast<std::int64_t>(
      std::max(1, std::abs(canvas_world_delta(std::max(1, capture_pixels), viewport))));
  auto snapped_x = x;
  auto snapped_y = y;
  auto x_distance = threshold + 1;
  auto y_distance = threshold + 1;
  const auto near_range = [threshold](std::int64_t first_start, std::int64_t first_end,
                                      std::int64_t second_start, std::int64_t second_end) {
    return first_end + threshold >= second_start && second_end + threshold >= first_start;
  };
  for (const auto& other : candidates) {
    if (!other.initialized) continue;
    const auto other_right = other.x + other.width;
    const auto other_bottom = other.y + other.height;
    if (near_range(y, y + height, other.y, other_bottom)) {
      for (const auto target : {other.x - gap - width, other_right + gap}) {
        const auto distance = std::abs(x - target);
        if (distance <= threshold && distance < x_distance) {
          snapped_x = target;
          x_distance = distance;
        }
      }
    }
    if (near_range(x, x + width, other.x, other_right)) {
      for (const auto target : {other.y - gap - height, other_bottom + gap}) {
        const auto distance = std::abs(y - target);
        if (distance <= threshold && distance < y_distance) {
          snapped_y = target;
          y_distance = distance;
        }
      }
    }
  }
  return {snapped_x, snapped_y};
}

std::optional<std::size_t> nearest_in_direction(
    FocusPoint origin, std::span<const FocusPoint> candidates, FocusDirection direction) {
  std::optional<std::size_t> nearest;
  double nearest_distance = std::numeric_limits<double>::infinity();
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const double dx = candidates[index].x - origin.x;
    const double dy = candidates[index].y - origin.y;
    const bool matches = direction == FocusDirection::left ? dx < 0.0 :
        direction == FocusDirection::right ? dx > 0.0 :
        direction == FocusDirection::up ? dy < 0.0 : dy > 0.0;
    if (!matches) continue;
    const double primary = direction == FocusDirection::left || direction == FocusDirection::right ? dx : dy;
    const double secondary = direction == FocusDirection::left || direction == FocusDirection::right ? dy : dx;
    const double distance = primary * primary + secondary * secondary * 2.0;
    if (distance < nearest_distance) {
      nearest = index;
      nearest_distance = distance;
    }
  }
  return nearest;
}

}  // namespace zwwm::layout
