#pragma once


#include <zwayland/server/display.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "zwwm/renderer/dmabuf.hpp"
#include "zwwm/output_capture.hpp"
#include "zwwm/output.hpp"

#include <sys/types.h>

namespace zwwm {

class RuntimeConfig;

// Valid for the duration of the commit observer call.
struct ShmBufferView {
  std::uint64_t surface_id = 0;
  std::uint64_t root_surface_id = 0;
  OutputId output;
  const std::uint8_t* pixels = nullptr;
  std::int32_t width = 0;
  std::int32_t height = 0;
  // Surface-local dimensions after buffer scale and viewport destination.
  std::int32_t logical_width = 0;
  std::int32_t logical_height = 0;
  // Normalized buffer-pixel crop used by GPU and software presentation.
  float source_left = 0.0F;
  float source_top = 0.0F;
  float source_right = 1.0F;
  float source_bottom = 1.0F;
  std::int32_t stride = 0;
  std::uint32_t format = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t damage_x = 0;
  std::int32_t damage_y = 0;
  std::int32_t damage_width = 0;
  std::int32_t damage_height = 0;
  std::uint32_t stack_index = 0;
  // Explicit authoritative ordering tuple.  It avoids lossy bit packing.
  std::uint32_t layer = 2;
  std::uint64_t scene_order = 0;
  std::uint32_t scene_rank = 2;
  std::int32_t layer_priority = 0;
  std::uint64_t root_order = 0;
  std::uint64_t tree_order = 0;
  // Root layer opacity, after client premultiplied alpha and animation opacity.
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
  // Snap bounds during interactive move or resize.
  bool suppress_geometry_animation = false;
  // Follow moving endless-canvas targets.
  bool track_geometry_animation = false;
  float camera_scale = 1.0F;
  std::int32_t camera_center_x = 0;
  std::int32_t camera_center_y = 0;
  // False until the initial post-configure content commit.
  bool content_ready = true;
  // Brackets atomic publication of an output-local tag transition.
  bool tag_transition = false;
  bool tag_transition_ready = false;
  std::int8_t tag_transition_direction = 0;
  // Root tile and content rectangles in global logical coordinates.
  std::int32_t assigned_tile_x = 0;
  std::int32_t assigned_tile_y = 0;
  std::int32_t assigned_tile_width = 0;
  std::int32_t assigned_tile_height = 0;
  std::int32_t assigned_content_x = 0;
  std::int32_t assigned_content_y = 0;
  std::int32_t assigned_content_width = 0;
  std::int32_t assigned_content_height = 0;
  std::int32_t window_geometry_x = 0;
  std::int32_t window_geometry_y = 0;
  std::int32_t window_geometry_width = 0;
  std::int32_t window_geometry_height = 0;
  // Cache key for backend imports. DMA-BUF data remains compositor-owned.
  std::uint64_t buffer_generation = 0;
  const renderer::DmabufAttributes* dmabuf = nullptr;
  std::string window_shader;
  std::string border_shader;
};

struct ToplevelInfo {
  std::uint64_t id = 0;
  std::string title;
  std::string app_id;
  std::int32_t x = 0, y = 0, width = 0, height = 0;
  std::uint32_t state = 0;
  std::uint32_t capture_x = 0, capture_y = 0, capture_width = 0, capture_height = 0;
  OutputId output;
};

struct TagInfo {
  OutputId output;
  std::string connector;
  std::uint8_t active = 1;
};

struct LayerInfo {
  std::uint64_t id = 0;
  OutputId output;
  std::string name_space;
  std::uint32_t layer = 0;
  std::int32_t priority = 0;
  std::int32_t exclusive_zone = 0;
  std::uint32_t blur_radius = 0;
  float opacity = 1.0F;
  bool mapped = false;
};

struct CameraInfo {
  OutputId output;
  std::string connector;
  std::uint8_t tag = 1;
  double x = 0.0;
  double y = 0.0;
  double zoom = 1.0;
  bool active = false;
};

struct KeyboardLayoutInfo {
  std::string name;
  std::uint32_t group = 0;
};

class CompositorServer {
 public:
  using SurfaceCommitObserver = void (*)(void*, const ShmBufferView&);
  // The Xcursor name remains valid only for the duration of the callback.
  using CursorShapeObserver = void (*)(void*, const char* xcursor_name);
  using PointerPositionObserver = void (*)(void*, std::int32_t x, std::int32_t y);
  using ToplevelObserver = void (*)(void*);
  using PresentationObserver = void (*)(void*);
  using EventObserver = void (*)(void*, const char* event);

