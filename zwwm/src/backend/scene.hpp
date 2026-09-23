#pragma once

#include "zwwm/renderer/scene.hpp"
#include "zwwm/renderer/opengl.hpp"
#include "zwwm/runtime_config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace zwwm::backend_scene {

inline void transform_geometry(renderer::DrawCall& draw, const OutputConfig& output,
                               renderer::Size physical, renderer::Point origin = {}) {
  if (!draw.geometry) return;
  const auto transform = [&](renderer::FloatRect& r) {
    const double scale = output.scale_per_mille / 1000.0;
    const double x = (r.origin.x - origin.x) * scale;
    const double y = (r.origin.y - origin.y) * scale;
    const double w = r.size.width * scale, h = r.size.height * scale;
    switch (output.transform) {
      case OutputTransform::rotate_90: r = {{physical.width - y - h, x}, {h, w}}; break;
      case OutputTransform::rotate_180: r = {{physical.width - x - w, physical.height - y - h}, {w, h}}; break;
      case OutputTransform::rotate_270: r = {{y, physical.height - x - w}, {h, w}}; break;
      default: r = {{x, y}, {w, h}}; break;
    }
  };
  transform(draw.geometry->bounds);
  transform(draw.geometry->toplevel);
  if (draw.geometry->clip) transform(*draw.geometry->clip);
  if (draw.geometry->texture) transform(*draw.geometry->texture);
}

struct WindowGeometry { int x; int y; int width; int height; };

inline bool intersects(renderer::Rect left, renderer::Rect right) {
  return static_cast<std::int64_t>(left.origin.x) + left.size.width > right.origin.x &&
         static_cast<std::int64_t>(right.origin.x) + right.size.width > left.origin.x &&
         static_cast<std::int64_t>(left.origin.y) + left.size.height > right.origin.y &&
         static_cast<std::int64_t>(right.origin.y) + right.size.height > left.origin.y;
}

template <typename Surface>
renderer::Rect base_assigned_tile(const Surface& surface) {
  return {{surface.assigned_tile_x, surface.assigned_tile_y},
          {static_cast<std::uint32_t>(std::max(0, surface.assigned_tile_width)),
           static_cast<std::uint32_t>(std::max(0, surface.assigned_tile_height))}};
}

template <typename Surface>
renderer::Rect assigned_tile(const Surface& surface) {
  const auto base = base_assigned_tile(surface);
  const double scale = surface.camera_scale;
  const auto left = static_cast<std::int32_t>(std::lround(
      surface.camera_offset_x + surface.camera_center_x + (base.origin.x - surface.camera_center_x) * scale));
  const auto top = static_cast<std::int32_t>(std::lround(
      surface.camera_offset_y + surface.camera_center_y + (base.origin.y - surface.camera_center_y) * scale));
  const auto right = static_cast<std::int32_t>(std::lround(surface.camera_offset_x + surface.camera_center_x +
      (static_cast<double>(base.origin.x) + base.size.width - surface.camera_center_x) * scale));
  const auto bottom = static_cast<std::int32_t>(std::lround(surface.camera_offset_y + surface.camera_center_y +
      (static_cast<double>(base.origin.y) + base.size.height - surface.camera_center_y) * scale));
  return {{left, top},
          {static_cast<std::uint32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(right) - left)),
           static_cast<std::uint32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(bottom) - top))}};
}

template <typename Surface>
renderer::Rect animated_content(const Surface& surface, const renderer::Rect& tile) {
  const auto original = base_assigned_tile(surface);
  if (original.size.width == 0 || original.size.height == 0) return tile;
  const auto project_x = [&](std::int64_t x) {
    return tile.origin.x + static_cast<std::int32_t>(std::lround(
        static_cast<double>(x - original.origin.x) * tile.size.width / original.size.width));
  };
  const auto project_y = [&](std::int64_t y) {
    return tile.origin.y + static_cast<std::int32_t>(std::lround(
        static_cast<double>(y - original.origin.y) * tile.size.height / original.size.height));
  };
  const auto left = project_x(surface.assigned_content_x);
  const auto top = project_y(surface.assigned_content_y);
  const auto right = project_x(static_cast<std::int64_t>(surface.assigned_content_x) +
                               surface.assigned_content_width);
  const auto bottom = project_y(static_cast<std::int64_t>(surface.assigned_content_y) +
                                surface.assigned_content_height);
  return {{left, top}, {static_cast<std::uint32_t>(std::max(0, right - left)),
                       static_cast<std::uint32_t>(std::max(0, bottom - top))}};
}

