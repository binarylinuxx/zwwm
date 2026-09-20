#include "zwwm/renderer/egl_dmabuf_importer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <libdrm/drm_fourcc.h>
#include <optional>
#include <string_view>
#include <vector>

namespace zwwm::renderer {
namespace {

constexpr std::array<EGLint, 4> kPlaneFd{
    EGL_DMA_BUF_PLANE0_FD_EXT,
    EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT,
    EGL_DMA_BUF_PLANE3_FD_EXT,
};
constexpr std::array<EGLint, 4> kPlaneOffset{
    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
    EGL_DMA_BUF_PLANE1_OFFSET_EXT,
    EGL_DMA_BUF_PLANE2_OFFSET_EXT,
    EGL_DMA_BUF_PLANE3_OFFSET_EXT,
};
constexpr std::array<EGLint, 4> kPlanePitch{
    EGL_DMA_BUF_PLANE0_PITCH_EXT,
    EGL_DMA_BUF_PLANE1_PITCH_EXT,
    EGL_DMA_BUF_PLANE2_PITCH_EXT,
    EGL_DMA_BUF_PLANE3_PITCH_EXT,
};
constexpr std::array<EGLint, 4> kPlaneModifierLo{
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
};
constexpr std::array<EGLint, 4> kPlaneModifierHi{
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
};

bool is_amd_dcc(std::uint64_t modifier) {
  // AMD DCC adds a compression plane that is not sampleable as GL_TEXTURE_2D.
  return (modifier >> 56U) == 2U && ((modifier >> 13U) & 0x1U) != 0U;
}

}  // namespace

EglDmabufImporter::EglDmabufImporter(EGLDisplay display) : display_(display) {}

bool EglDmabufImporter::initialize() {
  if (display_ == EGL_NO_DISPLAY) {
    last_error_ = "EGL display is unavailable";
    return false;
  }
  if (!has_extension("EGL_EXT_image_dma_buf_import") || !has_extension("EGL_EXT_image_dma_buf_import_modifiers")) {
    last_error_ = "EGL DMA-BUF import extensions are unavailable";
    return false;
  }
  if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
    last_error_ = "OpenGL ES EGLImage texture import is unavailable";
    return false;
  }
  create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  image_target_texture_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  query_formats_ = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
  query_modifiers_ = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
  if (create_image_ == nullptr || destroy_image_ == nullptr || image_target_texture_ == nullptr) {
    last_error_ = "EGL DMA-BUF import entrypoints are unavailable";
    return false;
  }
  last_error_.clear();
  return true;
}

