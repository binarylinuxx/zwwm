#include "zwwm/nested_backend.hpp"
#include "backend/scene.hpp"
#include "zwwm/layout/master_stack.hpp"
#include "zwwm/renderer/egl_dmabuf_importer.hpp"
#include "zwwm/renderer/opengl.hpp"
#include "zwwm/runtime_config.hpp"
#include "zwwm-egl/window.hpp"

#include <wayland-zwayland-client.h>
#include <xdg-shell-zwayland-client.h>
#include <zwayland/client/core.hpp>
#include <zwayland/server/event_loop.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <epoxy/gl.h>
#include <X11/Xcursor/Xcursor.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <libdrm/drm_fourcc.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace zwwm {
namespace protocol = zwayland::generated;
namespace client_core = zwayland::client::core;

constexpr std::string_view kNestedConnector = "nested-1";

struct NestedBackend::Window {
  std::uint64_t id = 0;
  std::uint64_t root_id = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t buffer_width = 0;
  std::int32_t buffer_height = 0;
  float source_left = 0.0F, source_top = 0.0F, source_right = 1.0F, source_bottom = 1.0F;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t geometry_x = 0;
  std::int32_t geometry_y = 0;
  std::int32_t geometry_width = 0;
  std::int32_t geometry_height = 0;
  std::int32_t assigned_tile_x = 0, assigned_tile_y = 0, assigned_tile_width = 0, assigned_tile_height = 0;
  std::int32_t assigned_content_x = 0, assigned_content_y = 0, assigned_content_width = 0, assigned_content_height = 0;
  bool toplevel = false;
  bool popup = false;
  bool fullscreen = false;
  bool focused = false;
  bool opaque = false;
  bool content_ready = false;
  bool suppress_geometry_animation = false;
  bool track_geometry_animation = false;
  float camera_scale = 1.0F;
  std::int32_t camera_center_x = 0, camera_center_y = 0;
  std::uint32_t stack_index = 0;
  std::uint64_t scene_order = 0;
  std::uint32_t scene_rank = 2;
  std::int32_t layer_priority = 0;
  std::uint64_t root_order = 0, tree_order = 0;
  float compositor_opacity = 1.0F;
  std::uint32_t background_blur_radius = 0;
  float background_blur_ignore_alpha = 0.0F;
  bool glass = false;
  bool layer_surface = false;
  std::int32_t damage_x = 0;
  std::int32_t damage_y = 0;
  std::int32_t damage_width = 0;
  std::int32_t damage_height = 0;
  std::uint32_t format = 0;
  bool damaged = true;
  bool removed = false;
  bool tag_outgoing = false;
  std::string window_shader;
  std::string border_shader;
  std::vector<std::uint32_t> pixels;
  GLuint texture = 0;
  GLuint upload_texture = 0;
  std::uint64_t buffer_generation = 0;
  bool dmabuf_texture = false;
};

class renderer_holder {
 public:
  renderer::OpenGlRenderer renderer;
  std::unique_ptr<renderer::EglDmabufImporter> importer;
};

struct NestedBackend::RegistryObserver {
  NestedBackend* backend;
  void global(zwayland::client::Proxy&, std::uint32_t, std::string, std::uint32_t) const;
  void global_remove(zwayland::client::Proxy&, std::uint32_t) const;
};
struct NestedBackend::WmBaseObserver {
  NestedBackend* backend;
  void ping(zwayland::client::Proxy&, std::uint32_t) const;
};
struct NestedBackend::XdgSurfaceObserver {
  NestedBackend* backend;
  void configure(zwayland::client::Proxy&, std::uint32_t) const;
};
struct NestedBackend::BufferObserver {
  NestedBackend* backend;
  void release(zwayland::client::Proxy&) const;
};
struct NestedBackend::FrameObserver {
  NestedBackend* backend;
  void done(zwayland::client::Proxy&, std::uint32_t) const;
};
struct NestedBackend::SeatObserver {
  NestedBackend* backend;
  void capabilities(zwayland::client::Proxy&, std::uint32_t) const;
  void name(zwayland::client::Proxy&, std::string) const;
};
struct NestedBackend::PointerObserver {
  NestedBackend* backend;
  void enter(zwayland::client::Proxy&, std::uint32_t, zwayland::client::Proxy*, double, double) const;
  void leave(zwayland::client::Proxy&, std::uint32_t, zwayland::client::Proxy*) const;
  void motion(zwayland::client::Proxy&, std::uint32_t, double, double) const;
  void button(zwayland::client::Proxy&, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) const;
  void axis(zwayland::client::Proxy&, std::uint32_t, std::uint32_t, double) const;
  void frame(zwayland::client::Proxy&) const;
  void axis_source(zwayland::client::Proxy&, std::uint32_t) const;
  void axis_stop(zwayland::client::Proxy&, std::uint32_t, std::uint32_t) const;
  void axis_discrete(zwayland::client::Proxy&, std::uint32_t, std::int32_t) const;
  void axis_value120(zwayland::client::Proxy&, std::uint32_t, std::int32_t) const;
  void axis_relative_direction(zwayland::client::Proxy&, std::uint32_t, std::uint32_t) const;
  void warp(zwayland::client::Proxy&, double, double) const;
};
struct NestedBackend::KeyboardObserver {
  NestedBackend* backend;
  void keymap(zwayland::client::Proxy&, std::uint32_t, int, std::uint32_t) const;
  void enter(zwayland::client::Proxy&, std::uint32_t, zwayland::client::Proxy*,
             std::span<const std::byte>) const;
  void leave(zwayland::client::Proxy&, std::uint32_t, zwayland::client::Proxy*) const;
  void key(zwayland::client::Proxy&, std::uint32_t, std::uint32_t, std::uint32_t,
           std::uint32_t) const;
  void modifiers(zwayland::client::Proxy&, std::uint32_t, std::uint32_t,
                 std::uint32_t, std::uint32_t, std::uint32_t) const;
  void repeat_info(zwayland::client::Proxy&, std::int32_t, std::int32_t) const;
};

namespace {

struct EmbeddedCursor {
  std::vector<std::uint32_t> pixels;
  std::int32_t width = 0, height = 0, hotspot_x = 0, hotspot_y = 0;
  std::int32_t x = 0, y = 0;
  bool visible = true;
};

std::unordered_map<const NestedBackend*, EmbeddedCursor> embedded_cursors;

using backend_scene::animated_content;
using backend_scene::animated_decoration_scale;
using backend_scene::assigned_tile;
using backend_scene::source_uv;
using backend_scene::window_geometry;

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void transform_frame(renderer::FramePlan& frame, const OutputConfig& output, renderer::Size physical) {
  const auto scale = output.scale_per_mille / 1000.0F;
  for (auto& draw : frame.draws) {
    draw.bounds = output.physical_bounds(draw.bounds, physical);
    if (draw.clip_bounds) draw.clip_bounds = output.physical_bounds(*draw.clip_bounds, physical);
    if (draw.texture_bounds) draw.texture_bounds = output.physical_bounds(*draw.texture_bounds, physical);
    draw.corner_radius *= scale;
    draw.texture_corner_radius *= scale;
    draw.border_width *= scale;
    draw.clip_radius *= scale;
    draw.background_blur_radius *= scale;
    draw.glass_refraction *= scale;
    draw.glass_dispersion *= scale;
    draw.texture_transform = static_cast<std::uint32_t>(output.transform);
  }
}

GLuint snapshot_texture(GLuint source, int width, int height) {
  if (source == 0 || width <= 0 || height <= 0) return 0;
  GLint previous_texture = 0, previous_framebuffer = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_framebuffer);
  GLuint copy = 0, framebuffer = 0;
  glGenTextures(1, &copy);
  glBindTexture(GL_TEXTURE_2D, copy);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
  if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, copy);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
  } else {
    glDeleteTextures(1, &copy);
    copy = 0;
  }
  glDeleteFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
  return copy;
}

bool inside_rounded_rect(int x, int y, int left, int top, int width, int height, int radius) {
  if (width <= 0 || height <= 0) return false;
  const int effective_radius = std::clamp(radius, 0, std::min(width, height) / 2);
  if (effective_radius == 0) return x >= left && y >= top && x < left + width && y < top + height;
  if (x < left || y < top || x >= left + width || y >= top + height) return false;
  const int inner_left = left + effective_radius;
  const int inner_right = left + width - effective_radius - 1;
  const int inner_top = top + effective_radius;
  const int inner_bottom = top + height - effective_radius - 1;
  if (x >= inner_left && x <= inner_right) return true;
  if (y >= inner_top && y <= inner_bottom) return true;
  const int center_x = x < inner_left ? inner_left : inner_right;
  const int center_y = y < inner_top ? inner_top : inner_bottom;
  const int dx = x - center_x;
  const int dy = y - center_y;
  return dx * dx + dy * dy <= effective_radius * effective_radius;
}

std::uint32_t source_over(std::uint32_t destination, std::uint32_t source) {
  const std::uint32_t source_alpha = source >> 24U;
  const std::uint32_t inverse_alpha = 255U - source_alpha;
  const std::uint32_t destination_alpha = destination >> 24U;
  const auto blend = [inverse_alpha](std::uint32_t source_channel, std::uint32_t destination_channel) {
    return std::min(255U, source_channel + (destination_channel * inverse_alpha + 127U) / 255U);
  };
  return blend(source_alpha, destination_alpha) << 24U |
         blend((source >> 16U) & 0xffU, (destination >> 16U) & 0xffU) << 16U |
         blend((source >> 8U) & 0xffU, (destination >> 8U) & 0xffU) << 8U |
         blend(source & 0xffU, destination & 0xffU);
}

std::uint32_t apply_opacity(std::uint32_t source, float opacity) {
  const auto alpha = static_cast<std::uint32_t>(std::lround((source >> 24U) * opacity));
  const auto channel = [opacity](std::uint32_t value) { return static_cast<std::uint32_t>(std::lround(value * opacity)); };
  return alpha << 24U | channel((source >> 16U) & 0xffU) << 16U |
         channel((source >> 8U) & 0xffU) << 8U | channel(source & 0xffU);
}

std::uint32_t premultiplied(const Color& color) {
  const std::uint32_t alpha = color.alpha;
  const auto channel = [alpha](std::uint8_t value) { return (static_cast<std::uint32_t>(value) * alpha + 127U) / 255U; };
  return alpha << 24U | channel(color.red) << 16U | channel(color.green) << 8U | channel(color.blue);
}

int create_anonymous_file() {
#ifdef SYS_memfd_create
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "zwwm-nested", MFD_CLOEXEC));
  if (fd >= 0) {
    return fd;
  }
#endif

  char path[] = "/tmp/zwwm-nested-XXXXXX";
  const int fallback_fd = mkostemp(path, O_CLOEXEC);
  if (fallback_fd < 0) {
    return -1;
  }
  (void)unlink(path);
  return fallback_fd;
}

}  // namespace

