#pragma once

#include <cstdint>
#include <vector>

#include "zwwm/output.hpp"

namespace zwwm {

struct OutputCapture {
  OutputId output;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Top-down XRGB8888 pixels in native byte order.
  std::vector<std::uint32_t> pixels;
};

}  // namespace zwwm
