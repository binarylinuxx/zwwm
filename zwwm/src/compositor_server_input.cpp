#include "compositor_server_internal.hpp"

#include <cursor-shape-zwayland-server.h>
#include <relative-pointer-zwayland-server.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace protocol = zwayland::generated;

namespace zwwm::detail {

struct CursorShapeDevice { SeatState* seat = nullptr; zwayland::server::Resource* pointer = nullptr; Observer* observer = nullptr; };

SeatState::~SeatState() { while (!idle_notifications.empty()) idle_notifications.back()->resource->destroy(); while (!relative_pointers.empty()) relative_pointers.back()->resource->destroy(); }

bool update_xkb(SeatState* seat, const KeyboardConfig& config) {
  if (seat->xkb_context_handle == nullptr) return false;
  const auto nullable = [](const std::string& value) { return value.empty() ? nullptr : value.c_str(); };
  const xkb_rule_names names{nullable(config.rules), nullable(config.model), nullable(config.layout),
                             nullable(config.variant), nullable(config.options)};
  xkb_keymap* keymap = xkb_keymap_new_from_names(seat->xkb_context_handle, &names,
                                                 XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_state* state = keymap == nullptr ? nullptr : xkb_state_new(keymap);
  char* text = keymap == nullptr ? nullptr : xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  if (keymap == nullptr || state == nullptr || text == nullptr) {
    if (text != nullptr) std::free(text);
    if (state != nullptr) xkb_state_unref(state);
    if (keymap != nullptr) xkb_keymap_unref(keymap);
    return false;
  }
  for (const auto key : seat->pressed_keys)
    if (key <= std::numeric_limits<xkb_keycode_t>::max() - 8)
      xkb_state_update_key(state, static_cast<xkb_keycode_t>(key + 8), XKB_KEY_DOWN);
  if (seat->xkb_state_handle != nullptr) xkb_state_unref(seat->xkb_state_handle);
  if (seat->xkb_keymap_handle != nullptr) xkb_keymap_unref(seat->xkb_keymap_handle);
  seat->xkb_keymap_handle = keymap;
  seat->xkb_state_handle = state;
  seat->keymap = text;
  std::free(text);
  return true;
}

bool initialize_xkb(SeatState* seat) {
  seat->xkb_context_handle = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  const auto* config = seat->display.observer == nullptr ? nullptr : seat->display.observer->config;
  return config != nullptr && update_xkb(seat, config->keyboard);
}

void release_xkb(SeatState* seat) { if (seat->xkb_state_handle != nullptr) xkb_state_unref(seat->xkb_state_handle); if (seat->xkb_keymap_handle != nullptr) xkb_keymap_unref(seat->xkb_keymap_handle); if (seat->xkb_context_handle != nullptr) xkb_context_unref(seat->xkb_context_handle); }

UniqueFd keymap_fd(const SeatState& seat) { constexpr unsigned kMfdCloexec = 0x0001U, kMfdAllowSealing = 0x0002U; constexpr int kAddSeals = 1033, kSealShrink = 0x0002, kSealGrow = 0x0004, kSealWrite = 0x0008; int descriptor = -1;
#ifdef SYS_memfd_create
  descriptor = static_cast<int>(syscall(SYS_memfd_create, "zwwm-keymap", kMfdCloexec | kMfdAllowSealing));
#endif
  if (descriptor < 0) { FILE* file = std::tmpfile(); if (file == nullptr) return {}; descriptor = fcntl(fileno(file), F_DUPFD_CLOEXEC, 0); std::fclose(file); if (descriptor < 0) return {}; }
  UniqueFd fd(descriptor); const char* data = seat.keymap.c_str(); std::size_t remaining = seat.keymap.size() + 1; while (remaining != 0) { const ssize_t written = write(fd.get(), data, remaining); if (written < 0 && errno == EINTR) continue; if (written <= 0) return {}; data += written; remaining -= static_cast<std::size_t>(written); } if (lseek(fd.get(), 0, SEEK_SET) < 0) return {}; (void)fcntl(fd.get(), kAddSeals, kSealShrink | kSealGrow | kSealWrite); return fd; }

void remember_serial(SeatState& seat, zwayland::server::Client* client, std::uint32_t serial) { seat.input_serials.emplace_back(client, serial); if (seat.input_serials.size() > 64) seat.input_serials.erase(seat.input_serials.begin()); }
bool valid_selection_serial(const SeatState& seat, zwayland::server::Client* client, std::uint32_t serial) { if (seat.keyboard_focus == nullptr || seat.keyboard_focus->resource->client != client) return false; return std::find(seat.input_serials.begin(), seat.input_serials.end(), std::pair{client, serial}) != seat.input_serials.end(); }
void send_modifiers(SeatState& seat) { if (seat.keyboard_focus == nullptr || seat.xkb_state_handle == nullptr) return; const auto depressed = xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_DEPRESSED), latched = xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_LATCHED), locked = xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_LOCKED), group = xkb_state_serialize_layout(seat.xkb_state_handle, XKB_STATE_LAYOUT_EFFECTIVE); const auto serial = seat.display->next_serial(); auto* client = seat.keyboard_focus->resource->client; bool sent = false; for (auto* resource : seat.keyboards) if (resource->client == client) { protocol::wl_keyboard_send_modifiers(*resource, serial, depressed, latched, locked, group); sent = true; } if (sent) remember_serial(seat, client, serial); }

