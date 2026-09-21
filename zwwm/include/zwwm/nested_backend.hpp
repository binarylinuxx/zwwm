#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <span>

#include "zwwm/compositor_server.hpp"
#include "zwwm/animation.hpp"
#include "zwwm/output_capture.hpp"
#include "zwwm/error_popup.hpp"
#include "zwwm/unique_fd.hpp"

namespace zwayland::client { class Display; struct Proxy; }
namespace zwayland::server { class EventLoop; }
namespace zwwm::egl { class Window; }

namespace zwwm {

class RuntimeConfig;

// WAYLAND_DISPLAY backend selected before direct DRM.
class NestedBackend {
 public:
  NestedBackend(zwayland::server::EventLoop* event_loop,
                std::shared_ptr<const RuntimeConfig> config);
  ~NestedBackend();

  [[nodiscard]] bool start();
  void stop();
  [[nodiscard]] bool connected() const;
  [[nodiscard]] const std::string& last_error() const;
  void present(const ShmBufferView& buffer);
  [[nodiscard]] bool capture_output(OutputCapture& capture, bool overlay_cursor = false);
  [[nodiscard]] bool capture_toplevel(std::uint64_t id, OutputCapture& capture,
                                      bool overlay_cursor = false);
  using PresentationObserver = void (*)(void*);
  void set_presentation_observer(PresentationObserver observer, void* data);
  void set_config(std::shared_ptr<const RuntimeConfig> config);
  [[nodiscard]] bool rebuild_switch_shaders(std::string* error);
  void set_input_target(CompositorServer* compositor);
  // Publishes linux-dmabuf when the presentation context can import it.
  void set_dmabuf_target(CompositorServer* compositor);
  // The parent compositor renders the nested cursor.
  void set_cursor_shape(const char* xcursor_name);
  [[nodiscard]] bool set_cursor_theme(const std::string& theme, std::uint32_t size,
                                      std::string* error);
  void set_cursor_position(std::int32_t x, std::int32_t y);
  void show_error(std::string message);
  void clear_error();

 private:
  struct RegistryObserver;
  struct WmBaseObserver;
  struct XdgSurfaceObserver;
  struct BufferObserver;
  struct FrameObserver;
  struct SeatObserver;
  struct PointerObserver;
  struct KeyboardObserver;

  [[nodiscard]] bool create_surface();
  [[nodiscard]] bool create_buffer();
  [[nodiscard]] bool create_egl_surface();
  void release_egl_surface();
  [[nodiscard]] bool arm_parent_display();
  void disarm_parent_display();
  void release_buffer();
  void release_wayland_objects();
  void repaint();
  void repaint_gpu();
  [[nodiscard]] bool request_parent_frame();
  void publish_dmabuf_importer();
  void deliver_pointer_motion(std::uint32_t time, std::int32_t x, std::int32_t y);

  zwayland::server::EventLoop* event_loop_ = nullptr;
  std::shared_ptr<const RuntimeConfig> config_;
  std::unique_ptr<zwayland::client::Display> parent_display_;
  int parent_source_ = -1;
  int error_timer_ = -1;
  zwayland::client::Proxy* registry_ = nullptr;
  zwayland::client::Proxy* compositor_ = nullptr;
  std::uint32_t compositor_version_ = 0;
  zwayland::client::Proxy* shm_ = nullptr;
  zwayland::client::Proxy* seat_ = nullptr;
  zwayland::client::Proxy* pointer_ = nullptr;
  zwayland::client::Proxy* keyboard_ = nullptr;
  zwayland::client::Proxy* wm_base_ = nullptr;
  zwayland::client::Proxy* surface_ = nullptr;
  zwayland::client::Proxy* xdg_surface_ = nullptr;
  zwayland::client::Proxy* toplevel_ = nullptr;
  zwayland::client::Proxy* buffer_pool_ = nullptr;
  zwayland::client::Proxy* buffer_ = nullptr;
  void* buffer_data_ = nullptr;
  std::size_t buffer_size_ = 0;
  UniqueFd buffer_fd_;
  bool parent_read_prepared_ = false;
  bool buffer_attached_ = false;
  bool buffer_in_flight_ = false;
  bool repaint_pending_ = false;
  bool frame_pending_ = false;
  bool tag_transition_preparing_ = false;
  bool capture_ready_ = false;
  bool capture_pending_ = false;
  bool egl_initialization_attempted_ = false;
  PresentationObserver presentation_observer_ = nullptr;
  void* presentation_observer_data_ = nullptr;
  CompositorServer* input_target_ = nullptr;
  struct PendingAxis { std::uint32_t time = 0, axis = 0, source = 0, relative_direction = 0; double value = 0; std::int32_t discrete = 0, value120 = 0; bool stop = false; };
  std::vector<PendingAxis> pending_axes_;
  std::uint32_t axis_source_ = 0;
  struct Window;
  std::vector<Window> windows_;
  std::string last_error_;
  std::string cursor_theme_ = "default";
  std::string cursor_shape_ = "left_ptr";
  std::uint32_t cursor_size_ = 24;
  std::unique_ptr<zwwm::egl::Window> egl_window_;
  zwayland::client::Proxy* frame_callback_ = nullptr;
  std::unique_ptr<class renderer_holder> renderer_;
  CompositorServer* dmabuf_target_ = nullptr;
  AnimationSystem animations_;
  std::uint32_t physical_width_ = 960, physical_height_ = 540;
  std::uint32_t logical_width_ = 960, logical_height_ = 540;
  std::vector<std::uint32_t> software_logical_;
  std::vector<std::uint32_t> capture_pixels_;
  ErrorPopupImage error_popup_;
  std::uint32_t error_popup_texture_ = 0;
  std::uint64_t error_popup_generation_ = 0;
  std::uint64_t uploaded_error_popup_generation_ = 0;
  bool error_popup_visible_ = false;
  bool error_popup_dirty_ = false;
};

}  // namespace zwwm
