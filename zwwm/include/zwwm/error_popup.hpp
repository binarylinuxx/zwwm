#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace zwwm {

struct ErrorPopupImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint32_t> pixels;

  [[nodiscard]] explicit operator bool() const { return width != 0 && height != 0 && !pixels.empty(); }
};

[[nodiscard]] ErrorPopupImage render_error_popup(std::string_view message);
void composite_error_popup(const ErrorPopupImage& popup, std::vector<std::uint32_t>& destination,
                           std::uint32_t destination_width, std::uint32_t destination_height,
                           std::int32_t x, std::int32_t y);

}  // namespace zwwm
