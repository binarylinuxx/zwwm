#include "zwwm/compositor_server.hpp"
#include "zwwm/runtime_config.hpp"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <xdg-shell-client-protocol.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <algorithm>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {
struct Client {
  wl_display* display = nullptr; wl_registry* registry = nullptr; wl_compositor* compositor = nullptr;
  wl_shm* shm = nullptr; wl_subcompositor* subcompositor = nullptr; wl_seat* seat = nullptr; wl_pointer* pointer = nullptr; wl_keyboard* keyboard = nullptr; xdg_wm_base* wm = nullptr;
  wl_surface* pointer_target = nullptr;
  std::uint32_t serial = 0, pointer_serial = 0, button_serial = 0; int pointer_x = -1, pointer_y = -1, popup_x = -1, popup_y = -1, popup_w = 0, popup_h = 0, popup_done = 0, keyboard_enters = 0;
};
void globals(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
  auto& c = *static_cast<Client*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0) c.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U)));
  if (std::strcmp(interface, wl_shm_interface.name) == 0) c.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
  if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) c.subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
  if (std::strcmp(interface, wl_seat_interface.name) == 0) c.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
  if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) c.wm = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 3));
}
void global_remove(void*, wl_registry*, std::uint32_t) {}
void ping(void*, xdg_wm_base* wm, std::uint32_t serial) { xdg_wm_base_pong(wm, serial); }
void configured(void* data, xdg_surface* surface, std::uint32_t serial) { auto& c = *static_cast<Client*>(data); c.serial = serial; xdg_surface_ack_configure(surface, serial); }
void top_configure(void*, xdg_toplevel*, std::int32_t, std::int32_t, wl_array*) {}
void top_close(void*, xdg_toplevel*) {}
void popup_configure(void* data, xdg_popup*, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) { auto& c = *static_cast<Client*>(data); c.popup_x = x; c.popup_y = y; c.popup_w = w; c.popup_h = h; }
void popup_done(void* data, xdg_popup*) { ++static_cast<Client*>(data)->popup_done; }
void popup_repositioned(void*, xdg_popup*, std::uint32_t) {}
void pointer_enter(void* data, wl_pointer*, std::uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y) { auto& c = *static_cast<Client*>(data); c.pointer_serial = serial; c.pointer_target = surface; c.pointer_x = wl_fixed_to_int(x); c.pointer_y = wl_fixed_to_int(y); }
void pointer_leave(void*, wl_pointer*, std::uint32_t, wl_surface*) {}
void pointer_motion(void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) { auto& c = *static_cast<Client*>(data); c.pointer_x = wl_fixed_to_int(x); c.pointer_y = wl_fixed_to_int(y); }
void pointer_button(void* data, wl_pointer*, std::uint32_t serial, std::uint32_t, std::uint32_t, std::uint32_t) { static_cast<Client*>(data)->button_serial = serial; }
void pointer_axis(void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {}
const wl_pointer_listener kPointerListener = [] { wl_pointer_listener value{}; value.enter = pointer_enter; value.leave = pointer_leave; value.motion = pointer_motion; value.button = pointer_button; value.axis = pointer_axis; value.frame = [](void*, wl_pointer*) {}; return value; }();
void keyboard_keymap(void*, wl_keyboard*, std::uint32_t, int fd, std::uint32_t) { close(fd); }
void keyboard_enter(void* data, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*) { ++static_cast<Client*>(data)->keyboard_enters; }
void keyboard_leave(void*, wl_keyboard*, std::uint32_t, wl_surface*) {}
void keyboard_key(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void keyboard_modifiers(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void keyboard_repeat(void*, wl_keyboard*, std::int32_t, std::int32_t) {}
const wl_keyboard_listener kKeyboardListener{keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat};
void seat_caps(void* data, wl_seat* seat, std::uint32_t caps) { auto& c = *static_cast<Client*>(data); if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && c.pointer == nullptr) { c.pointer = wl_seat_get_pointer(seat); wl_pointer_add_listener(c.pointer, &kPointerListener, &c); } if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && c.keyboard == nullptr) { c.keyboard = wl_seat_get_keyboard(seat); wl_keyboard_add_listener(c.keyboard, &kKeyboardListener, &c); } }
void seat_name(void*, wl_seat*, const char*) {}
void pump(wl_display* server, Client& client) { for (int i = 0; i != 8; ++i) { wl_display_flush(client.display); wl_event_loop_dispatch(wl_display_get_event_loop(server), 0); wl_display_flush_clients(server); pollfd fd{wl_display_get_fd(client.display), POLLIN, 0}; if (poll(&fd, 1, 10) > 0) assert(wl_display_dispatch(client.display) >= 0); } }
wl_buffer* buffer(Client& c, int width, int height) { const int size = width * height * 4; const int fd = static_cast<int>(syscall(SYS_memfd_create, "popup-test", 1)); assert(fd >= 0 && ftruncate(fd, size) == 0); auto* pixels = static_cast<std::uint32_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)); assert(pixels != MAP_FAILED); for (int i = 0; i < width * height; ++i) pixels[i] = 0xff44aaeeU; munmap(pixels, size); auto* pool = wl_shm_create_pool(c.shm, fd, size); close(fd); auto* result = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888); wl_shm_pool_destroy(pool); return result; }
struct View { std::uint64_t parent = 0, subsurface = 0; std::vector<std::uint64_t> popups; int popup_x = -1, popup_y = -1, geometry_x = 0, geometry_y = 0, geometry_width = 0, geometry_height = 0; };
void observed(void* data, const zwwm::ShmBufferView& view) { auto& result = *static_cast<View*>(data); if (view.toplevel) { result.parent = view.surface_id; result.geometry_x = view.window_geometry_x; result.geometry_y = view.window_geometry_y; result.geometry_width = view.window_geometry_width; result.geometry_height = view.window_geometry_height; } else if (view.popup && view.pixels != nullptr && std::find(result.popups.begin(), result.popups.end(), view.surface_id) == result.popups.end()) { result.popups.push_back(view.surface_id); result.popup_x = view.x; result.popup_y = view.y; } else if (!view.popup && view.pixels != nullptr) result.subsurface = view.surface_id; }
}

