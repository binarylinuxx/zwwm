#pragma once

// Copyright 2026 binarylinuxx
// SPDX-License-Identifier: BSD-3-Clause
#include "zwwm/renderer/scene.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace zwwm {

inline std::uint64_t overview_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct OverviewMotion {
  double from = 0, target = 0;
  std::uint64_t started = 0;
  std::uint32_t duration = 0;

  bool active(std::uint64_t now) const {
    return duration != 0 && from != target && (now < started || now - started < duration);
  }
  double sample(std::uint64_t now) const {
    if (!active(now)) return target;
    const double t = now <= started ? 0 : std::clamp(static_cast<double>(now - started) / duration, 0.0, 1.0);
    // A critically damped response, normalized to settle exactly at the endpoint.
    const double eased = (1 - (1 + 8 * t) * std::exp(-8 * t)) / (1 - 9 * std::exp(-8.0));
    return from + (target - from) * eased;
  }
  void reset(double value) { from = target = value; duration = 0; }
  void retarget(double value, std::uint64_t now, std::uint32_t ms) {
    from = sample(now); target = value; started = now; duration = ms;
    if (ms == 0) reset(value);
  }
};

inline std::int32_t overview_coordinate(double value) {
  return static_cast<std::int32_t>(std::clamp(std::round(value),
      static_cast<double>(std::numeric_limits<std::int32_t>::min()),
      static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}
inline renderer::Rect overview_tag_rect(renderer::Rect area, double first, int tag,
                                        double progress = 1, int active = 1,
                                        renderer::Rect = {}) {
  const double p = std::clamp(progress, 0.0, 1.0);
  const double gap = std::min(12U, std::min(area.size.width, area.size.height) / 8);
  const double height = std::max(1.0, (area.size.height - 4 * gap) / 3);
  const double zoom = std::min(std::max(1.0, area.size.width - 2 * gap) / std::max(1U, area.size.width),
                              height / std::max(1U, area.size.height));
  const double width = area.size.width * zoom;
  const double open_x = area.origin.x + (area.size.width - width) / 2;
  const double open_y = area.origin.y + gap + (tag - 1 - first) * (height + gap);
  const double closed_y = area.origin.y + (tag - active) * static_cast<double>(area.size.height);
  return {{overview_coordinate(area.origin.x + (open_x - area.origin.x) * p),
           overview_coordinate(closed_y + (open_y - closed_y) * p)},
          {static_cast<unsigned>(std::max(1.0, std::round(area.size.width + (width - area.size.width) * p))),
           static_cast<unsigned>(std::max(1.0, std::round(area.size.height + (height - area.size.height) * p)))}};
}

struct OverviewTransform { double scale = 1, x = 0, y = 0; };

inline OverviewTransform overview_scene_transform(renderer::Rect area, renderer::Rect,
                                                  double first, int tag, double progress, int active) {
  const auto card = overview_tag_rect(area, first, tag, 1, active);
  const double zoom = std::min(static_cast<double>(card.size.width) / std::max(1U, area.size.width),
                              static_cast<double>(card.size.height) / std::max(1U, area.size.height));
  const double x = card.origin.x - area.origin.x * zoom;
  const double y = card.origin.y - area.origin.y * zoom;
  const double p = std::clamp(progress, 0.0, 1.0);
  return {1 + (zoom - 1) * p, x * p,
          (tag - active) * static_cast<double>(area.size.height) * (1 - p) + y * p};
}

inline renderer::Rect overview_project(renderer::Rect source, OverviewTransform transform) {
  const auto left = overview_coordinate(source.origin.x * transform.scale + transform.x);
  const auto top = overview_coordinate(source.origin.y * transform.scale + transform.y);
  const auto right = overview_coordinate((source.origin.x + static_cast<double>(source.size.width)) * transform.scale + transform.x);
  const auto bottom = overview_coordinate((source.origin.y + static_cast<double>(source.size.height)) * transform.scale + transform.y);
  return {{left, top}, {static_cast<unsigned>(std::max<std::int64_t>(1, static_cast<std::int64_t>(right) - left)),
                       static_cast<unsigned>(std::max<std::int64_t>(1, static_cast<std::int64_t>(bottom) - top))}};
}
}  // namespace zwwm
