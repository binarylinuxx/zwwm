#include "zwwm/runtime_backend.hpp"
#include "backend/scene.hpp"
#include "zwwm/compositor_server.hpp"
#include "zwwm/runtime_config.hpp"
#include "zwwm/animation.hpp"
#include "zwwm/output_capture.hpp"
#include "zwwm/renderer/egl_dmabuf_importer.hpp"
#include "zwwm/renderer/opengl.hpp"

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <X11/Xcursor/Xcursor.h>

extern "C" {
#include <gbm.h>
#include <libinput.h>
#include <libseat.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace protocol = zwayland::generated;

namespace zwwm {
namespace {

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void clear_gl_errors() {
  // Bound capture checks so they cannot stall rendering.
  for (int count = 0; count < 8; ++count)
    if (glGetError() == GL_NO_ERROR) break;
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
  const bool framebuffer_complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (framebuffer_complete) {
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

using backend_scene::animated_content;
using backend_scene::animated_decoration_scale;
using backend_scene::assigned_tile;
using backend_scene::source_uv;
using backend_scene::window_geometry;

bool is_drm_card(const char* path) {
  if (path == nullptr) {
    return false;
  }
  const std::string_view node(path);
  const std::size_t slash = node.rfind('/');
  const std::string_view name = node.substr(slash == std::string_view::npos ? 0 : slash + 1);
  return name.starts_with("card") && name.size() > 4 &&
         std::all_of(name.begin() + 4, name.end(), [](char character) { return character >= '0' && character <= '9'; });
}

const libseat_seat_listener kSeatListener{
    .enable_seat = RuntimeBackend::enable_seat,
    .disable_seat = RuntimeBackend::disable_seat,
};

const libinput_interface kLibinputInterface{
    .open_restricted = RuntimeBackend::open_restricted,
    .close_restricted = RuntimeBackend::close_restricted,
};

std::uint32_t mode_refresh_millihz(const drmModeModeInfo& mode) {
  if (mode.htotal == 0 || mode.vtotal == 0) return 0;
  std::uint64_t refresh = static_cast<std::uint64_t>(mode.clock) * 1000000U / mode.htotal / mode.vtotal;
  if ((mode.flags & DRM_MODE_FLAG_INTERLACE) != 0) refresh *= 2;
  if ((mode.flags & DRM_MODE_FLAG_DBLSCAN) != 0) refresh /= 2;
  if (mode.vscan > 1) refresh /= mode.vscan;
  return static_cast<std::uint32_t>(refresh);
}

bool same_drm_mode(const drmModeModeInfo& left, const drmModeModeInfo& right) {
  return left.clock == right.clock && left.hdisplay == right.hdisplay &&
         left.hsync_start == right.hsync_start && left.hsync_end == right.hsync_end &&
         left.htotal == right.htotal && left.hskew == right.hskew &&
         left.vdisplay == right.vdisplay && left.vsync_start == right.vsync_start &&
         left.vsync_end == right.vsync_end && left.vtotal == right.vtotal &&
         left.vscan == right.vscan && left.flags == right.flags;
}

OutputId stable_output_id(std::string_view card, std::string_view connector) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto part : {card, connector}) for (const unsigned char byte : part) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return OutputId{hash == 0 ? 1 : hash};
}

void transform_frame(renderer::FramePlan& frame, const OutputConfig& output, renderer::Size physical,
                     renderer::Point logical_origin = {}) {
  const auto scale = output.scale_per_mille / 1000.0F;
  for (auto& draw : frame.draws) {
    draw.bounds.origin.x -= logical_origin.x;
    draw.bounds.origin.y -= logical_origin.y;
    if (draw.clip_bounds) {
      draw.clip_bounds->origin.x -= logical_origin.x;
      draw.clip_bounds->origin.y -= logical_origin.y;
    }
    if (draw.texture_bounds) {
      draw.texture_bounds->origin.x -= logical_origin.x;
      draw.texture_bounds->origin.y -= logical_origin.y;
    }
    const renderer::Rect output_clip{{0, 0}, output.logical_size(physical)};
    draw.clip_bounds = draw.clip_bounds ? draw.clip_bounds : std::optional<renderer::Rect>{output_clip};
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

}  // namespace

struct RuntimeBackend::DrmOutput {
  struct ShmTexture {
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
    std::uint32_t format = 0;
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
    GLuint texture = 0;
    bool owns_texture = false;
    bool removed = false;
    bool tag_outgoing = false;
    std::string window_shader;
    std::string border_shader;
    std::vector<std::uint32_t> pixels;
  };

  DrmDevice* device = nullptr;
  std::string connector_key;
  gbm_surface* scanout_surface = nullptr;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  std::vector<ShmTexture> shm_textures;
  bool tag_transition_preparing = false;
  std::uint32_t connector_id = 0;
  OutputInfo output;
  std::uint32_t crtc_id = 0;
  std::uint32_t crtc_index = 0;
  drmModeModeInfo mode{};
  gbm_bo* front_bo = nullptr;
  gbm_bo* pending_bo = nullptr;
  std::uint32_t front_fb = 0;
  std::uint32_t pending_fb = 0;
    bool flip_pending = false;
    bool needs_repaint = false;
    std::uint32_t scanout_bit_depth = 8;
  AnimationSystem animations;
  GLuint cursor_texture = 0;
  GLuint capture_texture = 0;
  GLuint capture_cursor_texture = 0;
  GLuint capture_framebuffer = 0;
  std::int32_t cursor_width = 0, cursor_height = 0, cursor_hotspot_x = 0, cursor_hotspot_y = 0;
  bool capture_ready = false;
  bool retired = false;

  explicit DrmOutput(AnimationConfig config) : animations(config) {}
};

struct RuntimeBackend::DrmDevice {
  struct DmabufTexture {
    std::uint64_t generation = 0;
    GLuint texture = 0;
    bool failed = false;
  };
  RuntimeBackend* backend = nullptr;
  std::string path;
  int fd = -1;
  int seat_device_id = -1;
  gbm_device* gbm = nullptr;
  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLContext egl_context = EGL_NO_CONTEXT;
  EGLConfig egl_config = nullptr;
  std::uint32_t scanout_format = DRM_FORMAT_XRGB8888;
  std::uint32_t scanout_bit_depth = 8;
  std::unique_ptr<renderer::OpenGlRenderer> renderer;
  std::unique_ptr<renderer::EglDmabufImporter> dmabuf_importer;
  GLuint error_popup_texture = 0;
  std::uint64_t error_popup_generation = 0;
  std::unordered_map<std::uint64_t, DmabufTexture> dmabuf_textures;
  std::vector<std::unique_ptr<DrmOutput>> outputs;
  std::vector<std::unique_ptr<DrmOutput>> retired_outputs;
  int source = -1;
};

RuntimeBackend::RuntimeBackend(zwayland::server::EventLoop* event_loop, std::shared_ptr<const RuntimeConfig> config)
    : event_loop_(event_loop), config_(std::move(config)) {
  if (const char* theme = std::getenv("XCURSOR_THEME"); theme != nullptr && *theme != '\0')
    cursor_theme_ = theme;
  if (const char* size = std::getenv("XCURSOR_SIZE"); size != nullptr)
    cursor_size_ = static_cast<std::uint32_t>(std::max(1, std::atoi(size)));
}
RuntimeBackend::~RuntimeBackend() { stop(); }

bool RuntimeBackend::start() {
  std::fprintf(stderr, "zwwm: direct backend: opening libseat session\n");
  active_ = false;
  libinput_suspended_ = false;
  if (event_loop_ == nullptr) {
    last_error_ = "Wayland event loop is unavailable";
    return false;
  }
  seat_ = libseat_open_seat(&kSeatListener, this);
  if (seat_ == nullptr) {
    last_error_ = "libseat could not open a compositor seat";
    return false;
  }
  std::fprintf(stderr, "zwwm: direct backend: libseat session opened\n");
  try {
    seat_source_ = event_loop_->add_fd(libseat_get_fd(seat_), EPOLLIN, [this](int fd, int mask) {
      on_seat_fd(fd, static_cast<std::uint32_t>(mask), this);
      return true;
    });
  } catch (const std::exception&) {
    last_error_ = "could not register libseat event source";
    stop();
    return false;
  }

  udev_ = udev_new();
  udev_monitor_ = udev_ == nullptr ? nullptr : udev_monitor_new_from_netlink(udev_, "udev");
  if (udev_monitor_ == nullptr || udev_monitor_filter_add_match_subsystem_devtype(udev_monitor_, "drm", nullptr) < 0 ||
      udev_monitor_enable_receiving(udev_monitor_) < 0) {
    last_error_ = "could not initialize the udev DRM monitor";
    stop();
    return false;
  }
  try {
    udev_source_ = event_loop_->add_fd(udev_monitor_get_fd(udev_monitor_), EPOLLIN, [this](int fd, int mask) {
      on_udev_fd(fd, static_cast<std::uint32_t>(mask), this);
      return true;
    });
  } catch (const std::exception&) {
    last_error_ = "could not register udev event source";
    stop();
    return false;
  }

  libinput_ = libinput_udev_create_context(&kLibinputInterface, this, udev_);
  if (libinput_ == nullptr || libinput_udev_assign_seat(libinput_, "seat0") != 0) {
    last_error_ = "could not initialize libinput for seat0";
    stop();
    return false;
  }
  std::fprintf(stderr, "zwwm: direct backend: input initialized, awaiting seat activation\n");
  try {
    libinput_source_ = event_loop_->add_fd(libinput_get_fd(libinput_), EPOLLIN, [this](int fd, int mask) {
      on_libinput_fd(fd, static_cast<std::uint32_t>(mask), this);
      return true;
    });
  } catch (const std::exception&) {
    last_error_ = "could not register libinput event source";
    stop();
    return false;
  }
  discover_drm_devices();
  // libseat enables the session asynchronously. On a fresh VT this first
  // enumeration can be empty; enable_seat will discover and modeset outputs.
  return true;
}

void RuntimeBackend::stop() {
  if (error_timer_ >= 0) {
    event_loop_->remove(error_timer_);
    error_timer_ = -1;
  }
  for (auto& device : devices_) {
    close_drm_device(*device);
  }
  devices_.clear();
  if (libinput_source_ >= 0) {
    event_loop_->remove(libinput_source_);
    libinput_source_ = -1;
  }
  if (libinput_ != nullptr) {
    libinput_unref(libinput_);
    libinput_ = nullptr;
  }
  if (udev_source_ >= 0) {
    event_loop_->remove(udev_source_);
    udev_source_ = -1;
  }
  if (udev_monitor_ != nullptr) {
    udev_monitor_unref(udev_monitor_);
    udev_monitor_ = nullptr;
  }
  if (udev_ != nullptr) {
    udev_unref(udev_);
    udev_ = nullptr;
  }
  if (seat_source_ >= 0) {
    event_loop_->remove(seat_source_);
    seat_source_ = -1;
  }
  if (seat_ != nullptr) {
    libseat_close_seat(seat_);
    seat_ = nullptr;
  }
  restricted_devices_.clear();
  active_ = false;
  libinput_suspended_ = false;
}

bool RuntimeBackend::active() const { return active_; }
const std::string& RuntimeBackend::last_error() const { return last_error_; }
void RuntimeBackend::set_config(std::shared_ptr<const RuntimeConfig> config) {
  if (config == nullptr) return;
  config_ = std::move(config);
  const std::uint64_t now = monotonic_ms();
  for (auto& device : devices_) for (auto& output : device->outputs) {
    output->capture_ready = false;
    output->animations.set_config(config_->animations, now);
    const auto& configured = config_->output_for(output->output.connector);
    const auto logical = configured.logical_size({static_cast<std::uint32_t>(output->mode.hdisplay),
                                                   static_cast<std::uint32_t>(output->mode.vdisplay)});
    output->output.logical_width = logical.width;
    output->output.logical_height = logical.height;
    output->output.scale_per_mille = configured.scale_per_mille;
    output->output.transform = configured.transform;
    output->output.refresh_millihz = mode_refresh_millihz(output->mode);
    repaint(*output);
  }
  publish_outputs();
}
bool RuntimeBackend::rebuild_switch_shaders(std::string* error) {
  renderer::ShaderSources sources;
  if (!load_shader_sources(*config_, &sources, error)) return false;
  std::vector<DrmDevice*> prepared;
  for (auto& device : devices_) {
    if (device->renderer == nullptr || device->outputs.empty()) continue;
    auto* output = device->outputs.front().get();
    if (output->egl_surface == EGL_NO_SURFACE ||
        eglMakeCurrent(device->egl_display, output->egl_surface, output->egl_surface,
                       device->egl_context) != EGL_TRUE ||
        !device->renderer->prepare_shaders(sources)) {
      if (error != nullptr && error->empty())
        *error = device->renderer == nullptr ? "renderer is unavailable" : device->renderer->last_error();
      for (auto* ready : prepared) {
        auto* ready_output = ready->outputs.front().get();
        if (eglMakeCurrent(ready->egl_display, ready_output->egl_surface, ready_output->egl_surface,
                           ready->egl_context) == EGL_TRUE)
          ready->renderer->discard_shaders();
      }
      return false;
    }
    prepared.push_back(device.get());
  }
  if (prepared.empty()) {
    if (error != nullptr) *error = "no active OpenGL renderer";
    return false;
  }
  for (auto* device : prepared) {
    auto* output = device->outputs.front().get();
    if (eglMakeCurrent(device->egl_display, output->egl_surface, output->egl_surface,
                       device->egl_context) == EGL_TRUE)
      device->renderer->commit_shaders();
    for (auto& item : device->outputs) repaint(*item);
  }
  return true;
}
void RuntimeBackend::show_error(std::string message) {
  auto popup = render_error_popup(message);
  if (!popup) return;
  error_popup_ = std::move(popup);
  error_popup_visible_ = true;
  ++error_popup_generation_;
  if (error_timer_ < 0) error_timer_ = event_loop_->add_timer([this] { on_error_timeout(this); });
  event_loop_->update_timer(error_timer_, 7000);
  for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
}
void RuntimeBackend::clear_error() {
  if (!error_popup_visible_) return;
  error_popup_visible_ = false;
  for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
}
int RuntimeBackend::on_error_timeout(void* data) {
  static_cast<RuntimeBackend*>(data)->clear_error();
  return 0;
}
void RuntimeBackend::set_input_target(CompositorServer* compositor) { input_target_ = compositor; }
void RuntimeBackend::set_dmabuf_target(CompositorServer* compositor) {
  dmabuf_target_ = compositor;
  publish_dmabuf_importer();
}
void RuntimeBackend::set_cursor_shape(const char* xcursor_name) {
  if (xcursor_name == nullptr) {
    cursor_visible_ = false;
    for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
    return;
  }
  cursor_visible_ = true;
  cursor_shape_ = xcursor_name;
  for (auto& device : devices_) for (auto& output : device->outputs) {
    if (output->egl_surface == EGL_NO_SURFACE ||
        eglMakeCurrent(device->egl_display, output->egl_surface, output->egl_surface, device->egl_context) != EGL_TRUE) continue;
    XcursorImages* cursor = XcursorLibraryLoadImages(xcursor_name, cursor_theme_.c_str(), cursor_size_);
    if (cursor == nullptr || cursor->nimage == 0 || cursor->images[0] == nullptr) {
      if (cursor != nullptr) XcursorImagesDestroy(cursor);
      continue;
    }
    const XcursorImage* image = cursor->images[0];
    if (output->cursor_texture == 0) glGenTextures(1, &output->cursor_texture);
    output->cursor_width = static_cast<std::int32_t>(image->width);
    output->cursor_height = static_cast<std::int32_t>(image->height);
    output->cursor_hotspot_x = static_cast<std::int32_t>(image->xhot);
    output->cursor_hotspot_y = static_cast<std::int32_t>(image->yhot);
    glBindTexture(GL_TEXTURE_2D, output->cursor_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, output->cursor_width, output->cursor_height, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, image->pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    XcursorImagesDestroy(cursor);
    repaint(*output);
  }
}
bool RuntimeBackend::set_cursor_theme(const std::string& theme, std::uint32_t size,
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
  if (cursor_visible_) set_cursor_shape(cursor_shape_.c_str());
  return true;
}
void RuntimeBackend::set_cursor_position(std::int32_t x, std::int32_t y) {
  cursor_x_ = x;
  cursor_y_ = y;
  for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
}

bool RuntimeBackend::capture_output(OutputCapture& capture, bool overlay_cursor) {
  const auto requested = capture.output;
  DrmOutput* selected = nullptr;
  for (const auto& device : devices_) for (const auto& output : device->outputs)
    if ((!requested || output->output.id == requested) && output->capture_ready &&
        output->egl_surface != EGL_NO_SURFACE && output->mode.hdisplay > 0 && output->mode.vdisplay > 0) {
      if (selected == nullptr || output->output.connector < selected->output.connector) selected = output.get();
    }
  if (selected == nullptr || eglMakeCurrent(selected->device->egl_display, selected->egl_surface,
      selected->egl_surface, selected->device->egl_context) != EGL_TRUE) return false;
  capture.output = selected->output.id;
  capture.width = static_cast<std::uint32_t>(selected->mode.hdisplay);
  capture.height = static_cast<std::uint32_t>(selected->mode.vdisplay);
  capture.pixels.resize(static_cast<std::size_t>(capture.width) * capture.height);
  GLuint capture_texture = selected->capture_texture;
  if (overlay_cursor && cursor_visible_) {
    if (selected->cursor_texture == 0 || selected->device->renderer == nullptr) return false;
    clear_gl_errors();
    if (selected->capture_cursor_texture == 0) glGenTextures(1, &selected->capture_cursor_texture);
    glBindTexture(GL_TEXTURE_2D, selected->capture_cursor_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, selected->mode.hdisplay, selected->mode.vdisplay,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    GLint previous_read = 0, previous_draw = 0, previous_viewport[4]{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    GLuint read_framebuffer = 0, draw_framebuffer = 0;
    glGenFramebuffers(1, &read_framebuffer);
    glGenFramebuffers(1, &draw_framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           selected->capture_texture, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           selected->capture_cursor_texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                          glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
      glBlitFramebuffer(0, 0, selected->mode.hdisplay, selected->mode.vdisplay,
                        0, 0, selected->mode.hdisplay, selected->mode.vdisplay,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
      renderer::FramePlan cursor_frame;
      cursor_frame.draws.push_back({std::numeric_limits<std::uint64_t>::max(),
                                    {{static_cast<std::int32_t>(std::lround(cursor_x_)) - selected->cursor_hotspot_x,
                                      static_cast<std::int32_t>(std::lround(cursor_y_)) - selected->cursor_hotspot_y},
                                     {static_cast<std::uint32_t>(selected->cursor_width),
                                      static_cast<std::uint32_t>(selected->cursor_height)}},
                                    1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, selected->cursor_texture});
      transform_frame(cursor_frame, config_->output_for(selected->output.connector),
                      {capture.width, capture.height},
                      {selected->output.logical_x, selected->output.logical_y});
      glViewport(0, 0, selected->mode.hdisplay, selected->mode.vdisplay);
      if (!selected->device->renderer->render(cursor_frame, {capture.width, capture.height})) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
        glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
        glDeleteFramebuffers(1, &read_framebuffer);
        glDeleteFramebuffers(1, &draw_framebuffer);
        return false;
      }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    glDeleteFramebuffers(1, &read_framebuffer);
    glDeleteFramebuffers(1, &draw_framebuffer);
    const auto cursor_error = glGetError();
    if (!complete || cursor_error != GL_NO_ERROR) return false;
    capture_texture = selected->capture_cursor_texture;
  }
  if (capture_texture == 0) return false;
  clear_gl_errors();
  bool framebuffer_complete = true;
  GLint previous_framebuffer = 0;
  GLuint framebuffer = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_framebuffer);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         capture_texture, 0);
  framebuffer_complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (framebuffer_complete) {
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, static_cast<GLsizei>(capture.width), static_cast<GLsizei>(capture.height),
                 GL_BGRA, GL_UNSIGNED_BYTE, capture.pixels.data());
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
  glDeleteFramebuffers(1, &framebuffer);
  const auto read_error = glGetError();
  if (!framebuffer_complete || read_error != GL_NO_ERROR) {
    capture = {};
    return false;
  }
  for (std::uint32_t y = 0; y < capture.height / 2; ++y) {
    auto top = capture.pixels.begin() + static_cast<std::ptrdiff_t>(y * capture.width);
    auto bottom = capture.pixels.begin() + static_cast<std::ptrdiff_t>((capture.height - y - 1) * capture.width);
    std::swap_ranges(top, top + capture.width, bottom);
  }
  for (auto& pixel : capture.pixels) pixel |= 0xff000000U;
  return true;
}
bool RuntimeBackend::capture_toplevel(std::uint64_t id, OutputCapture& capture, bool overlay_cursor) {
  DrmOutput* selected = nullptr;
  DrmOutput::ShmTexture* root_surface = nullptr;
  for (const auto& device : devices_) for (const auto& output : device->outputs) {
    const auto root = std::find_if(output->shm_textures.begin(), output->shm_textures.end(), [id](auto& surface) {
      return surface.id == id && surface.root_id == id && surface.toplevel && !surface.removed &&
             surface.content_ready && surface.texture != 0 && surface.width > 0 && surface.height > 0;
    });
    if (root != output->shm_textures.end()) { selected = output.get(); root_surface = &*root; break; }
  }
  if (selected == nullptr || root_surface == nullptr || selected->device->renderer == nullptr ||
      selected->egl_surface == EGL_NO_SURFACE ||
      eglMakeCurrent(selected->device->egl_display, selected->egl_surface, selected->egl_surface,
                     selected->device->egl_context) != EGL_TRUE) return false;

  capture.output = selected->output.id;
  capture.width = static_cast<std::uint32_t>(root_surface->width);
  capture.height = static_cast<std::uint32_t>(root_surface->height);
  renderer::FramePlan frame;
  for (const auto& surface : selected->shm_textures) {
    if (surface.root_id != id || surface.removed || surface.popup || surface.texture == 0 ||
        surface.width <= 0 || surface.height <= 0) continue;
    renderer::DrawCall draw{surface.id,
        {{surface.x - root_surface->x, surface.y - root_surface->y},
         {static_cast<std::uint32_t>(surface.width), static_cast<std::uint32_t>(surface.height)}},
        1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, surface.texture, 0.0F, 0.0F,
        {surface.source_left, surface.source_top, surface.source_right, surface.source_bottom}};
    draw.opaque = surface.opaque;
    frame.draws.push_back(draw);
  }
  if (overlay_cursor && cursor_visible_ && selected->cursor_texture != 0 &&
      root_surface->assigned_content_width > 0 && root_surface->assigned_content_height > 0) {
    const double scale_x = static_cast<double>(root_surface->width) / root_surface->assigned_content_width;
    const double scale_y = static_cast<double>(root_surface->height) / root_surface->assigned_content_height;
    const auto cursor_x = static_cast<std::int32_t>(std::lround(
        (cursor_x_ - root_surface->assigned_content_x) * scale_x - selected->cursor_hotspot_x));
    const auto cursor_y = static_cast<std::int32_t>(std::lround(
        (cursor_y_ - root_surface->assigned_content_y) * scale_y - selected->cursor_hotspot_y));
    frame.draws.push_back({std::numeric_limits<std::uint64_t>::max(),
                           {{cursor_x, cursor_y},
                            {static_cast<std::uint32_t>(selected->cursor_width),
                             static_cast<std::uint32_t>(selected->cursor_height)}},
                           1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, selected->cursor_texture});
  }
  if (frame.draws.empty() || !selected->device->renderer->render_pixels(
          frame, {capture.width, capture.height}, &capture.pixels)) {
    capture = {};
    return false;
  }
  for (auto& pixel : capture.pixels) pixel |= 0xff000000U;
  return true;
}
bool RuntimeBackend::supports_embedded_cursor() const {
  // Capability is known before asynchronous DRM output setup completes.
  return true;
}
void RuntimeBackend::publish_dmabuf_importer() {
  if (dmabuf_target_ == nullptr) return;
  std::vector<DrmDevice*> candidates;
  for (const auto& device : devices_) if (device->dmabuf_importer != nullptr && device->dmabuf_importer->supported())
    candidates.push_back(device.get());
  std::sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) { return left->path < right->path; });
  for (auto* device : candidates) {
    std::optional<dev_t> main_device;
    drmDevicePtr drm_device = nullptr;
    const bool got_device = drmGetDevice2(device->fd, 0, &drm_device) == 0 && drm_device != nullptr;
    const bool has_render_node = got_device && (drm_device->available_nodes & (1 << DRM_NODE_RENDER)) != 0 &&
                                 drm_device->nodes[DRM_NODE_RENDER] != nullptr;
    struct stat status {};
    if (has_render_node) {
      if (stat(drm_device->nodes[DRM_NODE_RENDER], &status) == 0) main_device = status.st_rdev;
    } else if (fstat(device->fd, &status) == 0) {
      main_device = status.st_rdev;
    }
    if (drm_device != nullptr) drmFreeDevice(&drm_device);
    dmabuf_target_->set_dmabuf_feedback(device->dmabuf_importer->supported_formats(), main_device);
    return;
  }
  dmabuf_target_->set_dmabuf_feedback({}, std::nullopt);
}
void RuntimeBackend::publish_outputs() {
  std::vector<OutputInfo> outputs;
  for (const auto& device : devices_) for (const auto& output : device->outputs)
    if (output->output.enabled) outputs.push_back(output->output);
  std::stable_sort(outputs.begin(), outputs.end(), [](const OutputInfo& left, const OutputInfo& right) {
    return left.connector < right.connector;
  });
  std::int32_t x = 0;
  std::uint32_t height = 0;
  for (auto& output : outputs) {
    output.logical_x = x;
    for (auto& device : devices_) for (auto& candidate : device->outputs)
      if (candidate->output.id == output.id) candidate->output.logical_x = x;
    x += static_cast<std::int32_t>(output.logical_width);
    height = std::max(height, output.logical_height);
  }
  output_width_ = static_cast<std::uint32_t>(std::max(1, x));
  output_height_ = std::max(1U, height);
  if (!outputs.empty()) {
    physical_output_width_ = outputs.front().physical_width;
    physical_output_height_ = outputs.front().physical_height;
  }
  if (input_target_ != nullptr) input_target_->set_outputs(std::move(outputs));
}
void RuntimeBackend::present(const ShmBufferView& buffer) {
  for (auto& device : devices_) for (auto& output_ptr : device->outputs) {
    auto* card = output_ptr.get();
    if (buffer.output && buffer.output != card->output.id) continue;
    if (device->renderer == nullptr || card->egl_surface == EGL_NO_SURFACE ||
        eglMakeCurrent(device->egl_display, card->egl_surface, card->egl_surface, device->egl_context) != EGL_TRUE) {
      continue;
    }
    if (buffer.tag_transition) {
      if (buffer.tag_transition_ready) {
        card->tag_transition_preparing = false;
        card->animations.start_tag_transition(buffer.tag_transition_direction, monotonic_ms());
        repaint(*card);
        continue;
      }
      for (auto& item : card->shm_textures) {
        const auto root = std::find_if(card->shm_textures.begin(), card->shm_textures.end(), [&item](const auto& candidate) { return candidate.id == item.root_id; });
        if (root != card->shm_textures.end() && root->toplevel) {
          item.tag_outgoing = true;
        }
      }
      card->tag_transition_preparing = true;
      continue;
    }
    auto surface = std::find_if(card->shm_textures.begin(), card->shm_textures.end(), [&buffer](const auto& item) {
      return item.id == buffer.surface_id;
    });
    if (buffer.pixels == nullptr && buffer.dmabuf == nullptr) {
      if (buffer.toplevel && buffer.assigned_content_width > 0 && buffer.assigned_content_height > 0) {
        DrmOutput::ShmTexture updated = surface == card->shm_textures.end() ? DrmOutput::ShmTexture{} : std::move(*surface);
        const bool retain_content = surface != card->shm_textures.end() && updated.texture != 0;
        const bool closing = retain_content && !buffer.content_ready;
        updated.id = buffer.surface_id;
        updated.root_id = buffer.root_surface_id;
        updated.assigned_tile_x = buffer.assigned_tile_x; updated.assigned_tile_y = buffer.assigned_tile_y;
        updated.assigned_tile_width = buffer.assigned_tile_width; updated.assigned_tile_height = buffer.assigned_tile_height;
        updated.assigned_content_x = buffer.assigned_content_x; updated.assigned_content_y = buffer.assigned_content_y;
        updated.assigned_content_width = buffer.assigned_content_width; updated.assigned_content_height = buffer.assigned_content_height;
        if (!retain_content) {
          updated.width = buffer.assigned_content_width;
          updated.height = buffer.assigned_content_height;
        }
        updated.x = buffer.x;
         updated.y = buffer.y;
         if (!retain_content) {
           updated.geometry_x = buffer.window_geometry_x;
           updated.geometry_y = buffer.window_geometry_y;
           updated.geometry_width = buffer.window_geometry_width;
           updated.geometry_height = buffer.window_geometry_height;
         }
        updated.stack_index = buffer.stack_index;
        updated.scene_order = buffer.scene_order;
        updated.background_blur_radius = buffer.background_blur_radius;
        updated.glass = buffer.glass;
        updated.toplevel = true;
        updated.popup = buffer.popup;
        updated.focused = buffer.focused;
        updated.window_shader = buffer.window_shader;
        updated.border_shader = buffer.border_shader;
        if (!retain_content) updated.content_ready = buffer.content_ready;
        updated.removed = closing; updated.tag_outgoing = false;
        updated.suppress_geometry_animation = buffer.suppress_geometry_animation;
        updated.track_geometry_animation = buffer.track_geometry_animation;
        updated.camera_scale = buffer.camera_scale;
        updated.camera_center_x = buffer.camera_center_x;
        updated.camera_center_y = buffer.camera_center_y;
        if (!retain_content) {
          updated.texture = 0;
          updated.owns_texture = false;
          updated.pixels.clear();
        }
        if (surface == card->shm_textures.end()) card->shm_textures.push_back(std::move(updated)); else *surface = std::move(updated);
        if (closing) {
          for (auto& item : card->shm_textures) if (item.root_id == buffer.surface_id) {
            if (item.texture != 0 && !item.owns_texture) {
              const GLuint copy = snapshot_texture(item.texture, item.buffer_width, item.buffer_height);
              if (copy != 0) { item.texture = copy; item.owns_texture = true; }
              else item.texture = 0;
              const auto imported = device->dmabuf_textures.find(item.id);
              if (imported != device->dmabuf_textures.end()) {
                if (imported->second.texture != 0 && device->dmabuf_importer != nullptr)
                  (void)device->dmabuf_importer->release(imported->second.texture);
                device->dmabuf_textures.erase(imported);
              }
            }
            item.content_ready = true;
            item.removed = true;
          }
        }
      } else if (surface != card->shm_textures.end()) {
        const auto retained_root = std::find_if(card->shm_textures.begin(), card->shm_textures.end(),
                                                [&](const auto& item) { return item.id == surface->root_id; });
        if (surface->tag_outgoing || (retained_root != card->shm_textures.end() && retained_root->removed)) {
          // Keep this scene intact until the output-level slide completes.
        } else if (surface->toplevel && config_->animations.enabled && config_->animations.close) {
          for (auto& item : card->shm_textures) if (item.root_id == surface->id) {
            if (item.texture != 0 && !item.owns_texture) {
              const GLuint copy = snapshot_texture(item.texture, item.buffer_width, item.buffer_height);
              if (copy != 0) { item.texture = copy; item.owns_texture = true; }
              else item.texture = 0;
              const auto imported = device->dmabuf_textures.find(item.id);
              if (imported != device->dmabuf_textures.end()) {
                if (imported->second.texture != 0 && device->dmabuf_importer != nullptr)
                  (void)device->dmabuf_importer->release(imported->second.texture);
                device->dmabuf_textures.erase(imported);
              }
            }
            item.removed = true;
          }
        } else {
          if (surface->texture != 0 && surface->owns_texture) glDeleteTextures(1, &surface->texture);
          card->shm_textures.erase(surface);
        }
      }
      if (!card->tag_transition_preparing) repaint(*card);
      continue;
    }
    if (buffer.width <= 0 || buffer.height <= 0 || buffer.width > std::numeric_limits<std::int32_t>::max() / 4 ||
        (buffer.pixels != nullptr &&
         ((buffer.format != protocol::WL_SHM_FORMAT_ARGB8888 && buffer.format != protocol::WL_SHM_FORMAT_XRGB8888) ||
          buffer.stride < buffer.width * 4))) {
      continue;
    }
    DrmOutput::ShmTexture updated = surface == card->shm_textures.end() ? DrmOutput::ShmTexture{} : std::move(*surface);
    if (buffer.pixels != nullptr) {
      const auto imported = device->dmabuf_textures.find(buffer.surface_id);
      if (imported != device->dmabuf_textures.end()) {
        if (imported->second.texture != 0 && device->dmabuf_importer != nullptr)
          (void)device->dmabuf_importer->release(imported->second.texture);
        device->dmabuf_textures.erase(imported);
      }
    }
    const bool resized = updated.buffer_width != buffer.width || updated.buffer_height != buffer.height;
    updated.id = buffer.surface_id;
    updated.root_id = buffer.root_surface_id;
    updated.assigned_tile_x = buffer.assigned_tile_x; updated.assigned_tile_y = buffer.assigned_tile_y;
    updated.assigned_tile_width = buffer.assigned_tile_width; updated.assigned_tile_height = buffer.assigned_tile_height;
    updated.assigned_content_x = buffer.assigned_content_x; updated.assigned_content_y = buffer.assigned_content_y;
    updated.assigned_content_width = buffer.assigned_content_width; updated.assigned_content_height = buffer.assigned_content_height;
    updated.width = buffer.logical_width;
    updated.height = buffer.logical_height;
    updated.buffer_width = buffer.width;
    updated.buffer_height = buffer.height;
    updated.source_left = buffer.source_left; updated.source_top = buffer.source_top;
    updated.source_right = buffer.source_right; updated.source_bottom = buffer.source_bottom;
    updated.x = buffer.x;
    updated.y = buffer.y;
    updated.geometry_x = buffer.window_geometry_x;
    updated.geometry_y = buffer.window_geometry_y;
    updated.geometry_width = buffer.window_geometry_width;
    updated.geometry_height = buffer.window_geometry_height;
    updated.format = buffer.format;
    updated.stack_index = buffer.stack_index;
    updated.scene_order = buffer.scene_order;
    updated.scene_rank = buffer.scene_rank; updated.layer_priority = buffer.layer_priority;
    updated.root_order = buffer.root_order; updated.tree_order = buffer.tree_order;
    updated.compositor_opacity = buffer.compositor_opacity;
    updated.background_blur_radius = buffer.background_blur_radius;
    updated.background_blur_ignore_alpha = buffer.background_blur_ignore_alpha;
    updated.glass = buffer.glass;
    updated.layer_surface = buffer.layer_surface;
    updated.toplevel = buffer.toplevel;
    updated.popup = buffer.popup;
    updated.fullscreen = buffer.fullscreen;
    updated.suppress_geometry_animation = buffer.suppress_geometry_animation;
    updated.track_geometry_animation = buffer.track_geometry_animation;
    updated.camera_scale = buffer.camera_scale;
    updated.camera_center_x = buffer.camera_center_x;
    updated.camera_center_y = buffer.camera_center_y;
    updated.focused = buffer.focused;
    updated.window_shader = buffer.window_shader;
    updated.border_shader = buffer.border_shader;
    updated.opaque = buffer.opaque;
    updated.content_ready = buffer.content_ready;
    updated.removed = false; updated.tag_outgoing = false;
    if (buffer.pixels != nullptr && (resized || updated.pixels.empty())) updated.pixels.resize(static_cast<std::size_t>(buffer.width) * buffer.height);
    const int left = std::clamp(resized ? 0 : buffer.damage_x, 0, buffer.width);
    const int top = std::clamp(resized ? 0 : buffer.damage_y, 0, buffer.height);
    const int right = std::clamp(resized ? buffer.width : buffer.damage_x + buffer.damage_width, left, buffer.width);
    const int bottom = std::clamp(resized ? buffer.height : buffer.damage_y + buffer.damage_height, top, buffer.height);
    for (int y = top; buffer.pixels != nullptr && y < bottom; ++y) {
      std::memcpy(updated.pixels.data() + static_cast<std::size_t>(y) * buffer.width + left,
                  buffer.pixels + static_cast<std::size_t>(y) * buffer.stride + left * sizeof(std::uint32_t),
                  static_cast<std::size_t>(right - left) * sizeof(std::uint32_t));
      if (buffer.format == protocol::WL_SHM_FORMAT_XRGB8888) {
        auto* row = updated.pixels.data() + static_cast<std::size_t>(y) * buffer.width;
        for (int x = left; x < right; ++x) row[x] |= 0xff000000U;
      }
    }
    if (buffer.pixels != nullptr) {
      if (updated.texture != 0 && !updated.owns_texture) updated.texture = 0;
      if (updated.texture == 0) glGenTextures(1, &updated.texture);
      updated.owns_texture = true;
      glBindTexture(GL_TEXTURE_2D, updated.texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, updated.buffer_width, updated.buffer_height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                   updated.pixels.data());
      glBindTexture(GL_TEXTURE_2D, 0);
    } else {
      if (updated.texture != 0 && updated.owns_texture) glDeleteTextures(1, &updated.texture);
      auto& imported = device->dmabuf_textures[buffer.surface_id];
      if (imported.generation != buffer.buffer_generation) {
        if (imported.texture != 0 && device->dmabuf_importer != nullptr)
          (void)device->dmabuf_importer->release(imported.texture);
        imported = {.generation = buffer.buffer_generation};
        if (device->dmabuf_importer != nullptr && device->dmabuf_importer->supported())
          imported.texture = device->dmabuf_importer->import(*buffer.dmabuf);
        imported.failed = imported.texture == 0;
        if (imported.failed) {
          std::fprintf(stderr, "zwwm: DRM %s: DMA-BUF import failed for surface %llu generation %llu: %s; skipping it on this device\n",
                       device->path.c_str(), static_cast<unsigned long long>(buffer.surface_id),
                       static_cast<unsigned long long>(buffer.buffer_generation),
                       device->dmabuf_importer == nullptr ? "importer unavailable" : device->dmabuf_importer->last_error().c_str());
        }
      }
      updated.texture = imported.failed ? 0 : imported.texture;
      updated.owns_texture = false;
      updated.pixels.clear();
    }
    if (surface == card->shm_textures.end()) card->shm_textures.push_back(std::move(updated)); else *surface = std::move(updated);
    std::stable_sort(card->shm_textures.begin(), card->shm_textures.end(), [](const auto& left, const auto& right) {
      return std::tie(left.scene_rank, left.layer_priority, left.root_order, left.tree_order) <
             std::tie(right.scene_rank, right.layer_priority, right.root_order, right.tree_order);
    });
    if (!card->tag_transition_preparing) repaint(*card);
  }
  if (buffer.pixels == nullptr && buffer.dmabuf == nullptr && !buffer.toplevel) {
    for (auto& device : devices_) {
      const auto imported = device->dmabuf_textures.find(buffer.surface_id);
      if (imported == device->dmabuf_textures.end()) continue;
      const auto retained_by_transition = [&](const auto& outputs) {
        return std::any_of(outputs.begin(), outputs.end(), [&](const auto& output) {
          return std::any_of(output->shm_textures.begin(), output->shm_textures.end(),
                             [&](const auto& surface) {
                               return surface.id == buffer.surface_id && (surface.tag_outgoing || surface.removed);
                             });
        });
      };
      if (retained_by_transition(device->outputs) || retained_by_transition(device->retired_outputs)) continue;
      DrmOutput* output = device->outputs.empty() ? nullptr : device->outputs.front().get();
      if (output != nullptr && output->egl_surface != EGL_NO_SURFACE &&
          eglMakeCurrent(device->egl_display, output->egl_surface, output->egl_surface, device->egl_context) == EGL_TRUE &&
          imported->second.texture != 0 && device->dmabuf_importer != nullptr)
        (void)device->dmabuf_importer->release(imported->second.texture);
      device->dmabuf_textures.erase(imported);
    }
  }
}

int RuntimeBackend::on_seat_fd(int, std::uint32_t, void* data) {
  static_cast<RuntimeBackend*>(data)->dispatch_seat();
  return 0;
}

int RuntimeBackend::on_udev_fd(int, std::uint32_t, void* data) {
  static_cast<RuntimeBackend*>(data)->dispatch_udev();
  return 0;
}

int RuntimeBackend::on_libinput_fd(int, std::uint32_t, void* data) {
  static_cast<RuntimeBackend*>(data)->dispatch_libinput();
  return 0;
}

int RuntimeBackend::on_drm_fd(int, std::uint32_t, void* data) {
  auto* device = static_cast<DrmDevice*>(data);
  device->backend->dispatch_drm(*device);
  return 0;
}

void RuntimeBackend::enable_seat(libseat*, void* data) {
  auto* backend = static_cast<RuntimeBackend*>(data);
  std::fprintf(stderr, "zwwm: direct backend: seat activated, discovering DRM outputs\n");
  if (backend->libinput_ != nullptr && backend->libinput_suspended_) {
    if (libinput_resume(backend->libinput_) == 0) {
      backend->libinput_suspended_ = false;
    } else {
      backend->last_error_ = "libinput could not resume after seat activation";
    }
  }
  backend->active_ = true;
  backend->resume_drm_devices();
  if (backend->udev_ != nullptr) {
    backend->discover_drm_devices();
  }
}

void RuntimeBackend::disable_seat(libseat* seat, void* data) {
  auto* backend = static_cast<RuntimeBackend*>(data);
  backend->active_ = false;
  backend->left_control_pressed_ = false;
  backend->right_control_pressed_ = false;
  backend->left_alt_pressed_ = false;
  backend->right_alt_pressed_ = false;
  backend->consumed_vt_key_ = 0;
  if (backend->input_target_ != nullptr) backend->input_target_->input_reset(true);
  if (backend->libinput_ != nullptr && !backend->libinput_suspended_) {
    libinput_suspend(backend->libinput_);
    backend->libinput_suspended_ = true;
  }
  backend->suspend_drm_devices();
  libseat_disable_seat(seat);
}

int RuntimeBackend::open_restricted(const char* path, int flags, void* data) {
  return static_cast<RuntimeBackend*>(data)->open_device(path, flags);
}

void RuntimeBackend::close_restricted(int fd, void* data) { static_cast<RuntimeBackend*>(data)->close_device(fd); }

void RuntimeBackend::dispatch_seat() {
  if (libseat_dispatch(seat_, 0) < 0) {
    last_error_ = "libseat event dispatch failed";
  }
}

void RuntimeBackend::dispatch_udev() {
  while (udev_device* device = udev_monitor_receive_device(udev_monitor_)) {
    const char* path = udev_device_get_devnode(device);
    const char* action = udev_device_get_action(device);
    if (action != nullptr) {
      udev_device* parent = is_drm_card(path) ? nullptr :
          udev_device_get_parent_with_subsystem_devtype(device, "drm", "drm_minor");
      const char* card_path = is_drm_card(path) ? path : parent == nullptr ? nullptr : udev_device_get_devnode(parent);
      const auto rescan = [&] {
        const auto drm_device = std::find_if(devices_.begin(), devices_.end(), [card_path](const auto& item) {
          return item->path == card_path;
        });
        if (drm_device == devices_.end()) { add_drm_device(card_path); return; }
        rescan_drm_device(**drm_device);
        publish_outputs();
        publish_dmabuf_importer();
        for (auto& output : (*drm_device)->outputs) repaint(*output);
      };
      if (std::strcmp(action, "add") == 0 && active_ && is_drm_card(card_path) && is_drm_card(path)) {
        add_drm_device(card_path);
      } else if (std::strcmp(action, "remove") == 0 && is_drm_card(card_path) && is_drm_card(path)) {
        remove_drm_device(card_path);
      } else if (active_ && is_drm_card(card_path) &&
                 (std::strcmp(action, "add") == 0 || std::strcmp(action, "remove") == 0 ||
                  std::strcmp(action, "change") == 0)) rescan();
      else if (active_ && !is_drm_card(card_path) &&
               (std::strcmp(action, "add") == 0 || std::strcmp(action, "remove") == 0 ||
                std::strcmp(action, "change") == 0)) {
        for (auto& drm_device : devices_) rescan_drm_device(*drm_device);
        publish_outputs();
        publish_dmabuf_importer();
        for (auto& drm_device : devices_) for (auto& output : drm_device->outputs) repaint(*output);
      }
    }
    udev_device_unref(device);
  }
}

void RuntimeBackend::dispatch_libinput() {
  if (libinput_dispatch(libinput_) != 0) {
    last_error_ = "libinput event dispatch failed";
    return;
  }
  while (libinput_event* event = libinput_get_event(libinput_)) {
    if (input_target_ != nullptr) {
      const auto type = libinput_event_get_type(event);
      if (type == LIBINPUT_EVENT_POINTER_MOTION) {
        auto* pointer = libinput_event_get_pointer_event(event);
        const double relative_dx = libinput_event_pointer_get_dx(pointer);
        const double relative_dy = libinput_event_pointer_get_dy(pointer);
        const double relative_dx_unaccelerated = libinput_event_pointer_get_dx_unaccelerated(pointer);
        const double relative_dy_unaccelerated = libinput_event_pointer_get_dy_unaccelerated(pointer);
        const OutputConfig* pointer_config = &config_->output;
        for (const auto& device : devices_) for (const auto& output : device->outputs) {
          if (cursor_x_ >= output->output.logical_x && cursor_y_ >= output->output.logical_y &&
              cursor_x_ < output->output.logical_x + output->output.logical_width &&
              cursor_y_ < output->output.logical_y + output->output.logical_height)
            pointer_config = &config_->output_for(output->output.connector);
        }
        double dx = relative_dx * 1000.0 / pointer_config->scale_per_mille;
        double dy = relative_dy * 1000.0 / pointer_config->scale_per_mille;
        double dx_unaccelerated = relative_dx_unaccelerated * 1000.0 / pointer_config->scale_per_mille;
        double dy_unaccelerated = relative_dy_unaccelerated * 1000.0 / pointer_config->scale_per_mille;
        const auto transform_delta = [pointer_config](double& x, double& y) {
          if (pointer_config->transform == OutputTransform::rotate_90) { const double next = y; y = -x; x = next; }
          else if (pointer_config->transform == OutputTransform::rotate_180) { x = -x; y = -y; }
          else if (pointer_config->transform == OutputTransform::rotate_270) { const double next = -y; y = x; x = next; }
        };
        transform_delta(dx, dy);
        transform_delta(dx_unaccelerated, dy_unaccelerated);
        input_target_->pointer_relative_motion(libinput_event_pointer_get_time_usec(pointer), relative_dx, relative_dy,
                                               relative_dx_unaccelerated, relative_dy_unaccelerated);
        const bool panning = input_target_->pointer_pan_motion(dx, dy);
        if (!panning) {
          cursor_x_ = std::clamp(cursor_x_ + dx, 0.0, static_cast<double>(output_width_ - 1));
          cursor_y_ = std::clamp(cursor_y_ + dy, 0.0, static_cast<double>(output_height_ - 1));
        }
        const bool inside = std::any_of(devices_.begin(), devices_.end(), [&](const auto& device) {
          return std::any_of(device->outputs.begin(), device->outputs.end(), [&](const auto& card) {
          const auto& output = card->output;
          return cursor_x_ >= output.logical_x && cursor_y_ >= output.logical_y &&
                 cursor_x_ < output.logical_x + output.logical_width &&
                 cursor_y_ < output.logical_y + output.logical_height;
          });
        });
        std::vector<DrmOutput*> outputs;
        for (auto& device : devices_) for (auto& output : device->outputs) outputs.push_back(output.get());
        if (!inside && !outputs.empty()) {
          const auto nearest = std::min_element(outputs.begin(), outputs.end(), [&](const auto* left, const auto* right) {
            const auto distance = [&](const auto& card) {
              const auto& output = card->output;
              const double x = std::clamp(cursor_x_, static_cast<double>(output.logical_x),
                                          static_cast<double>(output.logical_x + output.logical_width - 1));
              const double y = std::clamp(cursor_y_, static_cast<double>(output.logical_y),
                                          static_cast<double>(output.logical_y + output.logical_height - 1));
              return std::abs(cursor_x_ - x) + std::abs(cursor_y_ - y);
            };
            return distance(left) < distance(right);
          });
          const auto& output = (*nearest)->output;
          cursor_x_ = std::clamp(cursor_x_, static_cast<double>(output.logical_x),
                                 static_cast<double>(output.logical_x + output.logical_width - 1));
          cursor_y_ = std::clamp(cursor_y_, static_cast<double>(output.logical_y),
                                 static_cast<double>(output.logical_y + output.logical_height - 1));
        }
        if (!panning)
          input_target_->pointer_motion_global(libinput_event_pointer_get_time(pointer), static_cast<std::int32_t>(std::lround(cursor_x_)), static_cast<std::int32_t>(std::lround(cursor_y_)));
        for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
      } else if (type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
        auto* pointer = libinput_event_get_pointer_event(event);
        const renderer::Point physical{
            static_cast<std::int32_t>(libinput_event_pointer_get_absolute_x_transformed(pointer, physical_output_width_)),
            static_cast<std::int32_t>(libinput_event_pointer_get_absolute_y_transformed(pointer, physical_output_height_))};
        const auto logical = config_->output.physical_to_logical(physical, {physical_output_width_, physical_output_height_});
        cursor_x_ = std::clamp(static_cast<double>(logical.x), 0.0, static_cast<double>(output_width_ - 1));
        cursor_y_ = std::clamp(static_cast<double>(logical.y), 0.0, static_cast<double>(output_height_ - 1));
        input_target_->pointer_motion_global(libinput_event_pointer_get_time(pointer), static_cast<std::int32_t>(std::lround(cursor_x_)), static_cast<std::int32_t>(std::lround(cursor_y_)));
        for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
      } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
        auto* pointer = libinput_event_get_pointer_event(event);
        input_target_->pointer_button(libinput_event_pointer_get_time(pointer), libinput_event_pointer_get_button(pointer), libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED ? protocol::WL_POINTER_BUTTON_STATE_PRESSED : protocol::WL_POINTER_BUTTON_STATE_RELEASED);
      } else if (type == LIBINPUT_EVENT_POINTER_AXIS) {
        auto* pointer = libinput_event_get_pointer_event(event);
        const auto source = libinput_event_pointer_get_axis_source(pointer);
        const auto wayland_source = source == LIBINPUT_POINTER_AXIS_SOURCE_FINGER ? protocol::WL_POINTER_AXIS_SOURCE_FINGER : source == LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS ? protocol::WL_POINTER_AXIS_SOURCE_CONTINUOUS : source == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT ? protocol::WL_POINTER_AXIS_SOURCE_WHEEL_TILT : protocol::WL_POINTER_AXIS_SOURCE_WHEEL;
        for (const auto axis : {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL}) {
          if (!libinput_event_pointer_has_axis(pointer, axis)) continue;
          const auto wl_axis = axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ? protocol::WL_POINTER_AXIS_VERTICAL_SCROLL : protocol::WL_POINTER_AXIS_HORIZONTAL_SCROLL;
          const auto value = libinput_event_pointer_get_axis_value(pointer, axis);
          const auto discrete = libinput_event_pointer_get_axis_value_discrete(pointer, axis);
          const auto value120 = discrete * 120;
          input_target_->pointer_axis(libinput_event_pointer_get_time(pointer), wl_axis, value, wayland_source, discrete, value120, value == 0.0);
        }
      } else if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
        auto* keyboard = libinput_event_get_keyboard_event(event);
        const auto key = libinput_event_keyboard_get_key(keyboard);
        const bool pressed = libinput_event_keyboard_get_key_state(keyboard) == LIBINPUT_KEY_STATE_PRESSED;
        if (key == KEY_LEFTCTRL) left_control_pressed_ = pressed;
        else if (key == KEY_RIGHTCTRL) right_control_pressed_ = pressed;
        else if (key == KEY_LEFTALT) left_alt_pressed_ = pressed;
        else if (key == KEY_RIGHTALT) right_alt_pressed_ = pressed;

        bool consumed = key == consumed_vt_key_;
        if (consumed && !pressed) consumed_vt_key_ = 0;
        if (!consumed && pressed && (left_control_pressed_ || right_control_pressed_) &&
            (left_alt_pressed_ || right_alt_pressed_)) {
          int vt = 0;
          if (key >= KEY_F1 && key <= KEY_F12) vt = static_cast<int>(key - KEY_F1 + 1);
          else if (key >= KEY_1 && key <= KEY_9) vt = static_cast<int>(key - KEY_1 + 1);
          else if (key == KEY_0) vt = 10;
          if (vt != 0 && libseat_switch_session(seat_, vt) == 0) {
            consumed_vt_key_ = key;
            consumed = true;
          }
        }
        if (!consumed)
          input_target_->keyboard_key(libinput_event_keyboard_get_time(keyboard), key,
                                      pressed ? protocol::WL_KEYBOARD_KEY_STATE_PRESSED : protocol::WL_KEYBOARD_KEY_STATE_RELEASED);
      }
    }
    libinput_event_destroy(event);
  }
}

void RuntimeBackend::dispatch_drm(DrmDevice& device) {
  drmEventContext events{};
  events.version = DRM_EVENT_CONTEXT_VERSION;
  events.page_flip_handler = [](int, unsigned int, unsigned int, unsigned int, void* data) {
    auto* output = static_cast<DrmOutput*>(data);
    output->device->backend->page_flip_complete(*output);
  };
  if (drmHandleEvent(device.fd, &events) != 0) {
    std::fprintf(stderr, "zwwm: failed to dispatch DRM event for %s\n", device.path.c_str());
  }
}

void RuntimeBackend::repaint(DrmOutput& card) {
  auto& device = *card.device;
  const auto& output_config = config_->output_for(card.output.connector);
  if (card.flip_pending) { card.needs_repaint = true; return; }
  if (card.crtc_id == 0 || card.connector_id == 0 || card.scanout_surface == nullptr ||
      card.egl_surface == EGL_NO_SURFACE || device.renderer == nullptr) return;
  if (eglMakeCurrent(device.egl_display, card.egl_surface, card.egl_surface, device.egl_context) != EGL_TRUE) {
    last_error_ = "could not make direct EGL context current";
    std::fprintf(stderr, "zwwm: %s\n", last_error_.c_str());
    return;
  }
  glViewport(0, 0, card.mode.hdisplay, card.mode.vdisplay);
  glClearColor(0.125F, 0.141F, 0.169F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  renderer::FramePlan frame;
  frame.draw_background = true;
  frame.damage.push_back({{0, 0}, {static_cast<std::uint32_t>(card.mode.hdisplay), static_cast<std::uint32_t>(card.mode.vdisplay)}});
  const renderer::Size physical{static_cast<std::uint32_t>(card.mode.hdisplay), static_cast<std::uint32_t>(card.mode.vdisplay)};
  const std::uint64_t now = monotonic_ms();
  frame.shader_time = static_cast<float>(now % 1000000U) / 1000.0F;
  const auto tag = card.animations.tag_transition(now);
  if (!tag) {
    for (auto& surface : card.shm_textures) {
      if (!surface.tag_outgoing) continue;
      card.animations.remove(surface.root_id);
      surface.removed = true;
    }
  }
  std::vector<AnimationTarget> animation_targets;
  animation_targets.reserve(card.shm_textures.size());
  for (const auto& surface : card.shm_textures) {
    const auto bounds = assigned_tile(surface);
    const renderer::Rect viewport{{card.output.logical_x, card.output.logical_y},
                                  {card.output.logical_width, card.output.logical_height}};
    if (tag && surface.toplevel && !backend_scene::intersects(bounds, viewport)) {
      card.animations.remove(surface.id);
      continue;
    }
    if (surface.toplevel && !surface.removed && surface.content_ready && surface.texture != 0)
      animation_targets.push_back(
          {surface.id, bounds, !surface.suppress_geometry_animation,
            !tag || surface.tag_outgoing, surface.track_geometry_animation});
  }
  card.animations.update(animation_targets, now);
  for (auto& surface : card.shm_textures) surface.track_geometry_animation = false;
  for (auto it = card.shm_textures.begin(); it != card.shm_textures.end();) {
    if (it->removed && !card.animations.retains(it->root_id)) {
      if (it->texture != 0 && it->owns_texture) glDeleteTextures(1, &it->texture);
      const auto imported = card.device->dmabuf_textures.find(it->id);
      if (imported != card.device->dmabuf_textures.end()) {
        if (imported->second.texture != 0 && card.device->dmabuf_importer != nullptr)
          (void)card.device->dmabuf_importer->release(imported->second.texture);
        card.device->dmabuf_textures.erase(imported);
      }
      it = card.shm_textures.erase(it);
    } else ++it;
  }
  std::vector<layout::Placement> placements;
  for (const auto id : card.animations.retained_ids()) {
    const auto sample = card.animations.sample(id, now);
    if (sample) placements.push_back({id, sample->bounds});
  }
  std::optional<renderer::DrawCall> pending_border;
  std::uint64_t pending_border_root = 0;
  const auto apply_blur_parameters = [&](renderer::DrawCall& draw, std::string_view shader = {}) {
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
  for (const auto& surface : card.shm_textures) {
    if (pending_border && (surface.root_id != pending_border_root || surface.popup)) {
      frame.draws.push_back(std::move(*pending_border));
      pending_border.reset();
    }
    const auto placement = std::find_if(placements.begin(), placements.end(), [&surface](const auto& item) { return item.id == surface.root_id; });
    const auto root = std::find_if(card.shm_textures.begin(), card.shm_textures.end(), [&surface](const auto& item) { return item.id == surface.root_id; });
    if (root == card.shm_textures.end() || !root->content_ready || surface.texture == 0 || root->width <= 0 || root->height <= 0) continue;
    if (!root->toplevel) {
      frame.draws.push_back({surface.id, {{surface.x, surface.y}, {static_cast<std::uint32_t>(surface.width), static_cast<std::uint32_t>(surface.height)}}, surface.compositor_opacity, {1.0F, 1.0F, 1.0F, 1.0F}, surface.texture, 0.0F, 0.0F, {surface.source_left, surface.source_top, surface.source_right, surface.source_bottom}});
      frame.draws.back().opaque = surface.opaque && surface.compositor_opacity == 1.0F;
      frame.draws.back().background_blur_radius = static_cast<float>(surface.background_blur_radius);
      frame.draws.back().background_mask_texture = surface.layer_surface;
      frame.draws.back().background_mask_alpha_threshold = surface.background_blur_ignore_alpha;
      apply_blur_parameters(frame.draws.back());
      continue;
    }
    renderer::Rect tag_bounds;
    if (placement != placements.end()) tag_bounds = placement->bounds;
    else continue;
    const auto tag_root = tag ? transform_tag_root(tag_bounds, card.output.logical_width, *tag, root->tag_outgoing)
                               : TagRootSample{tag_bounds, 1.0F};
    tag_bounds = tag_root.bounds;
    const float decoration_scale = animated_decoration_scale(*root, tag_bounds);
    const float border_width = config_->border_width() * decoration_scale;
    const float inner_radius = config_->radius() * decoration_scale;
    const float outer_radius = inner_radius + border_width;
    const float border_join_width = border_width +
        0.75F / (output_config.scale_per_mille / 1000.0F);
    if (surface.toplevel && !root->fullscreen && border_width > 0.0F) {
      constexpr std::array<float, 4> border{1.0F, 1.0F, 1.0F, 1.0F};
      const auto animation = card.animations.sample(surface.id, now);
      pending_border.emplace(surface.id, tag_bounds, (animation ? animation->opacity : 1.0F) * tag_root.opacity, border, std::nullopt,
                             outer_radius, border_join_width);
      pending_border->shader_role = renderer::ShaderRole::border;
      pending_border->shader_name = surface.border_shader;
      pending_border->toplevel_id = surface.root_id;
      pending_border->toplevel_bounds = tag_bounds;
      pending_border->toplevel_state = surface.focused ? 1U : 0U;
      pending_border->shader_time = frame.shader_time;
      pending_border_root = surface.root_id;
    }
    const auto content = animated_content(*root, tag_bounds);
    const int content_width = static_cast<int>(content.size.width);
    const int content_height = static_cast<int>(content.size.height);
    if (content_width <= 0 || content_height <= 0) continue;
    const auto animation = card.animations.sample(surface.root_id, now);
    const auto geometry = surface.toplevel ? window_geometry(surface) : window_geometry(*root);
    const int goal_width = root->assigned_content_width > 0 ? root->assigned_content_width : geometry.width;
    const int goal_height = root->assigned_content_height > 0 ? root->assigned_content_height : geometry.height;
    const renderer::Rect root_texture_bounds{
        content.origin,
        {static_cast<std::uint32_t>(std::max<std::int64_t>(
             1, static_cast<std::int64_t>(geometry.width) * content_width / goal_width)),
         static_cast<std::uint32_t>(std::max<std::int64_t>(
             1, static_cast<std::int64_t>(geometry.height) * content_height / goal_height))}};
    const int relative_x = surface.x - root->x;
    const int relative_y = surface.y - root->y;
    const renderer::Rect bounds = surface.toplevel
                                        ? tag_bounds
                                       : renderer::Rect{{content.origin.x + (relative_x - geometry.x) * content_width / goal_width,
                                                          content.origin.y + (relative_y - geometry.y) * content_height / goal_height},
                                                        {static_cast<std::uint32_t>(std::max(1, surface.width * content_width / goal_width)),
                                                          static_cast<std::uint32_t>(std::max(1, surface.height * content_height / goal_height))}};
    const bool root_surface = surface.toplevel;
    frame.draws.push_back({surface.id, bounds,
                                 (animation ? animation->opacity : 1.0F) * tag_root.opacity * surface.compositor_opacity, {1.0F, 1.0F, 1.0F, 1.0F}, surface.texture,
                                  root_surface && !root->fullscreen ? outer_radius : 0.0F,
                              0.0F, surface.toplevel ? source_uv(geometry, surface) : std::array<float, 4>{surface.source_left, surface.source_top, surface.source_right, surface.source_bottom},
                                surface.popup ? std::nullopt : std::optional<renderer::Rect>{content},
                                  (surface.popup || root->fullscreen) ? 0.0F : inner_radius});
    frame.draws.back().opaque = surface.opaque;
    frame.draws.back().shader_role = renderer::ShaderRole::window;
    frame.draws.back().shader_name = surface.window_shader;
    frame.draws.back().toplevel_id = surface.root_id;
    frame.draws.back().toplevel_bounds = tag_bounds;
    frame.draws.back().toplevel_state = (surface.focused ? 1U : 0U) |
        (root->fullscreen ? 2U : 0U) | (surface.popup ? 4U : 0U);
    frame.draws.back().shader_time = frame.shader_time;
    if (root_surface) {
      frame.draws.back().texture_bounds = root_texture_bounds;
      frame.draws.back().texture_corner_radius = root->fullscreen ? 0.0F : inner_radius;
    }
    frame.draws.back().background_blur_radius = static_cast<float>(surface.background_blur_radius);
    apply_blur_parameters(frame.draws.back(), surface.window_shader);
  }
  if (pending_border) frame.draws.push_back(std::move(*pending_border));
  if (error_popup_visible_ && error_popup_) {
    if (device.error_popup_texture == 0) glGenTextures(1, &device.error_popup_texture);
    if (device.error_popup_generation != error_popup_generation_) {
      glBindTexture(GL_TEXTURE_2D, device.error_popup_texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(error_popup_.width),
                   static_cast<GLsizei>(error_popup_.height), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                   error_popup_.pixels.data());
      glBindTexture(GL_TEXTURE_2D, 0);
      device.error_popup_generation = error_popup_generation_;
    }
    const auto popup_x = card.output.logical_x + std::max(8, (static_cast<int>(card.output.logical_width) -
                                                              static_cast<int>(error_popup_.width)) / 2);
    frame.draws.push_back({std::numeric_limits<std::uint64_t>::max() - 1,
                           {{popup_x, card.output.logical_y + 24}, {error_popup_.width, error_popup_.height}},
                           1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, device.error_popup_texture});
  }
  renderer::FramePlan cursor_frame;
  if (cursor_visible_ && card.cursor_texture != 0 && card.cursor_width > 0 && card.cursor_height > 0) {
    cursor_frame.draws.push_back({std::numeric_limits<std::uint64_t>::max(),
                           {{static_cast<std::int32_t>(std::lround(cursor_x_)) - card.cursor_hotspot_x,
                             static_cast<std::int32_t>(std::lround(cursor_y_)) - card.cursor_hotspot_y},
                            {static_cast<std::uint32_t>(card.cursor_width), static_cast<std::uint32_t>(card.cursor_height)}},
                           1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, card.cursor_texture});
  }
  const bool continue_animation = card.animations.active(now);
  const renderer::Point logical_origin{card.output.logical_x, card.output.logical_y};
  transform_frame(frame, output_config, physical, logical_origin);
  transform_frame(cursor_frame, output_config, physical, logical_origin);
  if (card.capture_texture == 0) glGenTextures(1, &card.capture_texture);
  glBindTexture(GL_TEXTURE_2D, card.capture_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, card.mode.hdisplay, card.mode.vdisplay, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (card.capture_framebuffer == 0) glGenFramebuffers(1, &card.capture_framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, card.capture_framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, card.capture_texture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    last_error_ = "could not prepare offscreen frame";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  glViewport(0, 0, card.mode.hdisplay, card.mode.vdisplay);
  glClearColor(0.125F, 0.141F, 0.169F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!device.renderer->render(frame, {static_cast<std::uint32_t>(card.mode.hdisplay), static_cast<std::uint32_t>(card.mode.vdisplay)})) {
    last_error_ = "could not render offscreen frame";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, card.capture_framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, card.mode.hdisplay, card.mode.vdisplay,
                    0, 0, card.mode.hdisplay, card.mode.vdisplay,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!cursor_frame.draws.empty() && !device.renderer->render(cursor_frame, physical)) {
    last_error_ = "could not render direct cursor";
    return;
  }
  if (eglSwapBuffers(device.egl_display, card.egl_surface) != EGL_TRUE) {
    last_error_ = "could not swap direct EGL frame";
    std::fprintf(stderr, "zwwm: %s\n", last_error_.c_str());
    return;
  }
  card.capture_ready = true;
  gbm_bo* bo = gbm_surface_lock_front_buffer(card.scanout_surface);
  if (bo == nullptr) { last_error_ = "could not lock direct scanout buffer"; std::fprintf(stderr, "zwwm: %s\n", last_error_.c_str()); return; }
  std::uint32_t fb = 0;
  const auto handle = gbm_bo_get_handle(bo).u32;
  const std::uint32_t handles[4]{handle, 0, 0, 0};
  const std::uint32_t strides[4]{gbm_bo_get_stride(bo), 0, 0, 0};
  const std::uint32_t offsets[4]{0, 0, 0, 0};
  if (drmModeAddFB2(device.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo),
                    handles, strides, offsets, &fb, 0) != 0 &&
      drmModeAddFB(device.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
                   gbm_bo_get_stride(bo), handle, &fb) != 0) {
    gbm_surface_release_buffer(card.scanout_surface, bo);
    last_error_ = "could not create direct scanout framebuffer: " + std::string(std::strerror(errno));
    std::fprintf(stderr, "zwwm: %s\n", last_error_.c_str());
    return;
  }
  if (card.front_bo == nullptr) {
    if (drmModeSetCrtc(device.fd, card.crtc_id, fb, 0, 0, &card.connector_id, 1, &card.mode) != 0) {
      drmModeRmFB(device.fd, fb);
      gbm_surface_release_buffer(card.scanout_surface, bo);
      last_error_ = "could not modeset direct output";
      std::fprintf(stderr, "zwwm: %s: %s\n", last_error_.c_str(), std::strerror(errno));
      return;
    }
    card.front_bo = bo;
    card.front_fb = fb;
    card.needs_repaint = false;
    if (input_target_ != nullptr) input_target_->notify_frame_presented(card.output.id);
    if (continue_animation) repaint(card);
    return;
  }
  card.pending_bo = bo;
  card.pending_fb = fb;
  card.flip_pending = true;
  card.needs_repaint = continue_animation;
  if (drmModePageFlip(device.fd, card.crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, &card) != 0) {
    card.flip_pending = false;
    drmModeRmFB(device.fd, card.pending_fb);
    gbm_surface_release_buffer(card.scanout_surface, card.pending_bo);
    card.pending_bo = nullptr;
    card.pending_fb = 0;
    last_error_ = "could not queue direct page flip";
  }
}

void RuntimeBackend::page_flip_complete(DrmOutput& card) {
  auto& device = *card.device;
  if (!card.flip_pending) return;
  if (card.front_fb != 0) drmModeRmFB(device.fd, card.front_fb);
  if (card.front_bo != nullptr) gbm_surface_release_buffer(card.scanout_surface, card.front_bo);
  card.front_bo = card.pending_bo;
  card.front_fb = card.pending_fb;
  card.pending_bo = nullptr;
  card.pending_fb = 0;
  card.flip_pending = false;
  if (card.retired) {
    auto* retired = &card;
    close_drm_output(card);
    std::erase_if(device.retired_outputs, [retired](const auto& output) { return output.get() == retired; });
    rescan_drm_device(device);
    publish_outputs();
    publish_dmabuf_importer();
    for (auto& output : device.outputs) repaint(*output);
    return;
  }
  if (input_target_ != nullptr) input_target_->notify_frame_presented(card.output.id);
  if (card.needs_repaint) repaint(card);
}

void RuntimeBackend::discover_drm_devices() {
  udev_enumerate* enumeration = udev_enumerate_new(udev_);
  if (enumeration == nullptr) {
    return;
  }
  udev_enumerate_add_match_subsystem(enumeration, "drm");
  udev_enumerate_scan_devices(enumeration);
  udev_list_entry* entry = nullptr;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumeration)) {
    udev_device* device = udev_device_new_from_syspath(udev_, udev_list_entry_get_name(entry));
    if (device != nullptr) {
      if (active_) {
        add_drm_device(udev_device_get_devnode(device));
      }
      udev_device_unref(device);
    }
  }
  udev_enumerate_unref(enumeration);
}

void RuntimeBackend::add_drm_device(const char* path) {
  if (!is_drm_card(path) || std::any_of(devices_.begin(), devices_.end(), [path](const auto& device) { return device->path == path; })) {
    return;
  }
  auto device = std::make_unique<DrmDevice>();
  device->backend = this;
  device->path = path;
  device->fd = open_device(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (device->fd < 0) {
    std::fprintf(stderr, "zwwm: DRM %s: libseat could not open card\n", path);
    return;
  }
  device->seat_device_id = restricted_devices_.back().second;
  device->gbm = gbm_create_device(device->fd);
  if (device->gbm == nullptr) {
    std::fprintf(stderr, "zwwm: DRM %s: gbm_create_device failed\n", path);
    close_drm_device(*device);
    return;
  }
  device->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, device->gbm, nullptr);
  if (device->egl_display == EGL_NO_DISPLAY || eglInitialize(device->egl_display, nullptr, nullptr) != EGL_TRUE ||
       eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    std::fprintf(stderr, "zwwm: DRM %s: EGL display initialization failed: 0x%x\n", path, eglGetError());
    close_drm_device(*device);
    return;
  }
  constexpr EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  const auto choose_scanout = [&](EGLint color_bits, EGLint alpha_bits) {
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                 EGL_RED_SIZE, color_bits, EGL_GREEN_SIZE, color_bits, EGL_BLUE_SIZE, color_bits,
                                 EGL_ALPHA_SIZE, alpha_bits, EGL_NONE};
    EGLConfig candidate = nullptr;
    EGLint count = 0;
    if (eglChooseConfig(device->egl_display, attributes, &candidate, 1, &count) != EGL_TRUE || count == 0) return false;
    EGLint native_visual = color_bits == 10 ? static_cast<EGLint>(DRM_FORMAT_XRGB2101010) : static_cast<EGLint>(DRM_FORMAT_XRGB8888);
    (void)eglGetConfigAttrib(device->egl_display, candidate, EGL_NATIVE_VISUAL_ID, &native_visual);
    device->egl_config = candidate;
    device->scanout_format = native_visual == 0 ? (color_bits == 10 ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888)
                                                : static_cast<std::uint32_t>(native_visual);
    device->scanout_bit_depth = static_cast<std::uint32_t>(color_bits);
    return true;
  };
  bool scanout_ready = false;
  const bool wants_ten_bit = config_->output.bit_depth == 10 ||
      std::any_of(config_->outputs.begin(), config_->outputs.end(), [](const auto& configured) {
        return configured.second.bit_depth == 10;
      });
  if (wants_ten_bit) scanout_ready = choose_scanout(10, 0) || choose_scanout(10, 2);
  if (!scanout_ready) scanout_ready = choose_scanout(8, 0) || choose_scanout(8, 8);
  if (!scanout_ready) {
    std::fprintf(stderr, "zwwm: DRM %s: no compatible EGL/GBM scanout format (error=0x%x)\n", path, eglGetError());
    close_drm_device(*device);
    return;
  }
  if ((device->egl_context = eglCreateContext(device->egl_display, device->egl_config, EGL_NO_CONTEXT, context_attributes)) == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "zwwm: DRM %s: EGL context creation failed: 0x%x\n", path, eglGetError());
    close_drm_device(*device);
    return;
  }
  try {
    auto* monitored = device.get();
    device->source = event_loop_->add_fd(device->fd, EPOLLIN, [monitored](int fd, int mask) {
      on_drm_fd(fd, static_cast<std::uint32_t>(mask), monitored);
      return true;
    });
  } catch (const std::exception&) {
    std::fprintf(stderr, "zwwm: DRM %s: could not monitor DRM events\n", path);
    close_drm_device(*device);
    return;
  }
  devices_.push_back(std::move(device));
  rescan_drm_device(*devices_.back());
  std::fprintf(stderr, "zwwm: activated DRM device %s with %zu output(s), %u-bit using GBM backend %s\n",
               path, devices_.back()->outputs.size(), devices_.back()->scanout_bit_depth,
               gbm_device_get_backend_name(devices_.back()->gbm));
  publish_outputs();
  publish_dmabuf_importer();
  for (auto& output : devices_.back()->outputs) repaint(*output);
}

void RuntimeBackend::rescan_drm_device(DrmDevice& device) {
  struct Candidate {
    std::string key;
    std::string name;
    std::uint32_t connector_id = 0;
    std::uint32_t possible_crtcs = 0;
    std::uint32_t current_crtc = 0;
    drmModeModeInfo mode{};
    std::vector<OutputInfo::Mode> modes;
    std::uint32_t crtc_index = std::numeric_limits<std::uint32_t>::max();
  };
  drmModeRes* resources = drmModeGetResources(device.fd);
  if (resources == nullptr) return;
  const auto device_name = device.path.substr(device.path.rfind('/') == std::string::npos ? 0 : device.path.rfind('/') + 1);
  std::vector<Candidate> candidates;
  for (int connector_index = 0; connector_index < resources->count_connectors; ++connector_index) {
    drmModeConnector* connector = drmModeGetConnector(device.fd, resources->connectors[connector_index]);
    if (connector == nullptr) continue;
    const bool usable = connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0 &&
                        connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK;
    if (!usable) { drmModeFreeConnector(connector); continue; }
    const char* type = drmModeGetConnectorTypeName(connector->connector_type);
    Candidate candidate;
    candidate.name = device_name + "-" + (type == nullptr ? "UNKNOWN" : type) + "-" +
                     std::to_string(connector->connector_type_id);
    candidate.key = device.path + ":" + candidate.name;
    candidate.connector_id = connector->connector_id;
    candidate.modes.reserve(static_cast<std::size_t>(connector->count_modes));
    for (int index = 0; index < connector->count_modes; ++index) {
      const auto& available = connector->modes[index];
      candidate.modes.push_back({static_cast<std::uint32_t>(available.hdisplay),
                                 static_cast<std::uint32_t>(available.vdisplay),
                                 mode_refresh_millihz(available),
                                 (available.type & DRM_MODE_TYPE_PREFERRED) != 0});
    }
    const drmModeModeInfo* mode = &connector->modes[0];
    for (int index = 0; index < connector->count_modes; ++index)
      if ((connector->modes[index].type & DRM_MODE_TYPE_PREFERRED) != 0) { mode = &connector->modes[index]; break; }
    const auto& configured = config_->output_for(candidate.name);
    if (!configured.mode.preferred) {
      const auto requested = configured.mode;
      const auto exact = std::find_if(connector->modes, connector->modes + connector->count_modes,
          [requested](const drmModeModeInfo& item) {
            const auto refresh = mode_refresh_millihz(item);
            return item.hdisplay == requested.width && item.vdisplay == requested.height &&
                   refresh + 500U >= requested.refresh_millihz && refresh <= requested.refresh_millihz + 500U;
          });
      if (exact != connector->modes + connector->count_modes) mode = exact;
    }
    candidate.mode = *mode;
    for (int index = 0; index < connector->count_encoders; ++index) {
      drmModeEncoder* encoder = drmModeGetEncoder(device.fd, connector->encoders[index]);
      if (encoder == nullptr) continue;
      candidate.possible_crtcs |= encoder->possible_crtcs;
      if (connector->encoder_id == encoder->encoder_id) candidate.current_crtc = encoder->crtc_id;
      drmModeFreeEncoder(encoder);
    }
    if (configured.bit_depth == 10) for (int index = 0; index < connector->count_props; ++index) {
      drmModePropertyRes* property = drmModeGetProperty(device.fd, connector->props[index]);
      if (property != nullptr && std::strcmp(property->name, "max bpc") == 0 && property->count_values >= 2 && property->values[1] >= 10)
        (void)drmModeObjectSetProperty(device.fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR, property->prop_id, 10);
      if (property != nullptr) drmModeFreeProperty(property);
    }
    if (candidate.possible_crtcs != 0) candidates.push_back(std::move(candidate));
    drmModeFreeConnector(connector);
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
    return left.key < right.key;
  });
  const std::size_t crtc_count = static_cast<std::size_t>(resources->count_crtcs);
  std::vector<int> crtc_owner(crtc_count, -1);
  std::vector<bool> crtc_locked(crtc_count, false);
  for (const auto& output : device.retired_outputs) if (output->flip_pending && output->crtc_index < crtc_count)
    crtc_locked[output->crtc_index] = true;
  for (const auto& output : device.outputs) if (output->flip_pending && output->crtc_index < crtc_count) {
    const auto candidate = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& item) {
      return item.key == output->connector_key && item.connector_id == output->connector_id &&
             same_drm_mode(item.mode, output->mode);
    });
    if (candidate == candidates.end()) crtc_locked[output->crtc_index] = true;
  }
  // Preserve valid CRTC assignments during rescans.
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    auto& candidate = candidates[index];
    const auto existing = std::find_if(device.outputs.begin(), device.outputs.end(), [&](const auto& output) {
      return output->connector_key == candidate.key && output->connector_id == candidate.connector_id &&
             same_drm_mode(output->mode, candidate.mode);
    });
    if (existing == device.outputs.end() || (*existing)->crtc_index >= crtc_count || (*existing)->crtc_index >= 32 ||
        (candidate.possible_crtcs & (1U << (*existing)->crtc_index)) == 0 || crtc_locked[(*existing)->crtc_index] ||
        crtc_owner[(*existing)->crtc_index] != -1) continue;
    candidate.crtc_index = (*existing)->crtc_index;
    crtc_owner[candidate.crtc_index] = static_cast<int>(index);
    crtc_locked[candidate.crtc_index] = true;
  }
  std::function<bool(std::size_t, std::vector<bool>&)> assign_crtc;
  assign_crtc = [&](std::size_t candidate_index, std::vector<bool>& visited) {
    auto& candidate = candidates[candidate_index];
    std::vector<std::uint32_t> order;
    for (int index = 0; index < resources->count_crtcs && index < 32; ++index)
      if (resources->crtcs[index] == candidate.current_crtc) order.push_back(static_cast<std::uint32_t>(index));
    for (int index = 0; index < resources->count_crtcs && index < 32; ++index)
      if (order.empty() || order.front() != static_cast<std::uint32_t>(index)) order.push_back(static_cast<std::uint32_t>(index));
    for (const auto crtc : order) {
      if ((candidate.possible_crtcs & (1U << crtc)) == 0 || visited[crtc] || crtc_locked[crtc]) continue;
      visited[crtc] = true;
      const int owner = crtc_owner[crtc];
      if (owner >= 0 && !assign_crtc(static_cast<std::size_t>(owner), visited)) continue;
      crtc_owner[crtc] = static_cast<int>(candidate_index);
      candidate.crtc_index = crtc;
      return true;
    }
    return false;
  };
  for (std::size_t index = 0; index < candidates.size(); ++index) if (candidates[index].crtc_index == std::numeric_limits<std::uint32_t>::max()) {
    std::vector<bool> visited(crtc_count, false);
    (void)assign_crtc(index, visited);
  }
  std::vector<std::unique_ptr<DrmOutput>> next;
  for (auto& candidate : candidates) {
    if (candidate.crtc_index == std::numeric_limits<std::uint32_t>::max()) continue;
    const auto existing = std::find_if(device.outputs.begin(), device.outputs.end(), [&](const auto& output) {
      return output != nullptr && output->connector_key == candidate.key && output->connector_id == candidate.connector_id &&
             output->crtc_index == candidate.crtc_index && same_drm_mode(output->mode, candidate.mode);
    });
    if (existing != device.outputs.end()) {
      (*existing)->output.modes = candidate.modes;
      (*existing)->output.bit_depth = device.scanout_bit_depth;
      next.push_back(std::move(*existing));
      continue;
    }
    auto output = std::make_unique<DrmOutput>(config_->animations);
    output->device = &device;
    output->connector_key = candidate.key;
    output->connector_id = candidate.connector_id;
    output->crtc_index = candidate.crtc_index;
    output->crtc_id = resources->crtcs[candidate.crtc_index];
    output->mode = candidate.mode;
    output->scanout_bit_depth = device.scanout_bit_depth;
    output->scanout_surface = gbm_surface_create(device.gbm, candidate.mode.hdisplay, candidate.mode.vdisplay,
        device.scanout_format, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (output->scanout_surface == nullptr) continue;
    output->egl_surface = eglCreateWindowSurface(device.egl_display, device.egl_config,
        reinterpret_cast<EGLNativeWindowType>(output->scanout_surface), nullptr);
    if (output->egl_surface == EGL_NO_SURFACE) { gbm_surface_destroy(output->scanout_surface); continue; }
    const auto& configured = config_->output_for(candidate.name);
    const auto logical = configured.logical_size({static_cast<std::uint32_t>(candidate.mode.hdisplay),
                                                   static_cast<std::uint32_t>(candidate.mode.vdisplay)});
    output->output = {.id = stable_output_id(device.path, candidate.key), .connector = candidate.name,
        .logical_width = logical.width, .logical_height = logical.height,
        .physical_width = static_cast<std::uint32_t>(candidate.mode.hdisplay),
        .physical_height = static_cast<std::uint32_t>(candidate.mode.vdisplay),
        .refresh_millihz = mode_refresh_millihz(candidate.mode),
        .scale_per_mille = configured.scale_per_mille, .transform = configured.transform,
        .bit_depth = device.scanout_bit_depth, .modes = std::move(candidate.modes)};
    next.push_back(std::move(output));
  }
  if (next.empty() && !device.dmabuf_textures.empty() && !device.outputs.empty() &&
      eglMakeCurrent(device.egl_display, device.outputs.front()->egl_surface, device.outputs.front()->egl_surface,
                     device.egl_context) == EGL_TRUE && device.dmabuf_importer != nullptr) {
    for (const auto& [id, imported] : device.dmabuf_textures) {
      (void)id;
      if (imported.texture != 0) (void)device.dmabuf_importer->release(imported.texture);
    }
    device.dmabuf_textures.clear();
  }
  for (auto& output : device.outputs) if (output != nullptr) {
    output->retired = true;
    output->output.enabled = false;
    if (output->flip_pending) device.retired_outputs.push_back(std::move(output));
    else close_drm_output(*output);
  }
  device.outputs = std::move(next);
  drmModeFreeResources(resources);
  if (device.renderer == nullptr && !device.outputs.empty() && eglMakeCurrent(device.egl_display,
      device.outputs.front()->egl_surface, device.outputs.front()->egl_surface, device.egl_context) == EGL_TRUE) {
    device.renderer = std::make_unique<renderer::OpenGlRenderer>();
    device.dmabuf_importer = std::make_unique<renderer::EglDmabufImporter>(device.egl_display);
    if (!device.renderer->initialize() || !device.dmabuf_importer->initialize()) {
      last_error_ = "could not initialize shared DRM renderer/importer";
      device.dmabuf_importer->shutdown();
      device.renderer->shutdown();
      device.dmabuf_importer.reset();
      device.renderer.reset();
    } else {
      renderer::ShaderSources sources;
      std::string shader_error;
      if (load_shader_sources(*config_, &sources, &shader_error) &&
          device.renderer->prepare_shaders(sources))
        device.renderer->commit_shaders();
      else
        last_error_ = shader_error.empty() ? device.renderer->last_error() : shader_error;
    }
  }
  XcursorImages* cursor = XcursorLibraryLoadImages(cursor_shape_.c_str(), cursor_theme_.c_str(), cursor_size_);
  for (auto& output : device.outputs) if (output->cursor_texture == 0 && cursor != nullptr && cursor->nimage > 0 && cursor->images[0] != nullptr &&
      eglMakeCurrent(device.egl_display, output->egl_surface, output->egl_surface, device.egl_context) == EGL_TRUE) {
    const auto* image = cursor->images[0];
    output->cursor_width = image->width; output->cursor_height = image->height;
    output->cursor_hotspot_x = image->xhot; output->cursor_hotspot_y = image->yhot;
    glGenTextures(1, &output->cursor_texture);
    glBindTexture(GL_TEXTURE_2D, output->cursor_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, output->cursor_width, output->cursor_height, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, image->pixels);
  }
  if (cursor != nullptr) XcursorImagesDestroy(cursor);
}

