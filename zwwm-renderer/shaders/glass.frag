#version 300 es

precision highp float;

in vec2 local_pixel_coordinates;
in vec2 target_pixel_coordinates;

uniform vec2 rect_size;
uniform float corner_radius;
uniform sampler2D background_texture;
uniform sampler2D reflection_texture;
uniform vec2 target_size;
uniform bool has_clip;
uniform vec4 clip_rect;
uniform float clip_radius;
uniform int glass_quality;
uniform float glass_refraction;
uniform float glass_dispersion;
uniform float glass_frost;
uniform float glass_time;
uniform vec4 glass_tint;
uniform float glass_ior;
uniform float glass_thickness;
uniform float glass_roughness;
uniform float glass_opacity;

out vec4 fragment_color;

#include "rounded_rect.glsl"
#include "glass_physics.glsl"

void main() {
  float coverage = antialiased_coverage(rounded_rectangle_distance(local_pixel_coordinates, rect_size, corner_radius));
  if (has_clip) {
    float clip_distance = rounded_rectangle_distance(target_pixel_coordinates - clip_rect.xy, clip_rect.zw, clip_radius);
    coverage *= antialiased_coverage(clip_distance);
  }
  fragment_color = physically_based_glass(gl_FragCoord.xy / target_size) * (coverage * glass_opacity);
}