void publish_keymap(SeatState* seat) {
  if (seat->keyboards.empty()) return;
  const auto fd = keymap_fd(*seat);
  if (!fd) return;
  for (auto* keyboard : seat->keyboards)
    protocol::wl_keyboard_send_keymap(*keyboard, protocol::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd.get(),
                             static_cast<std::uint32_t>(seat->keymap.size() + 1));
  send_modifiers(*seat);
}

bool binding_modifiers_match(const Keybinding& binding, const SeatState& seat) {
  if (seat.xkb_state_handle == nullptr || seat.xkb_keymap_handle == nullptr) return false;
  xkb_mod_mask_t required = 0;
  std::size_t begin = 0;
  while (begin <= binding.modifiers.size()) {
    const auto end = binding.modifiers.find('+', begin);
    const auto name = binding.modifiers.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!name.empty()) {
      const char* modifier = name == "Alt" || name == "alt" ? "Mod1" :
                             name == "Super" || name == "super" ? "Mod4" : name.c_str();
      const auto index = xkb_keymap_mod_get_index(seat.xkb_keymap_handle, modifier);
      if (index == XKB_MOD_INVALID || index >= sizeof(required) * 8U) return false;
      required |= static_cast<xkb_mod_mask_t>(1) << index;
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  const auto active = xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_DEPRESSED) |
                       xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_LATCHED);
  xkb_mod_mask_t physical = 0;
  for (const char* name : {"Shift", "Control", "Mod1", "Mod3", "Mod4", "Mod5"}) {
    const auto index = xkb_keymap_mod_get_index(seat.xkb_keymap_handle, name);
    if (index != XKB_MOD_INVALID && index < sizeof(physical) * 8U)
      physical |= static_cast<xkb_mod_mask_t>(1) << index;
  }
  return (active & required) == required && (active & physical) == (required & physical);
}
bool binding_matches(const Keybinding& binding, const SeatState& seat, std::uint32_t key) {
  if (!binding_modifiers_match(binding, seat)) return false;
  if (seat.xkb_keymap_handle == nullptr || key > std::numeric_limits<xkb_keycode_t>::max() - 8) return false;
  const auto expected = xkb_keysym_from_name(binding.key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
  if (expected == XKB_KEY_NoSymbol) return false;
  const xkb_keysym_t* symbols = nullptr;
  const auto count = xkb_keymap_key_get_syms_by_level(
      seat.xkb_keymap_handle, static_cast<xkb_keycode_t>(key + 8), 0, 0, &symbols);
  return std::find(symbols, symbols + count, expected) != symbols + count;
}

bool execute_binding(const Keybinding& binding) {
  if (binding.action != KeyAction::exec || binding.argument.empty()) return false;
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    const pid_t command = fork();
    if (command == 0) {
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGCHLD);
      (void)sigprocmask(SIG_UNBLOCK, &mask, nullptr);
      execlp("sh", "sh", "-c", binding.argument.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    _exit(command < 0 ? 126 : 0);
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

const char* xcursor_name(std::uint32_t shape) {
  switch (shape) {
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT: return "left_ptr";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU: return "context-menu";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP: return "help";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER: return "pointer";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS: return "progress";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT: return "watch";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL: return "cell";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR: return "crosshair";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT: return "xterm";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT: return "vertical-text";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS: return "alias";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY: return "copy";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE: return "move";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP: return "no-drop";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED: return "not-allowed";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB: return "grab";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING: return "grabbing";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE: return "e-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE: return "n-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE: return "ne-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE: return "nw-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE: return "s-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE: return "se-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE: return "sw-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE: return "w-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE: return "ew-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE: return "ns-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE: return "nesw-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE: return "nwse-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE: return "col-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE: return "row-resize";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL: return "all-scroll";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN: return "zoom-in";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT: return "zoom-out";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK: return "dnd-ask";
    case protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE: return "all-resize";
    default: return "left_ptr";
  }
}

void apply_cursor_shape(SeatState* seat) {
  auto* observer = seat == nullptr ? nullptr : seat->display.observer;
  if (observer != nullptr && observer->cursor_callback != nullptr)
    observer->cursor_callback(observer->cursor_data,
                              seat->cursor_hidden_for_lock || seat->cursor_hidden_by_client ? nullptr :
                                  (seat->cursor_override_shape.empty() ? seat->cursor_shape.c_str() :
                                                                        seat->cursor_override_shape.c_str()));
}

void sync_pointer_position(SeatState* seat) {
  auto* observer = seat == nullptr ? nullptr : seat->display.observer;
  if (observer != nullptr && observer->pointer_position_callback != nullptr)
    observer->pointer_position_callback(observer->pointer_position_data, seat->pointer_x, seat->pointer_y);
}

void cursor_shape_set_shape(zwayland::server::Client* client, zwayland::server::Resource* resource, std::uint32_t serial, std::uint32_t shape) {
  auto* device = resource->data<CursorShapeDevice>();
  if (shape < protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT ||
      shape > (resource->version >= 2 ? protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE
                                     : protocol::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT)) {
    resource->post_error(protocol::WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE, "invalid cursor shape");
    return;
  }
  if (device == nullptr) return;
  const auto entered = device->seat->pointer_enter_serials.find(device->pointer);
  if (device->seat->pointer_focus == nullptr ||
      std::find(device->seat->pointers.begin(), device->seat->pointers.end(), device->pointer) == device->seat->pointers.end() ||
      device->pointer->client != client || entered == device->seat->pointer_enter_serials.end() ||
      entered->second != serial) return;
  device->seat->cursor_shape = xcursor_name(shape);
  device->seat->cursor_hidden_by_client = false;
  apply_cursor_shape(device->seat);
}

struct WpCursorShapeDeviceV1KCursorShapeDeviceHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_shape(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial, std::uint32_t shape) {
    (cursor_shape_set_shape)(&client, &resource, serial, shape);
  }
};

void get_cursor_shape_pointer(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* pointer) {
  auto* observer = manager->data<Observer>();
  auto* seat = observer == nullptr ? nullptr : observer->seat;
  const bool known_pointer = seat != nullptr && pointer != nullptr && pointer->client == client &&
      std::find(seat->pointers.begin(), seat->pointers.end(), pointer) != seat->pointers.end();
  if (!known_pointer) {
    client->post_error(0, "cursor shape requested for an unknown pointer");
    return;
  }
  auto* resource = client->create_resource(&protocol::wp_cursor_shape_device_v1_interface, id, manager->version);
  auto* device = resource == nullptr ? nullptr : new (std::nothrow) CursorShapeDevice{seat, pointer, observer};
  if (resource == nullptr || device == nullptr) {
    if (resource != nullptr) resource->destroy();
    client->post_no_memory();
    return;
  }
  resource->set_data(device); resource->set_handler(protocol::wp_cursor_shape_device_v1_handler(WpCursorShapeDeviceV1KCursorShapeDeviceHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { ([](zwayland::server::Resource* value) { delete value->data<CursorShapeDevice>(); })(&destroyed); });
}

struct WpCursorShapeManagerV1KCursorShapeManagerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void get_pointer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t cursor_shape_device, zwayland::server::Resource* pointer) {
    (get_cursor_shape_pointer)(&client, &resource, cursor_shape_device, pointer);
  }
  void get_tablet_tool_v2(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t cursor_shape_device, zwayland::server::Resource* tablet_tool) {
    ([](zwayland::server::Client* client, zwayland::server::Resource*, std::uint32_t, zwayland::server::Resource*) { client->post_error(0, "tablet cursor shapes are unsupported"); })(&client, &resource, cursor_shape_device, tablet_tool);
  }
};

void bind_cursor_shape_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::wp_cursor_shape_manager_v1_interface, id, std::min(version, 1U));
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<Observer*>(data)); resource->set_handler(protocol::wp_cursor_shape_manager_v1_handler(WpCursorShapeManagerV1KCursorShapeManagerHandler{}));
}

