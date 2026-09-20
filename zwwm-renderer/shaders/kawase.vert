#version 300 es

layout (location = 0) in vec2 position;

out vec2 texture_coordinates;

void main() {
  texture_coordinates = position;
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
