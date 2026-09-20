#include "zwwm/animation.hpp"

#include <array>
#include <cassert>

int main() {
  zwwm::AnimationConfig config;
  config.enabled = true;
  config.duration_ms = 200;
  zwwm::AnimationSystem animations(config);
  const zwwm::AnimationTarget first{1, {{100, 50}, {400, 300}}};
  animations.update(std::span(&first, 1), 1000);
  const auto opened = animations.sample(1, 1000);
  assert(opened && opened->opacity == 0.0F);
  assert(opened->bounds.size.width < first.bounds.size.width);
  assert(animations.active(1100));
  const auto finished = animations.sample(1, 1200);
  assert(finished && finished->opacity == 1.0F);
  assert(finished->bounds.size.width == 400);

  const zwwm::AnimationTarget resized{1, {{0, 0}, {800, 600}}};
  animations.update(std::span(&resized, 1), 1200);
  assert(animations.sample(1, 1200)->bounds.size.width == 400);
  assert(animations.sample(1, 1300)->bounds.size.width > 400);
  assert(animations.sample(1, 1400)->bounds.size.width == 800);

  // The shared curve is monotonic and reaches the endpoint without a clamped
  // overshoot plateau or a final positional jump.
  std::uint32_t previous = 400;
  for (std::uint64_t time = 1200; time <= 1400; time += 10) {
    const auto width = animations.sample(1, time)->bounds.size.width;
    assert(width >= previous && width <= 800);
    previous = width;
  }
  assert(animations.sample(1, 1390)->bounds.size.width >= 798);

  animations.update({}, 1400);
  assert(animations.retains(1));
  assert(animations.sample(1, 1400)->closing);
  assert(animations.sample(1, 1400)->opacity == 1.0F);
  animations.update({}, 1600);
  assert(!animations.retains(1));

  config.enabled = false;
  zwwm::AnimationSystem disabled(config);
  disabled.update(std::span(&first, 1), 0);
  assert(!disabled.active(0));
  assert(disabled.sample(1, 0)->bounds.size.width == 400);
  disabled.update({}, 1);
  assert(!disabled.retains(1));
}