NestedBackend::NestedBackend(zwayland::server::EventLoop* event_loop,
                             std::shared_ptr<const RuntimeConfig> config)
    : event_loop_(event_loop), config_(std::move(config)), animations_(config_ == nullptr ? AnimationConfig{} : config_->animations) {
  if (config_ == nullptr) throw std::invalid_argument("runtime configuration is required");
  const auto& output_config = config_->output_for(kNestedConnector);
  if (!output_config.mode.preferred) {
    physical_width_ = output_config.mode.width;
    physical_height_ = output_config.mode.height;
  }
  const auto logical = output_config.logical_size({physical_width_, physical_height_});
  logical_width_ = logical.width;
  logical_height_ = logical.height;
  if (const char* theme = std::getenv("XCURSOR_THEME"); theme != nullptr && *theme != '\0')
    cursor_theme_ = theme;
  if (const char* size = std::getenv("XCURSOR_SIZE"); size != nullptr)
    cursor_size_ = static_cast<std::uint32_t>(std::max(1, std::atoi(size)));
}
NestedBackend::~NestedBackend() { stop(); }

bool NestedBackend::start() {
  if (event_loop_ == nullptr) {
    last_error_ = "Wayland event loop is unavailable";
    return false;
  }
  parent_display_ = zwayland::client::Display::connect();
  if (parent_display_ == nullptr) {
    last_error_ = "no parent Wayland compositor is available";
    return false;
  }
  registry_ = client_core::get_registry(*parent_display_);
  if (registry_ != nullptr)
    registry_->set_observer(protocol::wl_registry_observer(RegistryObserver{this}));
  if (registry_ == nullptr || !parent_display_->roundtrip()) {
    last_error_ = "could not discover parent Wayland globals";
    stop();
    return false;
  }
  if (compositor_ == nullptr || shm_ == nullptr || wm_base_ == nullptr || !create_surface()) {
    if (last_error_.empty()) {
      last_error_ = "parent Wayland compositor lacks required globals";
    }
    stop();
    return false;
  }
  parent_source_ = event_loop_->add_fd(
      parent_display_->fd(), EPOLLIN | EPOLLHUP | EPOLLERR,
      [this](int, int mask) {
        if ((mask & (EPOLLHUP | EPOLLERR)) != 0 ||
            ((mask & EPOLLOUT) != 0 && !parent_display_->flush()) ||
            ((mask & EPOLLIN) != 0 && !parent_display_->dispatch_pending())) {
          last_error_ = "parent Wayland compositor disconnected";
          return false;
        }
        event_loop_->update_fd(parent_display_->fd(),
            EPOLLIN | EPOLLHUP | EPOLLERR |
            (parent_display_->wants_write() ? static_cast<std::uint32_t>(EPOLLOUT) : 0U));
        return true;
      });
  if (parent_source_ < 0) {
    last_error_ = "could not register parent Wayland display event source";
    stop();
    return false;
  }
  return true;
}

void NestedBackend::stop() {
  embedded_cursors.erase(this);
  if (error_timer_ >= 0) {
    event_loop_->remove(error_timer_);
    error_timer_ = -1;
  }
  if (dmabuf_target_ != nullptr) dmabuf_target_->set_dmabuf_feedback({}, std::nullopt);
  dmabuf_target_ = nullptr;
  disarm_parent_display();
  if (parent_source_ >= 0) {
    event_loop_->remove(parent_source_);
    parent_source_ = -1;
  }
  release_wayland_objects();
  parent_display_.reset();
}

bool NestedBackend::connected() const { return parent_display_ != nullptr; }
const std::string& NestedBackend::last_error() const { return last_error_; }
void NestedBackend::set_presentation_observer(PresentationObserver observer, void* data) { presentation_observer_ = observer; presentation_observer_data_ = data; }
void NestedBackend::set_config(std::shared_ptr<const RuntimeConfig> config) {
  if (config == nullptr) return;
  config_ = std::move(config);
  const auto logical = config_->output_for(kNestedConnector).logical_size({physical_width_, physical_height_});
  logical_width_ = logical.width;
  logical_height_ = logical.height;
  if (input_target_ != nullptr) input_target_->set_output_size(physical_width_, physical_height_);
  animations_.set_config(config_->animations, monotonic_ms());
  for (auto& window : windows_) window.damaged = true;
  repaint_pending_ = true;
  if (renderer_ != nullptr) repaint_gpu(); else repaint();
}
bool NestedBackend::rebuild_switch_shaders(std::string* error) {
  if (renderer_ == nullptr || egl_window_ == nullptr || !egl_window_->make_current()) {
    if (error != nullptr) *error = "nested OpenGL renderer is unavailable";
    return false;
  }
  renderer::ShaderSources sources;
  if (!load_shader_sources(*config_, &sources, error)) return false;
  if (!renderer_->renderer.prepare_shaders(sources)) {
    if (error != nullptr) *error = renderer_->renderer.last_error();
    return false;
  }
  renderer_->renderer.commit_shaders();
  repaint_pending_ = true;
  repaint_gpu();
  return true;
}
void NestedBackend::show_error(std::string message) {
  auto popup = render_error_popup(message);
  if (!popup) return;
  error_popup_ = std::move(popup);
  error_popup_visible_ = true;
  error_popup_dirty_ = true;
  ++error_popup_generation_;
  if (error_timer_ < 0)
    error_timer_ = event_loop_->add_timer(7000, [this] {
      error_timer_ = -1;
      clear_error();
    });
  else
    event_loop_->update_timer(error_timer_, 7000);
  repaint_pending_ = true;
  if (renderer_ != nullptr) repaint_gpu(); else repaint();
}
void NestedBackend::clear_error() {
  if (!error_popup_visible_) return;
  error_popup_visible_ = false;
  error_popup_dirty_ = true;
  repaint_pending_ = true;
  if (renderer_ != nullptr) repaint_gpu(); else repaint();
}
void NestedBackend::set_input_target(CompositorServer* compositor) {
  input_target_ = compositor;
  if (input_target_ != nullptr) input_target_->set_output_size(physical_width_, physical_height_);
}
void NestedBackend::set_dmabuf_target(CompositorServer* compositor) {
  if (dmabuf_target_ != nullptr && dmabuf_target_ != compositor) {
    dmabuf_target_->set_dmabuf_feedback({}, std::nullopt);
  }
  dmabuf_target_ = compositor;
  publish_dmabuf_importer();
}
void NestedBackend::set_cursor_shape(const char* xcursor_name) {
  auto& embedded = embedded_cursors[this];
  if (xcursor_name == nullptr) {
    embedded.visible = false;
    return;
  }
  embedded.visible = true;
  cursor_shape_ = xcursor_name;
  XcursorImages* cursor = XcursorLibraryLoadImages(xcursor_name, cursor_theme_.c_str(), cursor_size_);
  if (cursor == nullptr || cursor->nimage == 0 || cursor->images[0] == nullptr) {
    if (cursor != nullptr) XcursorImagesDestroy(cursor);
    return;
  }
  const auto* image = cursor->images[0];
  embedded.width = static_cast<std::int32_t>(image->width);
  embedded.height = static_cast<std::int32_t>(image->height);
  embedded.hotspot_x = static_cast<std::int32_t>(image->xhot);
  embedded.hotspot_y = static_cast<std::int32_t>(image->yhot);
  embedded.pixels.assign(image->pixels, image->pixels + static_cast<std::size_t>(image->width) * image->height);
  XcursorImagesDestroy(cursor);
}
bool NestedBackend::set_cursor_theme(const std::string& theme, std::uint32_t size,
                                     std::string* error) {
  if (theme.empty()) {
    if (error != nullptr) *error = "cursor theme must not be empty";
    return false;
  }
  if (size == 0 || size > 1024) {
    if (error != nullptr) *error = "cursor size must be between 1 and 1024";
    return false;
  }
  XcursorImages* cursor = XcursorLibraryLoadImages(cursor_shape_.c_str(), theme.c_str(), size);
  if (cursor == nullptr || cursor->nimage == 0 || cursor->images[0] == nullptr) {
    if (cursor != nullptr) XcursorImagesDestroy(cursor);
    if (error != nullptr) *error = "could not load cursor theme '" + theme + "'";
    return false;
  }
  XcursorImagesDestroy(cursor);
  cursor_theme_ = theme;
  cursor_size_ = size;
  if (embedded_cursors[this].visible) set_cursor_shape(cursor_shape_.c_str());
  return true;
}
void NestedBackend::set_cursor_position(std::int32_t x, std::int32_t y) {
  auto& cursor = embedded_cursors[this];
  cursor.x = x;
  cursor.y = y;
}

bool NestedBackend::capture_output(OutputCapture& capture, bool overlay_cursor) {
  const std::size_t pixels = static_cast<std::size_t>(physical_width_) * physical_height_;
  if (!capture_ready_ || capture_pixels_.size() != pixels) return false;
  capture.width = physical_width_;
  capture.height = physical_height_;
  capture.pixels = capture_pixels_;
  if (overlay_cursor) {
    const auto found = embedded_cursors.find(this);
    if (found == embedded_cursors.end() || !found->second.visible) return true;
    if (found->second.pixels.empty()) return false;
    const auto& cursor = found->second;
    const int left = cursor.x - cursor.hotspot_x;
    const int top = cursor.y - cursor.hotspot_y;
    const auto physical = config_->output_for(kNestedConnector).physical_bounds(
        {{left, top}, {static_cast<std::uint32_t>(cursor.width), static_cast<std::uint32_t>(cursor.height)}},
        {physical_width_, physical_height_});
    const int x0 = std::clamp(physical.origin.x, 0, static_cast<int>(physical_width_));
    const int y0 = std::clamp(physical.origin.y, 0, static_cast<int>(physical_height_));
    const int x1 = std::clamp(physical.origin.x + static_cast<int>(physical.size.width), x0, static_cast<int>(physical_width_));
    const int y1 = std::clamp(physical.origin.y + static_cast<int>(physical.size.height), y0, static_cast<int>(physical_height_));
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
      const auto logical = config_->output_for(kNestedConnector).physical_to_logical({x, y}, {physical_width_, physical_height_});
      const int sx = logical.x - left, sy = logical.y - top;
      if (sx >= 0 && sy >= 0 && sx < cursor.width && sy < cursor.height) {
        auto& destination = capture.pixels[static_cast<std::size_t>(y) * physical_width_ + x];
        destination = source_over(destination, cursor.pixels[static_cast<std::size_t>(sy) * cursor.width + sx]);
        destination |= 0xff000000U;
      }
    }
  }
  return true;
}