void relative_pointer_destroyed(zwayland::server::Resource* resource) {
  auto* relative = resource->data<RelativePointer>();
  if (relative == nullptr) return;
  std::erase(relative->seat->relative_pointers, relative);
  delete relative;
}

struct ZwpRelativePointerV1KRelativePointerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
struct ZwpRelativePointerManagerV1KRelativePointerManagerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void get_relative_pointer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* pointer) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* pointer) {
      auto* seat = manager->data<SeatState>();
      const bool valid = seat != nullptr && pointer != nullptr && pointer->client == client &&
                         std::find(seat->pointers.begin(), seat->pointers.end(), pointer) != seat->pointers.end();
      if (!valid) {
        client->post_error(0, "relative motion requested for an unknown pointer");
        return;
      }
      auto* resource = client->create_resource(&protocol::zwp_relative_pointer_v1_interface, id, 1);
      auto* relative = resource == nullptr ? nullptr : new (std::nothrow) RelativePointer{seat, pointer, resource};
      if (resource == nullptr || relative == nullptr) {
        if (resource != nullptr) resource->destroy();
        client->post_no_memory();
        return;
      }
      seat->relative_pointers.push_back(relative);
      resource->set_data(relative); resource->set_handler(protocol::zwp_relative_pointer_v1_handler(ZwpRelativePointerV1KRelativePointerHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (relative_pointer_destroyed)(&destroyed); });
    })(&client, &resource, id, pointer);
  }
};

