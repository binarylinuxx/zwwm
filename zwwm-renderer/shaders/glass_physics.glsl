const float GLASS_PI = 3.14159265358979323846;
const int GLASS_MAX_SAMPLES = 16;
const int GLASS_MAX_BOUNCES = 4;

// --- procedural noise ---

vec2 glass_ipart(vec2 x) { return floor(x); }
vec2 glass_fpart(vec2 x) { return fract(x); }

// Integer lattice hash with no trigonometric patterns.
uvec2 glass_hash22(uvec2 p) {
  p = uvec2(p.x * 1597334677U, p.y * 3812015801U);
  p ^= p >> 16U;
  p *= 0x45d9f3bU;
  p ^= p >> 16U;
  p *= 0x45d9f3bU;
  p ^= p >> 16U;
  return p;
}

float glass_value(vec2 p) {
  uvec2 i = uvec2(glass_ipart(p));
  vec2 f = glass_fpart(p);
  vec2 u = f * f * (3.0 - 2.0 * f); // smoothstep 5-tap
  uvec2 h00 = glass_hash22(i);
  uvec2 h10 = glass_hash22(i + uvec2(1U, 0U));
  uvec2 h01 = glass_hash22(i + uvec2(0U, 1U));
  uvec2 h11 = glass_hash22(i + uvec2(1U, 1U));
  float v00 = float((h00.x ^ h00.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v10 = float((h10.x ^ h10.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v01 = float((h01.x ^ h01.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v11 = float((h11.x ^ h11.y) & 0xFFFFU) / 32768.0 - 1.0;
  float a = mix(v00, v10, u.x);
  float b = mix(v01, v11, u.x);
  return mix(a, b, u.y);
}

// Fractional Brownian motion with four octaves.
float glass_fbm(vec2 p) {
  float value = 0.0;
  float amplitude = 0.5;
  float frequency = 1.0;
  for (int i = 0; i < 4; ++i) {
    value += amplitude * glass_value(p * frequency);
    amplitude *= 0.5;
    frequency *= 2.0;
  }
  return value;
}

float glass_grain(vec2 point) {
  float envelope = 0.55 + 0.45 * (glass_value(point * 0.035) * 0.5 + 0.5);
  vec2 rotated = mat2(0.8, -0.6, 0.6, 0.8) * point;
  float detail = glass_value(point * 1.7) * 0.7 +
                 glass_value(rotated * 3.9) * 0.3;
  return clamp(detail, -1.0, 1.0) * envelope * 0.5;
}

// Rapid per-pixel random for microfacet sampling.
float glass_random(inout float state) {
  uvec2 p = uvec2(uint(state * 65537.0) + 12345U);
  uvec2 h = glass_hash22(p);
  state = fract(float(h.x ^ h.y) / 4294967296.0 + state);
  return state;
}

float glass_height(vec2 point) {
  vec2 phase = point * 0.035;
  float waves = sin(phase.x + glass_time) * cos(phase.y - glass_time) * 0.5;
  vec2 grain_uv = point * 0.012 + glass_time * 1.5;
  float grain = glass_fbm(grain_uv) * glass_frost * 6.0;
  return waves * glass_refraction + grain;
}

vec3 glass_geometric_normal(vec2 point) {
  float center = glass_height(point);
  vec2 gradient = vec2(glass_height(point + vec2(1.0, 0.0)) - center,
                       glass_height(point + vec2(0.0, 1.0)) - center);
  vec2 panel_center = rect_size * 0.5;
  vec2 inward = normalize(panel_center - local_pixel_coordinates + vec2(0.0001));
  float edge_distance = min(min(local_pixel_coordinates.x, local_pixel_coordinates.y),
                            min(rect_size.x - local_pixel_coordinates.x, rect_size.y - local_pixel_coordinates.y));
  float meniscus_range = max(8.0, clip_radius + glass_thickness * 0.5);
  float meniscus = pow(1.0 - clamp(edge_distance / meniscus_range, 0.0, 1.0), 2.0);
  vec2 lens_gradient = inward * meniscus * (0.35 + glass_refraction * 0.045);
  return normalize(vec3(-gradient + lens_gradient, -1.0));
}

vec3 glass_microfacet_normal(vec3 normal, inout float random_state) {
  float first = max(glass_random(random_state), 0.00001);
  float second = glass_random(random_state);
  float radius = glass_roughness * sqrt(-2.0 * log(first));
  float angle = 2.0 * GLASS_PI * second;
  vec3 tangent = normalize(abs(normal.z) < 0.999 ? cross(normal, vec3(0.0, 0.0, 1.0)) :
                                                    cross(normal, vec3(0.0, 1.0, 0.0)));
  vec3 bitangent = cross(normal, tangent);
  vec3 perturbed = normalize(normal + tangent * (radius * cos(angle)) + bitangent * (radius * sin(angle)));
  return dot(perturbed, normal) < 0.0 ? -perturbed : perturbed;
}

float glass_fresnel(float cosine, float incident_ior, float transmitted_ior) {
  float f0 = (incident_ior - transmitted_ior) / (incident_ior + transmitted_ior);
  f0 *= f0;
  return f0 + (1.0 - f0) * pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0);
}

vec2 glass_advance(vec2 uv, vec3 direction) {
  return uv + direction.xy / max(abs(direction.z), 0.1) * glass_thickness / target_size;
}

vec3 glass_absorption() {
  return -log(clamp(glass_tint.rgb, vec3(0.02), vec3(0.999))) * glass_tint.a;
}

vec3 glass_reflection_radiance(vec2 uv, vec3 direction) {
  vec2 mirrored = vec2(1.0 - uv.x, uv.y);
  vec2 parallax = direction.xy / max(abs(direction.z), 0.2) * glass_thickness * 0.35 / target_size;
  vec3 scene_reflection = texture(reflection_texture, mirrored + parallax).rgb;
  float overhead = pow(clamp(0.5 - direction.y * 0.5, 0.0, 1.0), 4.0);
  vec3 laboratory_light = vec3(0.75, 0.86, 1.0) * overhead;
  return scene_reflection + laboratory_light * 0.18;
}

float glass_edge_proximity() {
  float distance = -rounded_rectangle_distance(local_pixel_coordinates, rect_size, corner_radius);
  float width = max(8.0, corner_radius * 0.45 + glass_thickness * 0.65);
  return exp(-max(distance, 0.0) / width);
}

vec2 glass_dome_offset() {
  vec2 centered = local_pixel_coordinates / max(rect_size, vec2(1.0)) * 2.0 - 1.0;
  vec2 gradient = -4.0 * centered * (1.0 - centered.yx * centered.yx);
  float interior = 1.0 - glass_edge_proximity();
  float strength = glass_refraction * min(rect_size.x, rect_size.y) * 0.00012;
  return gradient * strength * interior / target_size;
}

vec3 glass_finish(vec3 color, vec2 uv, vec3 incident, vec3 normal) {
  float edge = glass_edge_proximity();
  float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
  float luminance_curve = smoothstep(0.25, 0.58, luminance);

  color *= 1.0 - 0.12 * luminance_curve;
  color += vec3(0.07 * (1.0 - luminance_curve));

  float current_luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
  float chroma = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
  color = mix(vec3(current_luminance), color, 1.0 + chroma * (0.18 + 0.12 * luminance));

  float facing = clamp(dot(-incident, normal), 0.0, 1.0);
  float fresnel = glass_fresnel(facing, 1.0, glass_ior);
  float top_bias = pow(clamp(1.0 - local_pixel_coordinates.y / max(rect_size.y, 1.0), 0.0, 1.0), 2.0);
  float bottom_bias = pow(clamp(local_pixel_coordinates.y / max(rect_size.y, 1.0), 0.0, 1.0), 2.0);
  vec3 reflected = glass_reflection_radiance(uv, reflect(incident, normal));
  color = mix(color, reflected, edge * (0.12 + fresnel * 0.3));
  color += vec3(1.0, 0.985, 0.95) * top_bias * edge * edge * 0.09;
  color *= 1.0 - bottom_bias * edge * edge * 0.075;
  return color;
}

vec3 trace_glass_path(vec2 origin_uv, vec3 incident, vec3 geometric_normal, float ior,
                      inout float random_state) {
  vec3 front_normal = glass_microfacet_normal(geometric_normal, random_state);
  float front_cosine = clamp(dot(-incident, front_normal), 0.0, 1.0);
  float front_reflection = glass_fresnel(front_cosine, 1.0, ior);
  if (glass_random(random_state) < front_reflection) {
    vec3 reflected = reflect(incident, front_normal);
    return glass_reflection_radiance(origin_uv, reflected);
  }

  vec3 direction = refract(incident, front_normal, 1.0 / ior);
  if (dot(direction, direction) < 0.00001) {
    return glass_reflection_radiance(origin_uv, reflect(incident, front_normal));
  }

  vec2 uv = origin_uv;
  vec3 throughput = vec3(1.0);
  vec3 absorption = glass_absorption();
  for (int bounce = 0; bounce < GLASS_MAX_BOUNCES; ++bounce) {
    float distance_inside = glass_thickness / max(abs(direction.z), 0.1);
    throughput *= exp(-absorption * distance_inside / 24.0);
    uv = glass_advance(uv, direction);

    vec3 interface_normal = direction.z >= 0.0 ? geometric_normal : -geometric_normal;
    interface_normal = glass_microfacet_normal(interface_normal, random_state);
    float cosine = clamp(dot(-direction, interface_normal), 0.0, 1.0);
    vec3 transmitted = refract(direction, interface_normal, ior);
    float reflection_probability = dot(transmitted, transmitted) < 0.00001 ? 1.0 :
        glass_fresnel(cosine, ior, 1.0);
    if (glass_random(random_state) >= reflection_probability && reflection_probability < 1.0) {
      return texture(background_texture, glass_advance(uv, transmitted * 0.2)).rgb * throughput;
    }
    direction = reflect(direction, interface_normal);
  }
  return texture(background_texture, glass_advance(uv, direction * 0.2)).rgb * throughput;
}

vec4 physically_based_glass(vec2 uv) {
  vec2 panel_center = clip_rect.xy + clip_rect.zw * 0.5;
  vec2 view_offset = (target_pixel_coordinates - panel_center) / max(target_size.y, 1.0);
  vec3 incident = normalize(vec3(view_offset, 1.5));
  vec3 geometric_normal = glass_geometric_normal(target_pixel_coordinates);
  vec2 optical_uv = uv + glass_dome_offset();
  int sample_count = glass_quality <= 1 ? 4 : glass_quality == 2 ? 8 : 16;
  float dispersion = glass_dispersion * 0.001;
  vec3 integrated = vec3(0.0);

  for (int sample_index = 0; sample_index < GLASS_MAX_SAMPLES; ++sample_index) {
    if (sample_index >= sample_count) break;
    uvec2 seed_lattice = uvec2(mod(target_pixel_coordinates + vec2(float(sample_index) * 17.0,
                                      float(sample_index) * 47.0) + floor(glass_time * 59.0), 1048576.0));
    uvec2 seed_hash = glass_hash22(seed_lattice);
    float seed = fract(float(seed_hash.x ^ seed_hash.y) / 4294967296.0);
    if (glass_quality <= 1 || dispersion <= 0.0) {
      integrated += trace_glass_path(optical_uv, incident, geometric_normal, glass_ior, seed);
    } else {
      float red_seed = seed;
      float green_seed = fract(seed + 0.3333333);
      float blue_seed = fract(seed + 0.6666667);
      integrated.r += trace_glass_path(optical_uv, incident, geometric_normal, max(1.0001, glass_ior - dispersion), red_seed).r;
      integrated.g += trace_glass_path(optical_uv, incident, geometric_normal, glass_ior, green_seed).g;
      integrated.b += trace_glass_path(optical_uv, incident, geometric_normal, glass_ior + dispersion, blue_seed).b;
    }
  }

  integrated /= float(sample_count);
  float frost = glass_grain(target_pixel_coordinates + vec2(0.0, glass_time * 12.0)) * glass_frost;
  integrated = glass_finish(integrated, optical_uv, incident, geometric_normal);
  return vec4(clamp(integrated + frost, 0.0, 1.0), 1.0);
}
