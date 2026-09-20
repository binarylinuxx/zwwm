#include "zwwm/compositor_server.hpp"
#include "zwwm/runtime_config.hpp"

#include <fractional-scale-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
struct Client {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  wl_shm* shm = nullptr;
  wp_viewporter* viewporter = nullptr;
  wp_fractional_scale_manager_v1* fractional_manager = nullptr;
  std::uint32_t preferred_scale = 0;
  wl_output* output = nullptr;
  std::int32_t output_width = 0, output_height = 0, output_scale = 0, output_transform = -1;
};
void output_geometry(void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                     std::int32_t, const char*, const char*, std::int32_t);
void output_mode(void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t);
void output_done(void*, wl_output*);
void output_scale(void*, wl_output*, std::int32_t);

void global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
            std::uint32_t version) {
  auto& client = *static_cast<Client*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0)
    client.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U)));
  if (std::strcmp(interface, wl_shm_interface.name) == 0)
    client.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
  if (std::strcmp(interface, wp_viewporter_interface.name) == 0)
    client.viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
  if (std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
    client.fractional_manager = static_cast<wp_fractional_scale_manager_v1*>(
        wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
  if (std::strcmp(interface, wl_output_interface.name) == 0) {
    client.output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 3U)));
    static const wl_output_listener listener{output_geometry, output_mode, output_done, output_scale, nullptr, nullptr};
    wl_output_add_listener(client.output, &listener, &client);
  }
}
void global_remove(void*, wl_registry*, std::uint32_t) {}

bool pump(wl_display* server, Client& client) {
  for (int i = 0; i != 8; ++i) {
    if (wl_display_flush(client.display) < 0) return false;
    wl_event_loop_dispatch(wl_display_get_event_loop(server), 0);
    wl_display_flush_clients(server);
    pollfd fd{wl_display_get_fd(client.display), POLLIN, 0};
    if (poll(&fd, 1, 10) > 0 && wl_display_dispatch(client.display) < 0) return false;
  }
  return true;
}

Client connect_client(wl_display* server, const char* socket) {
  Client client;
  client.display = wl_display_connect(socket);
  assert(client.display != nullptr);
  client.registry = wl_display_get_registry(client.display);
  static const wl_registry_listener listener{global, global_remove};
  wl_registry_add_listener(client.registry, &listener, &client);
  assert(pump(server, client));
  assert(client.compositor && client.shm && client.viewporter && client.fractional_manager);
  return client;
}

wl_buffer* make_buffer(Client& client, int width, int height, std::uint32_t format = WL_SHM_FORMAT_XRGB8888) {
  const int size = width * height * 4;
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "viewport-test", 1));
  assert(fd >= 0 && ftruncate(fd, size) == 0);
  auto* pixels = static_cast<std::uint32_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  assert(pixels != MAP_FAILED);
  std::fill_n(pixels, width * height, 0xff336699U);
  munmap(pixels, size);
  auto* pool = wl_shm_create_pool(client.shm, fd, size);
  close(fd);
  auto* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, format);
  wl_shm_pool_destroy(pool);
  return buffer;
}

struct Observation { zwwm::ShmBufferView view; int commits = 0; };
void observed(void* data, const zwwm::ShmBufferView& view) {
  if (view.pixels == nullptr) return;
  auto& observation = *static_cast<Observation*>(data);
  observation.view = view;
  ++observation.commits;
}
void preferred(void* data, wp_fractional_scale_v1*, std::uint32_t scale) {
  static_cast<Client*>(data)->preferred_scale = scale;
}
void output_geometry(void* data, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                     std::int32_t, const char*, const char*, std::int32_t transform) {
  static_cast<Client*>(data)->output_transform = transform;
}
void output_mode(void* data, wl_output*, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t) {
  if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) { auto* client = static_cast<Client*>(data); client->output_width = width; client->output_height = height; }
}
void output_done(void*, wl_output*) {}
void output_scale(void* data, wl_output*, std::int32_t scale) { static_cast<Client*>(data)->output_scale = scale; }

