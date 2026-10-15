#include "compositor_server_internal.hpp"
#include <zwwm-focus-grab-v1-zwayland-server.h>
#include <algorithm>
#include <new>

namespace protocol = zwayland::generated;
namespace zwwm::detail {

struct FocusGrabState {
  SeatState* seat = nullptr;
  zwayland::server::Resource* resource = nullptr;
  SurfaceState* surface = nullptr;
  std::shared_ptr<bool> alive;
  SurfaceState* previous = nullptr;
  std::shared_ptr<bool> previous_alive;
  bool focused = false;
};

bool focus_grab_contains(SeatState* seat, SurfaceState* surface) {
  const auto* grab = seat == nullptr ? nullptr : seat->focus_grab;
  if (grab == nullptr || !grab->alive || !*grab->alive) return false;
  for (auto* current = surface; current != nullptr; current = current->parent)
    if (current == grab->surface) return true;
  return false;
}

void dismiss_focus_grab(SeatState* seat, std::uint32_t reason) {
  auto* grab = seat == nullptr ? nullptr : seat->focus_grab;
  if (grab == nullptr) return;
  seat->focus_grab = nullptr;
  const bool restore = grab->focused;
  grab->focused = false;
  protocol::zwwm_focus_grab_v1_send_dismissed(*grab->resource, reason);
  if (restore && seat->session_lock == nullptr) {
    auto* previous = grab->previous_alive && *grab->previous_alive ? grab->previous : nullptr;
    if (previous != nullptr && root(previous)->current_buffer == nullptr) previous = nullptr;
    seat->focus_grab_transition = true;
    set_keyboard_focus(seat, previous);
    seat->focus_grab_transition = false;
  }
  grab->previous = nullptr;
  grab->previous_alive.reset();
}

void refresh_focus_grab(SeatState* seat) {
  auto* grab = seat == nullptr ? nullptr : seat->focus_grab;
  if (grab == nullptr) return;
  if (!grab->alive || !*grab->alive || grab->surface->layer_surface == nullptr) {
    dismiss_focus_grab(seat, protocol::ZWWM_FOCUS_GRAB_V1_REASON_UNMAPPED);
    return;
  }
  if (seat->session_lock != nullptr) {
    dismiss_focus_grab(seat, protocol::ZWWM_FOCUS_GRAB_V1_REASON_LOCKED);
    return;
  }
  if (!grab->surface->layer_surface->mapped || grab->surface->layer_surface->closed) {
    if (grab->focused || grab->surface->layer_surface->closed) dismiss_focus_grab(seat, protocol::ZWWM_FOCUS_GRAB_V1_REASON_UNMAPPED);
    return;
  }
  if (!grab->focused) {
    seat->focus_grab_transition = true;
    set_keyboard_focus(seat, grab->surface);
    seat->focus_grab_transition = false;
    grab->focused = true;
    protocol::zwwm_focus_grab_v1_send_activated(*grab->resource);
  }
}

namespace {
struct GrabHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void activate(zwayland::server::Client&, zwayland::server::Resource& resource) {
    auto* grab = resource.data<FocusGrabState>();
    if (grab->seat->focus_grab == grab) return;
    dismiss_focus_grab(grab->seat, protocol::ZWWM_FOCUS_GRAB_V1_REASON_REPLACED);
    if (!grab->alive || !*grab->alive || grab->surface->layer_surface == nullptr) {
      protocol::zwwm_focus_grab_v1_send_dismissed(resource, protocol::ZWWM_FOCUS_GRAB_V1_REASON_UNMAPPED);
      return;
    }
    end_interactive(grab->seat);
    grab->previous = grab->seat->keyboard_focus;
    grab->previous_alive = grab->previous == nullptr ? nullptr : grab->previous->alive;
    grab->seat->focus_grab = grab;
    refresh_focus_grab(grab->seat);
  }
  void release(zwayland::server::Client&, zwayland::server::Resource& resource) {
    auto* grab = resource.data<FocusGrabState>();
    if (grab->seat->focus_grab == grab) dismiss_focus_grab(grab->seat, protocol::ZWWM_FOCUS_GRAB_V1_REASON_RELEASED);
  }
};
struct ManagerHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void get_grab(zwayland::server::Client& client, zwayland::server::Resource& manager,
                std::uint32_t id, zwayland::server::Resource* surface_resource) {
    auto* surface = compositor_surface_from_resource(&client, surface_resource);
    if (surface == nullptr || surface->layer_surface == nullptr || surface->parent != nullptr) {
      manager.post_error(protocol::ZWWM_FOCUS_GRAB_MANAGER_V1_ERROR_INVALID_SURFACE, "focus grab requires this client's layer wl_surface");
      return;
    }
    auto* resource = client.create_resource(&protocol::zwwm_focus_grab_v1_interface, id, 1);
    auto* grab = resource == nullptr ? nullptr : new (std::nothrow) FocusGrabState;
    if (grab == nullptr) { if (resource != nullptr) resource->destroy(); client.post_no_memory(); return; }
    grab->seat = manager.data<SeatState>(); grab->resource = resource;
    grab->surface = surface; grab->alive = surface->alive;
    grab->seat->focus_grabs.push_back(grab);
    resource->set_data(grab);
    resource->set_handler(protocol::zwwm_focus_grab_v1_handler(GrabHandler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& resource) {
      auto* grab = resource.data<FocusGrabState>();
      if (grab->seat->focus_grab == grab) dismiss_focus_grab(grab->seat, protocol::ZWWM_FOCUS_GRAB_V1_REASON_RELEASED);
      std::erase(grab->seat->focus_grabs, grab);
      delete grab;
    });
  }
};
}  // namespace

void destroy_focus_grabs(SeatState* seat) {
  seat->focus_grab = nullptr;
  while (!seat->focus_grabs.empty()) seat->focus_grabs.back()->resource->destroy();
}
void bind_focus_grab(zwayland::server::Client* client, void* data, std::uint32_t, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwwm_focus_grab_manager_v1_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::zwwm_focus_grab_manager_v1_handler(ManagerHandler{}));
}
}  // namespace zwwm::detail
