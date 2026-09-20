#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "zwwm/portal_capture.hpp"
#include "zwwm/unique_fd.hpp"

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <zwwm-screencopy-view-unstable-v1-zwayland-server.h>
#include <ext-zwwm-toplevels-unstable-v1-zwayland-server.h>
#include <xdg-output-zwayland-server.h>

#include <cerrno>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

namespace protocol = zwayland::generated;

namespace zwwm {
namespace {

constexpr std::uint32_t kMagic = 0x5a574350U;
enum class MessageType : std::uint32_t { attach = 1, ready = 2 };
struct Message {
  std::uint32_t magic = kMagic;
  MessageType type = MessageType::attach;
};

bool send_message(int socket, const Message& message) {
  iovec io{.iov_base = const_cast<Message*>(&message), .iov_len = sizeof(message)};
  msghdr header{};
  header.msg_iov = &io;
  header.msg_iovlen = 1;
  return sendmsg(socket, &header, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(message));
}

}  // namespace

struct PortalCapture::Impl {
  struct Frame { Impl* owner = nullptr; zwayland::server::Resource* resource = nullptr; OutputCapture capture; std::uint64_t target_id = 0; std::uint32_t x = 0, y = 0, width = 0, height = 0; bool copied = false, terminal = false; };
  struct CanonicalFrame { Impl* owner = nullptr; zwayland::server::Resource* resource = nullptr; OutputCapture capture; std::uint32_t x = 0, y = 0, width = 0, height = 0; bool copied = false, terminal = false; };
  struct Handle { Impl* owner = nullptr; zwayland::server::Resource* resource = nullptr; zwayland::server::Resource* manager = nullptr; ToplevelInfo info; bool closed = false; };
  struct ClientListener {
    zwayland::server::Listener listener{};
    Impl* owner = nullptr;
  } client_listener;

  zwayland::server::Display* display = nullptr;
  zwayland::server::EventLoop* event_loop = nullptr;
  Capture capture;
  Copy copy;
  MapRegion map_region;
  Toplevels toplevels;
  Output output;
  ResolveOutput resolve_output;
  PortalClient portal_client;
  std::uint32_t screencopy = 0;
  std::uint32_t canonical_screencopy = 0;
  std::uint32_t xdg_output_manager = 0;
  std::uint32_t toplevel_manager = 0;
  int source = -1;
  int child_source = -1;
  int restart_source = -1;
  zwayland::server::Client* client = nullptr;
  UniqueFd socket;
  pid_t child = -1;
  std::string error;
  std::string executable;
  std::vector<Handle*> handles;
  std::vector<Frame*> frames;
  std::vector<CanonicalFrame*> canonical_frames;
  std::vector<zwayland::server::Resource*> managers;
  std::vector<zwayland::server::Resource*> screencopy_managers;
  std::vector<std::pair<zwayland::server::Resource*, OutputInfo>> xdg_outputs;
  bool embedded_cursor = false;
  bool xdg_dimensions_pending = false;
  bool shutting_down = false;

  static void send_xdg_output(zwayland::server::Resource* resource, const OutputInfo& output,
                              zwayland::server::Resource* wl_output) {
    if (resource->version >= 2) protocol::zxdg_output_v1_send_name(*resource, output.connector.c_str());
    protocol::zxdg_output_v1_send_logical_position(*resource, output.logical_x, output.logical_y);
    protocol::zxdg_output_v1_send_logical_size(*resource, static_cast<std::int32_t>(output.logical_width),
                                     static_cast<std::int32_t>(output.logical_height));
    if (resource->version >= 3) {
      if (wl_output != nullptr) protocol::wl_output_send_done(*wl_output);
    } else {
      protocol::zxdg_output_v1_send_done(*resource);
    }
  }