void bind_relative_pointer_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwp_relative_pointer_manager_v1_interface, id, std::min(version, 1U));
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data)); resource->set_handler(protocol::zwp_relative_pointer_manager_v1_handler(ZwpRelativePointerManagerV1KRelativePointerManagerHandler{}));
}

std::vector<Rect> constraint_rects(const PointerConstraint* constraint) {
  if (constraint->surface == nullptr || constraint->surface->current_buffer == nullptr) return {};
  const auto* surface = constraint->surface;
  const auto width = surface->current_viewport.has_destination ? surface->current_viewport.destination_width :
                     surface->current_viewport.has_source ? static_cast<int>(surface->current_viewport.width) : surface->current_buffer->width / surface->current_buffer_scale;
  const auto height = surface->current_viewport.has_destination ? surface->current_viewport.destination_height :
                      surface->current_viewport.has_source ? static_cast<int>(surface->current_viewport.height) : surface->current_buffer->height / surface->current_buffer_scale;
  std::vector<Rect> base = surface->current_input_infinite ? std::vector<Rect>{{0, 0, width, height}}
                                                           : surface->current_input_region;
  if (!constraint->has_region) return base;
  std::vector<Rect> result;
  for (const Rect& left : base) for (const Rect& right : constraint->region) {
    const auto x1 = std::max(left.x, right.x), y1 = std::max(left.y, right.y);
    const auto x2 = std::min<std::int64_t>(static_cast<std::int64_t>(left.x) + left.width,
                                           static_cast<std::int64_t>(right.x) + right.width);
    const auto y2 = std::min<std::int64_t>(static_cast<std::int64_t>(left.y) + left.height,
                                           static_cast<std::int64_t>(right.y) + right.height);
    if (x2 > x1 && y2 > y1) result.push_back({x1, y1, static_cast<std::int32_t>(x2 - x1), static_cast<std::int32_t>(y2 - y1)});
  }
  return result;
}

