#pragma once

#include "zwwm/renderer/scene.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace zwwm {

struct AnimationConfig {
  bool enabled = false;
  std::uint32_t duration_ms = 320;
  std::uint32_t tag_duration_ms = 360;
  std::uint32_t spring_stiffness = 200;
  std::uint32_t spring_damping = 18;
  std::uint32_t spring_mass_per_mille = 1000;
  std::uint32_t bezier_x1_per_mille = 220;
  std::uint32_t bezier_y1_per_mille = 0;
  std::uint32_t bezier_x2_per_mille = 300;
  std::uint32_t bezier_y2_per_mille = 1000;
  bool open_window = true;
  bool resize = true;
  bool close = true;
  std::uint32_t open_scale_per_mille = 900;
  std::uint32_t close_scale_per_mille = 800;
  std::uint32_t open_offset_px = 0;
  std::uint32_t close_offset_px = 0;
  std::uint32_t tag_scale_per_mille = 1000;
  std::uint32_t tag_parallax_per_mille = 1000;
  std::uint32_t tag_fade_per_mille = 0;
};

struct AnimationTarget {
  std::uint64_t id = 0;
  renderer::Rect bounds;
  // False snaps bounds changes.
  bool animate = true;
  // False skips presence transitions.
  bool animate_presence = true;
  // Track continuously moving geometry.
  bool track = false;
  // Camera-driven geometry follows one shared viewport, never per-window easing.
  bool camera_motion = false;
};

struct AnimationSample {
  renderer::Rect bounds;
  float opacity = 1.0F;
  bool closing = false;
};
struct TagTransitionSample {
  int direction = 0;
  float progress = 1.0F;
  std::uint32_t scale_per_mille = 1000;
  std::uint32_t parallax_per_mille = 1000;
  std::uint32_t fade_per_mille = 0;
};

struct TagRootSample {
  renderer::Rect bounds;
  float opacity = 1.0F;
};

// Applies output-wide tag motion to one toplevel root.
[[nodiscard]] TagRootSample transform_tag_root(renderer::Rect bounds, std::uint32_t output_width,
                                               const TagTransitionSample& transition, bool outgoing);

class AnimationSystem {
 public:
  explicit AnimationSystem(AnimationConfig config = {});

  void set_config(AnimationConfig config, std::uint64_t now_ms);
  void update(std::span<const AnimationTarget> targets, std::uint64_t now_ms);
  [[nodiscard]] std::optional<AnimationSample> sample(std::uint64_t id, std::uint64_t now_ms) const;
  [[nodiscard]] bool active(std::uint64_t now_ms) const;
  [[nodiscard]] bool retains(std::uint64_t id) const;
  [[nodiscard]] std::vector<std::uint64_t> retained_ids() const;
  void start_tag_transition(int direction, std::uint64_t now_ms);
  [[nodiscard]] std::optional<TagTransitionSample> tag_transition(std::uint64_t now_ms) const;
  void remove(std::uint64_t id);

 private:
  enum class Transition : std::uint8_t { idle, opening, resizing, tracking, closing };
  struct State {
    renderer::Rect from;
    renderer::Rect target;
    float from_opacity = 1.0F;
    float target_opacity = 1.0F;
    std::uint64_t started_ms = 0;
    Transition transition = Transition::idle;
    bool present = true;
  };

  [[nodiscard]] float progress(const State& state, std::uint64_t now_ms) const;
  [[nodiscard]] float progress(std::uint64_t started_ms, std::uint32_t duration_ms,
                               std::uint64_t now_ms) const;
  [[nodiscard]] AnimationSample sample_state(const State& state, std::uint64_t now_ms) const;
  void prune(std::uint64_t now_ms);

  AnimationConfig config_;
  std::unordered_map<std::uint64_t, State> states_;
  std::optional<std::pair<int, std::uint64_t>> tag_transition_;
};

}  // namespace zwwm
