#include "zwwm/compositor_server.hpp"
#include "zwwm/runtime_config.hpp"

#include <pointer-constraints-client-protocol.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {
struct Client {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  wl_shm* shm = nullptr;
  wl_seat* seat = nullptr;
  wl_pointer* pointer = nullptr;
  zwp_pointer_constraints_v1* constraints = nullptr;
  int enters = 0, leaves = 0, motions = 0;
  int x = -1, y = -1;
  int confined = 0, unconfined = 0, locked = 0, unlocked = 0;
};

void pointer_enter(void* data, wl_pointer*, std::uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
  auto& client = *static_cast<Client*>(data);
  ++client.enters;
  client.x = wl_fixed_to_int(x);
  client.y = wl_fixed_to_int(y);
}
void pointer_leave(void* data, wl_pointer*, std::uint32_t, wl_surface*) { ++static_cast<Client*>(data)->leaves; }
void pointer_motion(void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
  auto& client = *static_cast<Client*>(data);
  ++client.motions;
  client.x = wl_fixed_to_int(x);
  client.y = wl_fixed_to_int(y);
}
const wl_pointer_listener kPointerListener = [] {
  wl_pointer_listener listener{};
  listener.enter = pointer_enter;
  listener.leave = pointer_leave;
  listener.motion = pointer_motion;
  listener.button = [](void*, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {};
  listener.axis = [](void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {};
  listener.frame = [](void*, wl_pointer*) {};
  return listener;
}();
void seat_capabilities(void* data, wl_seat* seat, std::uint32_t capabilities) {
  auto& client = *static_cast<Client*>(data);
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && client.pointer == nullptr) {
    client.pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(client.pointer, &kPointerListener, &client);
  }
}
void registry_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
  auto& client = *static_cast<Client*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0) client.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U)));
  if (std::strcmp(interface, wl_shm_interface.name) == 0) client.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
  if (std::strcmp(interface, wl_seat_interface.name) == 0) client.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
  if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) client.constraints = static_cast<zwp_pointer_constraints_v1*>(wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
}
void pump(wl_display* server, Client& client) {
  for (int i = 0; i != 8; ++i) {
    assert(wl_display_flush(client.display) >= 0);
    wl_event_loop_dispatch(wl_display_get_event_loop(server), 0);
    wl_display_flush_clients(server);
    pollfd fd{wl_display_get_fd(client.display), POLLIN, 0};
    if (poll(&fd, 1, 5) > 0) assert(wl_display_dispatch(client.display) >= 0);
  }
}
wl_buffer* make_buffer(Client& client, int width, int height) {
  const int size = width * height * 4;
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "pointer-constraints-test", 1));
  assert(fd >= 0 && ftruncate(fd, size) == 0);
  void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  assert(pixels != MAP_FAILED);
  std::memset(pixels, 0, static_cast<std::size_t>(size));
  munmap(pixels, size);
  auto* pool = wl_shm_create_pool(client.shm, fd, size);
  close(fd);
  auto* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  return buffer;
}
void observed(void* data, const zwwm::ShmBufferView& view) {
  if (view.pixels != nullptr) static_cast<std::vector<std::uint64_t>*>(data)->push_back(view.surface_id);
}
}

