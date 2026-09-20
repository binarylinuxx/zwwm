#pragma once

#include <EGL/egl.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <zwayland/client/display.hpp>

namespace zwwm::egl {

class Window {
 public:
  static std::unique_ptr<Window> create(zwayland::client::Display& display,
                                        zwayland::client::Proxy& surface,
                                        zwayland::client::Proxy& shm,
                                        std::uint32_t width,
                                        std::uint32_t height);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
  [[nodiscard]] bool make_current();
  [[nodiscard]] bool present();

  [[nodiscard]] EGLDisplay egl_display() const { return egl_display_; }
  [[nodiscard]] EGLContext egl_context() const { return egl_context_; }
  [[nodiscard]] EGLSurface egl_surface() const { return egl_surface_; }
  [[nodiscard]] std::span<const std::uint32_t> pixels() const;
  [[nodiscard]] const std::string& last_error() const { return last_error_; }

 private:
  struct Buffer;

  Window(zwayland::client::Display& display, zwayland::client::Proxy& surface,
         zwayland::client::Proxy& shm);

  [[nodiscard]] bool initialize(std::uint32_t width, std::uint32_t height);
  [[nodiscard]] bool create_surface(std::uint32_t width, std::uint32_t height);
  [[nodiscard]] bool create_buffers(std::uint32_t width, std::uint32_t height);
  void release_surface();
  void release_buffers();
  bool fail(std::string message);

  zwayland::client::Display& display_;
  zwayland::client::Proxy& surface_;
  zwayland::client::Proxy& shm_;
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;
  EGLConfig egl_config_ = nullptr;
  std::vector<std::shared_ptr<Buffer>> buffers_;
  Buffer* presented_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::string last_error_;
};

}  // namespace zwwm::egl
