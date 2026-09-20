#include "compositor_server_internal.hpp"
#ifdef ZWWM_XWAYLAND
#include "compositor_server_xwayland_internal.hpp"
#endif

#include <zwwm-data-control-v1-zwayland-server.h>
#include <wlr-data-control-unstable-v1-zwayland-server.h>

#include <algorithm>
#include <new>
#include <unistd.h>

namespace protocol = zwayland::generated;

namespace zwwm::detail {

constexpr std::uint32_t kDataDeviceManagerVersion = 3;

void data_offer_destroyed(zwayland::server::Resource* resource) { auto* offer = resource->data<DataOfferState>(); if (offer == nullptr) return; if (offer->drag && offer->dropped && offer->source != nullptr && offer->source->role == DataSourceRole::drag) { auto* source = offer->source; const bool other = std::any_of(offer->seat->data_offers.begin(), offer->seat->data_offers.end(), [offer, source](const auto* candidate) { return candidate != offer && candidate->dropped && candidate->source == source; }); if (!other) cancel_source(source); } std::erase(offer->seat->data_offers, offer); delete offer; }
bool offered_mime(const DataSourceState* source, const char* mime) { return source != nullptr && mime != nullptr && std::find(source->mime_types.begin(), source->mime_types.end(), mime) != source->mime_types.end(); }
void data_offer_accept(zwayland::server::Client*, zwayland::server::Resource* resource,
                       std::uint32_t, const char* mime) {
  auto* offer = resource->data<DataOfferState>();
  offer->accepted = offered_mime(offer->source, mime);
  if (offer->source == nullptr || offer->source->resource == nullptr ||
      offer->source->protocol != DataProtocol::standard) return;
  std::optional<std::string_view> target;
  if (offer->accepted) target = mime;
  protocol::wl_data_source_send_target(*offer->source->resource, target);
}
void send_source_data(DataSourceState* source, const char* mime, std::int32_t fd) {
  if (source->protocol == DataProtocol::zwwm) protocol::zwwm_data_control_source_v1_send_send(*source->resource, mime, fd);
  else if (source->protocol == DataProtocol::zwlr) protocol::zwlr_data_control_source_v1_send_send(*source->resource, mime, fd);
  else protocol::wl_data_source_send_send(*source->resource, mime, fd);
}
void send_source_cancelled(DataSourceState* source) {
  if (source->protocol == DataProtocol::zwwm) protocol::zwwm_data_control_source_v1_send_cancelled(*source->resource);
  else if (source->protocol == DataProtocol::zwlr) protocol::zwlr_data_control_source_v1_send_cancelled(*source->resource);
  else protocol::wl_data_source_send_cancelled(*source->resource);
}
void data_offer_receive(zwayland::server::Client*, zwayland::server::Resource* resource, const char* mime, std::int32_t fd) {
  auto* offer = resource->data<DataOfferState>();
#ifdef ZWWM_XWAYLAND
  if (fd >= 0 && offered_mime(offer->source, mime) && offer->source->xwayland_selection != nullptr &&
      !offer->source->cancelled) {
    xwayland_receive_selection(offer->source, mime, fd);
    return;
  }
#endif
  if (fd >= 0 && offered_mime(offer->source, mime) && offer->source->resource != nullptr &&
      !offer->source->cancelled) send_source_data(offer->source, mime, fd);
  if (fd >= 0) (void)close(fd);
}
std::uint32_t choose_drag_action(const DataOfferState* offer) {
  if (offer->source == nullptr) return protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
  const auto available = offer->source->actions & offer->actions;
  if ((available & offer->preferred_action) != 0) return offer->preferred_action;
  for (const auto action : {protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                            protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                            protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK})
    if ((available & action) != 0) return action;
  return protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}
void publish_drag_action(DataOfferState* offer) {
  const auto action = choose_drag_action(offer);
  if (action == offer->action) return;
  offer->action = action;
  if (offer->resource->version >= 3) protocol::wl_data_offer_send_action(*offer->resource, action);
  if (offer->active) {
    offer->seat->drag_action = action;
    if (offer->source != nullptr && offer->source->resource != nullptr && offer->source->resource->version >= 3)
      protocol::wl_data_source_send_action(*offer->source->resource, action);
  }
}
void data_offer_finish(zwayland::server::Client*, zwayland::server::Resource* resource) {
  auto* offer = resource->data<DataOfferState>();
  if (!offer->drag) { resource->post_error(protocol::WL_DATA_OFFER_ERROR_INVALID_OFFER, "selection offers cannot be finished"); return; }
  if (!offer->dropped) { resource->post_error(protocol::WL_DATA_OFFER_ERROR_INVALID_FINISH, "drag offer has not been dropped"); return; }
  if (offer->action == protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE) { resource->post_error(protocol::WL_DATA_OFFER_ERROR_INVALID_OFFER, "drag offer has no selected action"); return; }
  auto* source = offer->source;
  offer->source = nullptr;
  if (source != nullptr && source->role == DataSourceRole::drag) {
    source->role = DataSourceRole::unused;
    if (source->resource != nullptr) protocol::wl_data_source_send_dnd_finished(*source->resource);
    for (auto* other : source->seat->data_offers) if (other->source == source) other->source = nullptr;
  }
}
void data_offer_set_actions(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t actions, std::uint32_t preferred) { constexpr std::uint32_t supported = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK; if ((actions & ~supported) != 0) { resource->post_error(protocol::WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK, "invalid drag action mask"); return; } if (preferred != protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE && ((preferred & (preferred - 1U)) != 0 || (preferred & actions) == 0)) { resource->post_error(protocol::WL_DATA_OFFER_ERROR_INVALID_ACTION, "preferred drag action must be singular and offered"); return; } auto* offer = resource->data<DataOfferState>(); if (!offer->drag) { resource->post_error(protocol::WL_DATA_OFFER_ERROR_INVALID_OFFER, "selection offers do not negotiate drag actions"); return; } offer->actions = actions; offer->preferred_action = preferred; publish_drag_action(offer); }
struct WlDataOfferKDataOfferHandler {
  void accept(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial, std::optional<std::string> mime_type) {
    (data_offer_accept)(&client, &resource, serial, mime_type ? mime_type->c_str() : nullptr);
  }
  void receive(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string mime_type, int fd) {
    (data_offer_receive)(&client, &resource, mime_type.c_str(), fd);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void finish(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    (data_offer_finish)(&client, &resource);
  }
  void set_actions(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t dnd_actions, std::uint32_t preferred_action) {
    (data_offer_set_actions)(&client, &resource, dnd_actions, preferred_action);
  }
};
DataOfferState* make_data_offer(DataDeviceState* device, DataSourceState* source, bool drag = false) { auto* client = device->resource->client; auto* resource = client->create_resource(&protocol::wl_data_offer_interface, client->alloc_id(), device->resource->version); auto* offer = resource == nullptr ? nullptr : new (std::nothrow) DataOfferState{device->seat, source, resource}; if (resource == nullptr || offer == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return nullptr; } offer->drag = drag; offer->active = drag; if (drag && resource->version < 3) { offer->actions = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY; offer->preferred_action = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY; } device->seat->data_offers.push_back(offer); resource->set_data(offer); resource->set_handler(protocol::wl_data_offer_handler(WlDataOfferKDataOfferHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (data_offer_destroyed)(&destroyed); }); protocol::wl_data_device_send_data_offer(*device->resource, resource); for (const auto& mime : source->mime_types) protocol::wl_data_offer_send_offer(*resource, mime.c_str()); if (drag && resource->version >= 3) protocol::wl_data_offer_send_source_actions(*resource, source->actions); if (drag) publish_drag_action(offer); return offer; }
void control_offer_receive(zwayland::server::Client*, zwayland::server::Resource* resource, const char* mime, std::int32_t fd) { data_offer_receive(nullptr, resource, mime, fd); }
struct ZwwmDataControlOfferV1KDataControlOfferHandler {
  void receive(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string mime_type, int fd) {
    (control_offer_receive)(&client, &resource, mime_type.c_str(), fd);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
struct ZwlrDataControlOfferV1KWlrDataControlOfferHandler {
  void receive(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string mime_type, int fd) {
    (control_offer_receive)(&client, &resource, mime_type.c_str(), fd);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
zwayland::server::Resource* make_control_offer(DataDeviceState* device, DataSourceState* source) {
  auto* client = device->resource->client;
  const auto* interface = device->protocol == DataProtocol::zwlr ? &protocol::zwlr_data_control_offer_v1_interface : &protocol::zwwm_data_control_offer_v1_interface;
  auto* resource = client->create_resource(interface, client->alloc_id(), device->resource->version);
  auto* offer = resource == nullptr ? nullptr : new (std::nothrow) DataOfferState{device->seat, source, resource, device->protocol};
  if (resource == nullptr || offer == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return nullptr; }
  device->seat->data_offers.push_back(offer);
  resource->set_data(offer);
  if (device->protocol == DataProtocol::zwlr)
    resource->set_handler(protocol::zwlr_data_control_offer_v1_handler(ZwlrDataControlOfferV1KWlrDataControlOfferHandler{}));
  else
    resource->set_handler(protocol::zwwm_data_control_offer_v1_handler(ZwwmDataControlOfferV1KDataControlOfferHandler{}));
  resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { data_offer_destroyed(&destroyed); });
  if (device->protocol == DataProtocol::zwlr) {
    protocol::zwlr_data_control_device_v1_send_data_offer(*device->resource, resource);
    for (const auto& mime : source->mime_types) protocol::zwlr_data_control_offer_v1_send_offer(*resource, mime.c_str());
  } else {
    protocol::zwwm_data_control_device_v1_send_data_offer(*device->resource, resource);
    for (const auto& mime : source->mime_types) protocol::zwwm_data_control_offer_v1_send_offer(*resource, mime.c_str());
  }
  return resource;
}
void deactivate_offers(SeatState* seat, zwayland::server::Client* client) { for (auto* offer : seat->data_offers) if (offer->resource->client == client) offer->source = nullptr; }
void send_selection(SeatState* seat, zwayland::server::Client* client) { if (client == nullptr) return; for (auto* device : seat->data_devices) { if (device->resource->client != client) continue; auto* offer = seat->selection == nullptr ? nullptr : make_data_offer(device, seat->selection); if (seat->selection == nullptr || offer != nullptr) protocol::wl_data_device_send_selection(*device->resource, offer == nullptr ? nullptr : offer->resource); } }
void send_control_selection(DataDeviceState* device, bool primary) { auto* source = primary ? device->seat->primary_selection : device->seat->selection; zwayland::server::Resource* offer = source == nullptr ? nullptr : make_control_offer(device, source); if (source == nullptr || offer != nullptr) { if (device->protocol == DataProtocol::zwlr) { if (primary) { if (device->resource->version >= 2) protocol::zwlr_data_control_device_v1_send_primary_selection(*device->resource, offer); } else protocol::zwlr_data_control_device_v1_send_selection(*device->resource, offer); } else if (primary) protocol::zwwm_data_control_device_v1_send_primary_selection(*device->resource, offer); else protocol::zwwm_data_control_device_v1_send_selection(*device->resource, offer); } }
void send_control_selection(SeatState* seat, bool primary) { for (auto* device : seat->data_control_devices) send_control_selection(device, primary); }
void selection_focus_changed(SeatState* seat, zwayland::server::Client* old_client, zwayland::server::Client* new_client) { if (old_client == new_client) return; if (old_client != nullptr) { deactivate_offers(seat, old_client); for (auto* device : seat->data_devices) if (device->resource->client == old_client) protocol::wl_data_device_send_selection(*device->resource, nullptr); std::erase_if(seat->input_serials, [old_client](const auto& item) { return item.first == old_client; }); } send_selection(seat, new_client); }
void cancel_source(DataSourceState* source) { if (source == nullptr || source->cancelled) return; source->cancelled = true; source->role = DataSourceRole::unused; for (auto* offer : source->seat->data_offers) if (offer->source == source) offer->source = nullptr; if (source->resource != nullptr) send_source_cancelled(source); }
void clear_drag_target(SeatState* seat) {
  if (seat->drag_target == nullptr) return;
  auto* client = seat->drag_target->resource->client;
  for (auto* device : seat->data_devices) if (device->resource->client == client)
    protocol::wl_data_device_send_leave(*device->resource);
  for (auto* offer : seat->data_offers) if (offer->drag && offer->active) {
    offer->active = false;
    offer->source = nullptr;
  }
  seat->drag_target = nullptr;
  seat->drag_action = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
  if (seat->drag_source != nullptr && seat->drag_source->resource != nullptr &&
      seat->drag_source->resource->version >= 3)
    protocol::wl_data_source_send_action(*seat->drag_source->resource,
                                         protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
}
void finish_drag(SeatState* seat, bool drop) {
  auto* source = seat->drag_source;
  bool delivered = false;
  if (drop && seat->drag_target != nullptr) {
    auto* client = seat->drag_target->resource->client;
    delivered = source == nullptr;
    for (auto* offer : seat->data_offers) if (offer->drag && offer->active) {
      offer->dropped = offer->accepted && offer->action != protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
      delivered = delivered || offer->dropped;
    }
    if (delivered) for (auto* device : seat->data_devices) if (device->resource->client == client)
      protocol::wl_data_device_send_drop(*device->resource);
  }
  if (source != nullptr && source->resource != nullptr && source->resource->version >= 3 && delivered)
    protocol::wl_data_source_send_dnd_drop_performed(*source->resource);
  const bool destination_can_finish = std::any_of(seat->data_offers.begin(), seat->data_offers.end(), [](const auto* offer) {
    return offer->drag && offer->active && offer->dropped && offer->resource->version >= 3;
  });
  if (delivered && source != nullptr && !destination_can_finish) {
    source->role = DataSourceRole::unused;
    if (source->resource != nullptr && source->resource->version >= 3)
      protocol::wl_data_source_send_dnd_finished(*source->resource);
    for (auto* offer : seat->data_offers) if (offer->source == source) offer->source = nullptr;
  }
  if (!delivered) cancel_source(source);
  for (auto* offer : seat->data_offers) if (offer->drag && offer->active) {
    offer->active = false;
    if (!delivered) offer->source = nullptr;
  }
  if (seat->drag_icon != nullptr) {
    seat->drag_icon->x = 0;
    seat->drag_icon->y = 0;
    notify_surface(seat->drag_icon);
  }
  seat->drag_source = nullptr;
  seat->drag_origin = nullptr;
  seat->drag_icon = nullptr;
  seat->drag_target = nullptr;
  seat->drag_button = 0;
  seat->drag_action = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}
void cancel_drag(SeatState* seat) { if (seat->drag_origin != nullptr) finish_drag(seat, false); }
void drag_motion(SeatState* seat, std::uint32_t time, SurfaceState* target, std::int32_t x, std::int32_t y) {
  if (seat->drag_origin == nullptr) return;
  if (seat->drag_icon != nullptr) {
    seat->drag_icon->x = seat->pointer_x;
    seat->drag_icon->y = seat->pointer_y;
    notify_surface(seat->drag_icon);
  }
  auto* target_client = target == nullptr ? nullptr : target->resource->client;
  const bool has_device = target_client != nullptr && std::any_of(seat->data_devices.begin(), seat->data_devices.end(), [target_client](const auto* device) { return device->resource->client == target_client; });
  if (!has_device) target = nullptr;
  if (target != seat->drag_target) {
    clear_drag_target(seat);
    seat->drag_target = target;
    if (target != nullptr) {
      const auto serial = seat->display->next_serial();
      for (auto* device : seat->data_devices) {
        if (device->resource->client != target_client) continue;
        auto* offer = seat->drag_source == nullptr ? nullptr : make_data_offer(device, seat->drag_source, true);
        protocol::wl_data_device_send_enter(*device->resource, serial, target->resource, (x), (y), offer == nullptr ? nullptr : offer->resource);
      }
    }
    return;
  }
  if (target != nullptr) for (auto* device : seat->data_devices) if (device->resource->client == target_client)
    protocol::wl_data_device_send_motion(*device->resource, time, (x), (y));
}
void drag_button(SeatState* seat, std::uint32_t button, std::uint32_t state) {
  if (seat->drag_origin == nullptr) return;
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED) seat->pressed_buttons.push_back(button);
  else std::erase(seat->pressed_buttons, button);
  if (state == protocol::WL_POINTER_BUTTON_STATE_RELEASED && button == seat->drag_button)
    finish_drag(seat, true);
}
void drag_surface_destroyed(SeatState* seat, SurfaceState* surface) {
  if (seat->drag_origin == surface) { cancel_drag(seat); return; }
  if (seat->drag_target == surface) clear_drag_target(seat);
  if (seat->drag_icon == surface) seat->drag_icon = nullptr;
}
void data_source_destroyed(zwayland::server::Resource* resource) {
  auto* source = resource->data<DataSourceState>();
  if (source == nullptr) return;
  auto* seat = source->seat;
  source->resource = nullptr;
  if (seat->drag_source == source) cancel_drag(seat);
  for (auto* offer : seat->data_offers) if (offer->source == source) offer->source = nullptr;
  const bool clipboard = seat->selection == source;
  const bool primary = seat->primary_selection == source;
  if (clipboard) {
    seat->selection = nullptr;
    zwayland::server::Client* focused = seat->keyboard_focus == nullptr ? nullptr : seat->keyboard_focus->resource->client;
    send_selection(seat, focused);
    send_control_selection(seat, false);
  }
  if (primary) {
    seat->primary_selection = nullptr;
    send_control_selection(seat, true);
  }
#ifdef ZWWM_XWAYLAND
  if (clipboard) xwayland_wayland_selection_changed(seat, false);
  if (primary) xwayland_wayland_selection_changed(seat, true);
#endif
  std::erase(seat->data_sources, source);
  delete source;
}
void data_source_offer(zwayland::server::Client*, zwayland::server::Resource* resource, const char* mime) { auto* source = resource->data<DataSourceState>(); if (mime != nullptr && mime[0] != '\0' && std::find(source->mime_types.begin(), source->mime_types.end(), mime) == source->mime_types.end()) source->mime_types.emplace_back(mime); }
void data_source_set_actions(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t actions) { auto* source = resource->data<DataSourceState>(); constexpr std::uint32_t supported = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK; if ((actions & ~supported) != 0) { resource->post_error(protocol::WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK, "invalid drag action mask"); return; } source->actions = actions; }
struct WlDataSourceKDataSourceHandler {
  void offer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string mime_type) {
    (data_source_offer)(&client, &resource, mime_type.c_str());
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_actions(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t dnd_actions) {
    (data_source_set_actions)(&client, &resource, dnd_actions);
  }
};
void data_device_destroyed(zwayland::server::Resource* resource) { auto* device = resource->data<DataDeviceState>(); if (device == nullptr) return; if (device->protocol != DataProtocol::standard) std::erase(device->seat->data_control_devices, device); else std::erase(device->seat->data_devices, device); delete device; }
void data_device_start_drag(zwayland::server::Client* client, zwayland::server::Resource* resource, zwayland::server::Resource* source_resource, zwayland::server::Resource* origin_resource, zwayland::server::Resource* icon_resource, std::uint32_t serial) {
  auto* device = resource->data<DataDeviceState>();
  auto* seat = device->seat;
  auto* source = source_resource == nullptr ? nullptr : source_resource->data<DataSourceState>();
  auto* origin = compositor_surface_from_resource(client, origin_resource);
  auto* icon = icon_resource == nullptr ? nullptr : compositor_surface_from_resource(client, icon_resource);
  const bool valid = seat->drag_origin == nullptr && seat->button_client == client && seat->button_serial == serial &&
      seat->button != 0 && std::find(seat->pressed_buttons.begin(), seat->pressed_buttons.end(), seat->button) != seat->pressed_buttons.end() &&
      origin != nullptr && seat->pointer_focus != nullptr && root(seat->pointer_focus) == root(origin);
  if (!valid) return;
  if (source != nullptr && source->role != DataSourceRole::unused) { resource->post_error(protocol::WL_DATA_DEVICE_ERROR_USED_SOURCE, "data source is already in use"); return; }
  if (icon != nullptr && (icon->xdg_surface != nullptr || icon->layer_role_assigned || icon->lock_surface != nullptr || icon->parent != nullptr)) {
    resource->post_error(protocol::WL_DATA_DEVICE_ERROR_ROLE, "drag icon surface already has a role");
    return;
  }
  if (source != nullptr) { source->role = DataSourceRole::drag; source->cancelled = false; if (source->resource->version < 3) source->actions = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY; }
  if (source != nullptr && source->resource->version >= 3)
    protocol::wl_data_source_send_action(*source->resource, protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
  if (icon != nullptr) icon->drag_icon_role = true;
  seat->drag_source = source;
  seat->drag_origin = origin;
  seat->drag_icon = icon;
  seat->drag_button = seat->button;
  seat->drag_action = protocol::WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
  seat->pointer_grab = nullptr;
  if (auto* constraint = active_constraint(seat); constraint != nullptr) deactivate_constraint(constraint, true);
  set_pointer_focus(seat, nullptr, 0, 0);
  std::int32_t x = 0, y = 0;
  if (seat->hit_target != nullptr) surface_local_from_global(seat->hit_target, seat->pointer_x, seat->pointer_y, &x, &y);
  drag_motion(seat, 0, seat->hit_target, x, y);
}
void replace_selection(SeatState* seat, DataSourceState* next, bool primary, zwayland::server::Resource* resource) {
  auto*& current = primary ? seat->primary_selection : seat->selection;
  if (next != nullptr && next != current && next->role != DataSourceRole::unused) {
    const auto* device = resource->data<DataDeviceState>();
    if (device != nullptr && device->protocol == DataProtocol::zwlr)
      resource->post_error(protocol::ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE, "data source is already in use");
    else if (device != nullptr && device->protocol == DataProtocol::standard)
      resource->post_error(protocol::WL_DATA_DEVICE_ERROR_USED_SOURCE, "data source is already in use");
    else resource->client->post_error(0, "data control source is already in use");
    return;
  }
  if (next == current) return;
  auto* previous = current;
  current = next;
  if (next != nullptr) next->role = DataSourceRole::selection;
  if (previous != nullptr) cancel_source(previous);
  if (primary) send_control_selection(seat, true);
  else {
    zwayland::server::Client* focused = seat->keyboard_focus == nullptr ? nullptr : seat->keyboard_focus->resource->client;
    send_selection(seat, focused);
    send_control_selection(seat, false);
  }
#ifdef ZWWM_XWAYLAND
  xwayland_wayland_selection_changed(seat, primary);
#endif
}
void data_device_set_selection(zwayland::server::Client* client, zwayland::server::Resource* resource, zwayland::server::Resource* source_resource, std::uint32_t serial) { auto* device = resource->data<DataDeviceState>(); if (!valid_selection_serial(*device->seat, client, serial)) return; replace_selection(device->seat, source_resource == nullptr ? nullptr : source_resource->data<DataSourceState>(), false, resource); }
struct WlDataDeviceKDataDeviceHandler {
  void start_drag(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* source, zwayland::server::Resource* origin, zwayland::server::Resource* icon, std::uint32_t serial) {
    (data_device_start_drag)(&client, &resource, source, origin, icon, serial);
  }
  void set_selection(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* source, std::uint32_t serial) {
    (data_device_set_selection)(&client, &resource, source, serial);
  }
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
void manager_create_source(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id) { auto* seat = manager->data<SeatState>(); auto* resource = client->create_resource(&protocol::wl_data_source_interface, id, manager->version); auto* source = resource == nullptr ? nullptr : new (std::nothrow) DataSourceState; if (source != nullptr) { source->seat = seat; source->resource = resource; } if (resource == nullptr || source == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; } seat->data_sources.push_back(source); resource->set_data(source); resource->set_handler(protocol::wl_data_source_handler(WlDataSourceKDataSourceHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (data_source_destroyed)(&destroyed); }); }
void manager_get_device(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* seat_resource) { auto* seat = manager->data<SeatState>(); if (seat_resource == nullptr || seat_resource->data<SeatState>() != seat) { client->post_error(0, "data device requested for an unknown seat"); return; } auto* resource = client->create_resource(&protocol::wl_data_device_interface, id, manager->version); auto* device = resource == nullptr ? nullptr : new (std::nothrow) DataDeviceState{seat, resource}; if (resource == nullptr || device == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; } seat->data_devices.push_back(device); resource->set_data(device); resource->set_handler(protocol::wl_data_device_handler(WlDataDeviceKDataDeviceHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (data_device_destroyed)(&destroyed); }); if (seat->keyboard_focus != nullptr && seat->keyboard_focus->resource->client == client) send_selection(seat, client); }
struct WlDataDeviceManagerKDataDeviceManagerHandler {
  void create_data_source(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    (manager_create_source)(&client, &resource, id);
  }
  void get_data_device(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* seat) {
    (manager_get_device)(&client, &resource, id, seat);
  }
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
void bind_data_device_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) { auto* resource = client->create_resource(&protocol::wl_data_device_manager_interface, id, std::min(version, kDataDeviceManagerVersion)); if (resource == nullptr) { client->post_no_memory(); return; } resource->set_data(static_cast<SeatState*>(data)); resource->set_handler(protocol::wl_data_device_manager_handler(WlDataDeviceManagerKDataDeviceManagerHandler{})); }
void data_control_source_offer(zwayland::server::Client*, zwayland::server::Resource* resource, const char* mime) { auto* source = resource->data<DataSourceState>(); if (mime != nullptr && mime[0] != '\0' && std::find(source->mime_types.begin(), source->mime_types.end(), mime) == source->mime_types.end()) source->mime_types.emplace_back(mime); }
struct ZwwmDataControlSourceV1KDataControlSourceHandler {
  void offer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string mime_type) {
    (data_control_source_offer)(&client, &resource, mime_type.c_str());
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
struct ZwlrDataControlSourceV1KWlrDataControlSourceHandler {
  void offer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string mime_type) {
    (data_control_source_offer)(&client, &resource, mime_type.c_str());
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
void data_control_set_selection(zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* source_resource) { auto* device = resource->data<DataDeviceState>(); replace_selection(device->seat, source_resource == nullptr ? nullptr : source_resource->data<DataSourceState>(), false, resource); }
void data_control_set_primary_selection(zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* source_resource) { auto* device = resource->data<DataDeviceState>(); replace_selection(device->seat, source_resource == nullptr ? nullptr : source_resource->data<DataSourceState>(), true, resource); }
struct ZwwmDataControlDeviceV1KDataControlDeviceHandler {
  void set_selection(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* source) {
    (data_control_set_selection)(&client, &resource, source);
  }
  void set_primary_selection(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* source) {
    (data_control_set_primary_selection)(&client, &resource, source);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
struct ZwlrDataControlDeviceV1KWlrDataControlDeviceHandler {
  void set_selection(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* source) {
    (data_control_set_selection)(&client, &resource, source);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_primary_selection(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* source) {
    (data_control_set_primary_selection)(&client, &resource, source);
  }
};
void create_control_source(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, DataProtocol protocol) { auto* seat = manager->data<SeatState>(); const auto* interface = protocol == DataProtocol::zwlr ? &protocol::zwlr_data_control_source_v1_interface : &protocol::zwwm_data_control_source_v1_interface; auto* resource = client->create_resource(interface, id, manager->version); auto* source = resource == nullptr ? nullptr : new (std::nothrow) DataSourceState; if (source != nullptr) { source->seat = seat; source->resource = resource; source->protocol = protocol; } if (resource == nullptr || source == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; } seat->data_sources.push_back(source); resource->set_data(source); if (protocol == DataProtocol::zwlr) resource->set_handler(protocol::zwlr_data_control_source_v1_handler(ZwlrDataControlSourceV1KWlrDataControlSourceHandler{})); else resource->set_handler(protocol::zwwm_data_control_source_v1_handler(ZwwmDataControlSourceV1KDataControlSourceHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { data_source_destroyed(&destroyed); }); }
void get_control_device(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* seat_resource, DataProtocol protocol) { auto* seat = manager->data<SeatState>(); if (seat_resource == nullptr || seat_resource->data<SeatState>() != seat) { client->post_error(0, "data control device requested for an unknown seat"); return; } const auto* interface = protocol == DataProtocol::zwlr ? &protocol::zwlr_data_control_device_v1_interface : &protocol::zwwm_data_control_device_v1_interface; auto* resource = client->create_resource(interface, id, manager->version); auto* device = resource == nullptr ? nullptr : new (std::nothrow) DataDeviceState{seat, resource, protocol}; if (resource == nullptr || device == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; } seat->data_control_devices.push_back(device); resource->set_data(device); if (protocol == DataProtocol::zwlr) resource->set_handler(protocol::zwlr_data_control_device_v1_handler(ZwlrDataControlDeviceV1KWlrDataControlDeviceHandler{})); else resource->set_handler(protocol::zwwm_data_control_device_v1_handler(ZwwmDataControlDeviceV1KDataControlDeviceHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { data_device_destroyed(&destroyed); }); send_control_selection(device, false); send_control_selection(device, true); }
void data_control_create_source(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id) { create_control_source(client, manager, id, DataProtocol::zwwm); }
void data_control_get_device(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* seat_resource) { get_control_device(client, manager, id, seat_resource, DataProtocol::zwwm); }
struct ZwwmDataControlManagerV1KDataControlManagerHandler {
  void create_source(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    (data_control_create_source)(&client, &resource, id);
  }
  void get_device(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* seat) {
    (data_control_get_device)(&client, &resource, id, seat);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
void bind_data_control_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) { auto* resource = client->create_resource(&protocol::zwwm_data_control_manager_v1_interface, id, std::min(version, 1U)); if (resource == nullptr) { client->post_no_memory(); return; } resource->set_data(static_cast<SeatState*>(data)); resource->set_handler(protocol::zwwm_data_control_manager_v1_handler(ZwwmDataControlManagerV1KDataControlManagerHandler{})); }
void wlr_data_control_create_source(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id) { create_control_source(client, manager, id, DataProtocol::zwlr); }
void wlr_data_control_get_device(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* seat_resource) { get_control_device(client, manager, id, seat_resource, DataProtocol::zwlr); }
struct ZwlrDataControlManagerV1KWlrDataControlManagerHandler {
  void create_data_source(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    (wlr_data_control_create_source)(&client, &resource, id);
  }
  void get_data_device(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* seat) {
    (wlr_data_control_get_device)(&client, &resource, id, seat);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
void bind_wlr_data_control_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) { auto* resource = client->create_resource(&protocol::zwlr_data_control_manager_v1_interface, id, std::min(version, 2U)); if (resource == nullptr) { client->post_no_memory(); return; } resource->set_data(static_cast<SeatState*>(data)); resource->set_handler(protocol::zwlr_data_control_manager_v1_handler(ZwlrDataControlManagerV1KWlrDataControlManagerHandler{})); }

}  // namespace zwwm::detail