void expect_protocol_error(wl_display* server, const char* socket, const auto& request,
                           const wl_interface* interface, std::uint32_t code) {
  Client client = connect_client(server, socket);
  request(client);
  assert(!pump(server, client));
  const wl_interface* actual_interface = nullptr;
  std::uint32_t actual_id = 0;
  assert(wl_display_get_protocol_error(client.display, &actual_interface, &actual_id) == code);
  assert(actual_interface == interface && actual_id != 0);
  wl_display_disconnect(client.display);
}
}  // namespace

int main() {
  char runtime[] = "/tmp/zwwm-viewport-test-XXXXXX";
  assert(mkdtemp(runtime) != nullptr && setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
  wl_display* server = wl_display_create();
  assert(server != nullptr);
  const char* socket = wl_display_add_socket_auto(server);
  assert(socket != nullptr);
  {
    auto config = std::make_shared<zwwm::RuntimeConfig>();
    config->output.scale_per_mille = 1250;
    config->output.transform = zwwm::OutputTransform::rotate_90;
    zwwm::CompositorServer compositor(server, config);
    Observation observation;
    compositor.set_surface_commit_observer(observed, &observation);

    Client client = connect_client(server, socket);
    auto* surface = wl_compositor_create_surface(client.compositor);
    auto* viewport = wp_viewporter_get_viewport(client.viewporter, surface);
    auto* fractional = wp_fractional_scale_manager_v1_get_fractional_scale(client.fractional_manager, surface);
    static const wp_fractional_scale_v1_listener scale_listener{preferred};
    wp_fractional_scale_v1_add_listener(fractional, &scale_listener, &client);
    assert(pump(server, client) && client.preferred_scale == 150);
    assert(client.output_width == 960 && client.output_height == 540);
    assert(client.output_scale == 2 && client.output_transform == WL_OUTPUT_TRANSFORM_90);
    auto reloaded = std::make_shared<zwwm::RuntimeConfig>(*config);
    reloaded->output.scale_per_mille = 1500;
    reloaded->output.transform = zwwm::OutputTransform::rotate_180;
    compositor.set_config(reloaded);
    assert(pump(server, client) && client.preferred_scale == 180);
    assert(client.output_scale == 2 && client.output_transform == WL_OUTPUT_TRANSFORM_180);

    auto* buffer = make_buffer(client, 200, 100);
    wl_surface_set_buffer_scale(surface, 2);
    wp_viewport_set_source(viewport, wl_fixed_from_int(10), wl_fixed_from_int(5), wl_fixed_from_int(40), wl_fixed_from_int(20));
    wp_viewport_set_destination(viewport, 80, 30);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    assert(pump(server, client));
    assert(observation.commits == 1 && observation.view.width == 200 && observation.view.height == 100);
    assert(observation.view.damage_x == 0 && observation.view.damage_y == 0 && observation.view.damage_width == 200 && observation.view.damage_height == 100);
    assert(observation.view.opaque);
    assert(observation.view.logical_width == 80 && observation.view.logical_height == 30);
    assert(std::abs(observation.view.source_left - 0.1F) < 0.0001F);
    assert(std::abs(observation.view.source_top - 0.1F) < 0.0001F);
    assert(std::abs(observation.view.source_right - 0.5F) < 0.0001F);
    assert(std::abs(observation.view.source_bottom - 0.5F) < 0.0001F);
    wl_surface_damage(surface, 2, 3, 4, 5);
    wl_surface_commit(surface);
    assert(pump(server, client) && observation.commits == 2);
    assert(observation.view.damage_x == 22 && observation.view.damage_y == 14 && observation.view.damage_width == 4 && observation.view.damage_height == 7);
    wl_surface_commit(surface);
    assert(pump(server, client) && observation.commits == 3 && observation.view.damage_width == 0 && observation.view.damage_height == 0);
    wl_surface_damage_buffer(surface, 30, 20, 7, 9);
    wl_surface_commit(surface);
    assert(pump(server, client) && observation.commits == 4);
    assert(observation.view.damage_x == 30 && observation.view.damage_y == 20 && observation.view.damage_width == 7 && observation.view.damage_height == 9);
    auto* opaque_region = wl_compositor_create_region(client.compositor);
    wl_region_add(opaque_region, 0, 0, 80, 30);
    wl_surface_set_opaque_region(surface, opaque_region);
    wl_surface_attach(surface, make_buffer(client, 200, 100, WL_SHM_FORMAT_ARGB8888), 0, 0);
    wl_surface_commit(surface);
    assert(pump(server, client) && observation.commits == 5 && observation.view.opaque);
    wl_region_destroy(opaque_region);
    wp_viewport_set_destination(viewport, 70, 25);
    assert(pump(server, client) && observation.commits == 5 && observation.view.logical_width == 80);
    wp_viewport_set_destination(viewport, 80, 30);

    wp_viewport_destroy(viewport);
    wl_surface_commit(surface);
    assert(pump(server, client));
    assert(observation.view.logical_width == 100 && observation.view.logical_height == 50);
    auto* replacement = wp_viewporter_get_viewport(client.viewporter, surface);
    wp_fractional_scale_v1_destroy(fractional);
    auto* replacement_fractional = wp_fractional_scale_manager_v1_get_fractional_scale(client.fractional_manager, surface);
    wp_fractional_scale_v1_add_listener(replacement_fractional, &scale_listener, &client);
    client.preferred_scale = 0;
    assert(pump(server, client) && client.preferred_scale == 180);
    wp_viewport_destroy(replacement);
    wp_fractional_scale_v1_destroy(replacement_fractional);
    wl_surface_destroy(surface);
    wl_display_disconnect(client.display);

    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      wp_viewporter_get_viewport(c.viewporter, s);
      wp_viewporter_get_viewport(c.viewporter, s);
    }, &wp_viewporter_interface, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS);
    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional_manager, s);
      wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional_manager, s);
    }, &wp_fractional_scale_manager_v1_interface,
       WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS);
    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      auto* v = wp_viewporter_get_viewport(c.viewporter, s);
      wp_viewport_set_destination(v, 0, 10);
    }, &wp_viewport_interface, WP_VIEWPORT_ERROR_BAD_VALUE);
    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      auto* v = wp_viewporter_get_viewport(c.viewporter, s);
      wp_viewport_set_source(v, 0, 0, wl_fixed_from_double(2.5), wl_fixed_from_int(2));
      wl_surface_attach(s, make_buffer(c, 10, 10), 0, 0);
      wl_surface_commit(s);
    }, &wp_viewport_interface, WP_VIEWPORT_ERROR_BAD_SIZE);
    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      auto* v = wp_viewporter_get_viewport(c.viewporter, s);
      wp_viewport_set_source(v, wl_fixed_from_int(9), 0, wl_fixed_from_int(2), wl_fixed_from_int(2));
      wl_surface_attach(s, make_buffer(c, 10, 10), 0, 0);
      wl_surface_commit(s);
    }, &wp_viewport_interface, WP_VIEWPORT_ERROR_OUT_OF_BUFFER);
    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      auto* v = wp_viewporter_get_viewport(c.viewporter, s);
      wl_surface_destroy(s);
      wp_viewport_set_destination(v, 10, 10);
    }, &wp_viewport_interface, WP_VIEWPORT_ERROR_NO_SURFACE);
    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      wl_surface_set_buffer_scale(s, 0);
    }, &wl_surface_interface, WL_SURFACE_ERROR_INVALID_SCALE);
    expect_protocol_error(server, socket, [](Client& c) {
      auto* s = wl_compositor_create_surface(c.compositor);
      wl_surface_set_buffer_scale(s, 2);
      wl_surface_attach(s, make_buffer(c, 9, 10), 0, 0);
      wl_surface_commit(s);
    }, &wl_surface_interface, WL_SURFACE_ERROR_INVALID_SIZE);
  }
  wl_display_destroy(server);
  rmdir(runtime);
}
