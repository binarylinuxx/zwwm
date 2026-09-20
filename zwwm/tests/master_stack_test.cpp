#include "zwwm/layout/master_stack.hpp"

#include <cassert>
#include <limits>
#include <vector>

namespace {

using zwwm::layout::MasterStack;
using zwwm::renderer::Rect;

constexpr Rect kWorkArea{{10, 20}, {100, 100}};

void assert_valid(const std::vector<zwwm::layout::Placement>& placements) {
  for (const auto& placement : placements) {
    assert(placement.bounds.size.width > 0);
    assert(placement.bounds.size.height > 0);
  }
}

}  // namespace

int main() {
  MasterStack layout;
  assert(layout.set_outer_gap(0));
  assert(layout.set_inner_gap(0));
  assert(layout.arrange({}, kWorkArea).empty());

  const auto one = layout.arrange({1}, kWorkArea);
  assert(one.size() == 1);
  assert(one[0].id == 1);
  assert(one[0].bounds.origin.x == 10 && one[0].bounds.origin.y == 20);
  assert(one[0].bounds.size.width == 100 && one[0].bounds.size.height == 100);

  const auto two = layout.arrange({1, 2}, kWorkArea);
  assert(two.size() == 2);
  assert(two[0].bounds.size.width == 60 && two[1].bounds.size.width == 40);
  assert(two[0].bounds.size.height == 100 && two[1].bounds.size.height == 100);

  const auto three = layout.arrange({1, 2, 3}, kWorkArea);
  assert(three.size() == 3);
  assert(three[1].bounds.origin.x == 70 && three[2].bounds.origin.x == 70);
  assert(three[1].bounds.size.height == 50 && three[2].bounds.size.height == 50);

  assert(layout.set_master_count(2));
  const auto multiple_masters = layout.arrange({1, 2, 3, 4}, kWorkArea);
  assert(multiple_masters.size() == 4);
  assert(multiple_masters[0].bounds.size.width == 60);
  assert(multiple_masters[0].bounds.size.height == 50);
  assert(multiple_masters[1].bounds.origin.y == 70);
  assert(multiple_masters[2].bounds.size.width == 40);

  MasterStack gapped;
  assert(gapped.set_outer_gap(5));
  assert(gapped.set_inner_gap(4));
  const auto gaps = gapped.arrange({1, 2, 3}, kWorkArea);
  assert(gaps.size() == 3);
  assert(gaps[0].bounds.origin.x == 15 && gaps[0].bounds.origin.y == 25);
  assert(gaps[0].bounds.size.width == 51);
  assert(gaps[1].bounds.origin.x == 70);
  assert(gaps[2].bounds.origin.y ==
         gaps[1].bounds.origin.y + static_cast<std::int32_t>(gaps[1].bounds.size.height) + 4);
  assert_valid(gaps);

  assert(!layout.set_master_count(0));
  assert(layout.master_count() == 2);
  assert(!layout.set_master_ratio(0.0F));
  assert(!layout.set_master_ratio(1.0F));
  assert(!layout.set_master_ratio(std::numeric_limits<float>::quiet_NaN()));
  assert(!layout.set_outer_gap(-1));
  assert(!layout.set_inner_gap(-1));
  assert(layout.arrange({1}, Rect{{0, 0}, {0, 100}}).empty());
  assert(layout.set_master_count(1));
  assert(layout.arrange({1, 2}, Rect{{0, 0}, {1, 100}}).empty());
}
