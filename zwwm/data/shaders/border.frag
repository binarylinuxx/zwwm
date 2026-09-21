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
uniform vec4 zwwm_config_gradient_colors[20];
uniform int zwwm_config_gradient_colors_count;
uniform vec4 zwwm_config_focused_gradient_colors[20];
uniform int zwwm_config_focused_gradient_colors_count;

out vec4 fragment_color;

float rounded_rect_distance(vec2 point, vec2 size, float radius) {
  float r = clamp(radius, 0.0, 0.5 * min(size.x, size.y));
  vec2 q = abs(point - size * 0.5) - size * 0.5 + vec2(r);
  return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

vec4 gradient_color(vec4 colors[20], int count, float position) {
  if (count <= 1) return colors[0];
  float offset = fract(position) * float(count);
  int first = min(int(floor(offset)), count - 1);
  return mix(colors[first], colors[(first + 1) % count], fract(offset));
}

float rounded_perimeter_position(vec2 point, vec2 size, float radius, float width) {
  const float pi = 3.14159265358979323846;
  float inset = width * 0.5;
  float outer_radius = clamp(radius, width, 0.5 * min(size.x, size.y));
  float center_radius = max(outer_radius - inset, 0.001);
  float horizontal = max(size.x - 2.0 * outer_radius, 0.0);
  float vertical = max(size.y - 2.0 * outer_radius, 0.0);
  float quarter = 0.5 * pi * center_radius;
  float perimeter = 2.0 * horizontal + 2.0 * vertical + 4.0 * quarter;

  float best_distance = 1e20;
  float best_position = 0.0;

  vec2 nearest = vec2(clamp(point.x, outer_radius, size.x - outer_radius), inset);
  float distance = length(point - nearest);
  if (distance < best_distance) {
    best_distance = distance;
    best_position = nearest.x - outer_radius;
  }

  vec2 center = vec2(size.x - outer_radius, outer_radius);
  float angle = clamp(atan(point.y - center.y, point.x - center.x), -0.5 * pi, 0.0);
  nearest = center + vec2(cos(angle), sin(angle)) * center_radius;
  distance = length(point - nearest);
  if (distance < best_distance) {
    best_distance = distance;
    best_position = horizontal + (angle + 0.5 * pi) * center_radius;
  }

  nearest = vec2(size.x - inset, clamp(point.y, outer_radius, size.y - outer_radius));
  distance = length(point - nearest);
  if (distance < best_distance) {
    best_distance = distance;
    best_position = horizontal + quarter + nearest.y - outer_radius;
  }

  center = vec2(size.x - outer_radius, size.y - outer_radius);
  angle = clamp(atan(point.y - center.y, point.x - center.x), 0.0, 0.5 * pi);
  nearest = center + vec2(cos(angle), sin(angle)) * center_radius;
  distance = length(point - nearest);
  if (distance < best_distance) {
    best_distance = distance;
    best_position = horizontal + quarter + vertical + angle * center_radius;
  }

  nearest = vec2(clamp(point.x, outer_radius, size.x - outer_radius), size.y - inset);
  distance = length(point - nearest);
  if (distance < best_distance) {
    best_distance = distance;
    best_position = horizontal + 2.0 * quarter + vertical + size.x - outer_radius - nearest.x;
  }

  center = vec2(outer_radius, size.y - outer_radius);
  angle = clamp(atan(point.y - center.y, point.x - center.x), 0.5 * pi, pi);
  nearest = center + vec2(cos(angle), sin(angle)) * center_radius;
  distance = length(point - nearest);
  if (distance < best_distance) {
    best_distance = distance;
    best_position = 2.0 * horizontal + 2.0 * quarter + vertical +
                    (angle - 0.5 * pi) * center_radius;
  }

  nearest = vec2(inset, clamp(point.y, outer_radius, size.y - outer_radius));
  distance = length(point - nearest);
  if (distance < best_distance) {
    best_distance = distance;
    best_position = 2.0 * horizontal + 3.0 * quarter + vertical +
                    size.y - outer_radius - nearest.y;
  }

  center = vec2(outer_radius);
  angle = atan(point.y - center.y, point.x - center.x);
  if (angle < 0.0) angle += 2.0 * pi;
  angle = clamp(angle, pi, 1.5 * pi);
  nearest = center + vec2(cos(angle), sin(angle)) * center_radius;
  distance = length(point - nearest);
  if (distance < best_distance) {
    best_position = 2.0 * horizontal + 2.0 * vertical + 3.0 * quarter +
                    (angle - pi) * center_radius;
  }
  return best_position / max(perimeter, 0.001);
}

void main() {
  vec2 size = zwwm_draw_rect.zw;
  float width = zwwm_border_width;
  float radius = zwwm_corner_radius;
  float outer = clamp(0.5 - rounded_rect_distance(local_pixel_coordinates, size,
                                                   radius), 0.0, 1.0);
  vec2 inner_size = size - vec2(2.0 * width);
  float inner = inner_size.x <= 0.0 || inner_size.y <= 0.0 ? 0.0 :
      clamp(0.5 - rounded_rect_distance(local_pixel_coordinates - vec2(width),
                                         inner_size,
                                         max(radius - width, 0.0)),
             0.0, 1.0);
  float gradient_position = rounded_perimeter_position(local_pixel_coordinates, size, radius, width);
  vec4 selected = (zwwm_state & 1) != 0
      ? gradient_color(zwwm_config_focused_gradient_colors,
                       zwwm_config_focused_gradient_colors_count, gradient_position)
      : gradient_color(zwwm_config_gradient_colors,
                       zwwm_config_gradient_colors_count, gradient_position);
  vec4 color = vec4(selected.rgb * selected.a, selected.a);
  fragment_color = color * zwwm_opacity * max(outer - inner, 0.0);
}
