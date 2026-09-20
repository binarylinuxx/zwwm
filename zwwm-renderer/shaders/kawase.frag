#version 300 es

precision highp float;

in vec2 texture_coordinates;

uniform sampler2D source_texture;
uniform vec2 texel_size;
uniform float sample_offset;
uniform bool upsample;
uniform bool finalize;
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float noise;

out vec4 fragment_color;

uvec2 grain_hash22(uvec2 p) {
  p = uvec2(p.x * 1597334677U, p.y * 3812015801U);
  p ^= p >> 16U;
  p *= 0x45d9f3bU;
  p ^= p >> 16U;
  p *= 0x45d9f3bU;
  p ^= p >> 16U;
  return p;
}

float grain_value(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  uvec2 h00 = grain_hash22(uvec2(i));
  uvec2 h10 = grain_hash22(uvec2(i) + uvec2(1U, 0U));
  uvec2 h01 = grain_hash22(uvec2(i) + uvec2(0U, 1U));
  uvec2 h11 = grain_hash22(uvec2(i) + uvec2(1U, 1U));
  float v00 = float((h00.x ^ h00.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v10 = float((h10.x ^ h10.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v01 = float((h01.x ^ h01.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v11 = float((h11.x ^ h11.y) & 0xFFFFU) / 32768.0 - 1.0;
  return mix(mix(v00, v10, u.x), mix(v01, v11, u.x), u.y);
}

// Coarse noise varies the grain density without becoming a color offset.
float grain_noise(vec2 point) {
  float envelope = 0.55 + 0.45 * (grain_value(point * 0.035) * 0.5 + 0.5);
  vec2 rotated = mat2(0.8, -0.6, 0.6, 0.8) * point;
  float detail = grain_value(point * 1.7) * 0.7 +
                 grain_value(rotated * 3.9) * 0.3;
  return clamp(detail, -1.0, 1.0) * envelope * 0.5;
}

void main() {
  vec2 step_size = texel_size * sample_offset;
  if (!upsample) {
    fragment_color = texture(source_texture, texture_coordinates) * 4.0;
    fragment_color += texture(source_texture, texture_coordinates + vec2(step_size.x, step_size.y));
    fragment_color += texture(source_texture, texture_coordinates + vec2(-step_size.x, step_size.y));
    fragment_color += texture(source_texture, texture_coordinates + vec2(step_size.x, -step_size.y));
    fragment_color += texture(source_texture, texture_coordinates - step_size);
    fragment_color /= 8.0;
  } else {
    fragment_color = texture(source_texture, texture_coordinates + vec2(step_size.x, 0.0)) * 2.0;
    fragment_color += texture(source_texture, texture_coordinates - vec2(step_size.x, 0.0)) * 2.0;
    fragment_color += texture(source_texture, texture_coordinates + vec2(0.0, step_size.y)) * 2.0;
    fragment_color += texture(source_texture, texture_coordinates - vec2(0.0, step_size.y)) * 2.0;
    fragment_color += texture(source_texture, texture_coordinates + step_size);
    fragment_color += texture(source_texture, texture_coordinates - step_size);
    fragment_color += texture(source_texture, texture_coordinates + vec2(step_size.x, -step_size.y));
    fragment_color += texture(source_texture, texture_coordinates + vec2(-step_size.x, step_size.y));
    fragment_color /= 12.0;
  }
  if (finalize) {
    float luminance = dot(fragment_color.rgb, vec3(0.2126, 0.7152, 0.0722));
    fragment_color.rgb = mix(vec3(luminance), fragment_color.rgb, saturation);
    fragment_color.rgb = (fragment_color.rgb - vec3(0.5)) * contrast + vec3(0.5);
    fragment_color.rgb *= brightness;
    fragment_color.rgb += vec3(grain_noise(gl_FragCoord.xy) * noise);
    fragment_color.rgb = clamp(fragment_color.rgb, 0.0, 1.0);
  }
}
