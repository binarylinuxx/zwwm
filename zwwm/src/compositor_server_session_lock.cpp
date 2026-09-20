#include "compositor_server_internal.hpp"

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <session-lock-zwayland-server.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>

namespace protocol = zwayland::generated;

namespace zwwm::detail {
namespace {

void blank_unlocked_scene(SessionLockState* lock) {
  end_interactive(lock == nullptr || lock->observer == nullptr ? nullptr : lock->observer->seat);
  auto* observer = lock->observer;
  auto* seat = observer->seat;
  for (auto* constraint : seat->constraints) deactivate_constraint(constraint, true);
  set_pointer_focus(seat, nullptr, 0, 0);
  set_keyboard_focus(seat, nullptr);
  for (auto* surface : *observer->surfaces) if (surface->parent == nullptr && surface->lock_surface == nullptr && observer->callback != nullptr) {
    observer->callback(observer->data, ShmBufferView{.surface_id = surface->id, .root_surface_id = surface->id,
                                                       .output = surface_output(surface),
                                                       .window_shader = {}, .border_shader = {}});
  }
}

void restore_unlocked_scene(SessionLockState* lock) {
  auto* observer = lock->observer;
  auto* seat = observer->seat;
  if (seat->keyboard_focus != nullptr && seat->keyboard_focus->lock_surface != nullptr)
    set_keyboard_focus(seat, nullptr);
  for (auto* item : lock->surfaces) {
    if (item->surface != nullptr && observer->callback != nullptr)
      observer->callback(observer->data, ShmBufferView{.surface_id = item->surface->id,
          .root_surface_id = item->surface->id, .output = item->output,
          .window_shader = {}, .border_shader = {}});
    item->mapped = false;
    item->lock = nullptr;
  }
  if (seat->session_lock == lock) seat->session_lock = nullptr;
  for (auto* surface : *observer->surfaces)
    if (surface->parent == nullptr && surface->lock_surface == nullptr) notify_surface_tree(surface);
  auto* focus = seat->regular_focus;
  if (focus != nullptr && std::find(observer->surfaces->begin(), observer->surfaces->end(), focus) == observer->surfaces->end()) {
    seat->regular_focus = nullptr;
    focus = nullptr;
  }
  if (focus != nullptr) set_keyboard_focus(seat, focus);
}

void lock_surface_destroyed(zwayland::server::Resource* resource) {
  auto* lock_surface = resource->data<LockSurfaceState>();
  if (lock_surface == nullptr) return;
  if (lock_surface->surface != nullptr && lock_surface->lock != nullptr) {
    auto* observer = lock_surface->lock->observer;
    if (observer->callback != nullptr) observer->callback(observer->data,
        ShmBufferView{.surface_id = lock_surface->surface->id, .root_surface_id = lock_surface->surface->id,
                      .output = lock_surface->output, .window_shader = {}, .border_shader = {}});
    if (observer->seat->keyboard_focus == lock_surface->surface) set_keyboard_focus(observer->seat, nullptr);
  }
  if (lock_surface->lock != nullptr) std::erase(lock_surface->lock->surfaces, lock_surface);
  if (lock_surface->surface != nullptr) { lock_surface->surface->lock_surface = nullptr; lock_surface->surface->layer_role_assigned = false; }
  delete lock_surface;
}

void lock_surface_ack(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t serial) {
  auto* surface = resource->data<LockSurfaceState>();
  const auto found = std::find(surface->serials.begin(), surface->serials.end(), serial);
  if (found == surface->serials.end()) { resource->post_error(protocol::EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL, "invalid lock configure serial"); return; }
  surface->serials.erase(surface->serials.begin(), std::next(found)); surface->acked = serial;
}

struct ExtSessionLockSurfaceV1KSessionLockSurfaceHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void ack_configure(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial) {
    (lock_surface_ack)(&client, &resource, serial);
  }
};

void lock_get_surface(zwayland::server::Client* client, zwayland::server::Resource* resource, std::uint32_t id,
                      zwayland::server::Resource* surface_resource, zwayland::server::Resource* output_resource) {
  auto* lock = resource->data<SessionLockState>();
  auto* surface = compositor_surface_from_resource(client, surface_resource);
  auto* output = output_resource == nullptr || output_resource->client != client ||
      std::strcmp(output_resource->interface->name, "wl_output") != 0 ? nullptr :
      output_resource->data<OutputState>();
  const bool live_output = output != nullptr && !output->retired && lock != nullptr && lock->observer->outputs != nullptr &&
      std::any_of(lock->observer->outputs->begin(), lock->observer->outputs->end(), [&](const auto& item) { return item.get() == output; });
  if (surface == nullptr || surface->xdg_surface != nullptr || surface->layer_role_assigned || surface->parent != nullptr || surface->drag_icon_role) {
    resource->post_error(protocol::EXT_SESSION_LOCK_V1_ERROR_ROLE, "surface already has a role"); return;
  }
  if (surface->ever_committed || surface->current_buffer != nullptr || surface->pending_buffer != nullptr) { resource->post_error(protocol::EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED, "lock surface already constructed"); return; }
  if (!live_output) { resource->post_error(protocol::EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT, "unknown output"); return; }
  if (std::any_of(lock->surfaces.begin(), lock->surfaces.end(), [&](const auto* item) { return item->output == output->info.id; })) {
    resource->post_error(protocol::EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT, "duplicate lock output"); return;
  }
  auto* lock_resource = client->create_resource(&protocol::ext_session_lock_surface_v1_interface, id, 1);
  auto* state = lock_resource == nullptr ? nullptr : new (std::nothrow) LockSurfaceState;
  if (lock_resource == nullptr || state == nullptr) { if (lock_resource != nullptr) lock_resource->destroy(); client->post_no_memory(); return; }
  state->lock = lock; state->resource = lock_resource; state->surface = surface; state->output = output->info.id;
  state->width = output->info.logical_width; state->height = output->info.logical_height;
  surface->lock_surface = state; surface->layer_role_assigned = true;
  surface->x = output->info.logical_x; surface->y = output->info.logical_y;
  lock->surfaces.push_back(state);
  lock_resource->set_data(state); lock_resource->set_handler(protocol::ext_session_lock_surface_v1_handler(ExtSessionLockSurfaceV1KSessionLockSurfaceHandler{})); lock_resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (lock_surface_destroyed)(&destroyed); });
  const auto serial = lock->observer->seat->display->next_serial();
  state->serials.push_back(serial);
  protocol::ext_session_lock_surface_v1_send_configure(*lock_resource, serial, state->width, state->height);
}

