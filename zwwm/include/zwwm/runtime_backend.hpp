#pragma once


#include <zwayland/server/display.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zwwm/output.hpp"
#include "zwwm/error_popup.hpp"

struct libinput;
struct libseat;
struct udev;
struct udev_monitor;
namespace zwwm {

class CompositorServer;
class RuntimeConfig;
struct ShmBufferView;
struct OutputCapture;

class RuntimeBackend {
 public:
  RuntimeBackend(zwayland::server::EventLoop* event_loop, std::shared_ptr<const RuntimeConfig> config);
  ~RuntimeBackend();
  RuntimeBackend(const RuntimeBackend&) = delete;
  RuntimeBackend& operator=(const RuntimeBackend&) = delete;

  [[nodiscard]] bool start();
  void stop();
  [[nodiscard]] bool active() const;
  [[nodiscard]] const std::string& last_error() const;
  void set_config(std::shared_ptr<const RuntimeConfig> config);
  [[nodiscard]] bool rebuild_switch_shaders(std::string* error);
  void set_input_target(CompositorServer* compositor);
  void set_dmabuf_target(CompositorServer* compositor);
  void set_cursor_shape(const char* xcursor_name);
  [[nodiscard]] bool set_cursor_theme(const std::string& theme, std::uint32_t size,
                                      std::string* error);
  void set_cursor_position(std::int32_t x, std::int32_t y);
  void show_error(std::string message);
  void clear_error();
  void present(const ShmBufferView& buffer);
  [[nodiscard]] bool capture_output(OutputCapture& capture, bool overlay_cursor = false);
  [[nodiscard]] bool capture_toplevel(std::uint64_t id, OutputCapture& capture,
                                      bool overlay_cursor = false);
  [[nodiscard]] bool supports_embedded_cursor() const;

  static int on_seat_fd(int fd, std::uint32_t mask, void* data);
  static int on_udev_fd(int fd, std::uint32_t mask, void* data);
  static int on_libinput_fd(int fd, std::uint32_t mask, void* data);
  static int on_drm_fd(int fd, std::uint32_t mask, void* data);
  static void enable_seat(libseat* seat, void* data);
  static void disable_seat(libseat* seat, void* data);
  static int on_error_timeout(void* data);
  static int open_restricted(const char* path, int flags, void* data);
  static void close_restricted(int fd, void* data);

 private:
  struct DrmDevice;
  struct DrmOutput;

  void dispatch_seat();
  void dispatch_udev();
  void dispatch_libinput();
  void dispatch_drm(DrmDevice& device);
  void discover_drm_devices();
  void add_drm_device(const char* path);
  void remove_drm_device(const char* path);
  void suspend_drm_devices();
  void resume_drm_devices();
  void rescan_drm_device(DrmDevice& device);
  void close_drm_output(DrmOutput& output);
  void close_drm_device(DrmDevice& device);
  void repaint(DrmOutput& output);
  void page_flip_complete(DrmOutput& output);
  void publish_dmabuf_importer();
  void publish_outputs();
  [[nodiscard]] int open_device(const char* path, int flags);
  void close_device(int fd);

  zwayland::server::EventLoop* event_loop_ = nullptr;
  libseat* seat_ = nullptr;
  udev* udev_ = nullptr;
  udev_monitor* udev_monitor_ = nullptr;
  libinput* libinput_ = nullptr;
  int seat_source_ = -1;
  int udev_source_ = -1;
  int libinput_source_ = -1;
  int error_timer_ = -1;
  std::vector<std::unique_ptr<DrmDevice>> devices_;
  std::vector<std::pair<int, int>> restricted_devices_;
  bool active_ = false;
  bool libinput_suspended_ = false;
  bool left_control_pressed_ = false;
  bool right_control_pressed_ = false;
  bool left_alt_pressed_ = false;
  bool right_alt_pressed_ = false;
  std::uint32_t consumed_vt_key_ = 0;
  CompositorServer* input_target_ = nullptr;
  std::shared_ptr<const RuntimeConfig> config_;
  CompositorServer* dmabuf_target_ = nullptr;
  double cursor_x_ = 480.0;
  double cursor_y_ = 270.0;
  bool cursor_visible_ = true;
  std::string cursor_theme_ = "default";
  std::string cursor_shape_ = "left_ptr";
  std::uint32_t cursor_size_ = 24;
  std::uint32_t output_width_ = 960;
  std::uint32_t output_height_ = 540;
  std::uint32_t physical_output_width_ = 960;
  std::uint32_t physical_output_height_ = 540;
  ErrorPopupImage error_popup_;
  std::uint64_t error_popup_generation_ = 0;
  bool error_popup_visible_ = false;
  std::string last_error_;
};

}  // namespace zwwm
