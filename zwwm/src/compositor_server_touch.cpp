#include "compositor_server_internal.hpp"

#include <algorithm>

namespace protocol = zwayland::generated;
namespace zwwm::detail {
namespace {
void pending_frame(SeatState* seat, zwayland::server::Client* client) {
  if (std::find(seat->touch_frame_clients.begin(), seat->touch_frame_clients.end(), client) ==
      seat->touch_frame_clients.end()) seat->touch_frame_clients.push_back(client);
}

void cancel_client(SeatState* seat, zwayland::server::Client* client) {
  for (auto* resource : seat->touches)
    if (resource->client == client) protocol::wl_touch_send_cancel(*resource);
  std::erase_if(seat->touch_points, [client](const auto& point) { return point.second.client == client; });
  std::erase(seat->touch_frame_clients, client);
}

struct TouchHandler {
  void release(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};
}  // namespace

void bind_touch(zwayland::server::Client* client, SeatState* seat, std::uint32_t version,
                std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::wl_touch_interface, id, version);
  if (resource == nullptr) { client->post_no_memory(); return; }
  seat->touches.push_back(resource);
  resource->set_data(seat);
  resource->set_handler(protocol::wl_touch_handler(TouchHandler{}));
  resource->set_destroy_handler([](zwayland::server::Resource& resource) {
    auto* seat = resource.data<SeatState>();
    std::erase(seat->touches, &resource);
    for (auto& [id, point] : seat->touch_points) {
      (void)id;
      std::erase(point.recipients, &resource);
    }
    if (std::none_of(seat->touches.begin(), seat->touches.end(), [&](const auto* other) {
      return other->client == resource.client;
    })) {
      std::erase_if(seat->touch_points, [&](const auto& point) { return point.second.client == resource.client; });
      std::erase(seat->touch_frame_clients, resource.client);
    }
  });
}

void deliver_touch_down(SeatState* seat, std::uint32_t time, std::int32_t id,
                        std::int32_t x, std::int32_t y) {
  if (seat->touch_points.contains(id)) return;
  idle_activity(seat);
  auto* observer = seat->display.observer;
  auto* surface = surface_at_global(observer, x, y);
  if (surface == nullptr) return;
  auto* client = surface->resource->client;
  if (std::none_of(seat->touches.begin(), seat->touches.end(), [client](const auto* resource) {
    return resource->client == client;
  })) return;
  auto* top = root(surface);
  const auto output = surface_output(top);
  if (output) observer->active_output = output;
  if ((!observer->external_policy && managed_surface(top)) || top->lock_surface != nullptr ||
      (top->layer_surface != nullptr &&
       top->layer_surface->current.keyboard != protocol::ZWWM_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)) {
    set_keyboard_focus(seat, top);
    configure_layout(observer);
  }
  std::int32_t local_x = 0, local_y = 0;
  surface_local_from_global(surface, x, y, &local_x, &local_y);
  SeatState::TouchPoint point{surface, surface->alive, client, {}};
  for (auto* resource : seat->touches)
    if (resource->client == client) point.recipients.push_back(resource);
  auto& contact = seat->touch_points.emplace(id, std::move(point)).first->second;
  const auto serial = seat->display->next_serial();
  for (auto* resource : contact.recipients)
    protocol::wl_touch_send_down(*resource, serial, time, surface->resource, id, local_x, local_y);
  remember_serial(*seat, client, serial);
  pending_frame(seat, client);
}

void deliver_touch_motion(SeatState* seat, std::uint32_t time, std::int32_t id,
                          std::int32_t x, std::int32_t y) {
  const auto point = seat->touch_points.find(id);
  if (point == seat->touch_points.end()) return;
  idle_activity(seat);
  const auto& contact = point->second;
  if (!*contact.alive || contact.surface->current_buffer == nullptr) {
    cancel_client(seat, contact.client);
    return;
  }
  std::int32_t local_x = 0, local_y = 0;
  surface_local_from_global(contact.surface, x, y, &local_x, &local_y);
  for (auto* resource : contact.recipients) protocol::wl_touch_send_motion(*resource, time, id, local_x, local_y);
  pending_frame(seat, contact.client);
}

void deliver_touch_up(SeatState* seat, std::uint32_t time, std::int32_t id) {
  const auto point = seat->touch_points.find(id);
  if (point == seat->touch_points.end()) return;
  auto* client = point->second.client;
  if (!*point->second.alive) { cancel_client(seat, client); return; }
  const auto serial = seat->display->next_serial();
  for (auto* resource : point->second.recipients) protocol::wl_touch_send_up(*resource, serial, time, id);
  seat->touch_points.erase(point);
  pending_frame(seat, client);
}

void deliver_touch_frame(SeatState* seat) {
  for (auto* resource : seat->touches)
    if (std::find(seat->touch_frame_clients.begin(), seat->touch_frame_clients.end(), resource->client) !=
        seat->touch_frame_clients.end()) protocol::wl_touch_send_frame(*resource);
  seat->touch_frame_clients.clear();
}

void cancel_touches(SeatState* seat) {
  while (!seat->touch_points.empty()) cancel_client(seat, seat->touch_points.begin()->second.client);
  seat->touch_frame_clients.clear();
}
void cancel_touch_surface(SeatState* seat, SurfaceState* surface) {
  while (true) {
    const auto point = std::find_if(seat->touch_points.begin(), seat->touch_points.end(), [surface](const auto& item) {
      return !*item.second.alive || root(item.second.surface) == surface;
    });
    if (point == seat->touch_points.end()) break;
    cancel_client(seat, point->second.client);
  }
}
}  // namespace zwwm::detail