  static bool visible(const zwayland::server::Client* candidate, const std::uint32_t global, void* data) {
    auto* self = static_cast<Impl*>(data);
    return (global != self->screencopy && global != self->toplevel_manager) || candidate == self->client;
  }
  static void frame_destroy(zwayland::server::Resource* resource) { auto* frame = resource->data<Frame>(); if (frame != nullptr && frame->owner != nullptr) std::erase(frame->owner->frames, frame); delete frame; }
  static void canonical_frame_destroy(zwayland::server::Resource* resource) { auto* frame = resource->data<CanonicalFrame>(); if (frame != nullptr && frame->owner != nullptr) std::erase(frame->owner->canonical_frames, frame); delete frame; }
  static void frame_copy(zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* buffer) {
    auto* frame = resource->data<Frame>();
    if (frame == nullptr || frame->copied || frame->terminal) { resource->post_error(protocol::ZWWM_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED, "frame copy already requested or terminal"); return; }
    frame->copied = true;
    if (!frame->owner->copy(buffer, frame->capture, frame->x, frame->y, frame->width, frame->height)) {
      resource->post_error(protocol::ZWWM_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "buffer must be matching XRGB8888 wl_shm storage"); return;
    }
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      frame->terminal = true;
      protocol::zwwm_screencopy_frame_v1_send_failed(*resource, protocol::ZWWM_SCREENCOPY_FRAME_V1_FAILURE_UNKNOWN);
      return;
    }
    frame->terminal = true;
    const auto seconds = static_cast<std::uint64_t>(now.tv_sec);
    protocol::zwwm_screencopy_frame_v1_send_ready(*resource, static_cast<std::uint32_t>(seconds >> 32U),
                                        static_cast<std::uint32_t>(seconds),
                                        static_cast<std::uint32_t>(now.tv_nsec));
  }
  static void capture_request(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* output,
                               std::int32_t cursor, std::int32_t x, std::int32_t y, std::int32_t width,
                               std::int32_t height, bool physical = false, Impl* owner = nullptr,
                               std::uint64_t target_id = 0) {
    auto* self = owner == nullptr ? manager->data<Impl>() : owner;
    if (self == nullptr || client != self->client) { manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "unauthorized output capture"); return; }
    const auto selected = output == nullptr ? self->resolve_output(self->output(client, {})) : self->resolve_output(output);
    if (!selected || (output != nullptr && output != self->output(client, selected->id))) { manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "unknown output capture"); return; }
    auto* resource = client->create_resource(&protocol::zwwm_screencopy_frame_v1_interface, id, 1);
    if (resource == nullptr) { client->post_no_memory(); return; }
    auto* frame = new (std::nothrow) Frame;
    if (frame == nullptr) { resource->destroy(); client->post_no_memory(); return; }
    frame->owner = self; frame->resource = resource;
    struct ZwwmScreencopyFrameV1FrameImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
  void copy(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* buffer) {
    (frame_copy)(&client, &resource, buffer);
  }
};
    resource->set_data(frame); resource->set_handler(protocol::zwwm_screencopy_frame_v1_handler(ZwwmScreencopyFrameV1FrameImplHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (frame_destroy)(&destroyed); });
    self->frames.push_back(frame);
    frame->target_id = target_id;
    frame->capture.output = selected->id;
    const bool overlay_cursor = cursor != 0;
    if ((overlay_cursor && !self->embedded_cursor) || !self->capture(frame->capture, overlay_cursor, target_id) || frame->capture.width == 0 || frame->capture.height == 0) { frame->terminal = true; protocol::zwwm_screencopy_frame_v1_send_failed(*resource, protocol::ZWWM_SCREENCOPY_FRAME_V1_FAILURE_UNAVAILABLE); return; }
    if (target_id != 0 || width < 0 || height < 0) {
      frame->x = 0; frame->y = 0; frame->width = frame->capture.width; frame->height = frame->capture.height;
    } else if (physical) {
      frame->x = std::min(static_cast<std::uint32_t>(std::max(0, x)), frame->capture.width);
      frame->y = std::min(static_cast<std::uint32_t>(std::max(0, y)), frame->capture.height);
      frame->width = std::min(static_cast<std::uint32_t>(std::max(0, width)), frame->capture.width - frame->x);
      frame->height = std::min(static_cast<std::uint32_t>(std::max(0, height)), frame->capture.height - frame->y);
    } else if (!self->map_region(frame->capture, x, y, width, height, &frame->x, &frame->y,
                                 &frame->width, &frame->height)) {
      frame->terminal = true;
      protocol::zwwm_screencopy_frame_v1_send_failed(*resource, protocol::ZWWM_SCREENCOPY_FRAME_V1_FAILURE_UNAVAILABLE);
      return;
    }
    if (frame->width == 0 || frame->height == 0) { frame->terminal = true; protocol::zwwm_screencopy_frame_v1_send_failed(*resource, protocol::ZWWM_SCREENCOPY_FRAME_V1_FAILURE_UNAVAILABLE); return; }
    protocol::zwwm_screencopy_frame_v1_send_buffer(*resource, protocol::WL_SHM_FORMAT_XRGB8888, frame->width, frame->height, frame->width * 4U);
    protocol::zwwm_screencopy_frame_v1_send_flags(*resource, 0);
  }
  static void canonical_frame_copy(zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* buffer) {
    auto* frame = resource->data<CanonicalFrame>();
    if (frame == nullptr || frame->copied || frame->terminal) {
      resource->post_error(protocol::ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED, "frame copy already requested or terminal");
      return;
    }
    frame->copied = true;
    if (!frame->owner->copy(buffer, frame->capture, frame->x, frame->y, frame->width, frame->height)) {
      resource->post_error(protocol::ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "buffer must be matching XRGB8888 wl_shm storage");
      return;
    }
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      frame->terminal = true;
      protocol::zwlr_screencopy_frame_v1_send_failed(*resource);
      return;
    }
    frame->terminal = true;
    const auto seconds = static_cast<std::uint64_t>(now.tv_sec);
    protocol::zwlr_screencopy_frame_v1_send_ready(*resource, static_cast<std::uint32_t>(seconds >> 32U),
                                        static_cast<std::uint32_t>(seconds), static_cast<std::uint32_t>(now.tv_nsec));
  }
  static void canonical_capture_request(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id,
                                        std::int32_t cursor, zwayland::server::Resource* output, std::int32_t x,
                                        std::int32_t y, std::int32_t width, std::int32_t height) {
    auto* self = manager->data<Impl>();
    if (self == nullptr) return;
    const auto selected = output == nullptr ? std::nullopt : self->resolve_output(output);
    if (!selected || output != self->output(client, selected->id)) {
      manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "unknown output capture");
      return;
    }
    auto* resource = client->create_resource(&protocol::zwlr_screencopy_frame_v1_interface, id, 1);
    if (resource == nullptr) { client->post_no_memory(); return; }
    auto* frame = new (std::nothrow) CanonicalFrame;
    if (frame == nullptr) { resource->destroy(); client->post_no_memory(); return; }
    frame->owner = self;
    frame->resource = resource;
    struct ZwlrScreencopyFrameV1FrameImplHandler {
  void copy(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* buffer) {
    (canonical_frame_copy)(&client, &resource, buffer);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
};
    resource->set_data(frame); resource->set_handler(protocol::zwlr_screencopy_frame_v1_handler(ZwlrScreencopyFrameV1FrameImplHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (canonical_frame_destroy)(&destroyed); });
    self->canonical_frames.push_back(frame);
    frame->capture.output = selected->id;
    if (!self->capture(frame->capture, cursor != 0, 0) || frame->capture.width == 0 || frame->capture.height == 0) {
      frame->terminal = true;
      protocol::zwlr_screencopy_frame_v1_send_failed(*resource);
      return;
    }
    if (width < 0 || height < 0) {
      frame->x = 0; frame->y = 0; frame->width = frame->capture.width; frame->height = frame->capture.height;
    } else if (!self->map_region(frame->capture, x, y, width, height, &frame->x, &frame->y, &frame->width, &frame->height)) {
      frame->terminal = true;
      protocol::zwlr_screencopy_frame_v1_send_failed(*resource);
      return;
    }
    protocol::zwlr_screencopy_frame_v1_send_buffer(*resource, protocol::WL_SHM_FORMAT_XRGB8888, frame->width, frame->height, frame->width * 4U);
    protocol::zwlr_screencopy_frame_v1_send_flags(*resource, 0);
  }
  static void bind_canonical_screencopy(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
    auto* self = static_cast<Impl*>(data);
    auto* resource = client->create_resource(&protocol::zwlr_screencopy_manager_v1_interface, id, std::min(version, 1U));
    if (resource == nullptr) { client->post_no_memory(); return; }
    struct ZwlrScreencopyManagerV1ImplHandler {
  void capture_output(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t frame, std::int32_t overlay_cursor, zwayland::server::Resource* output) {
    ([](zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t frame, std::int32_t cursor, zwayland::server::Resource* output) {
          canonical_capture_request(c, r, frame, cursor, output, 0, 0, -1, -1);
        })(&client, &resource, frame, overlay_cursor, output);
  }
  void capture_output_region(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t frame, std::int32_t overlay_cursor, zwayland::server::Resource* output, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    ([](zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t frame, std::int32_t cursor, zwayland::server::Resource* output,
           std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
          canonical_capture_request(c, r, frame, cursor, output, x, y, width, height);
        })(&client, &resource, frame, overlay_cursor, output, x, y, width, height);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
};
    resource->set_data(self); resource->set_handler(protocol::zwlr_screencopy_manager_v1_handler(ZwlrScreencopyManagerV1ImplHandler{}));
  }
  static void get_xdg_output(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* output) {
    auto* self = manager->data<Impl>();
    if (self == nullptr) return;
    const auto info = output == nullptr ? std::nullopt : self->resolve_output(output);
    if (!info || output->client != client) {
      manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "unknown wl_output");
      return;
    }
    auto* resource = client->create_resource(&protocol::zxdg_output_v1_interface, id, std::min(manager->version, 3U));
    if (resource == nullptr) { client->post_no_memory(); return; }
    struct ZxdgOutputV1ImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
};
    resource->set_data(self); resource->set_handler(protocol::zxdg_output_v1_handler(ZxdgOutputV1ImplHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { ([](zwayland::server::Resource* r) {
      auto* owner = r->data<Impl>();
       if (owner != nullptr) std::erase_if(owner->xdg_outputs, [r](const auto& item) { return item.first == r; });
    })(&destroyed); });
    self->xdg_outputs.emplace_back(resource, *info);
    send_xdg_output(resource, *info, output);
  }
  static void bind_xdg_output_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
    auto* resource = client->create_resource(&protocol::zxdg_output_manager_v1_interface, id, std::min(version, 3U));
    if (resource == nullptr) { client->post_no_memory(); return; }
    struct ZxdgOutputManagerV1ImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
  void get_xdg_output(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* output) {
    (Impl::get_xdg_output)(&client, &resource, id, output);
  }
};
    resource->set_data(static_cast<Impl*>(data)); resource->set_handler(protocol::zxdg_output_manager_v1_handler(ZxdgOutputManagerV1ImplHandler{}));
  }
  static void bind_screencopy(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
    auto* self = static_cast<Impl*>(data);
    if (client != self->client) { client->post_error(0, "unauthorized screencopy bind"); return; }
    auto* resource = client->create_resource(&protocol::zwwm_screencopy_view_manager_v1_interface, id, std::min(version, 1U));
    if (resource == nullptr) { client->post_no_memory(); return; }
    struct ZwwmScreencopyViewManagerV1ImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
  void capture_output(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t frame, zwayland::server::Resource* output, std::int32_t overlay_cursor) {
    ([](zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t id, zwayland::server::Resource* o, std::int32_t cursor) { capture_request(c, r, id, o, cursor, 0, 0, -1, -1); })(&client, &resource, frame, output, overlay_cursor);
  }
  void capture_output_region(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t frame, zwayland::server::Resource* output, std::int32_t overlay_cursor, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    ([](zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t id, zwayland::server::Resource* o, std::int32_t cursor, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) { capture_request(c, r, id, o, cursor, x, y, w, h); })(&client, &resource, frame, output, overlay_cursor, x, y, width, height);
  }
};
    self->screencopy_managers.push_back(resource);
    resource->set_data(self); resource->set_handler(protocol::zwwm_screencopy_view_manager_v1_handler(ZwwmScreencopyViewManagerV1ImplHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { ([](zwayland::server::Resource* item) {
      auto* owner = item->data<Impl>();
      if (owner != nullptr) std::erase(owner->screencopy_managers, item);
    })(&destroyed); });
    protocol::zwwm_screencopy_view_manager_v1_send_cursor_modes(*resource,
        protocol::ZWWM_SCREENCOPY_VIEW_MANAGER_V1_CURSOR_MODE_HIDDEN |
        (self->embedded_cursor ? protocol::ZWWM_SCREENCOPY_VIEW_MANAGER_V1_CURSOR_MODE_EMBEDDED : 0));
  }
  static void handle_destroy(zwayland::server::Resource* resource) {
    auto* handle = resource->data<Handle>();
    if (handle != nullptr && handle->owner != nullptr) std::erase(handle->owner->handles, handle);
    delete handle;
  }
  static void handle_capture(zwayland::server::Client* client, zwayland::server::Resource* resource, std::uint32_t id, std::int32_t cursor) {
    auto* handle = resource->data<Handle>();
    if (handle == nullptr || handle->closed || handle->owner == nullptr) { resource->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "closed toplevel handle"); return; }
    capture_request(client, resource, id, handle->owner->output(client, handle->info.output), cursor,
                     0, 0, -1, -1, false, handle->owner, handle->info.id);
  }
  static void send_handle(zwayland::server::Client* client, zwayland::server::Resource* manager, const ToplevelInfo& info) {
    auto* resource = client->create_resource(&protocol::ext_zwwm_toplevel_handle_v1_interface, client->alloc_id(), 1);
    if (resource == nullptr) { client->post_no_memory(); return; }
    auto* handle = new (std::nothrow) Handle;
    if (handle == nullptr) { resource->destroy(); client->post_no_memory(); return; }
    handle->owner = manager->data<Impl>(); handle->resource = resource; handle->manager = manager; handle->info = info;
    struct ExtZwwmToplevelHandleV1ImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
  void capture(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t frame, std::int32_t overlay_cursor) {
    (handle_capture)(&client, &resource, frame, overlay_cursor);
  }
};
    resource->set_data(handle); resource->set_handler(protocol::ext_zwwm_toplevel_handle_v1_handler(ExtZwwmToplevelHandleV1ImplHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (handle_destroy)(&destroyed); });
    handle->owner->handles.push_back(handle);
    protocol::ext_zwwm_toplevels_v1_send_toplevel(*manager, resource);
    protocol::ext_zwwm_toplevel_handle_v1_send_title(*resource, info.title.c_str());
    protocol::ext_zwwm_toplevel_handle_v1_send_app_id(*resource, info.app_id.c_str());
    protocol::ext_zwwm_toplevel_handle_v1_send_geometry(*resource, info.x, info.y, info.width, info.height);
    protocol::ext_zwwm_toplevel_handle_v1_send_state(*resource, info.state);
    protocol::ext_zwwm_toplevel_handle_v1_send_output(*resource, handle->owner->output(client, info.output));
  }
  static void bind_toplevels(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
    auto* self = static_cast<Impl*>(data);
    if (client != self->client) { client->post_error(0, "unauthorized toplevel bind"); return; }
    auto* resource = client->create_resource(&protocol::ext_zwwm_toplevels_v1_interface, id, std::min(version, 1U));
    if (resource == nullptr) { client->post_no_memory(); return; }
    struct ExtZwwmToplevelsV1ImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
};
    resource->set_data(self); resource->set_handler(protocol::ext_zwwm_toplevels_v1_handler(ExtZwwmToplevelsV1ImplHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { ([](zwayland::server::Resource* r) { auto* self = r->data<Impl>(); if (self == nullptr) return; for (auto* handle : self->handles) if (handle->manager == r && !handle->closed) { handle->closed = true; protocol::ext_zwwm_toplevel_handle_v1_send_closed(*handle->resource); } std::erase(self->managers, r); })(&destroyed); });
    self->managers.push_back(resource);
    for (const auto& info : self->toplevels()) send_handle(client, resource, info);
    protocol::ext_zwwm_toplevels_v1_send_done(*resource);
  }

  void update_toplevels() {
    const auto current = toplevels();
    bool changed = false;
    for (auto* handle : std::vector<Handle*>(handles)) {
      if (handle->closed) continue;
      const auto found = std::find_if(current.begin(), current.end(), [handle](const ToplevelInfo& info) { return info.id == handle->info.id; });
      if (found == current.end()) { handle->closed = true; protocol::ext_zwwm_toplevel_handle_v1_send_closed(*handle->resource); for (auto* frame : frames) if (!frame->terminal && frame->target_id == handle->info.id) { frame->terminal = true; protocol::zwwm_screencopy_frame_v1_send_failed(*frame->resource, protocol::ZWWM_SCREENCOPY_FRAME_V1_FAILURE_SOURCE_LOST); } changed = true; continue; }
      const auto old = handle->info; handle->info = *found;
      if (old.title == found->title && old.app_id == found->app_id && old.x == found->x && old.y == found->y && old.width == found->width && old.height == found->height && old.state == found->state) continue;
      if (old.title != found->title) protocol::ext_zwwm_toplevel_handle_v1_send_title(*handle->resource, found->title.c_str());
      if (old.app_id != found->app_id) protocol::ext_zwwm_toplevel_handle_v1_send_app_id(*handle->resource, found->app_id.c_str());
      if (old.x != found->x || old.y != found->y || old.width != found->width || old.height != found->height) protocol::ext_zwwm_toplevel_handle_v1_send_geometry(*handle->resource, found->x, found->y, found->width, found->height);
      if (old.state != found->state) protocol::ext_zwwm_toplevel_handle_v1_send_state(*handle->resource, found->state);
      changed = true;
    }
    for (const auto& info : current) for (auto* manager : managers) {
      const bool known = std::any_of(handles.begin(), handles.end(), [&info, manager](const Handle* handle) { return handle->manager == manager && !handle->closed && handle->info.id == info.id; });
      if (!known) { send_handle(client, manager, info); changed = true; }
    }
    if (changed) for (auto* manager : managers) protocol::ext_zwwm_toplevels_v1_send_done(*manager);
  }

  void close_channel(bool destroy_client) {
    if (source >= 0) {
      event_loop->remove(source);
      source = -1;
    }
    socket.reset();
    if (destroy_client && client != nullptr) { for (auto* frame : frames) if (!frame->terminal) { frame->terminal = true; protocol::zwwm_screencopy_frame_v1_send_failed(*frame->resource, protocol::ZWWM_SCREENCOPY_FRAME_V1_FAILURE_REVOKED); } for (auto* manager : managers) protocol::ext_zwwm_toplevels_v1_send_finished(*manager); client->flush(); client->destroy(); }
    if (portal_client) portal_client(nullptr);
    client = nullptr;
  }

  static void client_destroyed(zwayland::server::Listener* listener, void*) {
    auto* wrapper = reinterpret_cast<ClientListener*>(listener);
    wrapper->owner->client = nullptr;
    wrapper->owner->close_channel(false);
  }

  static int readable(int, std::uint32_t mask, void* data) {
    auto* self = static_cast<Impl*>(data);
    if ((mask & (EPOLLHUP | EPOLLERR)) != 0) {
      self->close_channel(true);
      return 0;
    }

    Message message{};
    iovec io{.iov_base = &message, .iov_len = sizeof(message)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(ucred))]{};
    msghdr header{};
    header.msg_iov = &io;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
    const ssize_t received = recvmsg(self->socket.get(), &header, MSG_CMSG_CLOEXEC);
    if (received != static_cast<ssize_t>(sizeof(message)) || message.magic != kMagic) {
      self->close_channel(true);
      return 0;
    }
    int received_fd = -1;
    const ucred* credentials = nullptr;
    for (auto* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr; cmsg = CMSG_NXTHDR(&header, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
          cmsg->cmsg_len == CMSG_LEN(sizeof(int))) std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS &&
          cmsg->cmsg_len == CMSG_LEN(sizeof(ucred))) credentials = reinterpret_cast<const ucred*>(CMSG_DATA(cmsg));
    }
    const bool child_credentials = credentials != nullptr && credentials->pid == self->child &&
                                   credentials->uid == getuid() && credentials->gid == getgid();
    UniqueFd passed_fd(received_fd);
    if (!child_credentials) {
      self->close_channel(true);
      return 0;
    }
    if (message.type == MessageType::attach && self->client == nullptr && passed_fd) {
      self->client = self->display->add_client(passed_fd.get());
      if (self->client == nullptr) {
        self->close_channel(false);
        return 0;
      }
      (void)passed_fd.release();
      if (self->portal_client) self->portal_client(self->client);
      self->client_listener.owner = self;
      self->client_listener.listener.notify = client_destroyed;
      self->client->add_destroy_listener(self->client_listener.listener);
      if (!send_message(self->socket.get(), Message{.type = MessageType::ready})) self->close_channel(true);
      return 0;
    }
    self->close_channel(true);
    return 0;
  }

  static int child_exited(int, void* data) {
    auto* self = static_cast<Impl*>(data);
    if (self->child <= 0 || waitpid(self->child, nullptr, WNOHANG) != self->child) return 0;
    self->child = -1;
    self->close_channel(true);
    self->schedule_restart();
    return 0;
  }

  void schedule_restart() {
    if (shutting_down || executable.empty() || restart_source >= 0) return;
    restart_source = event_loop->add_timer(1000, [this] {
      restart_source = -1;
      if (!shutting_down && !start_child()) schedule_restart();
    });
  }

  bool start_child() {
    if (child > 0 || executable.empty()) return false;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
      error = std::string("could not create portal capability socket: ") + std::strerror(errno);
      return false;
    }
    int enabled = 1;
    if (setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0) {
      close(sockets[0]);
      close(sockets[1]);
      error = "could not enable portal credential passing";
      return false;
    }
    const pid_t spawned = fork();
    if (spawned < 0) {
      close(sockets[0]);
      close(sockets[1]);
      error = std::string("could not fork portal capture child: ") + std::strerror(errno);
      return false;
    }
    if (spawned == 0) {
      close(sockets[0]);
      if (dup2(sockets[1], 3) < 0) _exit(126);
      if (sockets[1] != 3) close(sockets[1]);
      execlp(executable.c_str(), executable.c_str(), "--capability-fd", "3", static_cast<char*>(nullptr));
      _exit(127);
    }
    close(sockets[1]);
    socket.reset(sockets[0]);
    child = spawned;
    try {
      if (child_source < 0)
        child_source = event_loop->add_signal(SIGCHLD, [this](int signal) {
          child_exited(signal, this);
          return true;
        });
      source = event_loop->add_fd(socket.get(), EPOLLIN | EPOLLHUP | EPOLLERR, [this](int fd, int mask) {
        readable(fd, static_cast<std::uint32_t>(mask), this);
        return source >= 0;
      });
    } catch (const std::exception&) {
      error = "could not supervise portal capability child";
      close_channel(false);
      kill(spawned, SIGTERM);
      (void)waitpid(spawned, nullptr, 0);
      child = -1;
      return false;
    }
    return true;
  }
};

