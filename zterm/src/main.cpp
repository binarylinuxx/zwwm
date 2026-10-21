#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gpu.hpp"
#include "session.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#include "connection.hpp"
#include <zterm/terminal.hpp>

#include <zwayland/client/core.hpp>
#include <xdg-shell-zwayland-client.h>
#include <linux-dmabuf-zwayland-client.h>
#include <viewporter-zwayland-client.h>
#include <fractional-scale-zwayland-client.h>
#include <ext-zwwm-manager-v1-zwayland-client.h>
#include <cursor-shape-zwayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <pty.h>
#include <pwd.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <clocale>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

extern char** environ;

namespace zterm {
namespace p = zwayland::generated;
namespace core = zwayland::client::core;
using Proxy = zwayland::client::Proxy;
using Parser = zwayland::wire::MessageParser;

class App final : public Session {
 public:
  App(Options options, std::function<Options()> reload, bool managed = false)
      : options_(std::move(options)), terminal_(24, 80, options_.history_lines), reload_(std::move(reload)), managed_(managed) {
    width_ = options_.width; height_ = options_.height;
    terminal_.set_default_colors(options_.foreground, options_.background);
    terminal_.set_palette(options_.palette);
    terminal_.set_cursor_style(options_.cursor);
    terminal_.set_input_options(options_.mouse_reporting, options_.alternate_scroll);
  }
  ~App() {
    try { retire(); } catch (...) {}
    if (compose_) xkb_compose_state_unref(compose_);
    if (compose_table_) xkb_compose_table_unref(compose_table_);
    if (key_state_) xkb_state_unref(key_state_);
    if (keymap_) xkb_keymap_unref(keymap_);
    if (xkb_) xkb_context_unref(xkb_);
  }

  void prepare() {
    (void)signal(SIGPIPE, SIG_IGN);
    display_ = connect_display(options_.standalone);
    if (!display_) throw std::runtime_error("cannot connect to Wayland");
    registry_ = core::get_registry(*display_);
    struct Registry {
      App* app;
      void global(Proxy&, std::uint32_t name, const std::string& interface, std::uint32_t version) const {
        app->global(name, interface, version);
      }
      void global_remove(Proxy&, std::uint32_t name) const { app->outputs_.erase(name); }
    };
    registry_->set_observer(p::wl_registry_observer(Registry{this}));
    if (!display_->roundtrip() || !display_->roundtrip()) throw std::runtime_error("Wayland discovery failed");
    if (!compositor_ || !wm_ || !dmabuf_ || !feedback_done_ || formats_.empty() || !device_)
      throw std::runtime_error("zterm requires xdg-shell and linux-dmabuf v4 GPU feedback");
    gpu_ = std::make_unique<Gpu>(*display_);
    gpu_->initialize(device_, formats_, options_.fonts, options_.size, options_.weight, options_.text_rounding, options_.icon_scale);
    configure_gpu(options_);
    gpu_->released = [this] { buffer_available_ = true; };
    if (managed_) {
      gpu_->resize(options_.width, options_.height, options_.size);
      gpu_->warm(*dmabuf_);
    }
    (void)display_->flush();
    running_ = false;
  }

  void open(Options options, std::function<Options()> reload, std::string directory,
            std::vector<std::string> environment) override {
    if (!display_->dispatch_pending()) throw std::runtime_error("terminal connection closed");
    options_ = std::move(options); reload_ = std::move(reload);
    directory_ = std::move(directory); environment_ = std::move(environment);
    terminal_ = Terminal(24,80,options_.history_lines);
    terminal_.set_default_colors(options_.foreground, options_.background);
    terminal_.set_palette(options_.palette); terminal_.set_cursor_style(options_.cursor);
    terminal_.set_input_options(options_.mouse_reporting, options_.alternate_scroll);
    configure_gpu(options_);
    width_ = options_.width; height_ = options_.height;
    running_ = true; configured_ = false; resize_ = true; grid_initialized_ = false;
    grid_resize_pending_ = false; buffer_available_ = true; presented_ = false;
    cursor_on_ = true; reload_pending_ = false; click_count_ = 0;
    start_pty();
    surface_ = display_->find_proxy(p::wl_compositor_create_surface(*display_, compositor_->id));
    surface_->set_observer([this](Proxy&, std::uint32_t opcode, Parser& parser) {
      if (opcode == 0 || opcode == 1) {
        const auto output = parser.read_object();
        if (opcode == 0) entered_.push_back(output);
        else std::erase(entered_, output);
        if (!fractional_) update_integer_scale();
      } else if (opcode == 2) { scale_ = std::clamp(parser.read_int(), 1, 8) * 120; resize_ = true; }
      else if (opcode == 3) (void)parser.read_uint();
    });
    if (viewporter_ && fractional_manager_) {
      viewport_ = display_->find_proxy(p::wp_viewporter_get_viewport(*display_, viewporter_->id, surface_));
      fractional_ = display_->find_proxy(p::wp_fractional_scale_manager_v1_get_fractional_scale(*display_, fractional_manager_->id, surface_));
      fractional_->set_observer([this](Proxy&, std::uint32_t, Parser& parser) {
        scale_ = std::clamp(parser.read_uint(), 30U, 960U); resize_ = true;
      });
    }
    xdg_ = display_->find_proxy(p::xdg_wm_base_get_xdg_surface(*display_, wm_->id, surface_));
    xdg_->set_observer([this](Proxy&, std::uint32_t, Parser& parser) {
      p::xdg_surface_ack_configure(*display_, xdg_->id, parser.read_uint());
      configured_ = true; resize_ = true;
    });
    top_ = display_->find_proxy(p::xdg_surface_get_toplevel(*display_, xdg_->id));
    top_->set_observer([this](Proxy&, std::uint32_t opcode, Parser& parser) {
      if (opcode == 0) {
        const auto width = parser.read_int(), height = parser.read_int();
        (void)parser.read_array();
        if (width > 0) width_ = std::clamp(width, 1, 8192);
        if (height > 0) height_ = std::clamp(height, 1, 8192);
      } else if (opcode == 1) running_ = false;
    });
    p::xdg_toplevel_set_app_id(*display_, top_->id, "zterm");
    p::xdg_toplevel_set_title(*display_, top_->id, "zterm");
    terminal_.title = [this](const std::string& title) { p::xdg_toplevel_set_title(*display_, top_->id, title); };
    p::wl_surface_commit(*display_, surface_->id);
    timers();
  }

