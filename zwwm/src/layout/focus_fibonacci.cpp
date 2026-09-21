#include "zwwm/layout/focus_fibonacci.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace zwwm::layout {

void reconcile_fibonacci_leaves(std::vector<FibonacciLeaf>& leaves,
                                const std::vector<WindowId>& window_ids,
                                WindowId focused_id) {
  std::erase_if(leaves, [&window_ids](const FibonacciLeaf& leaf) {
    return std::find(window_ids.begin(), window_ids.end(), leaf.id) == window_ids.end();
  });

  while (!leaves.empty()) {
    bool collapsed = false;
    for (const auto& candidate : leaves) {
      for (std::size_t depth = candidate.path.size(); depth > 0 && !collapsed; --depth) {
        const std::vector<bool> parent(candidate.path.begin(),
                                       candidate.path.begin() + static_cast<std::ptrdiff_t>(depth - 1));
        bool first = false;
        bool second = false;
        for (const auto& leaf : leaves) {
          if (leaf.path.size() < depth || !std::equal(parent.begin(), parent.end(), leaf.path.begin())) continue;
          if (leaf.path[depth - 1]) second = true;
          else first = true;
        }
        if (first == second) continue;
        for (auto& leaf : leaves) {
          if (leaf.path.size() >= depth && std::equal(parent.begin(), parent.end(), leaf.path.begin()))
            leaf.path.erase(leaf.path.begin() + static_cast<std::ptrdiff_t>(depth - 1));
        }
        collapsed = true;
      }
      if (collapsed) break;
    }
    if (!collapsed) break;
  }

  for (const auto id : window_ids) {
    if (std::any_of(leaves.begin(), leaves.end(), [id](const auto& leaf) { return leaf.id == id; })) continue;
    if (leaves.empty()) {
      leaves.push_back({id, {}});
      continue;
    }
    auto target = std::find_if(leaves.begin(), leaves.end(), [focused_id](const auto& leaf) {
      return leaf.id == focused_id;
    });
    if (target == leaves.end()) target = std::prev(leaves.end());
    auto new_path = target->path;
    target->path.push_back(false);
    new_path.push_back(true);
    leaves.push_back({id, std::move(new_path)});
  }
}

std::vector<Placement> arrange_focus_fibonacci(const std::vector<FibonacciLeaf>& leaves,
                                                renderer::Rect work_area,
                                                std::int32_t outer_gap,
                                                std::int32_t inner_gap) {
  const std::int64_t left = static_cast<std::int64_t>(work_area.origin.x) + outer_gap;
  const std::int64_t top = static_cast<std::int64_t>(work_area.origin.y) + outer_gap;
  const std::int64_t width = std::max<std::int64_t>(
      1, static_cast<std::int64_t>(work_area.size.width) - 2LL * outer_gap);
  const std::int64_t height = std::max<std::int64_t>(
      1, static_cast<std::int64_t>(work_area.size.height) - 2LL * outer_gap);
  std::vector<Placement> placements;
  placements.reserve(leaves.size());
  for (const auto& leaf : leaves) {
    std::int64_t x = left;
    std::int64_t y = top;
    std::int64_t w = width;
    std::int64_t h = height;
    for (std::size_t depth = 0; depth < leaf.path.size(); ++depth) {
      const bool second = leaf.path[depth];
      if (depth % 2 == 0) {
        const auto gap = std::min<std::int64_t>(inner_gap, std::max<std::int64_t>(0, w - 2));
        const auto first = (w - gap) / 2;
        if (second) {
          x += first + gap;
          w -= first + gap;
        } else {
          w = first;
        }
      } else {
        const auto gap = std::min<std::int64_t>(inner_gap, std::max<std::int64_t>(0, h - 2));
        const auto first = (h - gap) / 2;
        if (second) {
          y += first + gap;
          h -= first + gap;
        } else {
          h = first;
        }
      }
    }
    placements.push_back({leaf.id,
                          {{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                           {static_cast<std::uint32_t>(std::max<std::int64_t>(1, w)),
                            static_cast<std::uint32_t>(std::max<std::int64_t>(1, h))}}});
  }
  return placements;
}

}  // namespace zwwm::layout