void RuntimeBackend::remove_drm_device(const char* path) {
  const auto device = std::find_if(devices_.begin(), devices_.end(), [path](const auto& item) { return item->path == path; });
  if (device != devices_.end()) {
    close_drm_device(**device);
    devices_.erase(device);
    publish_outputs();
    publish_dmabuf_importer();
  }
}

void RuntimeBackend::suspend_drm_devices() {
  for (auto& device : devices_) {
    if (device->source >= 0) {
      event_loop_->remove(device->source);
      device->source = -1;
    }
  }
}

void RuntimeBackend::resume_drm_devices() {
  for (auto& device : devices_) {
    if (device->source < 0) {
      auto* resumed = device.get();
      try {
        device->source = event_loop_->add_fd(device->fd, EPOLLIN, [resumed](int fd, int mask) {
          on_drm_fd(fd, static_cast<std::uint32_t>(mask), resumed);
          return true;
        });
      } catch (const std::exception&) {
        device->source = -1;
      }
    }
  }
}

void RuntimeBackend::close_drm_output(DrmOutput& output) {
  auto& device = *output.device;
  if (device.fd >= 0 && output.crtc_id != 0)
    (void)drmModeSetCrtc(device.fd, output.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  if (device.egl_display != EGL_NO_DISPLAY && device.egl_context != EGL_NO_CONTEXT && output.egl_surface != EGL_NO_SURFACE)
    eglMakeCurrent(device.egl_display, output.egl_surface, output.egl_surface, device.egl_context);
  if (output.pending_fb != 0) drmModeRmFB(device.fd, output.pending_fb);
  if (output.pending_bo != nullptr && output.scanout_surface != nullptr) gbm_surface_release_buffer(output.scanout_surface, output.pending_bo);
  if (output.front_fb != 0) drmModeRmFB(device.fd, output.front_fb);
  if (output.front_bo != nullptr && output.scanout_surface != nullptr) gbm_surface_release_buffer(output.scanout_surface, output.front_bo);
  for (const auto& surface : output.shm_textures) if (surface.texture != 0 && surface.owns_texture) glDeleteTextures(1, &surface.texture);
  if (output.cursor_texture != 0) glDeleteTextures(1, &output.cursor_texture);
  if (output.capture_texture != 0) glDeleteTextures(1, &output.capture_texture);
  if (output.capture_cursor_texture != 0) glDeleteTextures(1, &output.capture_cursor_texture);
  if (output.capture_framebuffer != 0) glDeleteFramebuffers(1, &output.capture_framebuffer);
  output.shm_textures.clear();
  if (output.egl_surface != EGL_NO_SURFACE) eglDestroySurface(device.egl_display, output.egl_surface);
  if (output.scanout_surface != nullptr) gbm_surface_destroy(output.scanout_surface);
  output.egl_surface = EGL_NO_SURFACE;
  output.scanout_surface = nullptr;
  output.pending_bo = output.front_bo = nullptr;
  output.pending_fb = output.front_fb = 0;
}

void RuntimeBackend::close_drm_device(DrmDevice& device) {
  if (device.source >= 0) {
    event_loop_->remove(device.source);
    device.source = -1;
  }
  DrmOutput* current = !device.outputs.empty() ? device.outputs.front().get() :
                       !device.retired_outputs.empty() ? device.retired_outputs.front().get() : nullptr;
  if (current != nullptr && current->egl_surface != EGL_NO_SURFACE)
    eglMakeCurrent(device.egl_display, current->egl_surface, current->egl_surface, device.egl_context);
  if (device.dmabuf_importer != nullptr) for (const auto& [id, imported] : device.dmabuf_textures) {
    (void)id;
    if (imported.texture != 0) (void)device.dmabuf_importer->release(imported.texture);
  }
  device.dmabuf_textures.clear();
  if (device.dmabuf_importer != nullptr) device.dmabuf_importer->shutdown();
  if (device.error_popup_texture != 0) glDeleteTextures(1, &device.error_popup_texture);
  device.error_popup_texture = 0;
  if (device.renderer != nullptr) device.renderer->shutdown();
  device.dmabuf_importer.reset();
  device.renderer.reset();
  for (auto& output : device.outputs) close_drm_output(*output);
  for (auto& output : device.retired_outputs) close_drm_output(*output);
  device.outputs.clear();
  device.retired_outputs.clear();
  if (device.egl_display != EGL_NO_DISPLAY) eglMakeCurrent(device.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (device.egl_context != EGL_NO_CONTEXT) eglDestroyContext(device.egl_display, device.egl_context);
  if (device.egl_display != EGL_NO_DISPLAY) eglTerminate(device.egl_display);
  device.egl_context = EGL_NO_CONTEXT;
  device.egl_display = EGL_NO_DISPLAY;
  if (device.gbm != nullptr) {
    gbm_device_destroy(device.gbm);
    device.gbm = nullptr;
  }
  if (device.fd >= 0) {
    close_device(device.fd);
    device.fd = -1;
  }
}

int RuntimeBackend::open_device(const char* path, int flags) {
  int fd = -1;
  const int device_id = libseat_open_device(seat_, path, &fd);
  if (device_id < 0) {
    std::fprintf(stderr, "zwwm: libseat could not open %s: %s\n", path, std::strerror(-device_id));
    return -1;
  }
  if ((flags & O_NONBLOCK) != 0) {
    const int current_flags = fcntl(fd, F_GETFL);
    if (current_flags >= 0) {
      fcntl(fd, F_SETFL, current_flags | O_NONBLOCK);
    }
  }
  restricted_devices_.push_back({fd, device_id});
  return fd;
}

void RuntimeBackend::close_device(int fd) {
  const auto device = std::find_if(restricted_devices_.begin(), restricted_devices_.end(), [fd](const auto& item) { return item.first == fd; });
  if (device != restricted_devices_.end()) {
    libseat_close_device(seat_, device->second);
    restricted_devices_.erase(device);
  }
}

}  // namespace zwwm
