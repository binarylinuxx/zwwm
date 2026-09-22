#version 300 es

precision highp float;

uniform vec2 zwwm_output_size;
uniform sampler2D zwwm_background_image;

out vec4 fragment_color;

void main() {
  vec2 uv = gl_FragCoord.xy / max(zwwm_output_size, vec2(1.0));
  float output_aspect = zwwm_output_size.x / max(zwwm_output_size.y, 1.0);
  vec2 image_size = vec2(textureSize(zwwm_background_image, 0));
  float image_aspect = image_size.x / max(image_size.y, 1.0);
  if (output_aspect > image_aspect)
    uv.y = 0.5 + (uv.y - 0.5) * image_aspect / output_aspect;
  else
    uv.x = 0.5 + (uv.x - 0.5) * output_aspect / image_aspect;
  fragment_color = texture(zwwm_background_image, vec2(uv.x, 1.0 - uv.y));
}
