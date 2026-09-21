#include "zwwm/layout/master_stack.hpp"

#include <algorithm>
#include <limits>
#include <optional>

namespace zwwm::layout {
namespace {

using renderer::Rect;

[[nodiscard]] bool fits_coordinate(std::int64_t coordinate) {
  return coordinate >= std::numeric_limits<std::int32_t>::min() &&
         coordinate <= std::numeric_limits<std::int32_t>::max();
}

[[nodiscard]] std::optional<Rect> make_rect(std::int64_t x, std::int64_t y,
                                             std::uint64_t width, std::uint64_t height) {
  if (!fits_coordinate(x) || !fits_coordinate(y) || width == 0 || height == 0 ||
      width > std::numeric_limits<std::uint32_t>::max() ||
      height > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return Rect{{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
              {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}};
}

[[nodiscard]] std::optional<Rect> inset(Rect area, std::uint32_t gap) {
  const auto width = static_cast<std::uint64_t>(area.size.width);
  const auto height = static_cast<std::uint64_t>(area.size.height);
  const auto inset_gap = static_cast<std::uint64_t>(gap);
  if (width == 0 || height == 0 || inset_gap > (width - 1) / 2 ||
      inset_gap > (height - 1) / 2) {
    return std::nullopt;
  }
  return make_rect(static_cast<std::int64_t>(area.origin.x) + inset_gap,
                   static_cast<std::int64_t>(area.origin.y) + inset_gap,
                   width - 2 * inset_gap, height - 2 * inset_gap);
}

[[nodiscard]] std::optional<std::vector<Rect>> tile_vertically(Rect area, std::uint64_t count,
                                                                 std::uint32_t inner_gap,
                                                                 const std::vector<float>& weights = {}) {
  if (count == 0) {
    return std::vector<Rect>{};
  }

  const auto height = static_cast<std::uint64_t>(area.size.height);
  if (area.size.width == 0 || height < count) {
    return std::nullopt;
  }

  const auto gaps = count - 1;
  const auto gap = gaps == 0 ? 0ULL : std::min<std::uint64_t>(inner_gap, (height - count) / gaps);
  const auto content_height = height - gap * gaps;
  double weight_total = 0.0;
  for (std::uint64_t index = 0; index < count; ++index)
    weight_total += index < weights.size() ? std::max(0.01F, weights[index]) : 1.0F;

  std::vector<Rect> result;
  result.reserve(static_cast<std::size_t>(count));
  std::int64_t y = area.origin.y;
  for (std::uint64_t index = 0; index < count; ++index) {
    const auto weight = index < weights.size() ? std::max(0.01F, weights[index]) : 1.0F;
    const auto used = static_cast<std::uint64_t>(std::max<std::int64_t>(0, y - area.origin.y)) - gap * index;
    const auto remaining = content_height - used;
    const auto tile_height = index + 1 == count ? remaining :
        std::clamp<std::uint64_t>(static_cast<std::uint64_t>(remaining * weight / weight_total), 1,
                                  content_height - used - (count - index - 1));
    weight_total -= weight;
    const auto tile = make_rect(area.origin.x, y, area.size.width, tile_height);
    if (!tile) {
      return std::nullopt;
    }
    result.push_back(*tile);
    y += static_cast<std::int64_t>(tile_height + gap);
  }
  return result;
}

}  // namespace

bool MasterStack::set_master_count(std::uint32_t master_count) {
  if (master_count == 0) {
    return false;
  }
  master_count_ = master_count;
  return true;
}

bool MasterStack::set_master_ratio(float master_ratio) {
  if (!(master_ratio >= kMinimumMasterRatio && master_ratio <= kMaximumMasterRatio)) {
    return false;
  }
  master_ratio_ = master_ratio;
  return true;
}

bool MasterStack::set_outer_gap(std::int32_t outer_gap) {
  if (outer_gap < 0) {
    return false;
  }
  outer_gap_ = static_cast<std::uint32_t>(outer_gap);
  return true;
}

bool MasterStack::set_inner_gap(std::int32_t inner_gap) {
  if (inner_gap < 0) {
    return false;
  }
  inner_gap_ = static_cast<std::uint32_t>(inner_gap);
  return true;
}

std::uint32_t MasterStack::master_count() const { return master_count_; }

float MasterStack::master_ratio() const { return master_ratio_; }

std::uint32_t MasterStack::outer_gap() const { return outer_gap_; }

std::uint32_t MasterStack::inner_gap() const { return inner_gap_; }

std::vector<Placement> MasterStack::arrange(const std::vector<WindowId>& window_ids,
                                             Rect work_area, const std::vector<float>& weights) const {
  if (window_ids.empty()) {
    return {};
  }

  const auto usable_area = inset(work_area, outer_gap_);
  if (!usable_area) {
    return {};
  }

  const auto window_count = static_cast<std::uint64_t>(window_ids.size());
  const auto masters = std::min<std::uint64_t>(master_count_, window_count);
  std::optional<std::vector<Rect>> master_tiles;
  std::optional<std::vector<Rect>> stack_tiles;

  if (window_count <= master_count_) {
    master_tiles = tile_vertically(*usable_area, masters, inner_gap_, weights);
  } else {
    const auto width = static_cast<std::uint64_t>(usable_area->size.width);
    if (width < 2) {
      return {};
    }
    const auto column_gap = std::min<std::uint64_t>(inner_gap_, width - 2);
    const auto distributable_width = width - column_gap;
    auto master_width = static_cast<std::uint64_t>(distributable_width * master_ratio_);
    master_width = std::clamp<std::uint64_t>(master_width, 1, distributable_width - 1);
    const auto stack_width = distributable_width - master_width;

    const auto master_area = make_rect(usable_area->origin.x, usable_area->origin.y, master_width,
                                       usable_area->size.height);
    const auto stack_area = make_rect(static_cast<std::int64_t>(usable_area->origin.x) +
                                          master_width + column_gap,
                                      usable_area->origin.y, stack_width, usable_area->size.height);
    if (!master_area || !stack_area) {
      return {};
    }
    master_tiles = tile_vertically(*master_area, masters, inner_gap_, weights);
    const auto stack_begin = std::min<std::size_t>(masters, weights.size());
    const std::vector<float> stack_weights(weights.begin() + static_cast<std::ptrdiff_t>(stack_begin), weights.end());
    stack_tiles = tile_vertically(*stack_area, window_count - masters, inner_gap_,
                                  stack_weights);
  }

  if (!master_tiles || (!stack_tiles && window_count > master_count_)) {
    return {};
  }

  std::vector<Placement> placements;
  placements.reserve(window_ids.size());
  for (std::size_t index = 0; index < master_tiles->size(); ++index) {
    placements.push_back({window_ids[index], (*master_tiles)[index]});
  }
  if (stack_tiles) {
    for (std::size_t index = 0; index < stack_tiles->size(); ++index) {
      placements.push_back({window_ids[masters + index], (*stack_tiles)[index]});
    }
  }
  return placements;
}

}  // namespace zwwm::layout
