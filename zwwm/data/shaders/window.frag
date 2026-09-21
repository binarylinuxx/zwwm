#version 300 es

precision highp float;

in vec2 texture_coordinates;
in vec2 target_pixel_coordinates;
flat in vec4 source_uv_value;
flat in int texture_transform_value;

uniform sampler2D zwwm_window_texture;
uniform sampler2D zwwm_blurred_backdrop_texture;
uniform vec4 zwwm_color;
uniform float zwwm_opacity;
uniform bool zwwm_has_texture;
uniform vec4 zwwm_clip_rect;
uniform vec4 zwwm_texture_rect;
uniform vec2 zwwm_output_size;
uniform float zwwm_clip_radius;
uniform int zwwm_state;
uniform int zwwm_config_radius;

out vec4 fragment_color;

float rounded_rect_distance(vec2 point, vec2 size, float radius) {
  float r = clamp(radius, 0.0, 0.5 * min(size.x, size.y));
  vec2 q = abs(point - size * 0.5) - size * 0.5 + vec2(r);
  return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
  vec4 color = vec4(zwwm_color.rgb * zwwm_color.a, zwwm_color.a);
  vec2 position = clamp((target_pixel_coordinates - zwwm_texture_rect.xy) /
                        max(zwwm_texture_rect.zw, vec2(1.0)), vec2(0.0), vec2(1.0));
  if (texture_transform_value == 1) position = vec2(position.y, 1.0 - position.x);
  else if (texture_transform_value == 2) position = vec2(1.0) - position;
  else if (texture_transform_value == 3) position = vec2(1.0 - position.y, position.x);
  vec2 uv = mix(source_uv_value.xy, source_uv_value.zw, position);
  if (zwwm_has_texture) color *= texture(zwwm_window_texture, uv);
  if ((zwwm_state & 4) != 0) {
    fragment_color = color * zwwm_opacity;
    return;
  }
  vec2 backdrop_uv = vec2(target_pixel_coordinates.x,
                          zwwm_output_size.y - target_pixel_coordinates.y) /
                     max(zwwm_output_size, vec2(1.0));
  vec3 backdrop = texture(zwwm_blurred_backdrop_texture,
                          clamp(backdrop_uv, vec2(0.0), vec2(1.0))).rgb;
  color += vec4(backdrop, 1.0) * (1.0 - color.a);
  float coverage = clamp(0.5 - rounded_rect_distance(
      target_pixel_coordinates - zwwm_clip_rect.xy, zwwm_clip_rect.zw,
      zwwm_clip_radius), 0.0, 1.0);
  fragment_color = color * zwwm_opacity * coverage;
}