bool NestedBackend::capture_toplevel(std::uint64_t id, OutputCapture& capture, bool overlay_cursor) {
  const auto root = std::find_if(windows_.begin(), windows_.end(), [id](const Window& window) {
    return window.id == id && window.root_id == id && window.toplevel && !window.removed &&
           window.content_ready && window.width > 0 && window.height > 0;
  });
  if (root == windows_.end()) return false;
  capture.width = static_cast<std::uint32_t>(root->width);
  capture.height = static_cast<std::uint32_t>(root->height);
  bool captured = false;
  if (renderer_ != nullptr && root->texture != 0) {
    renderer::FramePlan frame;
    for (const auto& window : windows_) {
      if (window.root_id != id || window.removed || window.popup || window.texture == 0 ||
          window.width <= 0 || window.height <= 0) continue;
      renderer::DrawCall draw{window.id,
          {{window.x - root->x, window.y - root->y},
           {static_cast<std::uint32_t>(window.width), static_cast<std::uint32_t>(window.height)}},
          1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, window.texture, 0.0F, 0.0F,
          {window.source_left, window.source_top, window.source_right, window.source_bottom}};
      draw.opaque = window.opaque;
      frame.draws.push_back(draw);
    }
    captured = !frame.draws.empty() && renderer_->renderer.render_pixels(
        frame, {capture.width, capture.height}, &capture.pixels);
  } else if (!root->pixels.empty() && root->buffer_width > 0 && root->buffer_height > 0) {
    capture.pixels.resize(static_cast<std::size_t>(capture.width) * capture.height);
    const int source_x = std::clamp(static_cast<int>(std::floor(root->source_left * root->buffer_width)), 0,
                                    root->buffer_width - 1);
    const int source_y = std::clamp(static_cast<int>(std::floor(root->source_top * root->buffer_height)), 0,
                                    root->buffer_height - 1);
    const int source_width = std::max(1, static_cast<int>(std::ceil(
        (root->source_right - root->source_left) * root->buffer_width)));
    const int source_height = std::max(1, static_cast<int>(std::ceil(
        (root->source_bottom - root->source_top) * root->buffer_height)));
    for (std::uint32_t y = 0; y < capture.height; ++y) for (std::uint32_t x = 0; x < capture.width; ++x) {
      const int sx = std::min(root->buffer_width - 1, source_x + static_cast<int>(x * source_width / capture.width));
      const int sy = std::min(root->buffer_height - 1, source_y + static_cast<int>(y * source_height / capture.height));
      capture.pixels[static_cast<std::size_t>(y) * capture.width + x] =
          root->pixels[static_cast<std::size_t>(sy) * root->buffer_width + sx] | 0xff000000U;
    }
    captured = true;
  }
  if (!captured) { capture = {}; return false; }

  if (overlay_cursor) {
    const auto found = embedded_cursors.find(this);
    if (found != embedded_cursors.end() && found->second.visible && !found->second.pixels.empty() &&
        root->assigned_content_width > 0 && root->assigned_content_height > 0) {
      const auto& cursor = found->second;
      const double scale_x = static_cast<double>(root->width) / root->assigned_content_width;
      const double scale_y = static_cast<double>(root->height) / root->assigned_content_height;
      const int left = static_cast<int>(std::lround((cursor.x - root->assigned_content_x) * scale_x)) - cursor.hotspot_x;
      const int top = static_cast<int>(std::lround((cursor.y - root->assigned_content_y) * scale_y)) - cursor.hotspot_y;
      for (int y = 0; y < cursor.height; ++y) for (int x = 0; x < cursor.width; ++x) {
        const int dx = left + x, dy = top + y;
        if (dx < 0 || dy < 0 || dx >= static_cast<int>(capture.width) || dy >= static_cast<int>(capture.height)) continue;
        auto& destination = capture.pixels[static_cast<std::size_t>(dy) * capture.width + dx];
        destination = source_over(destination, cursor.pixels[static_cast<std::size_t>(y) * cursor.width + x]);
        destination |= 0xff000000U;
      }
    }
  }
  return true;
}

void NestedBackend::publish_dmabuf_importer() {
  if (dmabuf_target_ == nullptr || renderer_ == nullptr || renderer_->importer == nullptr ||
      !renderer_->importer->supported()) {
    return;
  }
  dmabuf_target_->set_dmabuf_feedback(renderer_->importer->supported_formats(), std::nullopt);
}

void NestedBackend::present(const ShmBufferView& buffer) {
  if (buffer.tag_transition) {
    if (buffer.tag_transition_ready) {
      tag_transition_preparing_ = false;
      animations_.start_tag_transition(buffer.tag_transition_direction, monotonic_ms());
      repaint_pending_ = true;
      if (renderer_ != nullptr) repaint_gpu(); else repaint();
      return;
    }
    for (auto& window : windows_) {
      const auto root = std::find_if(windows_.begin(), windows_.end(), [&window](const Window& item) { return item.id == window.root_id; });
      if (root != windows_.end() && root->toplevel) {
        window.tag_outgoing = true;
      }
      window.damaged = true;
    }
    tag_transition_preparing_ = true;
    return;
  }
  const auto it = std::find_if(windows_.begin(), windows_.end(), [&buffer](const Window& window) {
    return window.id == buffer.surface_id;
  });
  const bool is_new = it == windows_.end();
  if (buffer.pixels == nullptr && buffer.dmabuf == nullptr) {
    if (buffer.toplevel && buffer.assigned_content_width > 0 && buffer.assigned_content_height > 0) {
      Window window = is_new ? Window{} : *it;
      const bool retain_content = !is_new && (window.texture != 0 || !window.pixels.empty());
      const bool closing = retain_content && !buffer.content_ready;
      window.id = buffer.surface_id;
      window.root_id = buffer.root_surface_id;
      window.assigned_tile_x = buffer.assigned_tile_x; window.assigned_tile_y = buffer.assigned_tile_y;
      window.assigned_tile_width = buffer.assigned_tile_width; window.assigned_tile_height = buffer.assigned_tile_height;
      window.assigned_content_x = buffer.assigned_content_x; window.assigned_content_y = buffer.assigned_content_y;
      window.assigned_content_width = buffer.assigned_content_width; window.assigned_content_height = buffer.assigned_content_height;
      if (!retain_content) {
        window.width = buffer.assigned_content_width;
        window.height = buffer.assigned_content_height;
      }
      window.x = buffer.x;
      window.y = buffer.y;
      window.toplevel = true;
      window.removed = false; window.tag_outgoing = false;
       window.suppress_geometry_animation = buffer.suppress_geometry_animation;
       window.track_geometry_animation = buffer.track_geometry_animation;
       window.camera_scale = buffer.camera_scale;
       window.camera_center_x = buffer.camera_center_x;
       window.camera_center_y = buffer.camera_center_y;
        window.focused = buffer.focused;
        window.window_shader = buffer.window_shader;
        window.border_shader = buffer.border_shader;
       if (!retain_content) {
         window.geometry_x = buffer.window_geometry_x;
         window.geometry_y = buffer.window_geometry_y;
         window.geometry_width = buffer.window_geometry_width;
         window.geometry_height = buffer.window_geometry_height;
       }
       window.stack_index = buffer.stack_index;
       window.scene_order = buffer.scene_order;
       window.background_blur_radius = buffer.background_blur_radius;
       window.glass = buffer.glass;
       if (!retain_content) window.content_ready = buffer.content_ready;
       window.removed = closing;
       if (!retain_content) {
         window.texture = 0;
         window.pixels.clear();
      }
      window.damaged = true;
      if (is_new) windows_.push_back(std::move(window)); else *it = std::move(window);
      if (closing) {
        for (auto& item : windows_) if (item.root_id == buffer.surface_id) {
          if (renderer_ != nullptr && item.texture != 0 && item.upload_texture == 0) {
            const GLuint imported = item.texture;
            item.upload_texture = snapshot_texture(imported, item.buffer_width, item.buffer_height);
            if (item.upload_texture != 0) {
              item.texture = item.upload_texture;
              if (item.dmabuf_texture && renderer_->importer != nullptr)
                (void)renderer_->importer->release(imported);
              item.dmabuf_texture = false;
            }
          }
          item.content_ready = true;
          item.removed = true;
        }
      }
      for (auto& item : windows_) item.damaged = true;
      if (!tag_transition_preparing_) {
        repaint_pending_ = true;
        if (renderer_ != nullptr) repaint_gpu(); else repaint();
      }
      return;
    }
    if (it != windows_.end()) {
      const auto retained_root = std::find_if(windows_.begin(), windows_.end(),
                                              [&](const Window& window) { return window.id == it->root_id; });
      if (it->tag_outgoing || (retained_root != windows_.end() && retained_root->removed)) {
        for (auto& window : windows_) if (window.root_id == it->root_id) window.damaged = true;
      } else if (it->toplevel && config_->animations.enabled && config_->animations.close) {
        for (auto& window : windows_) if (window.root_id == it->id) {
          if (renderer_ != nullptr && window.texture != 0 && window.upload_texture == 0) {
            const GLuint imported = window.texture;
            window.upload_texture = snapshot_texture(imported, window.buffer_width, window.buffer_height);
            if (window.upload_texture != 0) {
              window.texture = window.upload_texture;
              if (window.dmabuf_texture && renderer_->importer != nullptr)
                (void)renderer_->importer->release(imported);
              window.dmabuf_texture = false;
            }
          }
          window.removed = true;
        }
      } else {
        if (renderer_ != nullptr) {
          if (it->dmabuf_texture && it->texture != 0 && renderer_->importer != nullptr)
            (void)renderer_->importer->release(it->texture);
          if (it->upload_texture != 0) glDeleteTextures(1, &it->upload_texture);
        }
        windows_.erase(it);
      }
      for (auto& window : windows_) window.damaged = true;
      repaint_pending_ = true;
      if (renderer_ != nullptr) repaint_gpu(); else repaint();
    }
    return;
  }
  const bool shm = buffer.pixels != nullptr;
  const bool supported_format = shm
                                    ? buffer.format == protocol::WL_SHM_FORMAT_ARGB8888 ||
                                          buffer.format == protocol::WL_SHM_FORMAT_XRGB8888
                                    : buffer.format == DRM_FORMAT_ARGB8888 ||
                                          buffer.format == DRM_FORMAT_XRGB8888;
  if (buffer.width <= 0 || buffer.height <= 0 || !supported_format ||
      (shm && (buffer.width > std::numeric_limits<std::int32_t>::max() / 4 ||
               buffer.stride < buffer.width * 4))) {
    return;
  }
  Window window = is_new ? Window{} : *it;
  const bool resized = window.buffer_width != buffer.width || window.buffer_height != buffer.height;
  const bool moved = !is_new && (window.x != buffer.x || window.y != buffer.y ||
                                 window.geometry_x != buffer.window_geometry_x || window.geometry_y != buffer.window_geometry_y ||
                                 window.geometry_width != buffer.window_geometry_width || window.geometry_height != buffer.window_geometry_height);
  window.id = buffer.surface_id;
  window.root_id = buffer.root_surface_id;
  window.assigned_tile_x = buffer.assigned_tile_x; window.assigned_tile_y = buffer.assigned_tile_y;
  window.assigned_tile_width = buffer.assigned_tile_width; window.assigned_tile_height = buffer.assigned_tile_height;
  window.assigned_content_x = buffer.assigned_content_x; window.assigned_content_y = buffer.assigned_content_y;
  window.assigned_content_width = buffer.assigned_content_width; window.assigned_content_height = buffer.assigned_content_height;
  window.width = buffer.logical_width;
  window.height = buffer.logical_height;
  window.buffer_width = buffer.width;
  window.buffer_height = buffer.height;
  window.source_left = buffer.source_left; window.source_top = buffer.source_top;
  window.source_right = buffer.source_right; window.source_bottom = buffer.source_bottom;
  window.x = buffer.x;
  window.y = buffer.y;
  window.geometry_x = buffer.window_geometry_x;
  window.geometry_y = buffer.window_geometry_y;
  window.geometry_width = buffer.window_geometry_width;
  window.geometry_height = buffer.window_geometry_height;
  window.toplevel = buffer.toplevel;
  window.popup = buffer.popup;
  window.fullscreen = buffer.fullscreen;
  window.suppress_geometry_animation = buffer.suppress_geometry_animation;
  window.track_geometry_animation = buffer.track_geometry_animation;
  window.camera_scale = buffer.camera_scale;
  window.camera_center_x = buffer.camera_center_x;
  window.camera_center_y = buffer.camera_center_y;
  window.focused = buffer.focused;
  window.window_shader = buffer.window_shader;
  window.border_shader = buffer.border_shader;
  window.opaque = buffer.opaque;
  window.content_ready = buffer.content_ready;
  window.removed = false; window.tag_outgoing = false;
  window.stack_index = buffer.stack_index;
  window.scene_order = buffer.scene_order;
  window.scene_rank = buffer.scene_rank; window.layer_priority = buffer.layer_priority;
  window.root_order = buffer.root_order; window.tree_order = buffer.tree_order;
  window.compositor_opacity = buffer.compositor_opacity;
  window.background_blur_radius = buffer.background_blur_radius;
  window.background_blur_ignore_alpha = buffer.background_blur_ignore_alpha;
  window.glass = buffer.glass;
  window.layer_surface = buffer.layer_surface;
  window.format = buffer.format;
  if (resized || window.pixels.empty()) window.pixels.resize(static_cast<std::size_t>(buffer.width) * buffer.height);
  const int left = std::clamp(resized ? 0 : buffer.damage_x, 0, buffer.width);
  const int top = std::clamp(resized ? 0 : buffer.damage_y, 0, buffer.height);
  const int right = std::clamp(resized ? buffer.width : buffer.damage_x + buffer.damage_width, left, buffer.width);
  const int bottom = std::clamp(resized ? buffer.height : buffer.damage_y + buffer.damage_height, top, buffer.height);
  window.damage_x = left;
  window.damage_y = top;
  window.damage_width = right - left;
  window.damage_height = bottom - top;
  window.damaged = true;
  for (int y = top; buffer.pixels != nullptr && y < bottom; ++y) {
    std::memcpy(window.pixels.data() + static_cast<std::size_t>(y) * buffer.width + left,
                buffer.pixels + static_cast<std::size_t>(y) * buffer.stride + left * sizeof(std::uint32_t),
                static_cast<std::size_t>(right - left) * sizeof(std::uint32_t));
    if (buffer.format == protocol::WL_SHM_FORMAT_XRGB8888) {
      auto* row = window.pixels.data() + static_cast<std::size_t>(y) * buffer.width;
      for (int x = left; x < right; ++x) row[x] |= 0xff000000U;
    }
  }
  if (renderer_ != nullptr && buffer.pixels != nullptr) {
    if (window.dmabuf_texture && window.texture != 0 && renderer_->importer != nullptr)
      (void)renderer_->importer->release(window.texture);
    window.dmabuf_texture = false;
    window.buffer_generation = buffer.buffer_generation;
    if (window.upload_texture == 0) glGenTextures(1, &window.upload_texture);
    window.texture = window.upload_texture;
    glBindTexture(GL_TEXTURE_2D, window.upload_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (resized) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, buffer.width, buffer.height, 0, GL_BGRA, GL_UNSIGNED_BYTE, window.pixels.data());
    } else if (right > left && bottom > top) {
      glPixelStorei(GL_UNPACK_ROW_LENGTH, buffer.width);
      glTexSubImage2D(GL_TEXTURE_2D, 0, left, top, right - left, bottom - top, GL_BGRA, GL_UNSIGNED_BYTE,
                      window.pixels.data() + static_cast<std::size_t>(top) * buffer.width + left);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  } else if (renderer_ != nullptr && buffer.dmabuf != nullptr &&
             window.buffer_generation != buffer.buffer_generation) {
    if (window.dmabuf_texture && window.texture != 0 && renderer_->importer != nullptr)
      (void)renderer_->importer->release(window.texture);
    window.texture = renderer_->importer == nullptr ? 0 : renderer_->importer->import(*buffer.dmabuf);
    window.buffer_generation = buffer.buffer_generation;
    window.dmabuf_texture = window.texture != 0;
    if (window.texture == 0)
      std::fprintf(stderr, "zwwm: nested EGL: DMA-BUF import failed for surface %llu generation %llu: %s; skipping it\n",
                   static_cast<unsigned long long>(buffer.surface_id),
                   static_cast<unsigned long long>(buffer.buffer_generation),
                   renderer_->importer == nullptr ? "importer unavailable" : renderer_->importer->last_error().c_str());
  }
  if (is_new) windows_.push_back(std::move(window)); else *it = std::move(window);
  std::stable_sort(windows_.begin(), windows_.end(), [](const Window& left, const Window& right) {
    return std::tie(left.scene_rank, left.layer_priority, left.root_order, left.tree_order) <
           std::tie(right.scene_rank, right.layer_priority, right.root_order, right.tree_order);
  });
  if (moved || is_new) for (auto& item : windows_) item.damaged = true;
  repaint_pending_ = true;
  if (!tag_transition_preparing_) {
    if (renderer_ != nullptr) repaint_gpu(); else repaint();
  }
}

