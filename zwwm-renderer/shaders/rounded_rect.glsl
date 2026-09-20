float rounded_rectangle_distance(vec2 point, vec2 size, float radius) {
  vec2 half_size = size * 0.5;
  vec2 corner = abs(point - half_size) - half_size + vec2(radius);
  return length(max(corner, vec2(0.0))) + min(max(corner.x, corner.y), 0.0) - radius;
}

float antialiased_coverage(float distance) {
  float smoothing = max(fwidth(distance), 0.0001);
  return 1.0 - smoothstep(-0.5 * smoothing, 0.5 * smoothing, distance);
}
