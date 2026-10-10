#include "compositor_server_internal.hpp"

#include <zwwm-decoration-manager-v1-zwayland-server.h>
#include <wayland-zwayland-server.h>

#include <stdexcept>

namespace protocol = zwayland::generated;

namespace zwwm::detail {
namespace {
constexpr zwayland::server::Message compat_manager_requests[] = {{"create", "no", 1}};
constexpr zwayland::server::Message compat_manager_events[] = {{"default_mode", "u", 1}};
constexpr zwayland::server::Interface compat_manager{
    "org_kde_kwin_server_decoration_manager", 1, compat_manager_requests, 1, compat_manager_events, 1};
constexpr zwayland::server::Message compat_decoration_requests[] = {
    {"release", "", 1}, {"request_mode", "u", 1}};
constexpr zwayland::server::Message compat_decoration_events[] = {{"mode", "u", 1}};
constexpr zwayland::server::Interface compat_decoration{
    "org_kde_kwin_server_decoration", 1, compat_decoration_requests, 2, compat_decoration_events, 1};

void send_server_mode(zwayland::server::Resource& resource) {
  // Both frontends carry their selected/default mode in event zero.
  zwayland::wire::MessageBuilder message(resource.id);
  message.set_opcode(0);
  message.append_uint(protocol::ZWWM_DECORATION_MANAGER_V1_MODE_SERVER_SIDE);
  resource.client->send_event(resource.id, 0, message.finish());
}

struct DecorationHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) {
    resource.destroy();
  }
  void set_mode(zwayland::server::Client&, zwayland::server::Resource& resource,
                      std::uint32_t mode) {
    if (mode != protocol::ZWWM_DECORATION_MANAGER_V1_MODE_CLIENT_SIDE &&
        mode != protocol::ZWWM_DECORATION_MANAGER_V1_MODE_SERVER_SIDE) {
      resource.post_error(protocol::ZWWM_SURFACE_DECORATION_V1_ERROR_INVALID_MODE, "invalid decoration mode");
      return;
    }
    send_server_mode(resource);
  }
};

void create_decoration(zwayland::server::Client& client, zwayland::server::Resource& manager,
                       std::uint32_t id, zwayland::server::Resource* surface, bool compatibility) {
  if (compositor_surface_from_resource(&client, surface) == nullptr) {
    manager.post_error(compatibility ? protocol::WL_DISPLAY_ERROR_INVALID_OBJECT :
                       protocol::ZWWM_DECORATION_MANAGER_V1_ERROR_INVALID_SURFACE,
                       "decoration requires a live wl_surface");
    return;
  }
  auto* resource = client.create_resource(compatibility ? &compat_decoration :
                                           &protocol::zwwm_surface_decoration_v1_interface, id, 1);
  if (resource == nullptr) { client.post_no_memory(); return; }
  if (compatibility) {
    resource->set_handler([](zwayland::server::Client&, zwayland::server::Resource& resource,
                             std::uint32_t opcode, zwayland::wire::MessageParser& parser) {
      if (opcode == 0) resource.destroy();
      else if (opcode == 1) {
        const auto mode = parser.read_uint();
        if (mode > protocol::ZWWM_DECORATION_MANAGER_V1_MODE_SERVER_SIDE) {
          resource.post_error(protocol::WL_DISPLAY_ERROR_INVALID_METHOD, "invalid decoration mode");
          return;
        }
        send_server_mode(resource);
      }
    });
  } else resource->set_handler(protocol::zwwm_surface_decoration_v1_handler(DecorationHandler{}));
  send_server_mode(*resource);
}

struct ManagerHandler {
  void get_decoration(zwayland::server::Client& client, zwayland::server::Resource& manager,
               std::uint32_t id, zwayland::server::Resource* surface) {
    create_decoration(client, manager, id, surface, false);
  }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) {
    resource.destroy();
  }
};
}  // namespace

void bind_zwwm_decoration_manager(zwayland::server::Client* client, void*, std::uint32_t,
                                 std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwwm_decoration_manager_v1_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_handler(protocol::zwwm_decoration_manager_v1_handler(ManagerHandler{}));
  send_server_mode(*resource);
}

const zwayland::server::Interface& decoration_compat_interface() { return compat_manager; }

void bind_decoration_compat_manager(zwayland::server::Client* client, void*, std::uint32_t,
                                    std::uint32_t id) {
  auto* resource = client->create_resource(&compat_manager, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_handler([](zwayland::server::Client& client, zwayland::server::Resource& manager,
                           std::uint32_t, zwayland::wire::MessageParser& parser) {
    const auto id = parser.read_new_id();
    if (!client.valid_new_id(id)) throw std::runtime_error("invalid decoration object id");
    auto* surface = client.find_resource(parser.read_object());
    create_decoration(client, manager, id, surface, true);
  });
  send_server_mode(*resource);
}
}  // namespace zwwm::detail