  CompositorServer(zwayland::server::Display* display, std::shared_ptr<const RuntimeConfig> config);
  CompositorServer(zwayland::server::Display* display, std::shared_ptr<const RuntimeConfig> config,
                   zwayland::server::EventLoop* event_loop);
  ~CompositorServer();

  CompositorServer(const CompositorServer&) = delete;
  CompositorServer& operator=(const CompositorServer&) = delete;

  void set_surface_commit_observer(SurfaceCommitObserver observer, void* data);
  void set_cursor_shape_observer(CursorShapeObserver observer, void* data);
  void set_pointer_position_observer(PointerPositionObserver observer, void* data);
  void set_toplevel_observer(ToplevelObserver observer, void* data);
  void set_presentation_observer(PresentationObserver observer, void* data);
  // Coarse external state events, independent of portal observers.
  void set_event_observer(EventObserver observer, void* data);
  // Keep authenticated portal dialogs above requesting applications.
  void set_portal_client(zwayland::server::Client* client);
  void set_config(std::shared_ptr<const RuntimeConfig> config);
  // Replaces outputs in connector order and rebuilds their logical layout.
  void set_outputs(std::vector<OutputInfo> outputs);
  [[nodiscard]] std::vector<OutputInfo> outputs() const;
  void set_output_size(std::uint32_t width, std::uint32_t height, std::uint32_t refresh_millihz = 60000);
  [[nodiscard]] zwayland::server::Resource* output_resource(zwayland::server::Client* client,
                                                     OutputId output = {}) const;
  [[nodiscard]] OutputId output_id(const zwayland::server::Resource* resource) const;
  [[nodiscard]] std::optional<OutputInfo> output_info(OutputId output) const;
  // Copies post-composition pixels into an exact-size writable portal SHM buffer.
  bool copy_capture_buffer(zwayland::server::Resource* buffer, const OutputCapture& capture,
                           std::uint32_t source_x, std::uint32_t source_y,
                           std::uint32_t width, std::uint32_t height);
  bool map_capture_region(const OutputCapture& capture, std::int32_t x, std::int32_t y,
                          std::int32_t width, std::int32_t height,
                          std::uint32_t* capture_x, std::uint32_t* capture_y,
                          std::uint32_t* capture_width, std::uint32_t* capture_height) const;
  [[nodiscard]] std::vector<ToplevelInfo> toplevels() const;
  [[nodiscard]] std::vector<TagInfo> tags() const;
  [[nodiscard]] std::vector<LayerInfo> layers() const;
  [[nodiscard]] std::vector<CameraInfo> cameras() const;
  [[nodiscard]] KeyboardLayoutInfo keyboard_layout() const;
  // Executes a configured action name. Reload is owned by the caller.
  [[nodiscard]] bool dispatch_action(const std::string& action, const std::string& argument,
                                     std::string* error);
  void set_dmabuf_feedback(std::vector<std::pair<std::uint32_t, std::uint64_t>> formats,
                           std::optional<dev_t> main_device = std::nullopt);
  // Called by a backend after its target buffer has been committed.
  void notify_frame_presented(OutputId output = {});
  // Backend input is in the target surface's logical coordinates.
  void pointer_motion(std::uint32_t time, std::uint64_t surface_id, std::int32_t x, std::int32_t y);
  // Backend input in compositor-global logical coordinates.
  void pointer_motion_global(std::uint32_t time, std::int32_t x, std::int32_t y);
  [[nodiscard]] bool pointer_pan_motion(double dx, double dy);
  void pointer_relative_motion(std::uint64_t time_usec, double dx, double dy,
                               double dx_unaccelerated, double dy_unaccelerated);
  void pointer_leave();
  void pointer_button(std::uint32_t time, std::uint32_t button, std::uint32_t state);
  void pointer_axis(std::uint32_t time, std::uint32_t axis, double value,
                     std::uint32_t source = 0,
                    std::int32_t discrete = 0, std::int32_t value120 = 0, bool stop = false,
                     std::uint32_t relative_direction = 0);
  void keyboard_key(std::uint32_t time, std::uint32_t key, std::uint32_t state);
  void keyboard_modifiers(std::uint32_t depressed, std::uint32_t latched,
                           std::uint32_t locked, std::uint32_t group);
  void keyboard_repeat_info(std::int32_t rate, std::int32_t delay);
  void input_reset(bool preserve_keyboard_focus = false);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace zwwm