bool point_in_rects(const std::vector<Rect>& rects, std::int32_t x, std::int32_t y) {
  return std::any_of(rects.begin(), rects.end(), [x, y](const Rect& rect) {
    return rect.width > 0 && rect.height > 0 && x >= rect.x && y >= rect.y &&
           static_cast<std::int64_t>(x) < static_cast<std::int64_t>(rect.x) + rect.width &&
           static_cast<std::int64_t>(y) < static_cast<std::int64_t>(rect.y) + rect.height;
  });
}

bool clamp_to_rects(const std::vector<Rect>& rects, std::int32_t* x, std::int32_t* y) {
  if (point_in_rects(rects, *x, *y)) return true;
  bool found = false;
  std::int64_t best = std::numeric_limits<std::int64_t>::max();
  std::int32_t best_x = *x, best_y = *y;
  for (const Rect& rect : rects) {
    if (rect.width <= 0 || rect.height <= 0) continue;
    const auto right = static_cast<std::int32_t>(static_cast<std::int64_t>(rect.x) + rect.width - 1);
    const auto bottom = static_cast<std::int32_t>(static_cast<std::int64_t>(rect.y) + rect.height - 1);
    const auto candidate_x = std::clamp(*x, rect.x, right), candidate_y = std::clamp(*y, rect.y, bottom);
    const auto dx = static_cast<std::int64_t>(*x) - candidate_x, dy = static_cast<std::int64_t>(*y) - candidate_y;
    const auto distance = dx * dx + dy * dy;
    if (distance < best) { best = distance; best_x = candidate_x; best_y = candidate_y; found = true; }
  }
  if (found) { *x = best_x; *y = best_y; }
  return found;
}

bool confine_to_rects(const std::vector<Rect>& rects, std::int32_t old_x, std::int32_t old_y,
                      std::int32_t* x, std::int32_t* y) {
  for (const Rect& rect : rects) {
    if (!point_in_rects({rect}, old_x, old_y)) continue;
    const auto right = static_cast<std::int32_t>(static_cast<std::int64_t>(rect.x) + rect.width - 1);
    const auto bottom = static_cast<std::int32_t>(static_cast<std::int64_t>(rect.y) + rect.height - 1);
    if (*x >= rect.x && *x <= right && *y >= rect.y && *y <= bottom) return true;
  }
  for (const Rect& rect : rects) {
    if (!point_in_rects({rect}, old_x, old_y)) continue;
    const auto right = static_cast<std::int32_t>(static_cast<std::int64_t>(rect.x) + rect.width - 1);
    const auto bottom = static_cast<std::int32_t>(static_cast<std::int64_t>(rect.y) + rect.height - 1);
    *x = std::clamp(*x, rect.x, right);
    *y = std::clamp(*y, rect.y, bottom);
    return true;
  }
  return clamp_to_rects(rects, x, y);
}

void apply_cursor_hint(PointerConstraint* constraint) {
  if (!constraint->locked || !constraint->has_hint || constraint->surface == nullptr) return;
  std::int32_t x = static_cast<int>(constraint->hint_x), y = static_cast<int>(constraint->hint_y);
  auto rects = constraint_rects(constraint);
  if (!point_in_rects(rects, x, y)) return;
  if (root(constraint->surface) != nullptr && root(constraint->surface)->xdg_surface != nullptr)
    surface_global_from_local(constraint->surface, x, y, &constraint->seat->pointer_x, &constraint->seat->pointer_y);
  else {
    std::int32_t surface_x = 0, surface_y = 0;
    absolute_position(constraint->surface, &surface_x, &surface_y);
    constraint->seat->pointer_x = surface_x + x;
    constraint->seat->pointer_y = surface_y + y;
  }
  constraint->x = x;
  constraint->y = y;
  sync_pointer_position(constraint->seat);
}

void deactivate_constraint(PointerConstraint* constraint, bool send_event) {
  if (!constraint->active) return;
  apply_cursor_hint(constraint);
  constraint->active = false;
  if (constraint->locked) {
    constraint->seat->cursor_hidden_for_lock = false;
    apply_cursor_shape(constraint->seat);
  }
  if (send_event && constraint->resource != nullptr) {
    if (constraint->locked) protocol::zwp_locked_pointer_v1_send_unlocked(*constraint->resource);
    else protocol::zwp_confined_pointer_v1_send_unconfined(*constraint->resource);
  }
  if (constraint->lifetime == protocol::ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT) constraint->defunct = true;
}

