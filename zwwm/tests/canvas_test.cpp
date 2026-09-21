#include "zwwm/layout/canvas.hpp"

#include <array>

using zwwm::layout::CanvasBounds;
using zwwm::layout::CanvasRect;
using zwwm::layout::CanvasViewport;
using zwwm::layout::FocusDirection;
using zwwm::layout::FocusPoint;

int main() {
  CanvasViewport viewport{.x = 100.0, .y = 50.0, .scale = 0.5};
  const CanvasBounds bounds{.x = 120, .y = 70, .width = 200, .height = 100, .initialized = true};
  const auto view = zwwm::layout::canvas_view_bounds(bounds, viewport, {10, 20, 800, 600});
  if (view.x != 20 || view.y != 30 || view.width != 100 || view.height != 50) return 1;
  if (zwwm::layout::canvas_world_delta(16, viewport) != 32) return 2;

  CanvasBounds initialized;
  zwwm::layout::initialize_canvas_bounds(initialized, {}, {0, 0, 800, 600}, viewport,
                                         300, 200, 2);
  if (!initialized.initialized || initialized.width != 304 || initialized.height != 204) return 3;

  const std::array neighbors{CanvasBounds{.x = 300, .y = 100, .width = 100,
                                          .height = 100, .initialized = true}};
  const auto snapped = zwwm::layout::snap_canvas_window(
      185, 110, 100, 80, neighbors, 12, CanvasViewport{});
  if (snapped.first != 188 || snapped.second != 110) return 4;

  const std::array points{FocusPoint{-100.0, 0.0}, FocusPoint{80.0, 40.0},
                          FocusPoint{120.0, 0.0}, FocusPoint{0.0, -50.0}};
  const auto right = zwwm::layout::nearest_in_direction({0.0, 0.0}, points,
                                                         FocusDirection::right);
  if (!right || *right != 1) return 5;
  const auto up = zwwm::layout::nearest_in_direction({0.0, 0.0}, points,
                                                      FocusDirection::up);
  if (!up || *up != 3) return 6;
}