void session_lock_resource_destroyed(zwayland::server::Resource* resource) {
  auto* lock = resource->data<SessionLockState>();
  if (lock == nullptr) return;
  lock->resource = nullptr; lock->client = nullptr;
  if (lock->accepted && !lock->locked_sent && !lock->unlocked) restore_unlocked_scene(lock);
  if (!lock->accepted || lock->unlocked || !lock->locked_sent) delete lock;
}

struct ExtSessionLockV1KSessionLockHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { auto* lock = resource->data<SessionLockState>(); if (lock->locked_sent) { resource->post_error(protocol::EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY, "locked session must be unlocked"); return; } resource->destroy(); })(&client, &resource);
  }
  void get_lock_surface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface, zwayland::server::Resource* output) {
    (lock_get_surface)(&client, &resource, id, surface, output);
  }
  void unlock_and_destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { auto* lock = resource->data<SessionLockState>(); if (!lock->locked_sent) { resource->post_error(protocol::EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK, "lock was not presented"); return; } lock->unlocked = true; restore_unlocked_scene(lock); resource->destroy(); })(&client, &resource);
  }
};

struct ExtSessionLockManagerV1KSessionLockManagerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void lock(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    ([](zwayland::server::Client* lock_client, zwayland::server::Resource* manager, std::uint32_t lock_id) {
      auto* observer = manager->data<Observer>();
      auto* resource = lock_client->create_resource(&protocol::ext_session_lock_v1_interface, lock_id, 1);
      auto* lock = resource == nullptr ? nullptr : new (std::nothrow) SessionLockState;
      if (resource == nullptr || lock == nullptr) { if (resource != nullptr) resource->destroy(); lock_client->post_no_memory(); return; }
      lock->observer = observer; lock->client = lock_client; lock->resource = resource;
      resource->set_data(lock); resource->set_handler(protocol::ext_session_lock_v1_handler(ExtSessionLockV1KSessionLockHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (session_lock_resource_destroyed)(&destroyed); });
      if (observer->seat->session_lock != nullptr) { protocol::ext_session_lock_v1_send_finished(*resource); return; }
      lock->accepted = true; observer->seat->session_lock = lock; blank_unlocked_scene(lock);
    })(&client, &resource, id);
  }
};

}  // namespace

void bind_session_lock_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::ext_session_lock_manager_v1_interface, id, std::min(version, 1U));
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<Observer*>(data)); resource->set_handler(protocol::ext_session_lock_manager_v1_handler(ExtSessionLockManagerV1KSessionLockManagerHandler{}));
}

void configure_session_lock_outputs(SessionLockState* lock) {
  if (lock == nullptr) return;
  for (auto* surface : lock->surfaces) {
    auto* output = output_state(lock->observer, surface->output);
    if (output == nullptr) {
      if (surface->surface != nullptr && lock->observer->callback != nullptr)
        lock->observer->callback(lock->observer->data, ShmBufferView{.surface_id = surface->surface->id,
            .root_surface_id = surface->surface->id, .output = surface->output,
            .window_shader = {}, .border_shader = {}});
      surface->mapped = false; surface->presented = false;
      continue;
    }
    if (surface->surface != nullptr) { surface->surface->x = output->info.logical_x; surface->surface->y = output->info.logical_y; }
    if (surface->width != output->info.logical_width || surface->height != output->info.logical_height) {
      surface->width = output->info.logical_width; surface->height = output->info.logical_height;
      surface->mapped = false; surface->presented = false;
      const auto serial = lock->observer->seat->display->next_serial(); surface->serials.push_back(serial);
      protocol::ext_session_lock_surface_v1_send_configure(*surface->resource, serial, surface->width, surface->height);
    }
  }
}

void notify_session_lock_frame_presented(SessionLockState* lock, OutputId output) {
  if (lock == nullptr || lock->locked_sent) return;
  for (auto* surface : lock->surfaces) if ((!output || surface->output == output) && surface->mapped) surface->presented = true;
  const bool covered = std::all_of(lock->observer->outputs->begin(), lock->observer->outputs->end(), [&](const auto& enabled) {
    return std::any_of(lock->surfaces.begin(), lock->surfaces.end(), [&](const auto* surface) {
      return surface->output == enabled->info.id && surface->mapped && surface->presented;
    });
  });
  if (covered && lock->resource != nullptr) { lock->locked_sent = true; protocol::ext_session_lock_v1_send_locked(*lock->resource); }
}

}  // namespace zwwm::detail