  void prepare_frame() override {
    if (!display_->dispatch_pending()) { running_ = false; return; }
    if (!running_) { (void)display_->flush(); return; }
    if (reload_pending_) reload_config();
    if (resize_ && configured_) resize();
    if (grid_resize_pending_ && std::chrono::steady_clock::now() >= grid_resize_deadline_) apply_grid_resize();
    if (configured_ && terminal_.dirty && frame_ == nullptr && buffer_available_) render();
    output_ += terminal_.take_output();
    if (output_.size() > 4 * 1024 * 1024) throw std::runtime_error("PTY write queue is full");
    if (!display_->flush()) running_ = false;
  }

  void descriptors(std::vector<pollfd>& fds) const override {
    fds.insert(fds.end(), {
      {display_->fd(), static_cast<short>(POLLIN | (display_->wants_write() ? POLLOUT : 0)), 0},
      {pty_, static_cast<short>(POLLIN | (output_.empty() ? 0 : POLLOUT)), 0},
      {signals_, POLLIN, 0}, {repeat_timer_, POLLIN, 0}, {blink_timer_, POLLIN, 0}, {selection_timer_, POLLIN, 0}});
    if (clipboard_) clipboard_->poll_descriptors(fds);
  }

  int timeout() const override {
    return grid_resize_pending_ ? std::max(0, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
      grid_resize_deadline_ - std::chrono::steady_clock::now()).count()) + 1) : -1;
  }

  bool running() const override { return running_; }
  bool healthy() const override { return display_ && !display_->has_error() && display_->connection().healthy() && !display_->connection().peer_closed(); }
  bool presented() const override { return presented_; }
  pid_t child() const override { return child_; }
  void reap(pid_t child) override { if (child_ == child) child_ = -1; }

  void dispatch(std::span<const pollfd> fds) override {
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { running_ = false; return; }
    if (fds[0].revents & POLLIN) if (!display_->dispatch_pending()) { running_ = false; return; }
    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) read_pty();
    if (fds[1].revents & POLLOUT) write_pty();
    if (fds[2].revents & POLLIN) {
      signalfd_siginfo event{};
      while (read(signals_, &event, sizeof(event)) == sizeof(event)) {
        if (event.ssi_signo != SIGCHLD) running_ = false;
        else if (child_ > 0 && waitpid(child_, nullptr, WNOHANG) == child_) child_ = -1;
      }
    }
    if (fds[3].revents & POLLIN) {
      std::uint64_t count = 0; (void)read(repeat_timer_, &count, sizeof(count));
      for (std::uint64_t i = 0; i < std::min(count, std::uint64_t{16}); ++i) key(repeat_key_);
    }
    if (fds[4].revents & POLLIN) {
      std::uint64_t count = 0; (void)read(blink_timer_, &count, sizeof(count));
      if (terminal_.cursor_style().blink) { cursor_on_ = !cursor_on_; terminal_.dirty = true; }
    }
    if (fds[5].revents & POLLIN) {
      std::uint64_t count = 0; (void)read(selection_timer_, &count, sizeof(count));
      if (selecting_) {
        const double outside = pointer_y_ < content_top() ? content_top() - pointer_y_ : pointer_y_ - content_bottom();
        terminal_.scroll(pointer_y_ < content_top() ? std::clamp(static_cast<int>(outside / 20) + 1,1,8) : -std::clamp(static_cast<int>(outside / 20) + 1,1,8));
        extend_selection();
      }
    }
    if (clipboard_) clipboard_->dispatch(fds.subspan(6));
  }

  int run() {
    prepare();
    open(options_, reload_, "", {});
    while (running_) {
      prepare_frame();
      if (!running_) break;
      std::vector<pollfd> fds; descriptors(fds);
      const int result = poll(fds.data(), fds.size(), timeout());
      if (result < 0) { if (errno == EINTR) continue; throw std::runtime_error("poll failed"); }
      dispatch(fds);
    }
    if (display_->has_error()) throw std::runtime_error(display_->error_message());
    return 0;
  }

  void retire() override {
    running_ = false; terminal_.title = {};
    if (pty_ >= 0) { close(pty_); pty_ = -1; }
    if (child_ > 0) { kill(-child_, SIGHUP); (void)waitpid(child_, nullptr, WNOHANG); child_ = -1; }
    for (int* fd : {&signals_,&repeat_timer_,&blink_timer_,&selection_timer_}) if (*fd >= 0) { close(*fd); *fd = -1; }
    if (!display_) return;
    if (frame_) { display_->destroy_proxy(frame_->id); frame_ = nullptr; }
    if (top_) { p::xdg_toplevel_destroy(*display_, top_->id); top_ = nullptr; }
    if (xdg_) { p::xdg_surface_destroy(*display_, xdg_->id); xdg_ = nullptr; }
    if (fractional_) { p::wp_fractional_scale_v1_destroy(*display_, fractional_->id); fractional_ = nullptr; }
    if (viewport_) { p::wp_viewport_destroy(*display_, viewport_->id); viewport_ = nullptr; }
    if (surface_) { p::wl_surface_destroy(*display_, surface_->id); surface_ = nullptr; }
    if (gpu_) gpu_->detach();
    (void)display_->flush();
    entered_.clear(); output_.clear(); selecting_ = false; reported_buttons_.fill(false);
    axis_.fill(0); wheel_remainder_.fill(0); line_remainder_.fill(0); discrete_.fill(0); value120_.fill(0);
    if (compose_) xkb_compose_state_reset(compose_);
  }

 private:
  struct Output { Proxy* proxy; unsigned scale = 1; };
  Options options_;
  Terminal terminal_;
  std::unique_ptr<zwayland::client::Display> display_;
  std::unique_ptr<Gpu> gpu_;
  std::unique_ptr<Clipboard> clipboard_;
  std::function<Options()> reload_;
  bool managed_ = false, presented_ = false;
  std::string directory_;
  std::vector<std::string> environment_;
  bool reload_pending_ = false;
  Proxy *registry_ = nullptr, *compositor_ = nullptr, *wm_ = nullptr, *seat_ = nullptr,
        *keyboard_ = nullptr, *pointer_ = nullptr, *dmabuf_ = nullptr, *surface_ = nullptr,
        *xdg_ = nullptr, *top_ = nullptr, *frame_ = nullptr, *viewporter_ = nullptr,
        *fractional_manager_ = nullptr, *viewport_ = nullptr, *fractional_ = nullptr,
        *cursor_manager_ = nullptr, *cursor_device_ = nullptr, *data_manager_ = nullptr;
  std::unordered_map<std::uint32_t, Output> outputs_;
  std::vector<std::uint32_t> entered_;
  std::vector<Format> table_, formats_;
  dev_t device_ = 0, tranche_device_ = 0;
  bool feedback_done_ = false, running_ = true, configured_ = false, resize_ = true,
       buffer_available_ = true, cursor_on_ = true;
  int width_ = 960, height_ = 600;
  unsigned scale_ = 120;
  bool grid_initialized_ = false, grid_resize_pending_ = false;
  int pending_rows_ = 24, pending_columns_ = 80;
  std::chrono::steady_clock::time_point grid_resize_deadline_{};
  winsize last_pty_size_{};
  int pty_ = -1, signals_ = -1, repeat_timer_ = -1, blink_timer_ = -1, selection_timer_ = -1;
  pid_t child_ = -1;
  sigset_t original_mask_{};
  std::string output_;
  std::string shell_;
  xkb_context* xkb_ = nullptr;
  xkb_keymap* keymap_ = nullptr;
  xkb_state* key_state_ = nullptr;
  xkb_compose_table* compose_table_ = nullptr;
  xkb_compose_state* compose_ = nullptr;
  int repeat_rate_ = 25, repeat_delay_ = 600;
  std::uint32_t repeat_key_ = 0;
  std::array<double, 2> axis_{}, wheel_remainder_{}, line_remainder_{};
  std::array<int, 2> discrete_{}, value120_{};
  std::uint32_t axis_source_ = p::WL_POINTER_AXIS_SOURCE_WHEEL;
  std::array<bool, 5> reported_buttons_{};
  double pointer_x_ = 0, pointer_y_ = 0;
  bool selecting_ = false, suppress_repeat_ = false;
  std::uint32_t input_serial_ = 0, click_time_ = 0;
  Position click_cell_{};
  unsigned click_count_ = 0;

  void global(std::uint32_t name, const std::string& interface, std::uint32_t version) {
    const auto bind = [&](const auto& type, unsigned maximum) {
      return core::bind(*display_, *registry_, name, type, std::min(version, maximum));
    };
    if (interface == "wl_compositor") {
      if (version >= 4) compositor_ = bind(p::wl_compositor_interface, 4);
    } else if (interface == "xdg_wm_base") {
      wm_ = bind(p::xdg_wm_base_interface, 1);
      wm_->set_observer([this](Proxy& proxy, std::uint32_t, Parser& parser) {
        p::xdg_wm_base_pong(*display_, proxy.id, parser.read_uint());
      });
    } else if (interface == "wl_seat" && !seat_) {
      seat_ = bind(p::wl_seat_interface, 8);
      seat_->set_observer([this](Proxy&, std::uint32_t opcode, Parser& parser) {
        if (opcode == 0) {
          const auto capabilities = parser.read_uint();
          if ((capabilities & p::WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard_) {
            keyboard_ = display_->find_proxy(p::wl_seat_get_keyboard(*display_, seat_->id));
            keyboard_->set_observer([this](Proxy&, std::uint32_t event, Parser& data) { keyboard_event(event, data); });
          }
          if ((capabilities & p::WL_SEAT_CAPABILITY_POINTER) && !pointer_) {
            pointer_ = display_->find_proxy(p::wl_seat_get_pointer(*display_, seat_->id));
            pointer_->set_observer([this](Proxy&, std::uint32_t event, Parser& data) { pointer_event(event, data); });
          }
        } else (void)parser.read_string();
      });
    } else if (interface == "wl_output") {
      auto* proxy = bind(p::wl_output_interface, 4);
      outputs_.emplace(name, Output{proxy});
      proxy->set_observer([this, name](Proxy&, std::uint32_t opcode, Parser& parser) {
        if (opcode == 0) {
          for (int i = 0; i < 5; ++i) (void)parser.read_int();
          (void)parser.read_string(); (void)parser.read_string(); (void)parser.read_int();
        } else if (opcode == 1) {
          (void)parser.read_uint(); for (int i = 0; i < 3; ++i) (void)parser.read_int();
        } else if (opcode == 3) {
          auto it = outputs_.find(name);
          if (it != outputs_.end()) it->second.scale = std::clamp(parser.read_int(), 1, 8);
          if (!fractional_) update_integer_scale();
        } else if (opcode == 4 || opcode == 5) (void)parser.read_string();
      });
    } else if (interface == "zwp_linux_dmabuf_v1" && version >= 4) {
      dmabuf_ = bind(p::zwp_linux_dmabuf_v1_interface, 4);
      const auto feedback = p::zwp_linux_dmabuf_v1_get_default_feedback(*display_, dmabuf_->id);
      display_->find_proxy(feedback)->set_observer([this](Proxy&, std::uint32_t opcode, Parser& parser) { feedback_event(opcode, parser); });
    } else if (interface == "wp_viewporter") viewporter_ = bind(p::wp_viewporter_interface, 1);
    else if (interface == "wp_fractional_scale_manager_v1") fractional_manager_ = bind(p::wp_fractional_scale_manager_v1_interface, 1);
    else if (interface == "wp_cursor_shape_manager_v1") cursor_manager_ = bind(p::wp_cursor_shape_manager_v1_interface, 1);
    else if (interface == "wl_data_device_manager") data_manager_ = bind(p::wl_data_device_manager_interface, 3);
    else if (interface == "ext_zwwm_manager_v1" && !options_.standalone) {
      const auto manager = bind(p::ext_zwwm_manager_v1_interface, 7);
      const auto subscription = p::ext_zwwm_manager_v1_subscribe(*display_, manager->id, p::EXT_ZWWM_MANAGER_V1_EVENT_MASK_CONFIG);
      display_->find_proxy(subscription)->set_observer([this](Proxy&, std::uint32_t, Parser& parser) {
        if (parser.read_string() == "config") reload_pending_ = true;
      });
    }
    if (data_manager_ && seat_ && !clipboard_) {
      clipboard_ = std::make_unique<Clipboard>(*display_, *data_manager_, *seat_);
      clipboard_->received = [this](std::string text) {
        try { terminal_.paste(text); }
        catch (const std::exception& error) { std::fprintf(stderr, "zterm: %s\n", error.what()); }
      };
    }
  }

  void feedback_event(std::uint32_t opcode, Parser& parser) {
    if (opcode == 0) { feedback_done_ = true; return; }
    if (opcode == 1) {
      const int fd = parser.read_fd(); const auto size = parser.read_uint();
      struct stat info{};
      if (size == 0 || size > 16 * 1024 * 1024 || size % sizeof(Format) || fstat(fd, &info) != 0 || info.st_size < size) {
        close(fd); throw std::runtime_error("invalid DMA-BUF format table");
      }
      void* data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
      if (data == MAP_FAILED) throw std::runtime_error("cannot read DMA-BUF format table");
      table_.resize(size / sizeof(Format)); std::memcpy(table_.data(), data, size); munmap(data, size);
      formats_.clear();
    } else if (opcode == 2 || opcode == 4) {
      const auto bytes = parser.read_array();
      if (bytes.size() != sizeof(dev_t)) throw std::runtime_error("invalid GPU device identifier");
      std::memcpy(opcode == 2 ? &device_ : &tranche_device_, bytes.data(), sizeof(dev_t));
    } else if (opcode == 5) {
      const auto bytes = parser.read_array();
      if (bytes.size() % sizeof(std::uint16_t)) throw std::runtime_error("invalid GPU format indices");
      for (size_t i = 0; i < bytes.size(); i += sizeof(std::uint16_t)) {
        std::uint16_t index = 0; std::memcpy(&index, bytes.data() + i, sizeof(index));
        if (index >= table_.size()) throw std::runtime_error("invalid GPU format index");
        if (tranche_device_ == device_) formats_.push_back(table_[index]);
      }
    } else if (opcode == 6) (void)parser.read_uint();
  }

  void update_integer_scale() {
    unsigned integer = 1;
    for (const auto id : entered_) for (const auto& [name, output] : outputs_) {
      (void)name;
      if (output.proxy->id == id) integer = std::max(integer, output.scale);
    }
    if (scale_ != integer * 120) { scale_ = integer * 120; resize_ = true; }
  }

  void configure_gpu(const Options& options) {
    gpu_->configure(options.fonts, options.weight, options.text_rounding, options.icon_scale,
                     options.background, options.selection, options.background_opacity);
  }

  void reload_config() {
    reload_pending_ = false;
    try {
      auto next = reload_();
      configure_gpu(next);
      terminal_.set_palette(next.palette);
      terminal_.set_default_colors(next.foreground, next.background);
      terminal_.set_history_limit(next.history_lines);
      terminal_.set_cursor_style(next.cursor);
      terminal_.set_input_options(next.mouse_reporting, next.alternate_scroll);
      options_ = std::move(next);
      resize_ = true;
    } catch (const std::exception& error) { std::fprintf(stderr, "zterm: config reload failed: %s\n", error.what()); }
  }

  void resize() {
    const unsigned width = (static_cast<unsigned>(width_) * scale_ + 119) / 120;
    const unsigned height = (static_cast<unsigned>(height_) * scale_ + 119) / 120;
    gpu_->resize(width, height, std::max(1U, (options_.size * scale_ + 60) / 120));
    gpu_->set_padding((options_.padding_x * scale_ + 60) / 120,(options_.padding_y * scale_ + 60) / 120);
    terminal_.set_cell_size(gpu_->cell_width(), gpu_->cell_height());
    if (viewport_) p::wp_viewport_set_destination(*display_, viewport_->id, width_, height_);
    else p::wl_surface_set_buffer_scale(*display_, surface_->id, scale_ / 120);
    const int rows = std::clamp(gpu_->content_height() / gpu_->cell_height(), 1U, 512U);
    const int columns = std::clamp(gpu_->content_width() / gpu_->cell_width(), 1U, 1024U);
    if (!grid_initialized_) {
      pending_rows_ = rows; pending_columns_ = columns; apply_grid_resize();
    } else if (rows != terminal_.rows() || columns != terminal_.columns()) {
      if (!grid_resize_pending_ || rows != pending_rows_ || columns != pending_columns_)
        grid_resize_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(45);
      pending_rows_ = rows; pending_columns_ = columns; grid_resize_pending_ = true;
    } else { grid_resize_pending_ = false; notify_pty_size(); }
    terminal_.dirty = true; buffer_available_ = true; resize_ = false;
  }

  void notify_pty_size() {
    winsize size{}; size.ws_row = terminal_.rows(); size.ws_col = terminal_.columns();
    size.ws_xpixel = std::min(65535U, terminal_.columns() * gpu_->cell_width());
    size.ws_ypixel = std::min(65535U, terminal_.rows() * gpu_->cell_height());
    if (pty_ < 0 || (size.ws_row == last_pty_size_.ws_row && size.ws_col == last_pty_size_.ws_col &&
                    size.ws_xpixel == last_pty_size_.ws_xpixel && size.ws_ypixel == last_pty_size_.ws_ypixel)) return;
    if (ioctl(pty_, TIOCSWINSZ, &size) == 0) last_pty_size_ = size;
  }

  void apply_grid_resize() {
    terminal_.resize(pending_rows_, pending_columns_);
    grid_initialized_ = true; grid_resize_pending_ = false;
    selecting_ = false; selection_timer(); notify_pty_size();
  }

  void render() {
    if (!gpu_->draw(terminal_, *surface_, *dmabuf_, cursor_on_)) { buffer_available_ = false; return; }
    terminal_.dirty = false;
    frame_ = display_->find_proxy(p::wl_surface_frame(*display_, surface_->id));
    frame_->set_observer([this](Proxy& proxy, std::uint32_t, Parser& parser) {
      (void)parser.read_uint(); const auto id = proxy.id; frame_ = nullptr; presented_ = true; display_->destroy_proxy(id);
    });
    p::wl_surface_commit(*display_, surface_->id);
  }

  void start_pty() {
    std::unordered_map<std::string, std::string> values;
    const auto insert = [&](const std::string& entry) {
      const auto separator = entry.find('=');
      if (separator == std::string::npos || separator == 0) throw std::runtime_error("invalid shell environment");
      values[entry.substr(0,separator)] = entry.substr(separator+1);
    };
    if (environment_.empty()) for (char** entry = environ; *entry; ++entry) insert(*entry);
    else for (const auto& entry : environment_) insert(entry);
    std::vector<std::string> command = options_.command.empty() ? options_.shell : options_.command;
    if (command.empty()) {
      std::vector<char> storage(16384);
      passwd entry{}; passwd* result = nullptr;
      int error = 0;
      while ((error = getpwuid_r(getuid(), &entry, storage.data(), storage.size(), &result)) == ERANGE && storage.size() < 1024 * 1024)
        storage.resize(storage.size() * 2);
      if (error == 0 && result && result->pw_shell && *result->pw_shell) shell_ = result->pw_shell;
      else if (values.contains("SHELL") && !values["SHELL"].empty()) shell_ = values["SHELL"];
      else shell_ = "/bin/sh";
      values["SHELL"] = shell_;
      command.push_back(shell_);
    } else if (options_.command.empty()) {
      values["SHELL"] = command.front();
    }
    values["TERM"] = "xterm-256color"; values["COLORTERM"] = "truecolor"; values.erase("WAYLAND_SOCKET");
    const auto directory = directory_.empty() ? std::filesystem::current_path() : std::filesystem::path(directory_);
    values["PWD"] = directory.string();
    for (const char* name : {"WAYLAND_DISPLAY","XDG_RUNTIME_DIR"}) if (const auto* value = std::getenv(name)) values[name] = value;
    std::vector<char*> arguments;
    for (auto& argument : command) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    if (!managed_) {
      sigset_t mask; sigemptyset(&mask);
      for (int signal : {SIGCHLD, SIGTERM, SIGINT, SIGHUP}) sigaddset(&mask, signal);
      if (sigprocmask(SIG_BLOCK, &mask, &original_mask_) != 0) throw std::runtime_error("cannot block signals");
      signals_ = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
      if (signals_ < 0) throw std::runtime_error("cannot create signal descriptor");
    }
    pty_ = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    char slave[256]{};
    if (pty_ < 0 || grantpt(pty_) != 0 || unlockpt(pty_) != 0 || ptsname_r(pty_, slave, sizeof(slave)) != 0)
      throw std::runtime_error("cannot create PTY");
    struct winsize size{}; size.ws_row = 24; size.ws_col = 80;
    last_pty_size_ = size;
    (void)ioctl(pty_, TIOCSWINSZ, &size);
    struct Spawn {
      posix_spawn_file_actions_t files{};
      posix_spawnattr_t attributes{};
      Spawn() {
        if (posix_spawn_file_actions_init(&files) != 0) throw std::runtime_error("cannot initialize shell launch");
        if (posix_spawnattr_init(&attributes) != 0) { posix_spawn_file_actions_destroy(&files); throw std::runtime_error("cannot initialize shell attributes"); }
      }
      ~Spawn() { posix_spawn_file_actions_destroy(&files); posix_spawnattr_destroy(&attributes); }
    } spawn;
    int error = posix_spawn_file_actions_addchdir_np(&spawn.files, directory.c_str());
    if (!error) error = posix_spawn_file_actions_addopen(&spawn.files, 0, slave, O_RDWR, 0);
    if (!error) error = posix_spawn_file_actions_adddup2(&spawn.files, 0, 1);
    if (!error) error = posix_spawn_file_actions_adddup2(&spawn.files, 0, 2);
    sigset_t mask, defaults; sigemptyset(&mask); sigemptyset(&defaults);
    for (int signal : {SIGINT,SIGTERM,SIGHUP,SIGCHLD,SIGPIPE}) sigaddset(&defaults, signal);
    if (!error) error = posix_spawnattr_setsigmask(&spawn.attributes, &mask);
    if (!error) error = posix_spawnattr_setsigdefault(&spawn.attributes, &defaults);
    if (!error) error = posix_spawnattr_setflags(&spawn.attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);
    std::vector<std::string> environment;
    for (const auto& [name,value] : values) environment.push_back(name + "=" + value);
    std::vector<char*> env; for (auto& entry : environment) env.push_back(entry.data()); env.push_back(nullptr);
    std::filesystem::path executable(command.front());
    if (!executable.has_parent_path()) {
      const std::string path = values.contains("PATH") ? values["PATH"] : "/usr/bin:/bin";
      for (size_t start = 0; start <= path.size();) {
        const auto end = path.find(':', start);
        const auto folder = path.substr(start, end == std::string::npos ? end : end-start);
        auto candidate = std::filesystem::path(folder.empty() ? "." : folder) / command.front();
        if (candidate.is_relative()) candidate = directory / candidate;
        if (access(candidate.c_str(), X_OK) == 0) { executable = candidate; break; }
        if (end == std::string::npos) break;
        start = end + 1;
      }
    } else if (executable.is_relative()) executable = directory / executable;
    if (!error) error = posix_spawn(&child_, executable.c_str(), &spawn.files, &spawn.attributes, arguments.data(), env.data());
    if (error) {
      child_ = -1; throw std::runtime_error("cannot launch shell: " + std::string(std::strerror(error)));
    }
    const int flags = fcntl(pty_, F_GETFL);
    if (flags < 0 || fcntl(pty_, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(pty_, F_SETFD, FD_CLOEXEC) < 0)
      throw std::runtime_error("cannot configure PTY");
  }

  void read_pty() {
    std::array<char, 16384> data{};
    for (size_t total = 0; total < 1024 * 1024;) {
      const ssize_t size = read(pty_, data.data(), data.size());
      if (size > 0) { terminal_.feed({data.data(), static_cast<size_t>(size)}); total += size; }
      else if (size < 0 && errno == EINTR) continue;
      else { if (size == 0 || (size < 0 && errno == EIO)) running_ = false; break; }
    }
  }

  void write_pty() {
    const ssize_t written = write(pty_, output_.data(), output_.size());
    if (written > 0) output_.erase(0, written);
    else if (written < 0 && errno != EAGAIN && errno != EINTR) running_ = false;
  }

  void timers() {
    repeat_timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    blink_timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    selection_timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (repeat_timer_ < 0 || blink_timer_ < 0 || selection_timer_ < 0) throw std::runtime_error("cannot create timers");
    itimerspec timer{}; timer.it_value.tv_nsec = timer.it_interval.tv_nsec = 500000000;
    if (timerfd_settime(blink_timer_, 0, &timer, nullptr) != 0) throw std::runtime_error("cannot arm cursor timer");
  }

  void repeat(std::uint32_t key, bool pressed) {
    if (repeat_timer_ < 0) return;
    if (!pressed && key != repeat_key_) return;
    itimerspec timer{};
    if (pressed && !suppress_repeat_ && repeat_rate_ > 0 && keymap_ && xkb_keymap_key_repeats(keymap_, key + 8)) {
      repeat_key_ = key;
      const long delay = std::max(repeat_delay_, 1);
      timer.it_value = {delay / 1000, (delay % 1000) * 1000000};
      const long interval = 1000000000L / std::clamp(repeat_rate_, 1, 1000);
      timer.it_interval = {interval / 1000000000L, interval % 1000000000L};
    }
    (void)timerfd_settime(repeat_timer_, 0, &timer, nullptr);
  }

  void keyboard_event(std::uint32_t opcode, Parser& parser) {
    if (opcode == 0) {
      const auto format = parser.read_uint(); const int fd = parser.read_fd(); const auto size = parser.read_uint();
      struct stat info{};
      if (format != p::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > 16 * 1024 * 1024 ||
          fstat(fd, &info) != 0 || info.st_size < size) { close(fd); throw std::runtime_error("invalid keyboard keymap"); }
      void* data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
      if (data == MAP_FAILED) throw std::runtime_error("cannot map keymap");
      if (static_cast<const char*>(data)[size - 1] != '\0') { munmap(data, size); throw std::runtime_error("unterminated keymap"); }
      if (!xkb_) xkb_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      auto* map = xkb_keymap_new_from_string(xkb_, static_cast<const char*>(data), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
      munmap(data, size);
      if (!map) throw std::runtime_error("cannot compile keyboard keymap");
      if (key_state_) xkb_state_unref(key_state_);
      if (keymap_) xkb_keymap_unref(keymap_);
      keymap_ = map; key_state_ = xkb_state_new(map);
      if (!key_state_) throw std::runtime_error("cannot initialize keyboard state");
      if (!compose_table_) {
        const char* locale = setlocale(LC_CTYPE, nullptr);
        compose_table_ = xkb_compose_table_new_from_locale(xkb_, locale ? locale : "C.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (compose_table_) compose_ = xkb_compose_state_new(compose_table_, XKB_COMPOSE_STATE_NO_FLAGS);
      }
    } else if (opcode == 1) {
      (void)parser.read_uint(); (void)parser.read_object(); (void)parser.read_array();
      terminal_.focus(true);
    } else if (opcode == 2) {
      (void)parser.read_uint(); (void)parser.read_object(); repeat(repeat_key_, false);
      terminal_.focus(false);
      if (compose_) xkb_compose_state_reset(compose_);
    } else if (opcode == 3) {
      input_serial_ = parser.read_uint(); (void)parser.read_uint();
      const auto code = parser.read_uint(); const bool pressed = parser.read_uint() != 0;
      if (pressed) { suppress_repeat_ = false; key(code); }
      repeat(code, pressed);
    } else if (opcode == 4) {
      (void)parser.read_uint(); const auto depressed = parser.read_uint(), latched = parser.read_uint(), locked = parser.read_uint(), group = parser.read_uint();
      if (key_state_) xkb_state_update_mask(key_state_, depressed, latched, locked, 0, 0, group);
    } else if (opcode == 5) {
      repeat_rate_ = std::clamp(parser.read_int(), 0, 1000); repeat_delay_ = std::clamp(parser.read_int(), 0, 10000);
      repeat(repeat_key_, false);
    }
  }

  unsigned current_modifiers() const {
    if (!key_state_) return ModNone;
    unsigned modifiers = 0;
    if (xkb_state_mod_name_is_active(key_state_, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) modifiers |= ModShift;
    if (xkb_state_mod_name_is_active(key_state_, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) modifiers |= ModCtrl;
    if (xkb_state_mod_name_is_active(key_state_, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) modifiers |= ModAlt;
    return modifiers;
  }

  void key(std::uint32_t code) {
    if (!key_state_) return;
    auto symbol = xkb_state_key_get_one_sym(key_state_, code + 8);
    const auto modifiers = current_modifiers();
    if ((modifiers & (ModCtrl | ModShift)) == (ModCtrl | ModShift) && !(modifiers & ModAlt)) {
      const auto lower = xkb_keysym_to_lower(symbol);
      if (lower == XKB_KEY_c || lower == XKB_KEY_v || lower == XKB_KEY_r) {
        suppress_repeat_ = true;
        if (lower == XKB_KEY_r) reload_pending_ = true;
        else if (clipboard_) {
          if (lower == XKB_KEY_v) clipboard_->paste();
          else {
            try { clipboard_->copy(terminal_.selected_text(), input_serial_); }
            catch (const std::exception& error) { std::fprintf(stderr, "zterm: %s\n", error.what()); }
          }
        }
        return;
      }
    }
    if (modifiers & ModShift) {
      if (symbol == XKB_KEY_Page_Up) { terminal_.scroll(terminal_.rows() - 1); return; }
      if (symbol == XKB_KEY_Page_Down) { terminal_.scroll(1 - terminal_.rows()); return; }
    }
    Key special = Key::None;
    switch (symbol) {
      case XKB_KEY_Return: case XKB_KEY_KP_Enter: special = Key::Enter; break;
      case XKB_KEY_BackSpace: special = Key::Backspace; break;
      case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: special = Key::Tab; break;
      case XKB_KEY_Escape: special = Key::Escape; break;
      case XKB_KEY_Up: special = Key::Up; break;
      case XKB_KEY_Down: special = Key::Down; break;
      case XKB_KEY_Left: special = Key::Left; break;
      case XKB_KEY_Right: special = Key::Right; break;
      case XKB_KEY_Home: special = Key::Home; break;
      case XKB_KEY_End: special = Key::End; break;
      case XKB_KEY_Insert: special = Key::Insert; break;
      case XKB_KEY_Delete: special = Key::Delete; break;
      case XKB_KEY_Page_Up: special = Key::PageUp; break;
      case XKB_KEY_Page_Down: special = Key::PageDown; break;
      default:
        if (symbol >= XKB_KEY_F1 && symbol <= XKB_KEY_F35) special = static_cast<Key>(static_cast<unsigned>(Key::Function0) + symbol - XKB_KEY_F1 + 1);
    }
    if (special != Key::None) terminal_.key(special, modifiers);
    else {
      if (compose_ && xkb_compose_state_feed(compose_, symbol) == XKB_COMPOSE_FEED_ACCEPTED) {
        const auto status = xkb_compose_state_get_status(compose_);
        if (status == XKB_COMPOSE_COMPOSING) return;
        if (status == XKB_COMPOSE_COMPOSED) { symbol = xkb_compose_state_get_one_sym(compose_); xkb_compose_state_reset(compose_); }
        if (status == XKB_COMPOSE_CANCELLED) { xkb_compose_state_reset(compose_); return; }
      }
      const auto character = xkb_keysym_to_utf32(symbol);
      if (character) terminal_.character(character, modifiers);
    }
    cursor_on_ = true;
  }

  Position pointer_cell() const {
    if (!gpu_) return {};
    return {std::clamp(static_cast<int>(std::floor((pointer_y_ * scale_ / 120.0 - gpu_->padding_y()) / gpu_->cell_height())), 0, terminal_.rows() - 1),
            std::clamp(static_cast<int>(std::floor((pointer_x_ * scale_ / 120.0 - gpu_->padding_x()) / gpu_->cell_width())), 0, terminal_.columns() - 1)};
  }

  void extend_selection() {
    const auto point = pointer_cell(); terminal_.extend_selection(point.row, point.col);
  }

  MousePosition mouse_position() const {
    const auto cell = pointer_cell();
    return {cell.row, cell.col, static_cast<int>(pointer_x_ * scale_ / 120.0 - gpu_->padding_x()),
            static_cast<int>(pointer_y_ * scale_ / 120.0 - gpu_->padding_y())};
  }

  double content_top() const { return gpu_ ? gpu_->padding_y() * 120.0 / scale_ : 0; }
  double content_bottom() const { return gpu_ ? (gpu_->padding_y() + gpu_->content_height()) * 120.0 / scale_ : height_; }

  void selection_timer() {
    if (selection_timer_ < 0) return;
    itimerspec timer{};
    if (selecting_ && (pointer_y_ < content_top() || pointer_y_ >= content_bottom())) timer.it_value.tv_nsec = timer.it_interval.tv_nsec = 40000000;
    (void)timerfd_settime(selection_timer_, 0, &timer, nullptr);
  }

  void flush_scroll() {
    if (!gpu_) { axis_.fill(0); discrete_.fill(0); value120_.fill(0); return; }
    const auto modifiers = current_modifiers();
    const bool finger = axis_source_ == p::WL_POINTER_AXIS_SOURCE_FINGER || axis_source_ == p::WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    for (unsigned axis = 0; axis < 2; ++axis) {
      if (axis_[axis] == 0 && discrete_[axis] == 0 && value120_[axis] == 0) continue;
      const bool horizontal = axis == p::WL_POINTER_AXIS_HORIZONTAL_SCROLL;
      const double cell = (horizontal ? gpu_->cell_width() : gpu_->cell_height()) * 120.0 / scale_;
      const double sensitivity = finger ? options_.touchpad_scroll_per_mille / 1000.0 : 1;
      const double ticks = (value120_[axis] ? value120_[axis] / 120.0 : discrete_[axis] ? discrete_[axis] : axis_[axis] / (finger ? cell : 15.0)) * sensitivity;
      const bool reporting = terminal_.reports_mouse() && !(modifiers & ModShift) && !selecting_;
      auto& remainder = reporting ? wheel_remainder_[axis] : line_remainder_[axis];
      remainder += reporting ? ticks : finger ? ticks : ticks * options_.scroll_lines;
      remainder = std::clamp(remainder, -256.0, 256.0);
      const int steps = static_cast<int>(remainder); remainder -= steps;
      (reporting ? line_remainder_[axis] : wheel_remainder_[axis]) = 0;
      if (!steps) continue;
      if (reporting) terminal_.mouse_wheel(horizontal, steps, mouse_position(), modifiers);
      else if (!terminal_.alternate_scroll(steps, horizontal, modifiers) && !horizontal) terminal_.scroll(-steps);
    }
    axis_.fill(0); discrete_.fill(0); value120_.fill(0);
    if (selecting_) extend_selection();
  }

  void pointer_event(std::uint32_t opcode, Parser& parser) {
    if (opcode == 0) {
      const auto serial = parser.read_uint(); (void)parser.read_object(); pointer_x_ = parser.read_fixed(); pointer_y_ = parser.read_fixed();
      if (cursor_manager_) {
        if (!cursor_device_) cursor_device_ = display_->find_proxy(p::wp_cursor_shape_manager_v1_get_pointer(*display_, cursor_manager_->id, pointer_));
        p::wp_cursor_shape_device_v1_set_shape(*display_, cursor_device_->id, serial, p::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
      }
    } else if (opcode == 1) {
      (void)parser.read_uint(); (void)parser.read_object(); selecting_ = false; selection_timer();
      for (unsigned button = 0; button < reported_buttons_.size(); ++button) if (reported_buttons_[button]) {
        (void)terminal_.mouse_button(button, false, mouse_position(), current_modifiers()); reported_buttons_[button] = false;
      }
    }
    else if (opcode == 2) {
      (void)parser.read_uint(); pointer_x_ = parser.read_fixed(); pointer_y_ = parser.read_fixed();
      if (gpu_ && !selecting_ && (!(current_modifiers() & ModShift) || std::any_of(reported_buttons_.begin(), reported_buttons_.end(), [](bool pressed) { return pressed; })))
        (void)terminal_.mouse_motion(mouse_position(), current_modifiers());
      if (selecting_ && gpu_) {
        if (pointer_y_ < content_top()) terminal_.scroll(1);
        else if (pointer_y_ >= content_bottom()) terminal_.scroll(-1);
        extend_selection();
        selection_timer();
      }
    } else if (opcode == 3) {
      input_serial_ = parser.read_uint(); const auto time = parser.read_uint();
      const auto button = parser.read_uint(), state = parser.read_uint();
      const int index = button == 0x110 ? 0 : button == 0x112 ? 1 : button == 0x111 ? 2 : button == 0x113 ? 3 : button == 0x114 ? 4 : -1;
      const bool pressed = state == p::WL_POINTER_BUTTON_STATE_PRESSED;
      if (gpu_ && index >= 0) {
        const auto modifiers = current_modifiers();
        if (!pressed && reported_buttons_[index]) {
          (void)terminal_.mouse_button(index, false, mouse_position(), modifiers); reported_buttons_[index] = false; return;
        }
        if (pressed && !(modifiers & ModShift) && terminal_.mouse_button(index, true, mouse_position(), modifiers)) {
          reported_buttons_[index] = true; selecting_ = false; selection_timer(); return;
        }
        if (index == 1 && pressed && clipboard_) { clipboard_->paste(); return; }
      }
      if (button == 0x110 && gpu_) {
        if (state == p::WL_POINTER_BUTTON_STATE_PRESSED) {
          const auto point = pointer_cell();
          click_count_ = point.row == click_cell_.row && point.col == click_cell_.col && time - click_time_ < 400 ? click_count_ % 3 + 1 : 1;
          click_time_ = time; click_cell_ = point;
          const bool shift = key_state_ && xkb_state_mod_name_is_active(key_state_, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
          if (shift && terminal_.has_selection()) terminal_.extend_selection(point.row, point.col);
          else terminal_.begin_selection(point.row, point.col, click_count_ == 3 ? SelectionMode::Line : click_count_ == 2 ? SelectionMode::Word : SelectionMode::Character);
          selecting_ = true;
        } else selecting_ = false;
        selection_timer();
      }
    }
    else if (opcode == 4) {
      (void)parser.read_uint(); const auto axis = parser.read_uint(); const auto delta = parser.read_fixed();
      if (axis < axis_.size()) axis_[axis] += delta;
      if (pointer_->version < 5) flush_scroll();
    } else if (opcode == 5) {
      flush_scroll();
    } else if (opcode == 6) axis_source_ = parser.read_uint();
    else if (opcode == 7) { (void)parser.read_uint(); (void)parser.read_uint(); }
    else if (opcode == 8 || opcode == 9) {
      const auto axis = parser.read_uint(); const auto value = parser.read_int();
      if (axis < axis_.size()) { if (opcode == 8) discrete_[axis] += value; else value120_[axis] += value; }
    }
  }
};

std::unique_ptr<Session> prepare_session(Options options) {
  auto session = std::make_unique<App>(std::move(options), [] {
    char name[] = "zterm"; char* args[]{name}; return load_options(1,args);
  }, true);
  session->prepare(); return session;
}

}  // namespace zterm

int main(int argc, char** argv) {
  try {
    (void)setlocale(LC_ALL, "");
    if (MB_CUR_MAX < 4) (void)setlocale(LC_CTYPE, "C.UTF-8");
    auto options = zterm::load_options(argc, argv);
    if (options.action == zterm::Options::Action::Help) { zterm::print_help(); return 0; }
    if (options.action == zterm::Options::Action::Daemon) return zterm::run_daemon();
    return zterm::App(std::move(options), [argc, argv] { return zterm::load_options(argc, argv); }).run();
  } catch (const std::exception& error) { std::fprintf(stderr, "zterm: %s\n", error.what()); return 1; }
}
