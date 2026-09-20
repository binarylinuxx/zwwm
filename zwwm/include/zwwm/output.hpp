#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

#include "zwwm/runtime_config.hpp"

namespace zwwm {

struct OutputId {
  std::uint64_t value = 0;
  constexpr explicit operator bool() const { return value != 0; }
  friend constexpr bool operator==(OutputId, OutputId) = default;
  friend constexpr auto operator<=>(OutputId, OutputId) = default;
};

struct OutputInfo {
  struct Mode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_millihz = 0;
    bool preferred = false;
  };

  OutputId id;
  std::string connector;
  std::int32_t logical_x = 0;
  std::int32_t logical_y = 0;
  std::uint32_t logical_width = 0;
  std::uint32_t logical_height = 0;
  std::uint32_t physical_width = 0;
  std::uint32_t physical_height = 0;
  std::uint32_t refresh_millihz = 60000;
  std::uint32_t scale_per_mille = 1000;
  OutputTransform transform = OutputTransform::normal;
  bool enabled = true;
  std::uint32_t bit_depth = 8;
  std::vector<Mode> modes;
};

}  // namespace zwwm