void NestedBackend::repaint_gpu() {
  if (renderer_ == nullptr || egl_window_ == nullptr || frame_pending_) return;
  const std::uint64_t now = monotonic_ms();
  const auto tag = animations_.tag_transition(now);
  if (!tag) {
    for (auto& window : windows_) {
      if (!window.tag_outgoing) continue;
      animations_.remove(window.root_id);
      window.removed = true;
    }
  }
  std::vector<AnimationTarget> animation_targets;
  animation_targets.reserve(windows_.size());
  for (const auto& window : windows_) {
    const auto bounds = assigned_tile(window);
    const renderer::Rect viewport{{0, 0}, {logical_width_, logical_height_}};
    if (tag && window.toplevel && !backend_scene::intersects(bounds, viewport)) {
      animations_.remove(window.id);
      continue;
    }
    if (window.toplevel && !window.removed && window.content_ready && window.texture != 0)
      animation_targets.push_back({window.id, bounds,
                                     !window.suppress_geometry_animation,
                                     !tag || window.tag_outgoing,
                                     window.track_geometry_animation});
  }
  animations_.update(animation_targets, now);
  for (auto& window : windows_) window.track_geometry_animation = false;
  for (auto it = windows_.begin(); it != windows_.end();) {
    if (it->removed && !animations_.retains(it->root_id)) {
      if (it->dmabuf_texture && it->texture != 0 && renderer_->importer != nullptr)
        (void)renderer_->importer->release(it->texture);
      if (it->upload_texture != 0) glDeleteTextures(1, &it->upload_texture);
      it = windows_.erase(it);
    } else ++it;
  }
  std::vector<layout::Placement> placements;
  for (const auto id : animations_.retained_ids()) {
    const auto sample = animations_.sample(id, now);
    if (sample) placements.push_back({id, sample->bounds});
  }
  renderer::FramePlan frame;
  frame.draw_background = true;
  frame.shader_time = static_cast<float>(now % 1000000U) / 1000.0F;
  std::optional<renderer::DrawCall> pending_border;
  std::uint64_t pending_border_root = 0;
  const auto apply_effect_parameters = [&](renderer::DrawCall& draw, std::string_view shader = {}) {
    draw.background_blur_passes = static_cast<std::uint32_t>(std::clamp(
        config_->shader_integer(shader, "blur_passes", 3), 1, 8));
    draw.background_blur_brightness = std::clamp(
        config_->shader_integer(shader, "blur_brightness", 100), 0, 200) / 100.0F;
    draw.background_blur_contrast = std::clamp(
        config_->shader_integer(shader, "blur_contrast", 100), 0, 200) / 100.0F;
    draw.background_blur_saturation = std::clamp(
        config_->shader_integer(shader, "blur_saturation", 100), 0, 200) / 100.0F;
    draw.background_blur_noise = std::clamp(
        config_->shader_integer(shader, "blur_noise", 0), 0, 100) / 100.0F;
  };
  for (const auto& window : windows_) {
    if (pending_border && (window.root_id != pending_border_root || window.popup)) {
      frame.draws.push_back(std::move(*pending_border));
      pending_border.reset();
    }
    auto placement = std::find_if(placements.begin(), placements.end(), [&window](const auto& item) { return item.id == window.root_id; });
    const auto root = std::find_if(windows_.begin(), windows_.end(), [&window](const Window& item) { return item.id == window.root_id; });
    if (root == windows_.end() || !root->content_ready || root->width <= 0 || root->height <= 0 || window.texture == 0) continue;
    if (!root->toplevel) {
      frame.draws.push_back({window.id, {{window.x, window.y}, {static_cast<std::uint32_t>(window.width), static_cast<std::uint32_t>(window.height)}}, window.compositor_opacity, {1.0F, 1.0F, 1.0F, 1.0F}, window.texture, 0.0F, 0.0F, {window.source_left, window.source_top, window.source_right, window.source_bottom}});
      frame.draws.back().opaque = window.opaque && window.compositor_opacity == 1.0F;
      frame.draws.back().background_blur_radius = static_cast<float>(window.background_blur_radius);
      frame.draws.back().background_mask_texture = window.layer_surface;
      frame.draws.back().background_mask_alpha_threshold = window.background_blur_ignore_alpha;
      apply_effect_parameters(frame.draws.back());
      continue;
    }
    renderer::Rect tag_bounds;
    if (placement != placements.end()) tag_bounds = placement->bounds;
    else continue;
    const auto tag_root = tag ? transform_tag_root(tag_bounds, logical_width_, *tag, root->tag_outgoing)
                               : TagRootSample{tag_bounds, 1.0F};
    tag_bounds = tag_root.bounds;
    const float decoration_scale = animated_decoration_scale(*root, tag_bounds);
    const float border_width = config_->border_width() * decoration_scale;
    const float inner_radius = config_->radius() * decoration_scale;
    const float outer_radius = inner_radius + border_width;
    const float border_join_width = border_width +
        0.75F / (config_->output_for(kNestedConnector).scale_per_mille / 1000.0F);
    if (window.toplevel && !root->fullscreen && border_width > 0.0F) {
      constexpr std::array<float, 4> border{1.0F, 1.0F, 1.0F, 1.0F};
      const auto animation = animations_.sample(window.id, now);
      pending_border.emplace(window.id, tag_bounds, (animation ? animation->opacity : 1.0F) * tag_root.opacity, border, std::nullopt,
                             outer_radius, border_join_width);
      pending_border->shader_role = renderer::ShaderRole::border;
      pending_border->shader_name = window.border_shader;
      pending_border->toplevel_id = window.root_id;
      pending_border->toplevel_bounds = tag_bounds;
      pending_border->toplevel_state = window.focused ? 1U : 0U;
      pending_border->shader_time = frame.shader_time;
      pending_border_root = window.root_id;
    }
    const auto content = animated_content(*root, tag_bounds);
    const auto animation = animations_.sample(window.root_id, now);
    const int content_x = content.origin.x, content_y = content.origin.y;
    const int content_w = static_cast<int>(content.size.width), content_h = static_cast<int>(content.size.height);
    if (content_w <= 0 || content_h <= 0) continue;
    const auto geometry = window.toplevel ? window_geometry(window) : window_geometry(*root);
    const int goal_w = root->assigned_content_width > 0 ? root->assigned_content_width : geometry.width;
    const int goal_h = root->assigned_content_height > 0 ? root->assigned_content_height : geometry.height;
    const renderer::Rect root_texture_bounds{
        content.origin,
        {static_cast<std::uint32_t>(std::max<std::int64_t>(
             1, static_cast<std::int64_t>(geometry.width) * content_w / goal_w)),
         static_cast<std::uint32_t>(std::max<std::int64_t>(
             1, static_cast<std::int64_t>(geometry.height) * content_h / goal_h))}};
    const int relative_x = window.x - root->x;
    const int relative_y = window.y - root->y;
    renderer::Rect bounds = window.toplevel
                                 ? tag_bounds
                                 : renderer::Rect{{content_x + (relative_x - geometry.x) * content_w / goal_w,
                                                   content_y + (relative_y - geometry.y) * content_h / goal_h},
                                                  {static_cast<std::uint32_t>(std::max(1, window.width * content_w / goal_w)),
                                                    static_cast<std::uint32_t>(std::max(1, window.height * content_h / goal_h))}};
    const bool root_window = window.toplevel;
    const float radius = root_window && !root->fullscreen ? outer_radius : 0.0F;
    frame.draws.push_back({window.id, bounds, (animation ? animation->opacity : 1.0F) * tag_root.opacity * window.compositor_opacity, {1.0F, 1.0F, 1.0F, 1.0F}, window.texture, radius, 0.0F,
                           window.toplevel ? source_uv(geometry, window) : std::array<float, 4>{window.source_left, window.source_top, window.source_right, window.source_bottom},
                               window.popup ? std::nullopt : std::optional<renderer::Rect>{content},
                                 (window.popup || root->fullscreen) ? 0.0F : inner_radius});
    frame.draws.back().opaque = window.opaque;
    frame.draws.back().shader_role = renderer::ShaderRole::window;
    frame.draws.back().shader_name = window.window_shader;
    frame.draws.back().toplevel_id = window.root_id;
    frame.draws.back().toplevel_bounds = tag_bounds;
    frame.draws.back().toplevel_state = (window.focused ? 1U : 0U) |
        (root->fullscreen ? 2U : 0U) | (window.popup ? 4U : 0U);
    frame.draws.back().shader_time = frame.shader_time;
    if (root_window) {
      frame.draws.back().texture_bounds = root_texture_bounds;
      frame.draws.back().texture_corner_radius = root->fullscreen ? 0.0F : inner_radius;
    }
    frame.draws.back().background_blur_radius = static_cast<float>(window.background_blur_radius);
    apply_effect_parameters(frame.draws.back(), window.window_shader);
  }
  if (pending_border) frame.draws.push_back(std::move(*pending_border));
  if (error_popup_visible_ && error_popup_) {
    if (error_popup_texture_ == 0) glGenTextures(1, &error_popup_texture_);
    if (uploaded_error_popup_generation_ != error_popup_generation_) {
      glBindTexture(GL_TEXTURE_2D, error_popup_texture_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(error_popup_.width),
                   static_cast<GLsizei>(error_popup_.height), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                   error_popup_.pixels.data());
      glBindTexture(GL_TEXTURE_2D, 0);
      uploaded_error_popup_generation_ = error_popup_generation_;
    }
    const auto popup_x = std::max(8, (static_cast<int>(logical_width_) - static_cast<int>(error_popup_.width)) / 2);
    frame.draws.push_back({std::numeric_limits<std::uint64_t>::max() - 1,
                           {{popup_x, 24}, {error_popup_.width, error_popup_.height}},
                           1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, error_popup_texture_});
  }
  transform_frame(frame, config_->output_for(kNestedConnector), {physical_width_, physical_height_});
  glViewport(0, 0, static_cast<int>(physical_width_), static_cast<int>(physical_height_));
  glClearColor(0.125F, 0.141F, 0.169F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!renderer_->renderer.render(frame, {physical_width_, physical_height_}) ||
      !request_parent_frame()) {
    last_error_ = "could not render nested EGL frame";
    return;
  }
  if (!egl_window_->present()) {
    last_error_ = "could not present nested EGL frame: " + egl_window_->last_error();
    protocol::wl_surface_commit(*parent_display_, surface_->id);
    (void)parent_display_->flush();
    return;
  }
  const auto snapshot = egl_window_->pixels();
  capture_pixels_.assign(snapshot.begin(), snapshot.end());
  capture_pending_ = !capture_pixels_.empty();
  for (auto& window : windows_) window.damaged = false;
  repaint_pending_ = animations_.active(now);
}

