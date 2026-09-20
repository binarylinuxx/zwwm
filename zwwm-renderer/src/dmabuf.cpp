#include "zwwm/renderer/dmabuf.hpp"

#include <libdrm/drm_fourcc.h>

namespace zwwm::renderer {

std::size_t expected_plane_count(std::uint32_t format) {
  switch (format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV24:
    case DRM_FORMAT_NV61:
    case DRM_FORMAT_NV15:
    case DRM_FORMAT_NV20:
    case DRM_FORMAT_NV30:
    case DRM_FORMAT_NV42:
    case DRM_FORMAT_P010:
    case DRM_FORMAT_P012:
    case DRM_FORMAT_P016:
    case DRM_FORMAT_P030:
      return 2;
    case DRM_FORMAT_YUV411:
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YUV444:
    case DRM_FORMAT_YVU411:
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_YVU422:
    case DRM_FORMAT_YVU444:
    case DRM_FORMAT_Y0L0:
    case DRM_FORMAT_Y0L2:
      return 3;
    default:
      return 1;
  }
}

bool DmabufAttributes::valid(std::string* error) const {
  const auto fail = [error](const char* message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (width == 0 || height == 0 || format == 0) {
    return fail("DMA-BUF dimensions and format must be set");
  }
  if (planes.empty() || planes.size() > 4) {
    return fail("DMA-BUF must have between one and four planes");
  }
  const std::uint64_t modifier = planes.front().modifier;
  for (const DmabufPlane& plane : planes) {
    if (plane.fd < 0 || plane.stride == 0 || plane.modifier != modifier) {
      return fail("DMA-BUF planes need valid FDs, strides, and one modifier");
    }
  }
  return true;
}

}  // namespace zwwm::renderer
