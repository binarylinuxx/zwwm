#pragma once

#include "zwwm/renderer/dmabuf.hpp"

#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace zwwm::renderer {

// Imports borrowed DMA-BUF planes into GL_TEXTURE_2D on one EGL context.
class EglDmabufImporter {
 public:
  explicit EglDmabufImporter(EGLDisplay display);
  EglDmabufImporter(const EglDmabufImporter&) = delete;
  EglDmabufImporter& operator=(const EglDmabufImporter&) = delete;

  [[nodiscard]] bool initialize();
  [[nodiscard]] bool supported() const;
  // Exact import set for linux-dmabuf advertisement.
  [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint64_t>> supported_formats() const;
  [[nodiscard]] GLuint import(const DmabufAttributes& attributes);
  [[nodiscard]] bool release(GLuint texture);
  void shutdown();
  [[nodiscard]] const std::string& last_error() const;

 private:
  [[nodiscard]] bool has_extension(const char* name) const;
  void set_egl_error(const char* operation);

  EGLDisplay display_ = EGL_NO_DISPLAY;
  PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_ = nullptr;
  PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats_ = nullptr;
  PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers_ = nullptr;
  std::unordered_map<GLuint, EGLImageKHR> images_;
  std::string last_error_;
};

}  // namespace zwwm::renderer
