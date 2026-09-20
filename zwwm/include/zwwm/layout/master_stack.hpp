#pragma once

#include "zwwm/renderer/scene.hpp"

#include <cstdint>
#include <vector>

namespace zwwm::layout {

using WindowId = std::uint64_t;

struct Placement {
  WindowId id = 0;
  renderer::Rect bounds;
};

class MasterStack {
 public:
  static constexpr float kMinimumMasterRatio = 0.1F;
  static constexpr float kMaximumMasterRatio = 0.9F;

  [[nodiscard]] bool set_master_count(std::uint32_t master_count);
  [[nodiscard]] bool set_master_ratio(float master_ratio);
  [[nodiscard]] bool set_outer_gap(std::int32_t outer_gap);
  [[nodiscard]] bool set_inner_gap(std::int32_t inner_gap);

  [[nodiscard]] std::uint32_t master_count() const;
  [[nodiscard]] float master_ratio() const;
  [[nodiscard]] std::uint32_t outer_gap() const;
  [[nodiscard]] std::uint32_t inner_gap() const;

  [[nodiscard]] std::vector<Placement> arrange(
      const std::vector<WindowId>& window_ids, renderer::Rect work_area,
      const std::vector<float>& weights = {}) const;

 private:
  std::uint32_t master_count_ = 1;
  float master_ratio_ = 0.6F;
  std::uint32_t outer_gap_ = 0;
  std::uint32_t inner_gap_ = 0;
};

}  // namespace zwwm::layout
