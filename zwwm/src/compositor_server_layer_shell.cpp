#include "compositor_server_internal.hpp"

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <zwwm-layer-shell-v1-zwayland-server.h>
#include <xwlr-layer-shell-v1-zwayland-server.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>

namespace protocol = zwayland::generated;

namespace zwwm::detail {
namespace {

void layer_destroyed(zwayland::server::Resource* resource) { auto* layer = resource->data<LayerSurfaceState>(); if (layer == nullptr) return; if (layer->xwlr != nullptr) { layer->xwlr->layer = nullptr; layer->xwlr->resource->destroy(); } if (layer->surface != nullptr) { auto* observer = layer->surface->observer; const bool focused = observer != nullptr && observer->seat != nullptr && root(observer->seat->keyboard_focus) == layer->surface; if (auto* output = output_state(observer, layer->output); output != nullptr) std::erase(output->layer_roots[layer->current.layer], layer->surface); layer->surface->layer_surface = nullptr; if (focused) set_keyboard_focus(observer->seat, observer->seat->regular_focus); configure_layout(observer); refresh_layer_keyboard_focus(observer); } delete layer; }
void layer_set_size(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t width, std::uint32_t height) { auto* layer = resource->data<LayerSurfaceState>(); if (width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) || height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) { resource->post_error(protocol::ZWWM_LAYER_SURFACE_V1_ERROR_INVALID_SIZE, "invalid layer size"); return; } layer->pending.width = static_cast<int>(width); layer->pending.height = static_cast<int>(height); }
void layer_set_anchor(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t anchor) { if ((anchor & ~15U) != 0) { resource->post_error(protocol::ZWWM_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR, "invalid layer anchor"); return; } resource->data<LayerSurfaceState>()->pending.anchor = anchor; }
void layer_set_zone(zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t zone) { resource->data<LayerSurfaceState>()->pending.zone = zone; }
void layer_set_margin(zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left) { auto& state = resource->data<LayerSurfaceState>()->pending; state.top = top; state.right = right; state.bottom = bottom; state.left = left; }
void layer_set_keyboard(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t keyboard) { if (keyboard > protocol::ZWWM_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) { resource->post_error(protocol::ZWWM_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY, "invalid keyboard interactivity"); return; } resource->data<LayerSurfaceState>()->pending.keyboard = keyboard; }
void layer_ack(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t serial) { auto* layer = resource->data<LayerSurfaceState>(); const auto it = std::find(layer->serials.begin(), layer->serials.end(), serial); if (it != layer->serials.end()) { layer->serials.erase(layer->serials.begin(), std::next(it)); layer->last_acked = serial; layer->configured = true; } }
void layer_get_popup(zwayland::server::Client* client, zwayland::server::Resource* resource, zwayland::server::Resource* popup_resource) { associate_layer_popup(resource->data<LayerSurfaceState>(), client, resource, popup_resource); }
struct ZwwmLayerSurfaceV1Handler {
  void set_size(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t width, std::uint32_t height) {
    (layer_set_size)(&client, &resource, width, height);
  }
  void set_anchor(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t anchor) {
    (layer_set_anchor)(&client, &resource, anchor);
  }
  void set_exclusive_zone(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t zone) {
    (layer_set_zone)(&client, &resource, zone);
  }
  void set_margin(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left) {
    (layer_set_margin)(&client, &resource, top, right, bottom, left);
  }
  void set_keyboard_interactivity(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t keyboard_interactivity) {
    (layer_set_keyboard)(&client, &resource, keyboard_interactivity);
  }
  void get_popup(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* popup) {
    (layer_get_popup)(&client, &resource, popup);
  }
  void ack_configure(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial) {
    (layer_ack)(&client, &resource, serial);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_layer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t layer) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t value) { if (value > protocol::ZWWM_LAYER_SHELL_V1_LAYER_OVERLAY) { resource->post_error(protocol::ZWWM_LAYER_SHELL_V1_ERROR_INVALID_LAYER, "invalid layer"); return; } resource->data<LayerSurfaceState>()->pending.layer = value; })(&client, &resource, layer);
  }
};
void layer_get_surface(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* surface_resource, zwayland::server::Resource* output_resource, std::uint32_t value, const char* name_space) {
  auto* surface = surface_resource->data<SurfaceState>();
  auto* observer = manager->data<Observer>();
  OutputId output;
  if (output_resource != nullptr) {
    if (observer == nullptr || output_resource->client != client || observer->outputs == nullptr) {
      manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "output is not a live wl_output for this client"); return;
    }
    for (const auto& candidate : *observer->outputs) {
      if (!candidate->retired && std::find(candidate->resources.begin(), candidate->resources.end(), output_resource) != candidate->resources.end()) {
        output = candidate->info.id; break;
      }
    }
    if (!output) { manager->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "output is retired or foreign"); return; }
  } else if (output_state(observer, observer == nullptr ? OutputId{} : observer->active_output) != nullptr) {
    output = observer->active_output;
  } else if (observer != nullptr && observer->outputs != nullptr && !observer->outputs->empty()) {
    output = observer->outputs->front()->info.id;
  }
  // Valid for a null output to be omitted by the client; when no output is
  // known yet the surface is created without one and is pinned by set_outputs
  // once the backend publishes its first output.
  if (surface == nullptr || surface_resource->client != client || surface->xdg_surface != nullptr || surface->layer_surface != nullptr || surface->layer_role_assigned || surface->parent != nullptr || surface->drag_icon_role || surface->current_buffer != nullptr || value > protocol::ZWWM_LAYER_SHELL_V1_LAYER_OVERLAY) { manager->post_error(value > protocol::ZWWM_LAYER_SHELL_V1_LAYER_OVERLAY ? protocol::ZWWM_LAYER_SHELL_V1_ERROR_INVALID_LAYER : protocol::ZWWM_LAYER_SHELL_V1_ERROR_ROLE, "invalid layer surface"); return; } auto* resource = client->create_resource(&protocol::zwwm_layer_surface_v1_interface, id, 1); auto* layer = resource == nullptr ? nullptr : new (std::nothrow) LayerSurfaceState; if (resource == nullptr || layer == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; } static std::uint64_t creation = 1; layer->resource = resource; layer->surface = surface; layer->output = output; layer->name_space = name_space == nullptr ? "" : name_space; layer->creation = creation++; layer->pending.layer = layer->current.layer = value; surface->layer_surface = layer; surface->layer_role_assigned = true; if (auto* owner = output_state(surface->observer, output); owner != nullptr) { owner->layer_roots[value].push_back(surface); reorder_layer_roots(surface->observer, output, value); } resource->set_data(layer); resource->set_handler(protocol::zwwm_layer_surface_v1_handler(ZwwmLayerSurfaceV1Handler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (layer_destroyed)(&destroyed); }); }