PortalCapture::PortalCapture(zwayland::server::Display* display, zwayland::server::EventLoop* event_loop, Capture capture, Copy copy,
                             MapRegion map_region, Toplevels toplevels, Output output,
                             ResolveOutput resolve_output, bool embedded_cursor, PortalClient portal_client)
    : impl_(std::make_unique<Impl>()) {
  impl_->display = display;
  impl_->event_loop = event_loop;
  impl_->capture = std::move(capture);
  impl_->copy = std::move(copy);
  impl_->map_region = std::move(map_region);
  impl_->toplevels = std::move(toplevels);
  impl_->output = std::move(output);
  impl_->resolve_output = std::move(resolve_output);
  impl_->portal_client = std::move(portal_client);
  impl_->embedded_cursor = embedded_cursor;
  impl_->screencopy = display->add_global(&protocol::zwwm_screencopy_view_manager_v1_interface, 1, [data = impl_.get()](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (Impl::bind_screencopy)(&client, data, bound_version, id); });
  impl_->canonical_screencopy = display->add_global(&protocol::zwlr_screencopy_manager_v1_interface, 1, [data = impl_.get()](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (Impl::bind_canonical_screencopy)(&client, data, bound_version, id); });
  impl_->xdg_output_manager = display->add_global(&protocol::zxdg_output_manager_v1_interface, 3, [data = impl_.get()](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (Impl::bind_xdg_output_manager)(&client, data, bound_version, id); });
  impl_->toplevel_manager = display->add_global(&protocol::ext_zwwm_toplevels_v1_interface, 1, [data = impl_.get()](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (Impl::bind_toplevels)(&client, data, bound_version, id); });
  display->set_global_filter([self = impl_.get()](const zwayland::server::Client& candidate,
                                                  const zwayland::server::Global& global) {
    return (global.name != self->screencopy && global.name != self->toplevel_manager) ||
           &candidate == self->client;
  });
}

