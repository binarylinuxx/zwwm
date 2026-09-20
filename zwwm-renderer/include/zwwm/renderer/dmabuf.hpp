#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zwwm::renderer {

struct DmabufPlane {
  int fd = -1;
  std::uint32_t stride = 0;
  std::uint32_t offset = 0;
  std::uint64_t modifier = 0;
};

// Importers borrow plane FDs; ownership remains with the containing buffer.
struct DmabufAttributes {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::vector<DmabufPlane> planes;

  [[nodiscard]] bool valid(std::string* error = nullptr) const;
};

// Pixel-plane count, excluding auxiliary compression metadata.
[[nodiscard]] std::size_t expected_plane_count(std::uint32_t format);

}  // namespace zwwm::renderer
