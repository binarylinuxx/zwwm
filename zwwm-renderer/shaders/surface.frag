#version 300 es

precision highp float;

in vec2 texture_coordinates;
in vec2 local_pixel_coordinates;
in vec2 target_pixel_coordinates;
flat in vec4 source_uv_value;
flat in int texture_transform_value;

uniform vec4 color;
uniform vec2 rect_size;
uniform float corner_radius;
uniform float border_width;
uniform sampler2D surface_texture;
uniform sampler2D background_texture;
uniform bool has_surface_texture;
uniform bool has_texture_rect;
uniform vec4 texture_rect;
uniform float texture_corner_radius;
uniform vec2 target_size;
uniform bool background_only;
uniform bool has_clip;
uniform vec4 clip_rect;
uniform float clip_radius;
uniform vec3 input_to_clip_row_0;
uniform vec3 input_to_clip_row_1;
uniform float background_mask_alpha_threshold;

out vec4 fragment_color;

#include "rounded_rect.glsl"

void main() {
  vec4 premultiplied_color;
  if (background_only) {
    vec2 background_uv = gl_FragCoord.xy / target_size;
    premultiplied_color = texture(background_texture, background_uv) * color.a;
  } else {
    premultiplied_color = vec4(color.rgb * color.a, color.a);
  }
  if (has_surface_texture) {
    vec2 sample_coordinates = texture_coordinates;
    if (has_texture_rect) {
      vec2 source_position = clamp((target_pixel_coordinates - texture_rect.xy) /
                                   max(texture_rect.zw, vec2(1.0)), vec2(0.0), vec2(1.0));
      if (texture_transform_value == 1) source_position = vec2(source_position.y, 1.0 - source_position.x);
      else if (texture_transform_value == 2) source_position = vec2(1.0) - source_position;
      else if (texture_transform_value == 3) source_position = vec2(1.0 - source_position.y, source_position.x);
      sample_coordinates = mix(source_uv_value.xy, source_uv_value.zw, source_position);
    }
    vec4 surface_color = texture(surface_texture, sample_coordinates);
    if (background_only) {
      float mask = surface_color.a > background_mask_alpha_threshold ? surface_color.a : 0.0;
      premultiplied_color *= mask;
    }
    else premultiplied_color *= surface_color;
  }

  float distance = rounded_rectangle_distance(local_pixel_coordinates, rect_size, corner_radius);
  float coverage = antialiased_coverage(distance);
  if (has_texture_rect && !background_only) {
    float texture_distance = rounded_rectangle_distance(target_pixel_coordinates - texture_rect.xy,
                                                         texture_rect.zw, texture_corner_radius);
    coverage = antialiased_coverage(texture_distance);
  }
  if (has_clip) {
    vec2 input_uv = local_pixel_coordinates / max(rect_size, vec2(1.0));
    vec3 input_position = vec3(input_uv, 1.0);
    vec2 clip_coordinates = vec2(dot(input_to_clip_row_0, input_position),
                                 dot(input_to_clip_row_1, input_position)) * clip_rect.zw;
    float clip_distance = rounded_rectangle_distance(clip_coordinates, clip_rect.zw, clip_radius);
    coverage *= antialiased_coverage(clip_distance);
  }
  if (border_width > 0.0) {
    vec2 inner_size = rect_size - vec2(2.0 * border_width);
    if (inner_size.x > 0.0 && inner_size.y > 0.0) {
      float inner_radius = max(corner_radius - border_width, 0.0);
      float inner_distance = rounded_rectangle_distance(local_pixel_coordinates - vec2(border_width), inner_size, inner_radius);
      coverage = max(coverage - antialiased_coverage(inner_distance), 0.0);
    }
  }
  fragment_color = premultiplied_color * coverage;
}