PortalCapture::~PortalCapture() {
  impl_->shutting_down = true;
  if (impl_->restart_source >= 0) {
    impl_->event_loop->remove(impl_->restart_source);
    impl_->restart_source = -1;
  }
  impl_->close_channel(true);
  impl_->display->set_global_filter({});
  if (impl_->child_source >= 0) {
    impl_->event_loop->remove(impl_->child_source);
    impl_->child_source = -1;
  }
  if (impl_->child > 0) {
    if (waitpid(impl_->child, nullptr, WNOHANG) == 0) {
      kill(impl_->child, SIGTERM);
      (void)waitpid(impl_->child, nullptr, 0);
    }
  }
  if (impl_->toplevel_manager != 0) impl_->display->destroy_global(impl_->toplevel_manager);
  if (impl_->xdg_output_manager != 0) impl_->display->destroy_global(impl_->xdg_output_manager);
  if (impl_->canonical_screencopy != 0) impl_->display->destroy_global(impl_->canonical_screencopy);
  if (impl_->screencopy != 0) impl_->display->destroy_global(impl_->screencopy);
}

bool PortalCapture::spawn(const std::string& executable) {
  if (impl_->child > 0) {
    impl_->error = "portal capture child already started";
    return false;
  }
  impl_->executable = executable;
  return impl_->start_child();
}

const std::string& PortalCapture::last_error() const { return impl_->error; }
void PortalCapture::toplevels_changed() { if (impl_->client != nullptr) impl_->update_toplevels(); }
void PortalCapture::frame_presented() {
  if (impl_->client == nullptr) return;
  zwayland::server::Resource* output = impl_->output(impl_->client, {});
  if (output == nullptr) return;
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
  const auto seconds = static_cast<std::uint64_t>(now.tv_sec);
  if (impl_->xdg_dimensions_pending && !impl_->xdg_outputs.empty()) {
    OutputCapture capture;
    if (impl_->capture(capture, false, 0) && capture.width != 0 && capture.height != 0) {
      for (const auto& output_resource : impl_->xdg_outputs)
        Impl::send_xdg_output(output_resource.first, output_resource.second,
                              impl_->output(output_resource.first->client, output_resource.second.id));
      impl_->xdg_dimensions_pending = false;
    }
  }
  for (auto* manager : impl_->screencopy_managers)
    protocol::zwwm_screencopy_view_manager_v1_send_presented(*manager, output,
        static_cast<std::uint32_t>(seconds >> 32U), static_cast<std::uint32_t>(seconds),
        static_cast<std::uint32_t>(now.tv_nsec));
}

}  // namespace zwwm
