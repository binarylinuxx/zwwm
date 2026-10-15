#pragma once

#include "zwwm/renderer/scene.hpp"
#include "zwwm/renderer/opengl.hpp"
#include "zwwm/runtime_config.hpp"
#include "zwwm/overview.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace zwwm::backend_scene {

inline void overview_scenes(renderer::FramePlan& frame, renderer::Rect area, double scroll, unsigned active,
                            double progress, const std::string& background_shader, float radius = 0,
                            bool stack_master = false, double master_ratio = 0.6) {
  for (unsigned tag = 1; tag <= 9; ++tag) {
    const auto card = overview_tag_rect(area, scroll, tag, progress, active, {}, stack_master, master_ratio);
    if (card.origin.y + static_cast<int>(card.size.height) <= area.origin.y ||
        card.origin.y >= area.origin.y + static_cast<int>(area.size.height)) continue;
    const float amount = std::clamp(progress, 0.0, 1.0);
    // A neutral, scene-only shadow. No sampled background or colored glow.
    for (int step = 4; step >= 1; --step) {
      const int spread = static_cast<int>(std::lround(step * 2 * amount));
      const int offset = static_cast<int>(std::lround(3 * amount));
      auto& shadow = frame.draws.emplace_back(0,
          renderer::Rect{{card.origin.x - spread, card.origin.y - spread + offset},
                         {card.size.width + 2 * spread, card.size.height + 2 * spread}},
          amount * 0.035F, std::array<float, 4>{0, 0, 0, 1}, std::nullopt);
      shadow.corner_radius = radius * amount + spread;
    }
    auto& background = frame.draws.emplace_back(0, card, 1.0F, std::array<float, 4>{0.125F, 0.141F, 0.169F, 1.0F}, std::nullopt);
    background.shader_role = renderer::ShaderRole::background;
    background.shader_name = background_shader;
    background.shader_time = frame.shader_time;
    background.corner_radius = radius * std::clamp(progress, 0.0, 1.0);
    background.clip_bounds = card;
    background.clip_radius = background.corner_radius;
    background.strict_clip = true;
  }
}

inline renderer::Rect overview_intersection(renderer::Rect a, renderer::Rect b) {
  const auto left = std::max(a.origin.x, b.origin.x), top = std::max(a.origin.y, b.origin.y);
  const auto right = std::min(static_cast<std::int64_t>(a.origin.x) + a.size.width, static_cast<std::int64_t>(b.origin.x) + b.size.width);
  const auto bottom = std::min(static_cast<std::int64_t>(a.origin.y) + a.size.height, static_cast<std::int64_t>(b.origin.y) + b.size.height);
  return {{left, top}, {static_cast<unsigned>(std::max<std::int64_t>(0, right - left)), static_cast<unsigned>(std::max<std::int64_t>(0, bottom - top))}};
}

inline void overview_clip(renderer::DrawCall& draw, renderer::Rect scene, float radius = 0) {
  draw.clip_bounds = draw.clip_bounds ? overview_intersection(*draw.clip_bounds, scene) : scene;
  draw.clip_radius = radius;
  draw.strict_clip = true;
  if (draw.geometry) {
    renderer::FloatRect clip{{static_cast<double>(scene.origin.x), static_cast<double>(scene.origin.y)},
                            {static_cast<double>(scene.size.width), static_cast<double>(scene.size.height)}};
    if (const auto& old = draw.geometry->clip) {
      const double right = std::min(old->origin.x + old->size.width, clip.origin.x + clip.size.width);
      const double bottom = std::min(old->origin.y + old->size.height, clip.origin.y + clip.size.height);
      clip.origin.x = std::max(clip.origin.x, old->origin.x); clip.origin.y = std::max(clip.origin.y, old->origin.y);
      clip.size.width = std::max(0.0, right - clip.origin.x); clip.size.height = std::max(0.0, bottom - clip.origin.y);
    }
    draw.geometry->clip = clip;
  }
}

inline renderer::DrawCall overview_layer(renderer::DrawCall draw, renderer::Rect area, double scroll,
                                         unsigned active, double progress, unsigned tag, float radius = 0,
                                         bool stack_master = false, double master_ratio = 0.6) {
  const auto transform = overview_scene_transform(area, area, scroll, tag, progress, active, stack_master, master_ratio);
  const auto source = draw.bounds;
  draw.bounds = overview_project(source, transform);
  draw.toplevel_bounds = draw.bounds;
  draw.corner_radius *= transform.scale;
  draw.background_blur_radius *= transform.scale;
  renderer::DrawGeometry geometry;
  geometry.bounds = {{source.origin.x * transform.scale + transform.x, source.origin.y * transform.scale + transform.y},
                     {source.size.width * transform.scale, source.size.height * transform.scale}};
  geometry.toplevel = geometry.bounds;
  draw.geometry = geometry;
  overview_clip(draw, overview_tag_rect(area, scroll, tag, progress, active, {}, stack_master, master_ratio), radius * std::clamp(progress, 0.0, 1.0));
  return draw;
}

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

inline bool overview_draw_visible(const renderer::DrawCall& draw, renderer::Rect area) {
  const double x = draw.geometry ? draw.geometry->bounds.origin.x : draw.bounds.origin.x;
  const double y = draw.geometry ? draw.geometry->bounds.origin.y : draw.bounds.origin.y;
  const double width = draw.geometry ? draw.geometry->bounds.size.width : draw.bounds.size.width;
  const double height = draw.geometry ? draw.geometry->bounds.size.height : draw.bounds.size.height;
  return x + width > area.origin.x && y + height > area.origin.y &&
      x < area.origin.x + static_cast<double>(area.size.width) && y < area.origin.y + static_cast<double>(area.size.height);
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
  const double x = surface.canvas_camera ? static_cast<double>(surface.camera_world_x) :
      base.origin.x - surface.camera_center_x;
  const double y = surface.canvas_camera ? static_cast<double>(surface.camera_world_y) :
      base.origin.y - surface.camera_center_y;
  const double origin_x = surface.canvas_camera ? surface.camera_offset_x :
      surface.camera_offset_x + surface.camera_center_x;
  const double origin_y = surface.canvas_camera ? surface.camera_offset_y :
      surface.camera_offset_y + surface.camera_center_y;
  const auto left = static_cast<std::int32_t>(std::lround(
      origin_x + x * scale));
  const auto top = static_cast<std::int32_t>(std::lround(
      origin_y + y * scale));
  const auto right = static_cast<std::int32_t>(std::lround(origin_x + (x + base.size.width) * scale));
  const auto bottom = static_cast<std::int32_t>(std::lround(origin_y + (y + base.size.height) * scale));
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
  if (!root.canvas_camera) return;
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
    return {{root.camera_offset_x + (root.camera_world_x + (x - root.assigned_tile_x)) * scale,
             root.camera_offset_y + (root.camera_world_y + (y - root.assigned_tile_y)) * scale},
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
