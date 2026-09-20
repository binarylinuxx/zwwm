#version 300 es

precision highp float;

in vec2 local_pixel_coordinates;

uniform vec4 zwwm_color;
uniform float zwwm_opacity;
uniform float zwwm_corner_radius;
uniform float zwwm_border_width;
uniform vec4 zwwm_draw_rect;
uniform int zwwm_state;
uniform int zwwm_config_width;
uniform int zwwm_config_radius;
uniform vec4 zwwm_config_color;
uniform vec4 zwwm_config_focused_color;

out vec4 fragment_color;

float rounded_rect_distance(vec2 point, vec2 size, float radius) {
  float r = clamp(radius, 0.0, 0.5 * min(size.x, size.y));
  vec2 q = abs(point - size * 0.5) - size * 0.5 + vec2(r);
  return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
  vec2 size = zwwm_draw_rect.zw;
  float configured_width = float(zwwm_config_width);
  float width = configured_width + max(zwwm_border_width - configured_width, 0.0);
  float radius = max(zwwm_corner_radius, float(zwwm_config_radius) + configured_width);
  float outer = clamp(0.5 - rounded_rect_distance(local_pixel_coordinates, size,
                                                   radius), 0.0, 1.0);
  vec2 inner_size = size - vec2(2.0 * width);
  float inner = inner_size.x <= 0.0 || inner_size.y <= 0.0 ? 0.0 :
      clamp(0.5 - rounded_rect_distance(local_pixel_coordinates - vec2(width),
                                         inner_size,
                                         max(radius - width, 0.0)),
             0.0, 1.0);
  vec4 selected = (zwwm_state & 1) != 0 ? zwwm_config_focused_color : zwwm_config_color;
  vec4 color = vec4(selected.rgb * selected.a, selected.a);
  fragment_color = color * zwwm_opacity * max(outer - inner, 0.0);
}
