#version 300 es

layout (location = 0) in vec2 position;

uniform vec4 rect;
uniform vec2 rect_size;
uniform vec4 source_uv;
uniform vec2 draw_origin;
uniform int texture_transform;

out vec2 texture_coordinates;
out vec2 local_pixel_coordinates;
out vec2 target_pixel_coordinates;
flat out vec4 source_uv_value;
flat out int texture_transform_value;

void main() {
  gl_Position = vec4(rect.xy + position * rect.zw, 0.0, 1.0);
  vec2 source_position = position;
  if (texture_transform == 1) source_position = vec2(position.y, 1.0 - position.x);
  else if (texture_transform == 2) source_position = vec2(1.0) - position;
  else if (texture_transform == 3) source_position = vec2(1.0 - position.y, position.x);
  texture_coordinates = mix(source_uv.xy, source_uv.zw, source_position);
  source_uv_value = source_uv;
  texture_transform_value = texture_transform;
  local_pixel_coordinates = position * rect_size;
  target_pixel_coordinates = draw_origin + local_pixel_coordinates;
}
