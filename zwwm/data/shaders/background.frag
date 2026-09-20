#version 300 es

precision highp float;

uniform vec2 zwwm_output_size;
uniform vec4 zwwm_config_top_color;
uniform vec4 zwwm_config_bottom_color;

out vec4 fragment_color;

void main() {
  vec2 uv = gl_FragCoord.xy / max(zwwm_output_size, vec2(1.0));
  vec3 top = zwwm_config_top_color.rgb;
  vec3 bottom = zwwm_config_bottom_color.rgb;
  fragment_color = vec4(mix(bottom, top, uv.y), 1.0);
}