template <typename Surface>
float animated_decoration_scale(const Surface& surface, const renderer::Rect& tile) {
  const auto original = base_assigned_tile(surface);
  if (original.size.width == 0 || original.size.height == 0) return 1.0F;
  return std::max(0.0F, std::min(static_cast<float>(tile.size.width) / original.size.width,
                                 static_cast<float>(tile.size.height) / original.size.height));
}

template <typename Surface>
WindowGeometry window_geometry(const Surface& surface) {
  const int left = std::clamp(surface.geometry_x, 0, surface.width);
  const int top = std::clamp(surface.geometry_y, 0, surface.height);
  const int right = static_cast<int>(std::clamp<std::int64_t>(
      static_cast<std::int64_t>(surface.geometry_x) + surface.geometry_width, left, surface.width));
  const int bottom = static_cast<int>(std::clamp<std::int64_t>(
      static_cast<std::int64_t>(surface.geometry_y) + surface.geometry_height, top, surface.height));
  if (right == left || bottom == top) return {0, 0, surface.width, surface.height};
  return {left, top, right - left, bottom - top};
}

template <typename Surface>
std::array<float, 4> source_uv(const WindowGeometry& geometry, const Surface& surface) {
  const float width = surface.source_right - surface.source_left;
  const float height = surface.source_bottom - surface.source_top;
  return {surface.source_left + width * geometry.x / surface.width,
          surface.source_top + height * geometry.y / surface.height,
          surface.source_left + width * (geometry.x + geometry.width) / surface.width,
          surface.source_top + height * (geometry.y + geometry.height) / surface.height};
}

template <typename Surface>
void camera_geometry(renderer::DrawCall& draw, const Surface& root, const Surface& surface,
                     renderer::Rect sampled) {
  if (root.camera_scale == 1.0F && root.camera_offset_x == 0.0 && root.camera_offset_y == 0.0) return;
  const auto target = assigned_tile(root);
  if (root.fullscreen || sampled.origin.x != target.origin.x || sampled.origin.y != target.origin.y ||
      sampled.size.width != target.size.width || sampled.size.height != target.size.height) return;
  const double scale = root.camera_scale;
  const float rounded_scale = animated_decoration_scale(root, sampled);
  if (rounded_scale > 0.0F) {
    const float correction = static_cast<float>(scale) / rounded_scale;
    draw.corner_radius *= correction;
    draw.texture_corner_radius *= correction;
    draw.border_width *= correction;
    draw.clip_radius *= correction;
  }
  const auto project = [&](double x, double y, double w, double h) -> renderer::FloatRect {
    return {{root.camera_offset_x + root.camera_center_x + (x - root.camera_center_x) * scale,
             root.camera_offset_y + root.camera_center_y + (y - root.camera_center_y) * scale},
            {w * scale, h * scale}};
  };
  renderer::DrawGeometry precise;
  precise.toplevel = project(root.assigned_tile_x, root.assigned_tile_y,
                             root.assigned_tile_width, root.assigned_tile_height);
  const auto content = project(root.assigned_content_x, root.assigned_content_y,
                               root.assigned_content_width, root.assigned_content_height);
  precise.bounds = precise.toplevel;
  if (draw.shader_role != renderer::ShaderRole::border) {
    const auto geometry = window_geometry(root);
    const double sx = content.size.width / std::max(1, root.assigned_content_width);
    const double sy = content.size.height / std::max(1, root.assigned_content_height);
    if (surface.toplevel) {
      precise.texture = renderer::FloatRect{content.origin, {geometry.width * sx, geometry.height * sy}};
    } else {
      precise.bounds = {{content.origin.x + (surface.x - root.x - geometry.x) * sx,
                         content.origin.y + (surface.y - root.y - geometry.y) * sy},
                        {surface.width * sx, surface.height * sy}};
    }
    if (!surface.popup) precise.clip = content;
  }
  draw.geometry = precise;
}

}  // namespace zwwm::backend_scene