struct ZwwmLayerShellV1Handler {
  void get_layer_surface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface, zwayland::server::Resource* output, std::uint32_t layer, std::string name_space) {
    (layer_get_surface)(&client, &resource, id, surface, output, layer, name_space.c_str());
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

void xwlr_destroyed(zwayland::server::Resource* resource) {
  auto* extension = resource->data<XwlrLayerState>();
  if (extension == nullptr) return;
  auto* layer = extension->layer;
  if (layer != nullptr && layer->xwlr == extension) {
    layer->xwlr = nullptr;
    if (layer->surface != nullptr) {
      reorder_layer_roots(layer->surface->observer, layer->output, layer->current.layer);
      configure_layout(layer->surface->observer);
      refresh_layer_keyboard_focus(layer->surface->observer);
      notify_surface_tree(layer->surface);
    }
  }
  delete extension;
}
void xwlr_set_edge(zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t edge) {
  auto* extension = resource->data<XwlrLayerState>();
  if (edge != 0 && edge != 1 && edge != 2 && edge != 4 && edge != 8) { resource->post_error(protocol::XWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE, "exclusive edge must be NONE or one edge"); return; }
  extension->pending_edge = edge;
}
void xwlr_set_priority(zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t priority) {
  if (priority < -32768 || priority > 32767) { resource->post_error(protocol::XWLR_LAYER_SURFACE_V1_ERROR_INVALID_PRIORITY, "priority must be in [-32768, 32767]"); return; }
  resource->data<XwlrLayerState>()->pending_priority = priority;
}
void xwlr_set_opacity(zwayland::server::Client*, zwayland::server::Resource* resource, double opacity) {
  if (opacity < 0 || opacity > 256) { resource->post_error(protocol::XWLR_LAYER_SURFACE_V1_ERROR_INVALID_OPACITY, "opacity must be a fixed value in [0, 1]"); return; }
  resource->data<XwlrLayerState>()->pending_opacity = opacity;
}
struct XwlrLayerSurfaceV1KXwlrLayerSurfaceHandler {
  void set_exclusive_edge(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t edge) {
    (xwlr_set_edge)(&client, &resource, edge);
  }
  void set_priority(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t priority) {
    (xwlr_set_priority)(&client, &resource, priority);
  }
  void set_opacity(zwayland::server::Client& client, zwayland::server::Resource& resource, double opacity) {
    (xwlr_set_opacity)(&client, &resource, opacity);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};
void xwlr_get_layer_surface(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* layer_resource) {
  if (layer_resource == nullptr || layer_resource->client != client ||
       layer_resource->interface != &protocol::zwwm_layer_surface_v1_interface ||
      layer_resource->data<LayerSurfaceState>() == nullptr) { manager->post_error(protocol::XWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER_SURFACE, "not a zwwm layer surface for this client"); return; }
  auto* layer = layer_resource->data<LayerSurfaceState>();
  if (layer == nullptr || layer->xwlr != nullptr) { manager->post_error(protocol::XWLR_LAYER_SHELL_V1_ERROR_EXTENSION_EXISTS, "layer surface already has an extension"); return; }
  auto* resource = client->create_resource(&protocol::xwlr_layer_surface_v1_interface, id, 1);
  auto* extension = resource == nullptr ? nullptr : new (std::nothrow) XwlrLayerState;
  if (resource == nullptr || extension == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; }
  extension->resource = resource; extension->layer = layer; layer->xwlr = extension;
  resource->set_data(extension); resource->set_handler(protocol::xwlr_layer_surface_v1_handler(XwlrLayerSurfaceV1KXwlrLayerSurfaceHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (xwlr_destroyed)(&destroyed); });
}
struct XwlrLayerShellV1KXwlrLayerShellHandler {
  void get_layer_surface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* layer_surface) {
    (xwlr_get_layer_surface)(&client, &resource, id, layer_surface);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

}  // namespace

void configure_layer_surface(LayerSurfaceState* layer, Rect geometry) {
  if (layer == nullptr || layer->surface == nullptr || layer->resource == nullptr) return;
  if (!layer->configure_requested && layer->configured_width == geometry.width &&
      layer->configured_height == geometry.height) return;
  const auto serial = layer->resource->display->next_serial();
  layer->serials.push_back(serial); layer->last_sent = serial;
  layer->configured_width = geometry.width; layer->configured_height = geometry.height;
  layer->configure_requested = false;
  protocol::zwwm_layer_surface_v1_send_configure(*layer->resource, serial, geometry.width, geometry.height);
}

void bind_layer_shell(zwayland::server::Client* client, void* data, std::uint32_t, std::uint32_t id) { auto* resource = client->create_resource(&protocol::zwwm_layer_shell_v1_interface, id, 1); if (resource == nullptr) { client->post_no_memory(); return; } resource->set_data(static_cast<Observer*>(data)); resource->set_handler(protocol::zwwm_layer_shell_v1_handler(ZwwmLayerShellV1Handler{})); }
void bind_xwlr_layer_shell(zwayland::server::Client* client, void*, std::uint32_t version, std::uint32_t id) { auto* resource = client->create_resource(&protocol::xwlr_layer_shell_v1_interface, id, std::min(version, 1U)); if (resource == nullptr) { client->post_no_memory(); return; } resource->set_handler(protocol::xwlr_layer_shell_v1_handler(XwlrLayerShellV1KXwlrLayerShellHandler{})); }

}  // namespace zwwm::detail
