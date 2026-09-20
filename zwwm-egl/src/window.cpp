#include "zwwm-egl/window.hpp"

#include <wayland-zwayland-client.h>
#include <zwayland/wire/message.hpp>

#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace zwwm::egl {
namespace protocol = zwayland::generated;

namespace {

int create_anonymous_file() {
#ifdef SYS_memfd_create
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "zwwm-egl", 0x0001U));
  if (fd >= 0) return fd;
#endif
  char path[] = "/tmp/zwwm-egl-XXXXXX";
  const int fallback_fd = mkostemp(path, O_CLOEXEC);
  if (fallback_fd < 0) return -1;
  (void)unlink(path);
  return fallback_fd;
}

std::string egl_error(const char* operation) {
  char text[128]{};
  std::snprintf(text, sizeof(text), "%s failed (EGL error 0x%x)", operation,
                eglGetError());
  return text;
}

}  // namespace

struct Window::Buffer {
  zwayland::client::Display* display = nullptr;
  zwayland::client::Proxy* proxy = nullptr;
  zwayland::wire::FileDescriptor fd;
  void* mapping = nullptr;
  std::size_t size = 0;
  bool busy = false;
  bool retired = false;

  ~Buffer() {
    if (mapping != nullptr) (void)munmap(mapping, size);
  }

  struct Observer {
    std::shared_ptr<Buffer> buffer;
    void release(zwayland::client::Proxy&) const {
      buffer->busy = false;
      if (!buffer->retired || buffer->proxy == nullptr) return;
      const std::uint32_t id = buffer->proxy->id;
      buffer->proxy = nullptr;
      protocol::wl_buffer_destroy(*buffer->display, id);
    }
  };
};

Window::Window(zwayland::client::Display& display,
               zwayland::client::Proxy& surface,
               zwayland::client::Proxy& shm)
    : display_(display), surface_(surface), shm_(shm) {}

Window::~Window() {
  release_buffers();
  release_surface();
  if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT)
    (void)eglDestroyContext(egl_display_, egl_context_);
  if (egl_display_ != EGL_NO_DISPLAY) (void)eglTerminate(egl_display_);
}

std::unique_ptr<Window> Window::create(zwayland::client::Display& display,
                                       zwayland::client::Proxy& surface,
                                       zwayland::client::Proxy& shm,
                                       std::uint32_t width,
                                       std::uint32_t height) {
  auto window = std::unique_ptr<Window>(new Window(display, surface, shm));
  if (!window->initialize(width, height)) return nullptr;
  return window;
}

bool Window::initialize(std::uint32_t width, std::uint32_t height) {
  const auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (get_platform_display == nullptr) return fail("eglGetPlatformDisplayEXT is unavailable");
  egl_display_ = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                      EGL_DEFAULT_DISPLAY, nullptr);
  if (egl_display_ == EGL_NO_DISPLAY) return fail(egl_error("eglGetPlatformDisplayEXT"));
  if (eglInitialize(egl_display_, nullptr, nullptr) != EGL_TRUE)
    return fail(egl_error("eglInitialize"));
  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE)
    return fail(egl_error("eglBindAPI"));

  constexpr EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE};
  EGLint count = 0;
  if (eglChooseConfig(egl_display_, config_attributes, &egl_config_, 1, &count) != EGL_TRUE ||
      count != 1)
    return fail(egl_error("eglChooseConfig"));
  constexpr EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  context_attributes);
  if (egl_context_ == EGL_NO_CONTEXT) return fail(egl_error("eglCreateContext"));
  return resize(width, height);
}

bool Window::resize(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) return fail("window dimensions must be positive");
  if (width == width_ && height == height_ && egl_surface_ != EGL_NO_SURFACE) return true;
  release_buffers();
  release_surface();
  if (!create_surface(width, height) || !create_buffers(width, height)) return false;
  width_ = width;
  height_ = height;
  return true;
}

bool Window::create_surface(std::uint32_t width, std::uint32_t height) {
  if (width > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
      height > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()))
    return fail("window dimensions exceed EGL limits");
  const EGLint attributes[] = {EGL_WIDTH, static_cast<EGLint>(width),
                               EGL_HEIGHT, static_cast<EGLint>(height), EGL_NONE};
  egl_surface_ = eglCreatePbufferSurface(egl_display_, egl_config_, attributes);
  if (egl_surface_ == EGL_NO_SURFACE) return fail(egl_error("eglCreatePbufferSurface"));
  return make_current();
}

