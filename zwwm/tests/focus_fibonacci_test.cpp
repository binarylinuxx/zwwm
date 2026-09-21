#include "zwwm/layout/focus_fibonacci.hpp"

#include <vector>

int main() {
  std::vector<zwwm::layout::FibonacciLeaf> leaves;
  zwwm::layout::reconcile_fibonacci_leaves(leaves, {1, 2, 3}, 2);
  if (leaves.size() != 3) return 1;
  if (leaves[0].path != std::vector<bool>{false}) return 2;
  if (leaves[1].path != std::vector<bool>({true, false})) return 3;
  if (leaves[2].path != std::vector<bool>({true, true})) return 4;

  const auto placements = zwwm::layout::arrange_focus_fibonacci(
      leaves, {{0, 0}, {100, 100}}, 0, 0);
  if (placements.size() != 3) return 5;
  if (placements[0].bounds.origin.x != 0 || placements[0].bounds.size.width != 50) return 6;
  if (placements[1].bounds.origin.x != 50 || placements[1].bounds.origin.y != 0 ||
      placements[1].bounds.size.height != 50) return 7;
  if (placements[2].bounds.origin.x != 50 || placements[2].bounds.origin.y != 50) return 8;

  zwwm::layout::reconcile_fibonacci_leaves(leaves, {1, 3}, 1);
  if (leaves.size() != 2 || leaves[0].path != std::vector<bool>{false} ||
      leaves[1].path != std::vector<bool>{true}) return 9;
}