void NestedBackend::repaint() {
  if (buffer_data_ == nullptr || !buffer_attached_ || buffer_in_flight_ || frame_pending_ || surface_ == nullptr ||
      !repaint_pending_) {
    return;
  }
  software_logical_.resize(static_cast<std::size_t>(logical_width_) * logical_height_);
  auto* destination = software_logical_.data();
  const std::uint64_t now = monotonic_ms();
  const auto tag = animations_.tag_transition(now);
  const bool tag_cleanup = !tag &&
                           std::any_of(windows_.begin(), windows_.end(),
                                       [](const Window& window) { return window.tag_outgoing; });
  if (tag_cleanup) {
    for (const auto& window : windows_)
      if (window.tag_outgoing) animations_.remove(window.root_id);
    windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
                                  [](const Window& window) { return window.tag_outgoing; }),
                   windows_.end());
    for (auto& window : windows_) window.damaged = true;
  }
  std::vector<AnimationTarget> animation_targets;
  animation_targets.reserve(windows_.size());
  for (const auto& window : windows_) {
    const auto bounds = assigned_tile(window);
    const renderer::Rect viewport{{0, 0}, {logical_width_, logical_height_}};
    if (tag && window.toplevel && !backend_scene::intersects(bounds, viewport)) {
      animations_.remove(window.id);
      continue;
    }
    if (window.toplevel && !window.removed && window.content_ready && !window.pixels.empty())
      animation_targets.push_back({window.id, bounds,
                                     !window.suppress_geometry_animation,
                                     !tag || window.tag_outgoing,
                                     window.track_geometry_animation});
  }
  animations_.update(animation_targets, now);
  for (auto& window : windows_) window.track_geometry_animation = false;
  windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [&](const Window& window) {
                   return window.removed && !animations_.retains(window.root_id);
                 }),
                 windows_.end());
  std::vector<layout::Placement> placements;
  for (const auto id : animations_.retained_ids()) {
    const auto sample = animations_.sample(id, now);
    if (sample) placements.push_back({id, sample->bounds});
  }
  const bool animation_active = animations_.active(now);
  if (animation_active) for (auto& window : windows_) window.damaged = true;
  const auto placement_for = [&placements](std::uint64_t id) -> const renderer::Rect* {
    const auto it = std::find_if(placements.begin(), placements.end(), [id](const auto& item) { return item.id == id; });
    return it == placements.end() ? nullptr : &it->bounds;
  };
  int dirty_left = static_cast<int>(logical_width_), dirty_top = static_cast<int>(logical_height_), dirty_right = 0, dirty_bottom = 0;
  for (const auto& window : windows_) {
    if (!window.damaged) continue;
    const auto root_it = std::find_if(windows_.begin(), windows_.end(), [&window](const Window& item) { return item.id == window.root_id; });
    const auto* bounds = placement_for(window.root_id);
    renderer::Rect tag_bounds;
    if (root_it == windows_.end() || root_it->width == 0 || root_it->height == 0) continue;
    if (!root_it->toplevel) {
      dirty_left = std::min(dirty_left, window.x + window.damage_x); dirty_top = std::min(dirty_top, window.y + window.damage_y);
      dirty_right = std::max(dirty_right, window.x + window.damage_x + window.damage_width + 1); dirty_bottom = std::max(dirty_bottom, window.y + window.damage_y + window.damage_height + 1);
      continue;
    }
    if (bounds == nullptr) continue;
    if (tag) { tag_bounds = transform_tag_root(*bounds, logical_width_, *tag, root_it->tag_outgoing).bounds; bounds = &tag_bounds; }
    const auto content = animated_content(*root_it, *bounds);
    const int content_x = content.origin.x;
    const int content_y = content.origin.y;
    const int content_w = static_cast<int>(content.size.width);
    const int content_h = static_cast<int>(content.size.height);
    if (content_w <= 0 || content_h <= 0) continue;
    if (window.toplevel) {
      dirty_left = std::min(dirty_left, bounds->origin.x);
      dirty_top = std::min(dirty_top, bounds->origin.y);
      dirty_right = std::max(dirty_right, bounds->origin.x + static_cast<int>(bounds->size.width));
      dirty_bottom = std::max(dirty_bottom, bounds->origin.y + static_cast<int>(bounds->size.height));
    } else {
      const auto geometry = window_geometry(*root_it);
      const int goal_w = root_it->assigned_content_width > 0 ? root_it->assigned_content_width : geometry.width;
      const int goal_h = root_it->assigned_content_height > 0 ? root_it->assigned_content_height : geometry.height;
      const int relative_x = window.x - root_it->x - geometry.x;
      const int relative_y = window.y - root_it->y - geometry.y;
      dirty_left = std::min(dirty_left, content_x + (relative_x + window.damage_x) * content_w / goal_w);
      dirty_top = std::min(dirty_top, content_y + (relative_y + window.damage_y) * content_h / goal_h);
      dirty_right = std::max(dirty_right, content_x + (relative_x + window.damage_x + window.damage_width + 1) * content_w / goal_w);
      dirty_bottom = std::max(dirty_bottom, content_y + (relative_y + window.damage_y + window.damage_height + 1) * content_h / goal_h);
    }
  }
  if (animation_active || tag_cleanup) {
    dirty_left = 0;
    dirty_top = 0;
    dirty_right = static_cast<int>(logical_width_);
    dirty_bottom = static_cast<int>(logical_height_);
  }
  if ((error_popup_visible_ || error_popup_dirty_) && error_popup_) {
    const int popup_x = std::max(8, (static_cast<int>(logical_width_) - static_cast<int>(error_popup_.width)) / 2);
    dirty_left = std::min(dirty_left, popup_x);
    dirty_top = std::min(dirty_top, 24);
    dirty_right = std::max(dirty_right, popup_x + static_cast<int>(error_popup_.width));
    dirty_bottom = std::max(dirty_bottom, 24 + static_cast<int>(error_popup_.height));
  }
  dirty_left = std::clamp(dirty_left, 0, static_cast<int>(logical_width_)); dirty_top = std::clamp(dirty_top, 0, static_cast<int>(logical_height_));
  dirty_right = std::clamp(dirty_right, dirty_left, static_cast<int>(logical_width_)); dirty_bottom = std::clamp(dirty_bottom, dirty_top, static_cast<int>(logical_height_));
  for (int y = dirty_top; y < dirty_bottom; ++y) std::fill_n(destination + static_cast<std::size_t>(y) * logical_width_ + dirty_left, dirty_right - dirty_left, 0xff20242bU);
  for (const auto& window : windows_) {
    const auto root_it = std::find_if(windows_.begin(), windows_.end(), [&window](const Window& item) { return item.id == window.root_id; });
    const auto* bounds = placement_for(window.root_id);
    renderer::Rect tag_bounds;
    if (root_it == windows_.end() || !root_it->content_ready || root_it->width == 0 || root_it->height == 0) continue;
    if (!root_it->toplevel) {
      for (int y = std::max(dirty_top, window.y); y < std::min(dirty_bottom, window.y + window.height); ++y) for (int x = std::max(dirty_left, window.x); x < std::min(dirty_right, window.x + window.width); ++x) {
        const int sx = static_cast<int>((window.source_left + (window.source_right - window.source_left) * (x - window.x) / window.width) * window.buffer_width);
        const int sy = static_cast<int>((window.source_top + (window.source_bottom - window.source_top) * (y - window.y) / window.height) * window.buffer_height);
        if (sx >= 0 && sy >= 0 && sx < window.buffer_width && sy < window.buffer_height) {
          const auto pixel = window.pixels[static_cast<std::size_t>(sy) * window.buffer_width + sx] | (window.format == protocol::WL_SHM_FORMAT_XRGB8888 ? 0xff000000U : 0U);
          destination[static_cast<std::size_t>(y) * logical_width_ + x] = source_over(destination[static_cast<std::size_t>(y) * logical_width_ + x], apply_opacity(pixel, window.compositor_opacity));
        }
      }
      continue;
    }
    if (bounds == nullptr) continue;
    const auto tag_root = tag ? transform_tag_root(*bounds, logical_width_, *tag, root_it->tag_outgoing)
                              : TagRootSample{*bounds, 1.0F};
    tag_bounds = tag_root.bounds;
    bounds = &tag_bounds;
    const auto content = animated_content(*root_it, *bounds);
    const int content_x = content.origin.x, content_y = content.origin.y;
    const int content_w = static_cast<int>(content.size.width), content_h = static_cast<int>(content.size.height);
    if (content_w <= 0 || content_h <= 0) continue;
    const auto animation = animations_.sample(window.root_id, now);
    const float opacity = (animation ? animation->opacity : 1.0F) * tag_root.opacity * window.compositor_opacity;
    const float decoration_scale = animated_decoration_scale(*root_it, *bounds);
    const int border_width = std::max(0, static_cast<int>(std::lround(config_->border_width() * decoration_scale)));
    const int inner_radius = std::max(0, static_cast<int>(std::lround(config_->radius() * decoration_scale)));
    const int outer_radius = inner_radius + border_width;
    const auto geometry = window.toplevel ? window_geometry(window) : window_geometry(*root_it);
    const int goal_w = root_it->assigned_content_width > 0 ? root_it->assigned_content_width : geometry.width;
    const int goal_h = root_it->assigned_content_height > 0 ? root_it->assigned_content_height : geometry.height;
    const int relative_x = window.x - root_it->x;
    const int relative_y = window.y - root_it->y;
    const int draw_x = window.toplevel ? content_x : content_x + (relative_x - geometry.x) * content_w / goal_w;
    const int draw_y = window.toplevel ? content_y : content_y + (relative_y - geometry.y) * content_h / goal_h;
    const int draw_width = window.toplevel ? std::max(1, geometry.width * content_w / goal_w) : std::max(1, window.width * content_w / goal_w);
    const int draw_height = window.toplevel ? std::max(1, geometry.height * content_h / goal_h) : std::max(1, window.height * content_h / goal_h);
    const int clip_left = window.popup ? dirty_left : std::max(dirty_left, content_x);
    const int clip_top = window.popup ? dirty_top : std::max(dirty_top, content_y);
    const int clip_right = window.popup ? dirty_right : std::min(dirty_right, content_x + content_w);
    const int clip_bottom = window.popup ? dirty_bottom : std::min(dirty_bottom, content_y + content_h);
    for (int y = std::max(clip_top, draw_y); y < std::min(clip_bottom, draw_y + draw_height); ++y) for (int x = std::max(clip_left, draw_x); x < std::min(clip_right, draw_x + draw_width); ++x) {
       const int logical_x = window.toplevel ? geometry.x + (x - content_x) * goal_w / content_w :
                                                 (x - content_x) * goal_w / content_w + geometry.x - relative_x;
       const int logical_y = window.toplevel ? geometry.y + (y - content_y) * goal_h / content_h :
                                                 (y - content_y) * goal_h / content_h + geometry.y - relative_y;
       const int sx = static_cast<int>((window.source_left + (window.source_right - window.source_left) * logical_x / window.width) * window.buffer_width);
       const int sy = static_cast<int>((window.source_top + (window.source_bottom - window.source_top) * logical_y / window.height) * window.buffer_height);
      const int content_radius = root_it->fullscreen ? 0 : window.toplevel ? std::max(0, inner_radius - 1) : inner_radius;
      const bool inside_shape = window.popup ||
          (inside_rounded_rect(x, y, bounds->origin.x, bounds->origin.y,
                               static_cast<int>(bounds->size.width), static_cast<int>(bounds->size.height), outer_radius) &&
           inside_rounded_rect(x, y, content_x, content_y, content_w, content_h, content_radius));
        if (sx >= 0 && sy >= 0 && sx < window.buffer_width && sy < window.buffer_height && inside_shape) destination[static_cast<std::size_t>(y) * logical_width_ + x] = source_over(destination[static_cast<std::size_t>(y) * logical_width_ + x], apply_opacity(window.pixels[static_cast<std::size_t>(sy) * window.buffer_width + sx] | (window.format == protocol::WL_SHM_FORMAT_XRGB8888 ? 0xff000000U : 0U), opacity));
    }
    if (window.toplevel && !root_it->fullscreen && border_width > 0) {
      const auto border = apply_opacity(premultiplied(window.focused ? config_->decoration.focused_border_color
                                                                     : config_->decoration.border_color),
                                        (animation ? animation->opacity : 1.0F) * tag_root.opacity);
      for (int y = std::max(dirty_top, bounds->origin.y); y < std::min(dirty_bottom, bounds->origin.y + static_cast<int>(bounds->size.height)); ++y) {
        for (int x = std::max(dirty_left, bounds->origin.x); x < std::min(dirty_right, bounds->origin.x + static_cast<int>(bounds->size.width)); ++x) {
          if (inside_rounded_rect(x, y, bounds->origin.x, bounds->origin.y,
                                  static_cast<int>(bounds->size.width), static_cast<int>(bounds->size.height), outer_radius) &&
              !inside_rounded_rect(x, y, content_x, content_y, content_w, content_h, inner_radius)) {
            destination[static_cast<std::size_t>(y) * logical_width_ + x] =
                source_over(destination[static_cast<std::size_t>(y) * logical_width_ + x], border);
          }
        }
      }
    }
  }
  if (error_popup_visible_ && error_popup_) {
    const int popup_x = std::max(8, (static_cast<int>(logical_width_) - static_cast<int>(error_popup_.width)) / 2);
    composite_error_popup(error_popup_, software_logical_, logical_width_, logical_height_, popup_x, 24);
  }
  error_popup_dirty_ = false;
  for (auto& window : windows_) window.damaged = false;
  auto* output = static_cast<std::uint32_t*>(buffer_data_);
  for (std::uint32_t y = 0; y < physical_height_; ++y) {
    for (std::uint32_t x = 0; x < physical_width_; ++x) {
      auto logical = config_->output_for(kNestedConnector).physical_to_logical({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                                                          {physical_width_, physical_height_});
      logical.x = std::clamp(logical.x, 0, static_cast<std::int32_t>(logical_width_) - 1);
      logical.y = std::clamp(logical.y, 0, static_cast<std::int32_t>(logical_height_) - 1);
      output[static_cast<std::size_t>(y) * physical_width_ + x] =
          destination[static_cast<std::size_t>(logical.y) * logical_width_ + logical.x];
    }
  }
  if (compositor_version_ >= 4) {
    protocol::wl_surface_damage_buffer(*parent_display_, surface_->id, 0, 0,
                                       physical_width_, physical_height_);
  } else {
    protocol::wl_surface_damage(*parent_display_, surface_->id, 0, 0,
                                physical_width_, physical_height_);
  }
  if (!request_parent_frame()) return;
  buffer_in_flight_ = true;
  protocol::wl_surface_commit(*parent_display_, surface_->id);
  capture_pixels_.assign(output, output + static_cast<std::size_t>(physical_width_) * physical_height_);
  capture_pending_ = true;
  repaint_pending_ = animations_.active(now);
  (void)parent_display_->flush();
}