bool Window::create_buffers(std::uint32_t width, std::uint32_t height) {
  constexpr std::size_t count = 3;
  const std::uint64_t stride64 = static_cast<std::uint64_t>(width) * 4U;
  const std::uint64_t size64 = stride64 * height;
  if (stride64 > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
      size64 > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
    return fail("window buffer exceeds wl_shm limits");
  const auto stride = static_cast<std::int32_t>(stride64);
  const auto size = static_cast<std::size_t>(size64);

  buffers_.clear();
  buffers_.reserve(count);
  for (std::size_t index = 0; index != count; ++index) {
    auto buffer = std::make_shared<Buffer>();
    buffer->display = &display_;
    buffer->size = size;
    buffer->fd = zwayland::wire::FileDescriptor(create_anonymous_file());
    if (buffer->fd.get() < 0 || ftruncate(buffer->fd.get(), static_cast<off_t>(size)) < 0)
      return fail("could not allocate wl_shm presentation buffer");
    buffer->mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                            buffer->fd.get(), 0);
    if (buffer->mapping == MAP_FAILED) {
      buffer->mapping = nullptr;
      return fail("could not map wl_shm presentation buffer");
    }
    const std::uint32_t pool_id = protocol::wl_shm_create_pool(
        display_, shm_.id, buffer->fd.get(), static_cast<std::int32_t>(size));
    auto* pool = display_.find_proxy(pool_id);
    if (pool == nullptr) return fail("could not create wl_shm_pool proxy");
    const std::uint32_t buffer_id = protocol::wl_shm_pool_create_buffer(
        display_, pool->id, 0, static_cast<std::int32_t>(width),
        static_cast<std::int32_t>(height), stride, protocol::WL_SHM_FORMAT_ARGB8888);
    buffer->proxy = display_.find_proxy(buffer_id);
    protocol::wl_shm_pool_destroy(display_, pool->id);
    if (buffer->proxy == nullptr) return fail("could not create wl_buffer proxy");
    buffer->proxy->set_observer(protocol::wl_buffer_observer(Buffer::Observer{buffer}));
    buffers_.push_back(std::move(buffer));
  }
  if (!display_.flush()) return fail("could not flush wl_shm buffer creation");
  return true;
}

bool Window::make_current() {
  if (egl_display_ == EGL_NO_DISPLAY || egl_surface_ == EGL_NO_SURFACE ||
      egl_context_ == EGL_NO_CONTEXT)
    return fail("EGL window is not initialized");
  if (eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) != EGL_TRUE)
    return fail(egl_error("eglMakeCurrent"));
  return true;
}

bool Window::present() {
  if (!make_current()) return false;
  Buffer* buffer = nullptr;
  for (const auto& candidate : buffers_) {
    if (!candidate->busy) {
      buffer = candidate.get();
      break;
    }
  }
  if (buffer == nullptr) return fail("all presentation buffers are busy");

  glFinish();
  glReadPixels(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_),
               GL_BGRA_EXT, GL_UNSIGNED_BYTE, buffer->mapping);
  if (glGetError() != GL_NO_ERROR) return fail("glReadPixels failed");
  auto* pixels = static_cast<std::uint32_t*>(buffer->mapping);
  for (std::uint32_t y = 0; y < height_ / 2; ++y) {
    auto* top = pixels + static_cast<std::size_t>(y) * width_;
    auto* bottom = pixels + static_cast<std::size_t>(height_ - y - 1) * width_;
    std::swap_ranges(top, top + width_, bottom);
  }
  const std::size_t pixel_count = static_cast<std::size_t>(width_) * height_;
  for (std::size_t index = 0; index != pixel_count; ++index) pixels[index] |= 0xff000000U;

  protocol::wl_surface_attach(display_, surface_.id, buffer->proxy, 0, 0);
  if (surface_.version >= 4)
    protocol::wl_surface_damage_buffer(display_, surface_.id, 0, 0,
                                       std::numeric_limits<std::int32_t>::max(),
                                       std::numeric_limits<std::int32_t>::max());
  else
    protocol::wl_surface_damage(display_, surface_.id, 0, 0,
                                std::numeric_limits<std::int32_t>::max(),
                                std::numeric_limits<std::int32_t>::max());
  protocol::wl_surface_commit(display_, surface_.id);
  buffer->busy = true;
  presented_ = buffer;
  if (!display_.flush()) return fail("could not flush presented buffer");
  last_error_.clear();
  return true;
}

std::span<const std::uint32_t> Window::pixels() const {
  if (presented_ == nullptr || presented_->mapping == nullptr) return {};
  return {static_cast<const std::uint32_t*>(presented_->mapping),
          static_cast<std::size_t>(width_) * height_};
}

void Window::release_surface() {
  if (egl_display_ != EGL_NO_DISPLAY)
    (void)eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (egl_display_ != EGL_NO_DISPLAY && egl_surface_ != EGL_NO_SURFACE)
    (void)eglDestroySurface(egl_display_, egl_surface_);
  egl_surface_ = EGL_NO_SURFACE;
}

void Window::release_buffers() {
  presented_ = nullptr;
  for (const auto& buffer : buffers_) {
    if (buffer->proxy == nullptr) continue;
    if (buffer->busy) {
      buffer->retired = true;
      continue;
    }
    const std::uint32_t id = buffer->proxy->id;
    buffer->proxy = nullptr;
    protocol::wl_buffer_destroy(display_, id);
  }
  buffers_.clear();
}

bool Window::fail(std::string message) {
  last_error_ = std::move(message);
  return false;
}

}  // namespace zwwm::egl