void update_pointer_constraints(SeatState* seat, std::int32_t, std::int32_t) {
  if (seat == nullptr) return;
  for (auto* constraint : seat->constraints) {
    const bool focused = !constraint->defunct && constraint->pointer != nullptr &&
                         constraint->surface != nullptr && seat->pointer_focus != nullptr &&
                         root(seat->pointer_focus) == root(constraint->surface);
    if (constraint->active && !focused) { deactivate_constraint(constraint, true); continue; }
    if (!focused) continue;
    std::int32_t x = 0, y = 0;
    surface_local_from_global(constraint->surface, seat->pointer_x, seat->pointer_y, &x, &y);
    if (!constraint->active) {
      auto rects = constraint_rects(constraint);
      if (!clamp_to_rects(rects, &x, &y)) continue;
      constraint->active = true;
      constraint->x = x;
      constraint->y = y;
      if (constraint->locked) {
        seat->cursor_hidden_for_lock = true;
        apply_cursor_shape(seat);
        protocol::zwp_locked_pointer_v1_send_locked(*constraint->resource);
      }
      else protocol::zwp_confined_pointer_v1_send_confined(*constraint->resource);
    } else if (!constraint->locked) {
      auto rects = constraint_rects(constraint);
      if (!confine_to_rects(rects, constraint->x, constraint->y, &x, &y)) { deactivate_constraint(constraint, true); continue; }
      constraint->x = x;
      constraint->y = y;
    }
  }
}

PointerConstraint* active_constraint(SeatState* seat) {
  const auto found = std::find_if(seat->constraints.begin(), seat->constraints.end(), [](const PointerConstraint* constraint) { return constraint->active; });
  return found == seat->constraints.end() ? nullptr : *found;
}

void constraint_destroyed(zwayland::server::Resource* resource) {
  auto* constraint = resource->data<PointerConstraint>();
  if (constraint == nullptr) return;
  deactivate_constraint(constraint, false);
  std::erase(constraint->seat->constraints, constraint);
  delete constraint;
}

void constraint_set_region(PointerConstraint* constraint, zwayland::server::Resource* region) {
  constraint->pending_region.clear();
  constraint->pending_has_region = region != nullptr;
  if (region != nullptr) constraint->pending_region = region->data<RegionState>()->rects;
  constraint->region_pending = true;
}

struct ZwpLockedPointerV1KLockedPointerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_cursor_position_hint(zwayland::server::Client& client, zwayland::server::Resource& resource, double surface_x, double surface_y) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, double x, double y) { auto* constraint = resource->data<PointerConstraint>(); if (!constraint->defunct) { constraint->pending_hint_x = x; constraint->pending_hint_y = y; constraint->hint_pending = true; } })(&client, &resource, surface_x, surface_y);
  }
  void set_region(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* region) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* region) { auto* constraint = resource->data<PointerConstraint>(); if (!constraint->defunct) constraint_set_region(constraint, region); })(&client, &resource, region);
  }
};
struct ZwpConfinedPointerV1KConfinedPointerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_region(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* region) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* region) { auto* constraint = resource->data<PointerConstraint>(); if (!constraint->defunct) constraint_set_region(constraint, region); })(&client, &resource, region);
  }
};