bool NestedBackend::request_parent_frame() {
  if (surface_ == nullptr || frame_callback_ != nullptr) return false;
  const std::uint32_t callback_id =
      protocol::wl_surface_frame(*parent_display_, surface_->id);
  frame_callback_ = parent_display_->find_proxy(callback_id);
  if (frame_callback_ == nullptr) return false;
  frame_callback_->set_observer(protocol::wl_callback_observer(FrameObserver{this}));
  frame_pending_ = true;
  return true;
}

void NestedBackend::RegistryObserver::global(zwayland::client::Proxy&, std::uint32_t name,
                                              std::string interface,
                                              std::uint32_t version) const {
  if (interface == protocol::wl_compositor_interface.name && backend->compositor_ == nullptr) {
    backend->compositor_version_ = std::min(version, 4U);
    backend->compositor_ = client_core::bind(
        *backend->parent_display_, *backend->registry_, name,
        protocol::wl_compositor_interface, backend->compositor_version_);
  } else if (interface == protocol::wl_shm_interface.name && backend->shm_ == nullptr) {
    backend->shm_ = client_core::bind(*backend->parent_display_, *backend->registry_,
                                      name, protocol::wl_shm_interface, 1);
  } else if (interface == protocol::xdg_wm_base_interface.name && backend->wm_base_ == nullptr) {
    backend->wm_base_ = client_core::bind(*backend->parent_display_, *backend->registry_,
                                          name, protocol::xdg_wm_base_interface, 1);
    if (backend->wm_base_ != nullptr)
      backend->wm_base_->set_observer(protocol::xdg_wm_base_observer(WmBaseObserver{backend}));
  } else if (interface == protocol::wl_seat_interface.name && backend->seat_ == nullptr) {
    backend->seat_ = client_core::bind(*backend->parent_display_, *backend->registry_,
                                       name, protocol::wl_seat_interface,
                                       std::min(version, 5U));
    if (backend->seat_ != nullptr)
      backend->seat_->set_observer(protocol::wl_seat_observer(SeatObserver{backend}));
  }
}

