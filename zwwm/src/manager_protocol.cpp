#include "zwwm/manager_protocol.hpp"

#include "zwwm/compositor_server.hpp"

#include <ext-zwwm-manager-v1-zwayland-server.h>
#include <zwayland/server/display.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace protocol = zwayland::generated;

namespace zwwm {
namespace {

std::uint32_t high(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32U); }
std::uint32_t low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }

std::uint32_t event_bit(const std::string& event) {
  if (event == "ready") return protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_READY;
  if (event == "config") return protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_CONFIG;
  if (event == "tag") return protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_TAG;
  if (event == "window") return protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_WINDOW;
  if (event == "layer") return protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_LAYER;
  if (event == "output") return protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_OUTPUT;
  if (event == "shutdown") return protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_SHUTDOWN;
  return 0;
}

}  // namespace

struct ManagerProtocol::Impl {
  struct Subscription { Impl* owner = nullptr; zwayland::server::Resource* resource = nullptr; std::uint32_t events = 0; };

  zwayland::server::Display* display = nullptr;
  CompositorServer* compositor = nullptr;
  Reload reload;
  RebuildShaders rebuild_shaders;
  SetCursor set_cursor;
  std::uint32_t global = 0;
  std::vector<Subscription*> subscriptions;

  static void snapshot_destroyed(zwayland::server::Resource*) {}
  static void result_destroyed(zwayland::server::Resource*) {}
  static void subscription_destroyed(zwayland::server::Resource* resource) {
    auto* subscription = resource->data<Subscription>();
    if (subscription == nullptr) return;
    std::erase(subscription->owner->subscriptions, subscription);
    delete subscription;
  }

  struct DestroyHandler {
    void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  };

