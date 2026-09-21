#pragma once

#include "zwwm/layout/master_stack.hpp"

#include <cstdint>
#include <vector>

namespace zwwm::layout {

struct FibonacciLeaf {
  WindowId id = 0;
  std::vector<bool> path;
};

void reconcile_fibonacci_leaves(std::vector<FibonacciLeaf>& leaves,
                                const std::vector<WindowId>& window_ids,
                                WindowId focused_id);

[[nodiscard]] std::vector<Placement> arrange_focus_fibonacci(
    const std::vector<FibonacciLeaf>& leaves, renderer::Rect work_area,
    std::int32_t outer_gap, std::int32_t inner_gap);

}  // namespace zwwm::layout