int main() {
  char runtime[] = "/tmp/zwwm-popup-test-XXXXXX"; assert(mkdtemp(runtime) != nullptr); assert(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
  wl_display* server = wl_display_create(); assert(server != nullptr); const char* socket = wl_display_add_socket_auto(server); assert(socket != nullptr);
  { auto config = std::make_shared<zwwm::RuntimeConfig>(); zwwm::CompositorServer compositor(server, config); compositor.set_output_size(960, 540); View views; compositor.set_surface_commit_observer(observed, &views);
  Client c; c.display = wl_display_connect(socket); assert(c.display != nullptr); c.registry = wl_display_get_registry(c.display); static const wl_registry_listener registry_listener{globals, global_remove}; wl_registry_add_listener(c.registry, &registry_listener, &c); pump(server, c); assert(c.compositor && c.shm && c.subcompositor && c.seat && c.wm);
  static const xdg_wm_base_listener wm_listener{ping}; xdg_wm_base_add_listener(c.wm, &wm_listener, &c); static const wl_seat_listener seat_listener{seat_caps, seat_name}; wl_seat_add_listener(c.seat, &seat_listener, &c); if (c.pointer == nullptr) { c.pointer = wl_seat_get_pointer(c.seat); wl_pointer_add_listener(c.pointer, &kPointerListener, &c); } if (c.keyboard == nullptr) { c.keyboard = wl_seat_get_keyboard(c.seat); wl_keyboard_add_listener(c.keyboard, &kKeyboardListener, &c); }
  auto* parent_surface = wl_compositor_create_surface(c.compositor); auto* parent_xdg = xdg_wm_base_get_xdg_surface(c.wm, parent_surface); static const xdg_surface_listener surface_listener{configured}; xdg_surface_add_listener(parent_xdg, &surface_listener, &c); auto* top = xdg_surface_get_toplevel(parent_xdg); static const xdg_toplevel_listener top_listener = [] { xdg_toplevel_listener value{}; value.configure = top_configure; value.close = top_close; return value; }(); xdg_toplevel_add_listener(top, &top_listener, &c); xdg_surface_set_window_geometry(parent_xdg, 5, 7, 928, 508); wl_surface_commit(parent_surface); pump(server, c); assert(c.serial != 0);
   auto* parent_buffer = buffer(c, 928, 508); wl_surface_attach(parent_surface, parent_buffer, 0, 0); wl_surface_damage_buffer(parent_surface, 0, 0, 928, 508); wl_surface_commit(parent_surface); pump(server, c); assert(views.parent != 0); assert(c.keyboard_enters == 1); assert(views.geometry_x == 5 && views.geometry_y == 7 && views.geometry_width == 928 && views.geometry_height == 508);
  auto* subsurface_surface = wl_compositor_create_surface(c.compositor); auto* child_role = wl_subcompositor_get_subsurface(c.subcompositor, subsurface_surface, parent_surface); wl_subsurface_set_position(child_role, 20, 20); auto* child_buffer = buffer(c, 20, 20); wl_surface_attach(subsurface_surface, child_buffer, 0, 0); wl_surface_damage_buffer(subsurface_surface, 0, 0, 20, 20); wl_surface_commit(subsurface_surface); wl_surface_commit(parent_surface); pump(server, c); assert(views.subsurface != 0);
  const auto placement = config->placements({views.parent}, {{0, 0}, {960, 540}}).front(); const auto content = config->content_bounds(placement.bounds); const int child_global_x = content.origin.x + 25 * static_cast<int>(content.size.width) / 923, child_global_y = content.origin.y + 23 * static_cast<int>(content.size.height) / 501; compositor.pointer_motion_global(1, child_global_x, child_global_y); pump(server, c); assert(c.pointer_target == subsurface_surface && c.pointer_x >= 9 && c.pointer_x <= 11);
  compositor.pointer_button(2, 272, WL_POINTER_BUTTON_STATE_PRESSED); compositor.pointer_motion_global(3, content.origin.x + 100 * static_cast<int>(content.size.width) / 923, child_global_y); pump(server, c); assert(c.pointer_target == subsurface_surface && c.pointer_x > 20); compositor.pointer_button(4, 272, WL_POINTER_BUTTON_STATE_RELEASED);
  auto* empty_input = wl_compositor_create_region(c.compositor); wl_surface_set_input_region(subsurface_surface, empty_input); wl_surface_commit(subsurface_surface); pump(server, c); compositor.pointer_motion_global(5, child_global_x, child_global_y); pump(server, c); assert(c.pointer_target == parent_surface);
  wl_region_add(empty_input, 0, 0, 20, 20); wl_surface_set_input_region(subsurface_surface, empty_input); wl_surface_commit(subsurface_surface); pump(server, c); compositor.pointer_motion_global(6, child_global_x, child_global_y); pump(server, c); assert(c.pointer_target == subsurface_surface);
  wl_subsurface_place_below(child_role, parent_surface); pump(server, c); compositor.pointer_motion_global(7, child_global_x, child_global_y); pump(server, c); assert(c.pointer_target == parent_surface); wl_subsurface_place_above(child_role, parent_surface); pump(server, c); compositor.pointer_motion_global(8, child_global_x, child_global_y); pump(server, c); assert(c.pointer_target == subsurface_surface); wl_region_destroy(empty_input); wl_subsurface_destroy(child_role); wl_surface_destroy(subsurface_surface); pump(server, c);
  auto make_popup = [&](xdg_surface* parent, int size, int parent_size, int offset_x = 0, bool grab_before_commit = false) { auto* surface = wl_compositor_create_surface(c.compositor); auto* xdg = xdg_wm_base_get_xdg_surface(c.wm, surface); xdg_surface_add_listener(xdg, &surface_listener, &c); auto* positioner = xdg_wm_base_create_positioner(c.wm); xdg_positioner_set_size(positioner, size, size); xdg_positioner_set_anchor_rect(positioner, parent_size - 10, 10, 10, 10); xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT); xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_TOP_LEFT); xdg_positioner_set_constraint_adjustment(positioner, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X); xdg_positioner_set_offset(positioner, offset_x, 0); xdg_positioner_set_reactive(positioner); xdg_positioner_set_parent_size(positioner, parent_size, parent_size); auto* popup = xdg_surface_get_popup(xdg, parent, positioner); xdg_positioner_destroy(positioner); static const xdg_popup_listener popup_listener{popup_configure, popup_done, popup_repositioned}; xdg_popup_add_listener(popup, &popup_listener, &c); if (grab_before_commit) xdg_popup_grab(popup, c.seat, c.button_serial); wl_surface_commit(surface); pump(server, c); assert(c.popup_w == size && c.popup_h == size); auto* popup_buffer = buffer(c, size + 2, size + 2); wl_surface_attach(surface, popup_buffer, 0, 0); wl_surface_damage_buffer(surface, 0, 0, size + 2, size + 2); wl_surface_commit(surface); pump(server, c); return std::tuple{surface, xdg, popup}; };
  [[maybe_unused]] auto [popup_surface, popup_xdg, popup] = make_popup(parent_xdg, 30, 100); assert(views.popups.size() == 1 && views.popup_x == 75 && views.popup_y == -3);
  compositor.pointer_motion(1, views.parent, 1, 1); compositor.pointer_button(2, 1, WL_POINTER_BUTTON_STATE_PRESSED); pump(server, c); assert(c.button_serial != 0); xdg_popup_grab(popup, c.seat, c.button_serial); pump(server, c);
  [[maybe_unused]] auto [child_surface, child_xdg, child_popup] = make_popup(popup_xdg, 10, 30); assert(views.popups.size() == 2 && views.popup_x == 95 && views.popup_y == 7); xdg_popup_grab(child_popup, c.seat, c.button_serial); compositor.pointer_motion(3, views.popups[1], 1, 1); pump(server, c); assert(c.keyboard_enters >= 2);
  compositor.pointer_motion(4, views.parent, 1, 1); compositor.pointer_button(5, 1, WL_POINTER_BUTTON_STATE_PRESSED); pump(server, c); assert(c.popup_done == 2);
  xdg_popup_destroy(child_popup); xdg_popup_destroy(popup); pump(server, c);
  [[maybe_unused]] const int enters_before_precommit_grab = c.keyboard_enters; [[maybe_unused]] auto [pregrab_surface, pregrab_xdg, pregrab_popup] = make_popup(parent_xdg, 30, 100, 0, true); assert(c.keyboard_enters == enters_before_precommit_grab + 1); xdg_popup_destroy(pregrab_popup); pump(server, c);
  [[maybe_unused]] auto [baseline_surface, baseline_xdg, baseline_popup] = make_popup(parent_xdg, 100, 910); [[maybe_unused]] const int baseline_popup_x = c.popup_x; xdg_popup_destroy(baseline_popup); pump(server, c);
  [[maybe_unused]] auto [offset_surface, offset_xdg, offset_popup] = make_popup(parent_xdg, 100, 910, 20); assert(c.popup_x == baseline_popup_x + 20); xdg_popup_destroy(offset_popup); pump(server, c);
  [[maybe_unused]] auto [teardown_surface, teardown_xdg, teardown_popup] = make_popup(parent_xdg, 30, 100); [[maybe_unused]] auto [teardown_child_surface, teardown_child_xdg, teardown_child] = make_popup(teardown_xdg, 10, 30); wl_surface_attach(parent_surface, nullptr, 0, 0); wl_surface_commit(parent_surface); pump(server, c); std::fprintf(stderr, "popup-test: dismissal-count=%d\n", c.popup_done); assert(c.popup_done == 4);
  std::printf("popup-test: positioner=%d,%d %dx%d nested_maps=%zu keyboard_enters=%d outside_done=2 parent_teardown_done=2\n", c.popup_x, c.popup_y, c.popup_w, c.popup_h, views.popups.size(), c.keyboard_enters);
  xdg_popup_destroy(teardown_child); xdg_popup_destroy(teardown_popup); wl_display_disconnect(c.display); }
  wl_display_destroy(server); rmdir(runtime);
}
