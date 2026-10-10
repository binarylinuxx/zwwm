#include "compositor_server_internal.hpp"
#include <zwwm-virtual-keyboard-v1-zwayland-server.h>
#include <linux/input-event-codes.h>
#include <algorithm>
#include <new>

namespace protocol = zwayland::generated;
namespace zwwm::detail {
namespace {
struct VirtualKeyboard { SeatState* seat; std::uint64_t source; };

void send_keymap(zwayland::server::Resource& resource, SeatState* seat) {
  const auto fd = keymap_fd(*seat);
  if (!fd) { resource.client->post_no_memory(); return; }
  protocol::zwwm_virtual_keyboard_v1_send_keymap(resource, protocol::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                               fd.get(), static_cast<std::uint32_t>(seat->keymap.size() + 1));
}

struct KeyboardHandler {
  void key(zwayland::server::Client&, zwayland::server::Resource& resource,
           std::uint32_t time, std::uint32_t key, std::uint32_t state) {
    auto* keyboard = resource.data<VirtualKeyboard>();
    if (key > KEY_MAX) {
      resource.post_error(protocol::ZWWM_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEY, "invalid evdev keycode");
      return;
    }
    if (state > protocol::WL_KEYBOARD_KEY_STATE_PRESSED) {
      resource.post_error(protocol::ZWWM_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_STATE, "invalid key state");
      return;
    }
    if (keyboard->seat->session_lock != nullptr) return;
    keyboard->seat->compositor->keyboard_key_from_source(keyboard->source, time, key, state);
  }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};

struct ManagerHandler {
  void create_keyboard(zwayland::server::Client& client, zwayland::server::Resource& manager,
                        std::uint32_t id, zwayland::server::Resource* seat_resource) {
    auto* seat = manager.data<SeatState>();
    if (seat_resource == nullptr || seat_resource->client != &client ||
        seat_resource->interface != &protocol::wl_seat_interface || seat_resource->data<SeatState>() != seat) {
      manager.post_error(protocol::ZWWM_VIRTUAL_KEYBOARD_MANAGER_V1_ERROR_INVALID_SEAT, "unknown seat");
      return;
    }
    auto* resource = client.create_resource(&protocol::zwwm_virtual_keyboard_v1_interface, id, 1);
    auto* keyboard = resource == nullptr ? nullptr : new (std::nothrow) VirtualKeyboard{seat, seat->next_keyboard_source++};
    if (keyboard == nullptr) {
      if (resource != nullptr) resource->destroy();
      client.post_no_memory();
      return;
    }
    resource->set_data(keyboard);
    resource->set_handler(protocol::zwwm_virtual_keyboard_v1_handler(KeyboardHandler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& resource) {
      auto* keyboard = resource.data<VirtualKeyboard>();
      std::erase(keyboard->seat->virtual_keyboards, &resource);
      keyboard->seat->compositor->release_keyboard_source(keyboard->source);
      delete keyboard;
    });
    seat->virtual_keyboards.push_back(resource);
    send_keymap(*resource, seat);
    publish_virtual_modifiers(seat);
  }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};
}  // namespace

void publish_virtual_keymap(SeatState* seat) {
  for (auto* resource : seat->virtual_keyboards) send_keymap(*resource, seat);
  publish_virtual_modifiers(seat);
}

void publish_virtual_modifiers(SeatState* seat) {
  if (seat->xkb_state_handle == nullptr) return;
  for (auto* resource : seat->virtual_keyboards)
    protocol::zwwm_virtual_keyboard_v1_send_modifiers(*resource,
        xkb_state_serialize_mods(seat->xkb_state_handle, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(seat->xkb_state_handle, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(seat->xkb_state_handle, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(seat->xkb_state_handle, XKB_STATE_LAYOUT_EFFECTIVE));
}

void reset_virtual_keyboards(SeatState* seat) {
  for (auto* resource : seat->virtual_keyboards) {
    seat->compositor->release_keyboard_source(resource->data<VirtualKeyboard>()->source);
    protocol::zwwm_virtual_keyboard_v1_send_reset(*resource);
  }
}

void bind_virtual_keyboard_manager(zwayland::server::Client* client, void* data, std::uint32_t,
                                   std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwwm_virtual_keyboard_manager_v1_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::zwwm_virtual_keyboard_manager_v1_handler(ManagerHandler{}));
}
}  // namespace zwwm::detail