int main() {
  char runtime[] = "/tmp/zwwm-pointer-constraints-test-XXXXXX";
  assert(mkdtemp(runtime) != nullptr && setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
  auto* server = wl_display_create();
  assert(server != nullptr);
  const char* socket = wl_display_add_socket_auto(server);
  assert(socket != nullptr);
  {
    zwwm::CompositorServer compositor(server, std::make_shared<zwwm::RuntimeConfig>());
    std::vector<std::uint64_t> surface_ids;
    compositor.set_surface_commit_observer(observed, &surface_ids);
    Client client;
    client.display = wl_display_connect(socket);
    assert(client.display != nullptr);
    client.registry = wl_display_get_registry(client.display);
    static const wl_registry_listener registry_listener{registry_global, [](void*, wl_registry*, std::uint32_t) {}};
    wl_registry_add_listener(client.registry, &registry_listener, &client);
    pump(server, client);
    assert(client.compositor != nullptr && client.shm != nullptr && client.seat != nullptr && client.constraints != nullptr);
    static const wl_seat_listener seat_listener{seat_capabilities, [](void*, wl_seat*, const char*) {}};
    wl_seat_add_listener(client.seat, &seat_listener, &client);
    if (client.pointer == nullptr) {
      client.pointer = wl_seat_get_pointer(client.seat);
      wl_pointer_add_listener(client.pointer, &kPointerListener, &client);
    }
    pump(server, client);
    assert(client.pointer != nullptr);

    auto* first = wl_compositor_create_surface(client.compositor);
    auto* second = wl_compositor_create_surface(client.compositor);
    auto* first_buffer = make_buffer(client, 100, 100);
    auto* second_buffer = make_buffer(client, 100, 100);
    wl_surface_attach(first, first_buffer, 0, 0);
    wl_surface_commit(first);
    wl_surface_attach(second, second_buffer, 0, 0);
    wl_surface_commit(second);
    pump(server, client);
    assert(surface_ids.size() == 2);

    auto* region = wl_compositor_create_region(client.compositor);
    wl_region_add(region, 10, 10, 20, 20);
    auto* confined = zwp_pointer_constraints_v1_confine_pointer(client.constraints, first, client.pointer, region,
                                                                 ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    static const zwp_confined_pointer_v1_listener confined_listener{
        [](void* data, zwp_confined_pointer_v1*) { ++static_cast<Client*>(data)->confined; },
        [](void* data, zwp_confined_pointer_v1*) { ++static_cast<Client*>(data)->unconfined; }};
    zwp_confined_pointer_v1_add_listener(confined, &confined_listener, &client);
    compositor.pointer_motion(1, surface_ids[0], 15, 15);
    pump(server, client);
    assert(client.confined == 1 && client.x == 15 && client.y == 15);
    compositor.pointer_motion(2, surface_ids[0], 80, 90);
    pump(server, client);
    assert(client.x == 29 && client.y == 29);
    const int leaves = client.leaves;
    compositor.pointer_motion(3, surface_ids[1], 5, 5);
    pump(server, client);
    assert(client.leaves == leaves && client.x == 29 && client.y == 29);
    compositor.pointer_leave();
    pump(server, client);
    assert(client.unconfined == 1);
    compositor.pointer_motion(4, surface_ids[0], 12, 13);
    pump(server, client);
    assert(client.confined == 2);
    auto* replacement_region = wl_compositor_create_region(client.compositor);
    wl_region_add(replacement_region, 40, 40, 10, 10);
    zwp_confined_pointer_v1_set_region(confined, replacement_region);
    compositor.pointer_motion(5, surface_ids[0], 15, 16);
    pump(server, client);
    assert(client.x == 15 && client.y == 16);
    wl_surface_commit(first);
    pump(server, client);
    assert(client.x == 40 && client.y == 40);
    zwp_confined_pointer_v1_destroy(confined);
    wl_region_destroy(replacement_region);
    pump(server, client);

    auto* locked = zwp_pointer_constraints_v1_lock_pointer(client.constraints, first, client.pointer, nullptr,
                                                            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    static const zwp_locked_pointer_v1_listener locked_listener{
        [](void* data, zwp_locked_pointer_v1*) { ++static_cast<Client*>(data)->locked; },
        [](void* data, zwp_locked_pointer_v1*) { ++static_cast<Client*>(data)->unlocked; }};
    zwp_locked_pointer_v1_add_listener(locked, &locked_listener, &client);
    pump(server, client);
    assert(client.locked == 1);
    const int motions = client.motions;
    zwp_locked_pointer_v1_set_cursor_position_hint(locked, wl_fixed_from_int(7), wl_fixed_from_int(8));
    wl_surface_commit(first);
    compositor.pointer_motion(6, surface_ids[1], 70, 70);
    pump(server, client);
    assert(client.motions == motions && client.leaves == leaves + 1);
    compositor.pointer_leave();
    pump(server, client);
    assert(client.unlocked == 1);
    compositor.pointer_motion(7, surface_ids[0], 20, 21);
    pump(server, client);
    assert(client.locked == 1 && client.motions == motions + 1 && client.x == 20 && client.y == 21);
    zwp_locked_pointer_v1_destroy(locked);
    pump(server, client);

    auto* duplicate_region = wl_compositor_create_region(client.compositor);
    wl_region_add(duplicate_region, 0, 0, 50, 50);
    auto* duplicate_confined = zwp_pointer_constraints_v1_confine_pointer(client.constraints, first, client.pointer,
                                                                           duplicate_region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    assert(duplicate_confined != nullptr);
    auto* duplicate_locked = zwp_pointer_constraints_v1_lock_pointer(client.constraints, first, client.pointer, nullptr,
                                                                      ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    assert(duplicate_locked != nullptr);
    assert(wl_display_flush(client.display) >= 0);
    wl_event_loop_dispatch(wl_display_get_event_loop(server), 0);
    wl_display_flush_clients(server);
    pollfd error_fd{wl_display_get_fd(client.display), POLLIN, 0};
    assert(poll(&error_fd, 1, 100) > 0);
    assert(wl_display_dispatch(client.display) == -1);
    const wl_interface* error_interface = nullptr;
    std::uint32_t error_id = 0;
    assert(wl_display_get_protocol_error(client.display, &error_interface, &error_id) ==
           ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED);
    assert(error_interface == &zwp_pointer_constraints_v1_interface && error_id != 0);
    wl_display_disconnect(client.display);
    wl_event_loop_dispatch(wl_display_get_event_loop(server), 0);
  }
  wl_display_destroy(server);
  rmdir(runtime);
}