void NestedBackend::RegistryObserver::global_remove(zwayland::client::Proxy&,
                                                     std::uint32_t) const {}

void NestedBackend::WmBaseObserver::ping(zwayland::client::Proxy& wm_base,
                                         std::uint32_t serial) const {
  protocol::xdg_wm_base_pong(*backend->parent_display_, wm_base.id, serial);
}

void NestedBackend::XdgSurfaceObserver::configure(zwayland::client::Proxy& surface,
                                                  std::uint32_t serial) const {
  protocol::xdg_surface_ack_configure(*backend->parent_display_, surface.id, serial);
  if (!backend->egl_initialization_attempted_) {
    backend->egl_initialization_attempted_ = true;
    const char* requested_renderer = std::getenv("ZWWM_RENDERER");
    const bool force_shm = requested_renderer != nullptr && std::strcmp(requested_renderer, "shm") == 0;
    if (!force_shm && backend->create_egl_surface()) {
      std::fputs("zwwm: nested renderer=OpenGL\n", stderr);
      backend->buffer_attached_ = true;
      backend->publish_dmabuf_importer();
      backend->repaint_pending_ = true;
      backend->repaint_gpu();
      return;
    }
  }
  if (!backend->buffer_attached_ && backend->create_buffer()) {
    std::fputs("zwwm: nested renderer=SHM\n", stderr);
    protocol::wl_surface_attach(*backend->parent_display_, backend->surface_->id,
                                backend->buffer_, 0, 0);
    if (backend->compositor_version_ >= 4) {
       protocol::wl_surface_damage_buffer(*backend->parent_display_, backend->surface_->id,
                                          0, 0, backend->physical_width_, backend->physical_height_);
    } else {
       protocol::wl_surface_damage(*backend->parent_display_, backend->surface_->id,
                                   0, 0, backend->physical_width_, backend->physical_height_);
    }
    backend->buffer_attached_ = true;
    backend->repaint_pending_ = true;
    backend->repaint();
  }
}

void NestedBackend::BufferObserver::release(zwayland::client::Proxy&) const {
  backend->buffer_in_flight_ = false;
  backend->repaint();
}

void NestedBackend::FrameObserver::done(zwayland::client::Proxy& callback,
                                        std::uint32_t) const {
  if (backend->frame_callback_ == &callback) backend->frame_callback_ = nullptr;
  backend->frame_pending_ = false;
  if (backend->capture_pending_) {
    backend->capture_ready_ = true;
    backend->capture_pending_ = false;
  }
  if (backend->presentation_observer_ != nullptr) {
    backend->presentation_observer_(backend->presentation_observer_data_);
  }
  if (backend->repaint_pending_) {
    if (backend->renderer_ != nullptr) backend->repaint_gpu(); else backend->repaint();
  }
}

void NestedBackend::SeatObserver::capabilities(zwayland::client::Proxy& seat,
                                                std::uint32_t capabilities) const {
  if ((capabilities & protocol::WL_SEAT_CAPABILITY_POINTER) != 0 && backend->pointer_ == nullptr) {
    const auto id = protocol::wl_seat_get_pointer(*backend->parent_display_, seat.id);
    backend->pointer_ = backend->parent_display_->find_proxy(id);
    if (backend->pointer_ != nullptr)
      backend->pointer_->set_observer(protocol::wl_pointer_observer(PointerObserver{backend}));
  }
  if ((capabilities & protocol::WL_SEAT_CAPABILITY_POINTER) == 0 && backend->pointer_ != nullptr) {
    protocol::wl_pointer_release(*backend->parent_display_, backend->pointer_->id);
    backend->pointer_ = nullptr;
    backend->pending_axes_.clear();
    if (backend->input_target_ != nullptr) backend->input_target_->pointer_leave();
  }
  if ((capabilities & protocol::WL_SEAT_CAPABILITY_KEYBOARD) != 0 && backend->keyboard_ == nullptr) {
    const auto id = protocol::wl_seat_get_keyboard(*backend->parent_display_, seat.id);
    backend->keyboard_ = backend->parent_display_->find_proxy(id);
    if (backend->keyboard_ != nullptr)
      backend->keyboard_->set_observer(protocol::wl_keyboard_observer(KeyboardObserver{backend}));
  }
  if ((capabilities & protocol::WL_SEAT_CAPABILITY_KEYBOARD) == 0 && backend->keyboard_ != nullptr) {
    protocol::wl_keyboard_release(*backend->parent_display_, backend->keyboard_->id);
    backend->keyboard_ = nullptr;
    if (backend->input_target_ != nullptr) backend->input_target_->input_reset();
  }
}

void NestedBackend::SeatObserver::name(zwayland::client::Proxy&, std::string) const {}

void NestedBackend::PointerObserver::enter(zwayland::client::Proxy&, std::uint32_t,
                                            zwayland::client::Proxy*, double x,
                                            double y) const {
  backend->deliver_pointer_motion(0, static_cast<std::int32_t>(std::lround(x)),
                                  static_cast<std::int32_t>(std::lround(y)));
}
void NestedBackend::PointerObserver::leave(zwayland::client::Proxy&, std::uint32_t,
                                            zwayland::client::Proxy*) const {
  if (backend->input_target_ != nullptr) backend->input_target_->pointer_leave();
}
void NestedBackend::PointerObserver::motion(zwayland::client::Proxy&, std::uint32_t time,
                                             double x, double y) const {
  backend->deliver_pointer_motion(time, static_cast<std::int32_t>(std::lround(x)),
                                  static_cast<std::int32_t>(std::lround(y)));
}
void NestedBackend::PointerObserver::button(zwayland::client::Proxy&, std::uint32_t,
                                             std::uint32_t time, std::uint32_t button,
                                             std::uint32_t state) const {
  if (backend->input_target_ != nullptr) backend->input_target_->pointer_button(time, button, state);
}
void NestedBackend::PointerObserver::axis(zwayland::client::Proxy&, std::uint32_t time,
                                           std::uint32_t axis, double value) const {
  backend->pending_axes_.push_back({time, axis, backend->axis_source_,
      protocol::WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL, value});
}
void NestedBackend::PointerObserver::frame(zwayland::client::Proxy&) const {
  if (backend->input_target_ != nullptr)
    for (const auto& axis : backend->pending_axes_)
      backend->input_target_->pointer_axis(axis.time, axis.axis, axis.value, axis.source,
          axis.discrete, axis.value120, axis.stop, axis.relative_direction);
  backend->pending_axes_.clear();
}
void NestedBackend::PointerObserver::axis_source(zwayland::client::Proxy&,
                                                  std::uint32_t source) const { backend->axis_source_ = source; }
void NestedBackend::PointerObserver::axis_stop(zwayland::client::Proxy&, std::uint32_t time,
                                                std::uint32_t axis) const {
  backend->pending_axes_.push_back({time, axis, backend->axis_source_,
      protocol::WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL, 0, 0, 0, true});
}
void NestedBackend::PointerObserver::axis_discrete(zwayland::client::Proxy&, std::uint32_t axis,
                                                    std::int32_t discrete) const {
  for (auto it = backend->pending_axes_.rbegin(); it != backend->pending_axes_.rend(); ++it)
    if (it->axis == axis) { it->discrete = discrete; break; }
}
void NestedBackend::PointerObserver::axis_value120(zwayland::client::Proxy&, std::uint32_t axis,
                                                    std::int32_t value120) const {
  for (auto it = backend->pending_axes_.rbegin(); it != backend->pending_axes_.rend(); ++it)
    if (it->axis == axis) { it->value120 = value120; break; }
}
void NestedBackend::PointerObserver::axis_relative_direction(zwayland::client::Proxy&,
    std::uint32_t axis, std::uint32_t direction) const {
  for (auto it = backend->pending_axes_.rbegin(); it != backend->pending_axes_.rend(); ++it)
    if (it->axis == axis) { it->relative_direction = direction; break; }
}
void NestedBackend::PointerObserver::warp(zwayland::client::Proxy&, double x,
                                           double y) const {
  backend->deliver_pointer_motion(0, static_cast<std::int32_t>(std::lround(x)),
                                  static_cast<std::int32_t>(std::lround(y)));
}
void NestedBackend::KeyboardObserver::enter(zwayland::client::Proxy&, std::uint32_t,
    zwayland::client::Proxy*, std::span<const std::byte>) const {}
void NestedBackend::KeyboardObserver::keymap(zwayland::client::Proxy&, std::uint32_t,
                                              int fd, std::uint32_t) const { close(fd); }
void NestedBackend::KeyboardObserver::leave(zwayland::client::Proxy&, std::uint32_t,
                                             zwayland::client::Proxy*) const {
  if (backend->input_target_ != nullptr) backend->input_target_->input_reset(true);
}
void NestedBackend::KeyboardObserver::key(zwayland::client::Proxy&, std::uint32_t,
    std::uint32_t time, std::uint32_t key, std::uint32_t state) const {
  if (backend->input_target_ != nullptr) backend->input_target_->keyboard_key(time, key, state);
}
void NestedBackend::KeyboardObserver::modifiers(zwayland::client::Proxy&, std::uint32_t,
    std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
    std::uint32_t group) const {
  if (backend->input_target_ != nullptr)
    backend->input_target_->keyboard_modifiers(depressed, latched, locked, group);
}
void NestedBackend::KeyboardObserver::repeat_info(zwayland::client::Proxy&, std::int32_t rate,
                                                   std::int32_t delay) const {
  if (backend->input_target_ != nullptr) backend->input_target_->keyboard_repeat_info(rate, delay);
}