  zwayland::server::Resource* snapshot(zwayland::server::Client* client, std::uint32_t id) {
    auto* resource = client->create_resource(&protocol::ext_zwwm_snapshot_v1_interface, id, 1);
    if (resource == nullptr) { client->post_no_memory(); return nullptr; }
    resource->set_data(this);
    resource->set_handler(protocol::ext_zwwm_snapshot_v1_handler(DestroyHandler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { snapshot_destroyed(&destroyed); });
    return resource;
  }

  zwayland::server::Resource* result(zwayland::server::Client* client, std::uint32_t id) {
    auto* resource = client->create_resource(&protocol::ext_zwwm_result_v1_interface, id, 1);
    if (resource == nullptr) { client->post_no_memory(); return nullptr; }
    resource->set_data(this);
    resource->set_handler(protocol::ext_zwwm_result_v1_handler(DestroyHandler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { result_destroyed(&destroyed); });
    return resource;
  }

  void send_snapshot(zwayland::server::Resource* resource, std::uint32_t type) {
    if (type == protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_STATUS) {
      protocol::ext_zwwm_snapshot_v1_send_status(*resource, 4, compositor->outputs().size(),
                                        compositor->toplevels().size(), compositor->layers().size());
    } else if (type == protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_OUTPUTS) {
      for (const auto& output : compositor->outputs()) {
        protocol::ext_zwwm_snapshot_v1_send_output(*
            resource, output.connector.c_str(), high(output.id.value), low(output.id.value),
            output.logical_x, output.logical_y, output.logical_width, output.logical_height,
            output.physical_width, output.physical_height, output.refresh_millihz,
            output.scale_per_mille, static_cast<std::uint32_t>(output.transform), output.bit_depth,
            output.enabled ? 1U : 0U, output.modes.size());
        for (const auto& mode : output.modes)
          protocol::ext_zwwm_snapshot_v1_send_mode(*resource, mode.width, mode.height, mode.refresh_millihz,
                                          mode.preferred ? 1U : 0U);
      }
    } else if (type == protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_TOPLEVELS) {
      for (const auto& toplevel : compositor->toplevels())
        protocol::ext_zwwm_snapshot_v1_send_toplevel(*
            resource, high(toplevel.id), low(toplevel.id), toplevel.app_id.c_str(), toplevel.title.c_str(),
            high(toplevel.output.value), low(toplevel.output.value), toplevel.x, toplevel.y,
            toplevel.width, toplevel.height, toplevel.state);
    } else if (type == protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_TAGS) {
      for (const auto& tag : compositor->tags())
        protocol::ext_zwwm_snapshot_v1_send_tag(*resource, tag.connector.c_str(), high(tag.output.value),
                                       low(tag.output.value), tag.active);
    } else if (type == protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LAYERS) {
      for (const auto& layer : compositor->layers())
        protocol::ext_zwwm_snapshot_v1_send_layer(*
            resource, high(layer.id), low(layer.id), high(layer.output.value), low(layer.output.value),
            layer.name_space.c_str(), layer.layer, layer.priority, layer.exclusive_zone,
            layer.blur_radius, static_cast<std::uint32_t>(std::lround(layer.opacity * 1000000.0F)),
             layer.mapped ? 1U : 0U);
    } else if (type == protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_CAMERAS) {
      for (const auto& camera : compositor->cameras()) {
        const auto x = std::to_string(camera.x);
        const auto y = std::to_string(camera.y);
        const auto zoom = std::to_string(camera.zoom);
        protocol::ext_zwwm_snapshot_v1_send_camera(
            *resource, camera.connector.c_str(), high(camera.output.value), low(camera.output.value),
            camera.tag, x.c_str(), y.c_str(), zoom.c_str(), camera.active ? 1U : 0U);
      }
    } else {
      protocol::ext_zwwm_snapshot_v1_send_failed(*resource, "unknown snapshot type");
      return;
    }
    protocol::ext_zwwm_snapshot_v1_send_done(*resource);
  }

  static void get_snapshot(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, std::uint32_t type) {
    auto* self = manager->data<Impl>();
    if (auto* resource = self->snapshot(client, id); resource != nullptr) self->send_snapshot(resource, type);
  }

  static void reload_request(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id) {
    auto* self = manager->data<Impl>();
    auto* resource = self->result(client, id);
    if (resource == nullptr) return;
    const std::string error = self->reload ? self->reload() : "reload is unavailable";
    if (error.empty()) protocol::ext_zwwm_result_v1_send_succeeded(*resource, "reloaded");
    else protocol::ext_zwwm_result_v1_send_failed(*resource, error.c_str());
  }

  static void dispatch_request(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id,
                               const char* action, const char* argument) {
    auto* self = manager->data<Impl>();
    auto* resource = self->result(client, id);
    if (resource == nullptr) return;
    std::string error;
    if (action != nullptr && self->compositor->dispatch_action(action, argument == nullptr ? "" : argument, &error))
      protocol::ext_zwwm_result_v1_send_succeeded(*resource, "dispatched");
    else protocol::ext_zwwm_result_v1_send_failed(*resource, error.empty() ? "invalid action" : error.c_str());
  }

  static void rebuild_shaders_request(zwayland::server::Client* client,
                                      zwayland::server::Resource* manager,
                                      std::uint32_t id) {
    auto* self = manager->data<Impl>();
    auto* resource = self->result(client, id);
    if (resource == nullptr) return;
    const std::string error = self->rebuild_shaders ? self->rebuild_shaders() : "shader rebuild is unavailable";
    if (error.empty()) protocol::ext_zwwm_result_v1_send_succeeded(*resource, "shaders rebuilt and switched");
    else protocol::ext_zwwm_result_v1_send_failed(*resource, error.c_str());
  }

  static void set_cursor_request(zwayland::server::Client* client,
                                 zwayland::server::Resource* manager,
                                 std::uint32_t id, const char* theme,
                                 std::uint32_t size) {
    auto* self = manager->data<Impl>();
    auto* resource = self->result(client, id);
    if (resource == nullptr) return;
    const std::string error = self->set_cursor
        ? self->set_cursor(theme == nullptr ? "" : theme, size)
        : "cursor control is unavailable";
    if (error.empty()) protocol::ext_zwwm_result_v1_send_succeeded(*resource, "cursor theme set");
    else protocol::ext_zwwm_result_v1_send_failed(*resource, error.c_str());
  }

  static void subscribe_request(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id,
                                std::uint32_t events) {
    auto* self = manager->data<Impl>();
    auto* resource = client->create_resource(&protocol::ext_zwwm_subscription_v1_interface, id, 1);
    auto* subscription = resource == nullptr ? nullptr : new (std::nothrow) Subscription{self, resource, events};
    if (resource == nullptr || subscription == nullptr) {
      if (resource != nullptr) resource->destroy();
      client->post_no_memory();
      return;
    }
    self->subscriptions.push_back(subscription);
    resource->set_data(subscription);
    resource->set_handler(protocol::ext_zwwm_subscription_v1_handler(DestroyHandler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { subscription_destroyed(&destroyed); });
  }

  struct ManagerHandler {
    void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
    void get_snapshot(zwayland::server::Client& client, zwayland::server::Resource& resource,
                      std::uint32_t id, std::uint32_t type) {
      Impl::get_snapshot(&client, &resource, id, type);
    }
    void reload(zwayland::server::Client& client, zwayland::server::Resource& resource,
                std::uint32_t id) {
      reload_request(&client, &resource, id);
    }
    void dispatch(zwayland::server::Client& client, zwayland::server::Resource& resource,
                  std::uint32_t id, const std::string& action, const std::string& argument) {
      dispatch_request(&client, &resource, id, action.c_str(), argument.c_str());
    }
    void rebuild_switch_shaders(zwayland::server::Client& client,
                                zwayland::server::Resource& resource,
                                std::uint32_t id) {
      rebuild_shaders_request(&client, &resource, id);
    }
    void set_cursor(zwayland::server::Client& client,
                    zwayland::server::Resource& resource, std::uint32_t id,
                    const std::string& theme, std::uint32_t size) {
      set_cursor_request(&client, &resource, id, theme.c_str(), size);
    }
    void subscribe(zwayland::server::Client& client, zwayland::server::Resource& resource,
                   std::uint32_t id, std::uint32_t events) {
      subscribe_request(&client, &resource, id, events);
    }
  };
  static void bind(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
    auto* self = static_cast<Impl*>(data);
    auto* resource = client->create_resource(&protocol::ext_zwwm_manager_v1_interface, id, std::min(version, 4U));
    if (resource == nullptr) { client->post_no_memory(); return; }
    resource->set_data(self);
    resource->set_handler(protocol::ext_zwwm_manager_v1_handler(ManagerHandler{}));
  }
};

ManagerProtocol::ManagerProtocol(zwayland::server::Display* display, CompositorServer* compositor, Reload reload,
                                  RebuildShaders rebuild_shaders, SetCursor set_cursor)
    : impl_(new Impl) {
  if (display == nullptr || compositor == nullptr) throw std::invalid_argument("manager protocol requires a display and compositor");
  impl_->display = display;
  impl_->compositor = compositor;
  impl_->reload = std::move(reload);
  impl_->rebuild_shaders = std::move(rebuild_shaders);
  impl_->set_cursor = std::move(set_cursor);
  impl_->global = display->add_global(&protocol::ext_zwwm_manager_v1_interface, 4, [data = impl_](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (Impl::bind)(&client, data, bound_version, id); });
  if (impl_->global == 0) { delete impl_; throw std::runtime_error("could not create manager protocol global"); }
  compositor->set_event_observer([](void* data, const char* event) {
    if (event != nullptr) static_cast<ManagerProtocol*>(data)->emit(event);
  }, this);
}

ManagerProtocol::~ManagerProtocol() {
  impl_->compositor->set_event_observer(nullptr, nullptr);
  for (auto* subscription : impl_->subscriptions) subscription->resource->userdata.reset();
  if (impl_->global != 0) impl_->display->destroy_global(impl_->global);
  delete impl_;
}

void ManagerProtocol::emit(const std::string& event) {
  const std::uint32_t bit = event_bit(event);
  if (bit == 0) return;
  for (auto* subscription : impl_->subscriptions)
    if ((subscription->events & bit) != 0) protocol::ext_zwwm_subscription_v1_send_event(*subscription->resource, event.c_str());
}

}  // namespace zwwm