void create_constraint(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* surface_resource,
                       zwayland::server::Resource* pointer, zwayland::server::Resource* region, std::uint32_t lifetime, bool locked) {
  auto* seat = manager->data<SeatState>();
  auto* surface = compositor_surface_from_resource(client, surface_resource);
  const bool surface_resource_valid = surface != nullptr;
  const bool valid_surface = surface_resource_valid &&
                             surface != nullptr && surface->observer != nullptr && surface->observer->seat == seat;
  const bool valid_pointer = pointer != nullptr && pointer->client == client &&
                             std::find(seat->pointers.begin(), seat->pointers.end(), pointer) != seat->pointers.end();
  const bool valid_region = region == nullptr || (region->client == client &&
                             region->interface == &protocol::wl_region_interface &&
                             region->data<RegionState>() != nullptr);
  if (!valid_surface || !valid_pointer || !valid_region ||
      (lifetime != protocol::ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT &&
       lifetime != protocol::ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT)) {
    client->post_error(0, "invalid pointer constraint arguments");
    return;
  }
  if (std::any_of(seat->constraints.begin(), seat->constraints.end(), [surface](const PointerConstraint* item) { return item->surface == surface; })) {
    manager->post_error(protocol::ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED, "surface already has a pointer constraint");
    return;
  }
  const zwayland::server::Interface* interface = locked ? &protocol::zwp_locked_pointer_v1_interface : &protocol::zwp_confined_pointer_v1_interface;
  auto* resource = client->create_resource(interface, id, 1);
  auto* constraint = resource == nullptr ? nullptr : new (std::nothrow) PointerConstraint;
  if (resource == nullptr || constraint == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; }
  constraint->seat = seat;
  constraint->surface = surface;
  constraint->pointer = pointer;
  constraint->resource = resource;
  constraint->lifetime = lifetime;
  constraint->locked = locked;
  if (region != nullptr) { constraint->region = region->data<RegionState>()->rects; constraint->has_region = true; }
  seat->constraints.push_back(constraint);
  resource->set_data(constraint);
  if (locked)
    resource->set_handler(protocol::zwp_locked_pointer_v1_handler(ZwpLockedPointerV1KLockedPointerHandler{}));
  else
    resource->set_handler(protocol::zwp_confined_pointer_v1_handler(ZwpConfinedPointerV1KConfinedPointerHandler{}));
  resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { constraint_destroyed(&destroyed); });
  if ((seat->pointer_focus != nullptr && root(seat->pointer_focus) == root(surface)) ||
      seat->toplevel_focus == root(surface)) {
    std::int32_t x = 0, y = 0;
    surface_local_from_global(surface, seat->pointer_x, seat->pointer_y, &x, &y);
    if (seat->pointer_focus != surface) set_pointer_focus(seat, surface, x, y);
    else update_pointer_constraints(seat, x, y);
  }
}

struct ZwpPointerConstraintsV1KPointerConstraintsHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void lock_pointer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface, zwayland::server::Resource* pointer, zwayland::server::Resource* region, std::uint32_t lifetime) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* surface, zwayland::server::Resource* pointer, zwayland::server::Resource* region, std::uint32_t lifetime) { create_constraint(client, manager, id, surface, pointer, region, lifetime, true); })(&client, &resource, id, surface, pointer, region, lifetime);
  }
  void confine_pointer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface, zwayland::server::Resource* pointer, zwayland::server::Resource* region, std::uint32_t lifetime) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* surface, zwayland::server::Resource* pointer, zwayland::server::Resource* region, std::uint32_t lifetime) { create_constraint(client, manager, id, surface, pointer, region, lifetime, false); })(&client, &resource, id, surface, pointer, region, lifetime);
  }
};

void bind_pointer_constraints(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwp_pointer_constraints_v1_interface, id, std::min(version, 1U));
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data)); resource->set_handler(protocol::zwp_pointer_constraints_v1_handler(ZwpPointerConstraintsV1KPointerConstraintsHandler{}));
}

void pointer_resource_destroyed(zwayland::server::Resource* resource) {
  auto* seat = resource->data<SeatState>();
  if (seat == nullptr) return;
  seat->pointer_enter_serials.erase(resource);
  std::erase(seat->pointers, resource);
  for (auto* constraint : seat->constraints) if (constraint->pointer == resource) { deactivate_constraint(constraint, true); constraint->pointer = nullptr; constraint->defunct = true; }
  for (auto* relative : seat->relative_pointers) if (relative->pointer == resource) relative->pointer = nullptr;
}

struct WlPointerKPointerHandler {
  void set_cursor(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial, zwayland::server::Resource* surface, std::int32_t hotspot_x, std::int32_t hotspot_y) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* resource, std::uint32_t serial, zwayland::server::Resource* surface,
       std::int32_t, std::int32_t) {
      auto* seat = resource->data<SeatState>();
      if (seat == nullptr || seat->pointer_focus == nullptr ||
          seat->pointer_focus->resource->client != client) return;
      const auto entered = seat->pointer_enter_serials.find(resource);
      if (entered == seat->pointer_enter_serials.end() || entered->second != serial) return;
      if (surface != nullptr) {
        auto* cursor_surface = compositor_surface_from_resource(client, surface);
        if (cursor_surface == nullptr || cursor_surface->xdg_surface != nullptr ||
            cursor_surface->layer_surface != nullptr || cursor_surface->lock_surface != nullptr ||
            cursor_surface->parent != nullptr) return;
      }
      seat->cursor_hidden_by_client = surface == nullptr;
      apply_cursor_shape(seat);
    })(&client, &resource, serial, surface, hotspot_x, hotspot_y);
  }
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

