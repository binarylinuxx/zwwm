#include "compositor_server_internal.hpp"

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <fractional-scale-zwayland-server.h>
#include <viewporter-zwayland-server.h>

#include <algorithm>
#include <cstdint>
#include <new>

namespace protocol = zwayland::generated;

namespace zwwm::detail {
namespace {

void viewport_destroyed(zwayland::server::Resource* resource) {
  auto* viewport = resource->data<ViewportState>();
  if (viewport == nullptr) return;
  if (*viewport->surface_alive && viewport->surface->viewport == viewport) {
    viewport->surface->viewport = nullptr;
    viewport->surface->pending_viewport = {};
    viewport->surface->viewport_changed = true;
  }
  delete viewport;
}

void viewport_source(zwayland::server::Client*, zwayland::server::Resource* resource, double x, double y,
                     double width, double height) {
  auto* viewport = resource->data<ViewportState>();
  if (!*viewport->surface_alive) { resource->post_error(protocol::WP_VIEWPORT_ERROR_NO_SURFACE, "wl_surface no longer exists"); return; }
  constexpr double unset = -1.0;
  if (x == unset && y == unset && width == unset && height == unset) {
    viewport->surface->pending_viewport.has_source = false;
    viewport->surface->viewport_changed = true;
    return;
  }
  if (x < 0 || y < 0 || width <= 0 || height <= 0) {
    resource->post_error(protocol::WP_VIEWPORT_ERROR_BAD_VALUE, "invalid viewport source rectangle");
    return;
  }
  auto& state = viewport->surface->pending_viewport;
  state.x = x; state.y = y; state.width = width; state.height = height; state.has_source = true;
  viewport->surface->viewport_changed = true;
}

void viewport_destination(zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t width, std::int32_t height) {
  auto* viewport = resource->data<ViewportState>();
  if (!*viewport->surface_alive) { resource->post_error(protocol::WP_VIEWPORT_ERROR_NO_SURFACE, "wl_surface no longer exists"); return; }
  if (width == -1 && height == -1) {
    viewport->surface->pending_viewport.has_destination = false;
    viewport->surface->viewport_changed = true;
    return;
  }
  if (width <= 0 || height <= 0) {
    resource->post_error(protocol::WP_VIEWPORT_ERROR_BAD_VALUE, "viewport destination must be positive");
    return;
  }
  auto& state = viewport->surface->pending_viewport;
  state.destination_width = width; state.destination_height = height; state.has_destination = true;
  viewport->surface->viewport_changed = true;
}

struct WpViewportKViewportHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_source(zwayland::server::Client& client, zwayland::server::Resource& resource, double x, double y, double width, double height) {
    (viewport_source)(&client, &resource, x, y, width, height);
  }
  void set_destination(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t width, std::int32_t height) {
    (viewport_destination)(&client, &resource, width, height);
  }
};

void get_viewport(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* surface_resource) {
  auto* surface = compositor_surface_from_resource(client, surface_resource);
  if (surface == nullptr) {
    manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "not a zwwm wl_surface for this client");
    return;
  }
  if (surface->viewport != nullptr) {
    manager->post_error(protocol::WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, "surface already has a viewport");
    return;
  }
  auto* resource = client->create_resource(&protocol::wp_viewport_interface, id, 1);
  auto* viewport = resource == nullptr ? nullptr : new (std::nothrow) ViewportState{resource, surface, surface->alive};
  if (resource == nullptr || viewport == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; }
  surface->viewport = viewport;
  resource->set_data(viewport); resource->set_handler(protocol::wp_viewport_handler(WpViewportKViewportHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (viewport_destroyed)(&destroyed); });
}

struct WpViewporterKViewporterHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void get_viewport(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface) {
    (::zwwm::detail::get_viewport)(&client, &resource, id, surface);
  }
};

void fractional_scale_destroyed(zwayland::server::Resource* resource) {
  auto* fractional = resource->data<FractionalScaleState>();
  if (fractional == nullptr) return;
  if (*fractional->surface_alive && fractional->surface->fractional_scale == fractional) fractional->surface->fractional_scale = nullptr;
  delete fractional;
}

struct WpFractionalScaleV1KFractionalScaleHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

void get_fractional_scale(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id,
                          zwayland::server::Resource* surface_resource) {
  auto* surface = compositor_surface_from_resource(client, surface_resource);
  if (surface == nullptr) {
    manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "not a zwwm wl_surface for this client");
    return;
  }
  if (surface->fractional_scale != nullptr) {
    manager->post_error(protocol::WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                           "surface already has a fractional scale object");
    return;
  }
  auto* resource = client->create_resource(&protocol::wp_fractional_scale_v1_interface, id, 1);
  auto* fractional = resource == nullptr ? nullptr : new (std::nothrow) FractionalScaleState{resource, surface, surface->alive};
  if (resource == nullptr || fractional == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; }
  surface->fractional_scale = fractional;
  resource->set_data(fractional); resource->set_handler(protocol::wp_fractional_scale_v1_handler(WpFractionalScaleV1KFractionalScaleHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (fractional_scale_destroyed)(&destroyed); });
  const auto* observer = surface->observer;
  protocol::wp_fractional_scale_v1_send_preferred_scale(*resource, observer == nullptr || observer->config == nullptr
                                                            ? 120 : observer->config->output.fractional_scale_120());
}

struct WpFractionalScaleManagerV1KFractionalScaleManagerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void get_fractional_scale(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface) {
    (::zwwm::detail::get_fractional_scale)(&client, &resource, id, surface);
  }
};

}  // namespace

void bind_viewporter(zwayland::server::Client* client, void*, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::wp_viewporter_interface, id, std::min(version, 1U));
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_handler(protocol::wp_viewporter_handler(WpViewporterKViewporterHandler{}));
}

void bind_fractional_scale_manager(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::wp_fractional_scale_manager_v1_interface, id, std::min(version, 1U));
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<Observer*>(data)); resource->set_handler(protocol::wp_fractional_scale_manager_v1_handler(WpFractionalScaleManagerV1KFractionalScaleManagerHandler{}));
}

}  // namespace zwwm::detail