void NestedBackend::deliver_pointer_motion(std::uint32_t time, std::int32_t x, std::int32_t y) {
  const auto logical_point = config_->output_for(kNestedConnector).physical_to_logical({x, y}, {physical_width_, physical_height_});
  const int px = std::clamp(logical_point.x, 0, static_cast<int>(logical_width_) - 1), py = std::clamp(logical_point.y, 0, static_cast<int>(logical_height_) - 1);
  auto& cursor = embedded_cursors[this];
  cursor.x = px;
  cursor.y = py;
  if (input_target_ == nullptr) return;
  input_target_->pointer_motion_global(time, px, py);
}

bool NestedBackend::create_surface() {
  const auto surface_id = protocol::wl_compositor_create_surface(
      *parent_display_, compositor_->id);
  surface_ = parent_display_->find_proxy(surface_id);
  if (surface_ == nullptr) {
    last_error_ = "could not create parent Wayland surface";
    return false;
  }
  const auto region_id = protocol::wl_compositor_create_region(
      *parent_display_, compositor_->id);
  auto* opaque = parent_display_->find_proxy(region_id);
  if (opaque == nullptr) {
    last_error_ = "could not create parent opaque region";
    return false;
  }
  protocol::wl_region_add(*parent_display_, opaque->id, 0, 0,
                          static_cast<std::int32_t>(physical_width_),
                          static_cast<std::int32_t>(physical_height_));
  protocol::wl_surface_set_opaque_region(*parent_display_, surface_->id, opaque);
  protocol::wl_region_destroy(*parent_display_, opaque->id);
  const auto xdg_surface_id = protocol::xdg_wm_base_get_xdg_surface(
      *parent_display_, wm_base_->id, surface_);
  xdg_surface_ = parent_display_->find_proxy(xdg_surface_id);
  if (xdg_surface_ == nullptr) {
    last_error_ = "could not create parent xdg surface";
    return false;
  }
  xdg_surface_->set_observer(protocol::xdg_surface_observer(XdgSurfaceObserver{this}));
  const auto toplevel_id = protocol::xdg_surface_get_toplevel(
      *parent_display_, xdg_surface_->id);
  toplevel_ = parent_display_->find_proxy(toplevel_id);
  if (toplevel_ == nullptr) {
    last_error_ = "could not create parent xdg toplevel";
    return false;
  }
  protocol::xdg_toplevel_set_title(*parent_display_, toplevel_->id, "zwwm nested");
  protocol::xdg_toplevel_set_app_id(*parent_display_, toplevel_->id,
                                    "org.zwwm.Compositor");
  protocol::wl_surface_commit(*parent_display_, surface_->id);
  (void)parent_display_->flush();
  return true;
}

bool NestedBackend::create_buffer() {
  const int stride = static_cast<int>(physical_width_) * 4;
  buffer_size_ = static_cast<std::size_t>(stride) * physical_height_;
  buffer_fd_.reset(create_anonymous_file());
  if (!buffer_fd_ || ftruncate(buffer_fd_.get(), static_cast<off_t>(buffer_size_)) < 0) {
    last_error_ = "could not allocate parent Wayland buffer";
    release_buffer();
    return false;
  }
  buffer_data_ = mmap(nullptr, buffer_size_, PROT_READ | PROT_WRITE, MAP_SHARED, buffer_fd_.get(), 0);
  if (buffer_data_ == MAP_FAILED) {
    buffer_data_ = nullptr;
    last_error_ = "could not map parent Wayland buffer";
    release_buffer();
    return false;
  }
  auto* pixels = static_cast<std::uint32_t*>(buffer_data_);
  for (std::uint32_t y = 0; y < physical_height_; ++y) {
    for (std::uint32_t x = 0; x < physical_width_; ++x) {
      const bool border = x < 12 || x >= physical_width_ - std::min(12U, physical_width_) || y < 12 || y >= physical_height_ - std::min(12U, physical_height_);
      const bool stripe = ((x / 48) + (y / 48)) % 2 == 0;
      pixels[static_cast<std::size_t>(y) * physical_width_ + x] = border ? 0xffe8a317U : (stripe ? 0xff243b53U : 0xff355c7dU);
    }
  }
  UniqueFd transferred_fd(fcntl(buffer_fd_.get(), F_DUPFD_CLOEXEC, 0));
  if (!transferred_fd) {
    last_error_ = "could not duplicate parent Wayland buffer descriptor";
    release_buffer();
    return false;
  }
  const auto pool_id = protocol::wl_shm_create_pool(
      *parent_display_, shm_->id, transferred_fd.get(), static_cast<int>(buffer_size_));
  buffer_pool_ = parent_display_->find_proxy(pool_id);
  if (buffer_pool_ != nullptr) {
    const auto buffer_id = protocol::wl_shm_pool_create_buffer(
        *parent_display_, buffer_pool_->id, 0, physical_width_, physical_height_,
        stride, protocol::WL_SHM_FORMAT_ARGB8888);
    buffer_ = parent_display_->find_proxy(buffer_id);
  }
  if (buffer_ == nullptr) {
    last_error_ = "could not create parent Wayland buffer";
    release_buffer();
    return false;
  }
  buffer_->set_observer(protocol::wl_buffer_observer(BufferObserver{this}));
  return true;
}

bool NestedBackend::create_egl_surface() {
  egl_window_ = egl::Window::create(*parent_display_, *surface_, *shm_,
                                    physical_width_, physical_height_);
  if (egl_window_ == nullptr) {
    last_error_ = "native EGL window initialization failed";
    return false;
  }
  renderer_ = std::make_unique<renderer_holder>();
  renderer_->importer = std::make_unique<renderer::EglDmabufImporter>(
      egl_window_->egl_display());
  if (!renderer_->renderer.initialize()) {
    last_error_ = "OpenGL renderer initialization failed: " + renderer_->renderer.last_error();
    std::fprintf(stderr, "zwwm: nested EGL: %s\n", last_error_.c_str());
    release_egl_surface();
    return false;
  }
  {
    renderer::ShaderSources sources;
    std::string shader_error;
    if (load_shader_sources(*config_, &sources, &shader_error) &&
        renderer_->renderer.prepare_shaders(sources))
      renderer_->renderer.commit_shaders();
    else
      last_error_ = shader_error.empty() ? renderer_->renderer.last_error() : shader_error;
  }
  if (!renderer_->importer->initialize()) {
    last_error_ = "DMA-BUF importer initialization failed: " + renderer_->importer->last_error();
    std::fprintf(stderr, "zwwm: nested EGL: %s; retaining EGL wl_shm texture upload\n", last_error_.c_str());
  } else if (renderer_->importer->supported_formats().empty()) {
    last_error_ = "DMA-BUF importer reported no supported format/modifier pairs";
    std::fprintf(stderr, "zwwm: nested EGL: %s; retaining EGL wl_shm texture upload\n", last_error_.c_str());
  }
  publish_dmabuf_importer();
  return true;
}

void NestedBackend::release_egl_surface() {
  capture_ready_ = false;
  capture_pending_ = false;
  capture_pixels_.clear();
  if (renderer_ != nullptr) {
    if (egl_window_ != nullptr && egl_window_->make_current()) {
      for (auto& window : windows_) {
        if (window.dmabuf_texture && window.texture != 0 && renderer_->importer != nullptr)
          (void)renderer_->importer->release(window.texture);
        if (window.upload_texture != 0) glDeleteTextures(1, &window.upload_texture);
      }
      if (error_popup_texture_ != 0) glDeleteTextures(1, &error_popup_texture_);
      error_popup_texture_ = 0;
      uploaded_error_popup_generation_ = 0;
      renderer_->importer->shutdown();
      renderer_->renderer.shutdown();
    }
    renderer_.reset();
  }
  egl_window_.reset();
}

bool NestedBackend::arm_parent_display() {
  if (parent_display_ == nullptr) return false;
  (void)parent_display_->flush();
  event_loop_->update_fd(parent_display_->fd(),
      EPOLLIN | EPOLLHUP | EPOLLERR |
      (parent_display_->wants_write() ? static_cast<std::uint32_t>(EPOLLOUT) : 0U));
  return true;
}

void NestedBackend::disarm_parent_display() {
  parent_read_prepared_ = false;
}

void NestedBackend::release_buffer() {
  capture_ready_ = false;
  capture_pending_ = false;
  capture_pixels_.clear();
  if (buffer_ != nullptr) {
    protocol::wl_buffer_destroy(*parent_display_, buffer_->id);
    buffer_ = nullptr;
  }
  if (buffer_pool_ != nullptr) {
    protocol::wl_shm_pool_destroy(*parent_display_, buffer_pool_->id);
    buffer_pool_ = nullptr;
  }
  if (buffer_data_ != nullptr) {
    (void)munmap(buffer_data_, buffer_size_);
    buffer_data_ = nullptr;
  }
  buffer_fd_.reset();
  buffer_size_ = 0;
  buffer_attached_ = false;
  buffer_in_flight_ = false;
  repaint_pending_ = false;
  windows_.clear();
}

void NestedBackend::release_wayland_objects() {
  release_egl_surface();
  if (frame_callback_ != nullptr) {
    parent_display_->destroy_proxy(frame_callback_->id);
    frame_callback_ = nullptr;
  }
  frame_pending_ = false;
  if (keyboard_ != nullptr) {
    protocol::wl_keyboard_release(*parent_display_, keyboard_->id);
    keyboard_ = nullptr;
  }
  if (pointer_ != nullptr) {
    protocol::wl_pointer_release(*parent_display_, pointer_->id);
    pointer_ = nullptr;
  }
  if (seat_ != nullptr) {
    protocol::wl_seat_release(*parent_display_, seat_->id);
    seat_ = nullptr;
  }
  if (toplevel_ != nullptr) {
    protocol::xdg_toplevel_destroy(*parent_display_, toplevel_->id);
    toplevel_ = nullptr;
  }
  if (xdg_surface_ != nullptr) {
    protocol::xdg_surface_destroy(*parent_display_, xdg_surface_->id);
    xdg_surface_ = nullptr;
  }
  if (surface_ != nullptr) {
    protocol::wl_surface_destroy(*parent_display_, surface_->id);
    surface_ = nullptr;
  }
  release_buffer();
  if (wm_base_ != nullptr) {
    protocol::xdg_wm_base_destroy(*parent_display_, wm_base_->id);
    wm_base_ = nullptr;
  }
  if (shm_ != nullptr) {
    parent_display_->destroy_proxy(shm_->id);
    shm_ = nullptr;
  }
  if (compositor_ != nullptr) {
    parent_display_->destroy_proxy(compositor_->id);
    compositor_ = nullptr;
  }
  if (registry_ != nullptr) {
    parent_display_->destroy_proxy(registry_->id);
    registry_ = nullptr;
  }
}

}  // namespace zwwm