void resource_destroyed(zwayland::server::Resource* resource) {
  auto* resources = resource->data<std::vector<zwayland::server::Resource*>>();
  if (resources != nullptr) std::erase(*resources, resource);
}

void bind_seat(zwayland::server::Client* c, void* data, std::uint32_t v, std::uint32_t id) {
  auto* seat = static_cast<SeatState*>(data);
  auto* r = c->create_resource(&protocol::wl_seat_interface, id, std::min(v, kSeatVersion));
  if (r == nullptr) { c->post_no_memory(); return; }
  struct WlKeyboardKeyboardHandler {
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* q) { q->destroy(); })(&client, &resource);
  }
};
  struct WlSeatImplHandler {
  void get_pointer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* seat_resource, std::uint32_t object_id) {
        auto* state = seat_resource->data<SeatState>();
        auto* resource = client->create_resource(&protocol::wl_pointer_interface, object_id, seat_resource->version);
        if (resource == nullptr) { client->post_no_memory(); return; }
        state->pointers.push_back(resource);
        resource->set_data(state); resource->set_handler(protocol::wl_pointer_handler(WlPointerKPointerHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (pointer_resource_destroyed)(&destroyed); });
        if (state->pointer_focus == nullptr || state->pointer_focus->resource->client != client) return;
        std::int32_t x = 0, y = 0;
        surface_local_from_global(state->pointer_focus, state->pointer_x, state->pointer_y, &x, &y);
        const auto serial = state->display->next_serial();
        protocol::wl_pointer_send_enter(*resource, serial, state->pointer_focus->resource, (x), (y));
        state->pointer_enter_serials[resource] = serial;
        remember_serial(*state, client, serial);
        if (resource->version >= 5) protocol::wl_pointer_send_frame(*resource);
      })(&client, &resource, id);
  }
  void get_keyboard(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* seat_resource, std::uint32_t object_id) {
        auto* state = seat_resource->data<SeatState>();
        auto* resource = client->create_resource(&protocol::wl_keyboard_interface, object_id, seat_resource->version);
        if (resource == nullptr) { client->post_no_memory(); return; }
        const auto fd = keymap_fd(*state);
        if (!fd) { resource->destroy(); client->post_no_memory(); return; }
        state->keyboards.push_back(resource);
        resource->set_data(&state->keyboards); resource->set_handler(protocol::wl_keyboard_handler(WlKeyboardKeyboardHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (resource_destroyed)(&destroyed); });
        protocol::wl_keyboard_send_keymap(*resource, protocol::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd.get(), static_cast<std::uint32_t>(state->keymap.size() + 1));
        if (resource->version >= 4) protocol::wl_keyboard_send_repeat_info(*resource, state->repeat_rate, state->repeat_delay);
        if (state->keyboard_focus == nullptr || state->keyboard_focus->resource->client != client) return;
        const auto serial = state->display->next_serial();
        protocol::wl_keyboard_send_enter(*resource, serial, state->keyboard_focus->resource,
                                         std::as_bytes(std::span(state->pressed_keys)));
        remember_serial(*state, client, serial);
        send_modifiers(*state);
      })(&client, &resource, id);
  }
  void get_touch(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    ([](zwayland::server::Client*, zwayland::server::Resource*, std::uint32_t) {})(&client, &resource, id);
  }
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
  r->set_data(seat); r->set_handler(protocol::wl_seat_handler(WlSeatImplHandler{}));
  protocol::wl_seat_send_capabilities(*r, protocol::WL_SEAT_CAPABILITY_POINTER | protocol::WL_SEAT_CAPABILITY_KEYBOARD);
  if (r->version >= 2) protocol::wl_seat_send_name(*r, "seat0");
}

}  // namespace zwwm::detail