bool EglDmabufImporter::supported() const {
  return create_image_ != nullptr && destroy_image_ != nullptr && image_target_texture_ != nullptr;
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> EglDmabufImporter::supported_formats() const {
  std::vector<std::pair<std::uint32_t, std::uint64_t>> result;
  if (!supported() || query_formats_ == nullptr || query_modifiers_ == nullptr) return result;
  EGLint count = 0;
  if (query_formats_(display_, 0, nullptr, &count) != EGL_TRUE || count <= 0) return result;
  std::vector<EGLint> formats(static_cast<std::size_t>(count));
  if (query_formats_(display_, count, formats.data(), &count) != EGL_TRUE) return result;
  for (EGLint format : formats) {
    EGLint modifiers = 0;
    if (query_modifiers_(display_, format, 0, nullptr, nullptr, &modifiers) != EGL_TRUE || modifiers < 0) continue;
    if (modifiers == 0) {
      result.emplace_back(static_cast<std::uint32_t>(format), DRM_FORMAT_MOD_INVALID);
      continue;
    }
    std::vector<EGLuint64KHR> values(static_cast<std::size_t>(modifiers));
    std::vector<EGLBoolean> external_only(static_cast<std::size_t>(modifiers));
    if (query_modifiers_(display_, format, modifiers, values.data(), external_only.data(), &modifiers) != EGL_TRUE) continue;
    for (EGLint i = 0; i < modifiers; ++i) {
      if (external_only[static_cast<std::size_t>(i)] == EGL_FALSE &&
          !is_amd_dcc(values[static_cast<std::size_t>(i)])) {
        result.emplace_back(static_cast<std::uint32_t>(format), values[static_cast<std::size_t>(i)]);
      }
    }
  }
  return result;
}

GLuint EglDmabufImporter::import(const DmabufAttributes& attributes) {
  if (!supported()) {
    last_error_ = "EGL DMA-BUF importer is not initialized";
    return 0;
  }
  // Strip auxiliary compression planes from packed formats before import.
  const std::size_t expected = expected_plane_count(attributes.format);
  std::optional<DmabufAttributes> stripped;
  if (expected > 0 && attributes.planes.size() > expected) {
    stripped = attributes;
    stripped->planes.resize(expected);
  }

  const auto attempt = [this](const DmabufAttributes& candidate) -> GLuint {
    std::string validation_error;
    if (!candidate.valid(&validation_error)) {
      last_error_ = std::move(validation_error);
      return 0;
    }
    std::array<EGLint, 6 + (4 * 10) + 1> egl_attributes{};
    std::size_t index = 0;
    const auto append = [&egl_attributes, &index](EGLint key, EGLint value) {
      egl_attributes[index++] = key;
      egl_attributes[index++] = value;
    };
    append(EGL_WIDTH, static_cast<EGLint>(candidate.width));
    append(EGL_HEIGHT, static_cast<EGLint>(candidate.height));
    append(EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(candidate.format));
    for (std::size_t plane = 0; plane < candidate.planes.size(); ++plane) {
      const DmabufPlane& value = candidate.planes[plane];
      append(kPlaneFd[plane], value.fd);
      append(kPlaneOffset[plane], static_cast<EGLint>(value.offset));
      append(kPlanePitch[plane], static_cast<EGLint>(value.stride));
      if (value.modifier != DRM_FORMAT_MOD_INVALID) {
        append(kPlaneModifierLo[plane], static_cast<EGLint>(value.modifier & 0xffffffffU));
        append(kPlaneModifierHi[plane], static_cast<EGLint>(value.modifier >> 32U));
      }
    }
    append(EGL_IMAGE_PRESERVED_KHR, EGL_TRUE);
    egl_attributes[index] = EGL_NONE;

    EGLImageKHR image = create_image_(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, egl_attributes.data());
    if (image == EGL_NO_IMAGE_KHR) {
      set_egl_error("eglCreateImageKHR");
      return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
      destroy_image_(display_, image);
      last_error_ = "could not allocate an OpenGL texture for DMA-BUF";
      return 0;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_texture_(GL_TEXTURE_2D, image);
    const GLenum target_error = glGetError();
    if (target_error != GL_NO_ERROR) {
      glDeleteTextures(1, &texture);
      destroy_image_(display_, image);
      char code[32]{};
      std::snprintf(code, sizeof(code), "0x%x", static_cast<unsigned>(target_error));
      last_error_ = "could not bind EGLImage to an OpenGL texture (glGetError ";
      last_error_ += code;
      last_error_ += ")";
      return 0;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    images_.emplace(texture, image);
    last_error_.clear();
    return texture;
  };

  if (stripped) {
    if (GLuint texture = attempt(*stripped); texture != 0) return texture;
    if (attributes.planes.size() == expected) return 0;
    last_error_ = "full-plane import (" + last_error_ + ")";
  }
  if (GLuint texture = attempt(attributes); texture != 0) return texture;
  const bool fallback_needed = std::any_of(attributes.planes.begin(), attributes.planes.end(),
                                           [](const DmabufPlane& plane) { return plane.modifier != DRM_FORMAT_MOD_INVALID; });
  if (!fallback_needed) {
    last_error_ = "full-plane import (" + last_error_ + ")";
    return 0;
  }
  // Retry advertised but unusable modifiers as linear.
  DmabufAttributes linear = attributes;
  for (auto& plane : linear.planes) plane.modifier = DRM_FORMAT_MOD_INVALID;
  const auto kept = std::move(last_error_);
  if (GLuint texture = attempt(linear); texture != 0) return texture;
  last_error_ = kept + "; linear fallback failed (" + last_error_ + ")";
  return 0;
}

bool EglDmabufImporter::release(GLuint texture) {
  const auto image = images_.find(texture);
  if (image == images_.end()) {
    return false;
  }
  glDeleteTextures(1, &texture);
  const EGLBoolean destroyed = destroy_image_(display_, image->second);
  images_.erase(image);
  if (destroyed != EGL_TRUE) {
    set_egl_error("eglDestroyImageKHR");
    return false;
  }
  return true;
}

void EglDmabufImporter::shutdown() {
  while (!images_.empty()) {
    (void)release(images_.begin()->first);
  }
}

const std::string& EglDmabufImporter::last_error() const { return last_error_; }

bool EglDmabufImporter::has_extension(const char* name) const {
  const char* extensions = eglQueryString(display_, EGL_EXTENSIONS);
  if (extensions == nullptr) {
    return false;
  }
  const std::string_view required(name);
  std::string_view remaining(extensions);
  while (!remaining.empty()) {
    const std::size_t separator = remaining.find(' ');
    const std::string_view extension = remaining.substr(0, separator);
    if (extension == required) {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(separator + 1);
  }
  return false;
}

void EglDmabufImporter::set_egl_error(const char* operation) {
  const EGLint error = eglGetError();
  last_error_ = std::string(operation) + " failed with EGL error 0x";
  char code[16]{};
  std::snprintf(code, sizeof(code), "%x", error);
  last_error_ += code;
}

}  // namespace zwwm::renderer
