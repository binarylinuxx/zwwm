#include "compositor_server_internal.hpp"

#include <server-decoration-zwayland-server.h>
#include <wayland-zwayland-server.h>

namespace protocol = zwayland::generated;

namespace zwwm::detail {
namespace {
struct DecorationHandler {
  void release(zwayland::server::Client&, zwayland::server::Resource& resource) {
    resource.destroy();
  }
  void request_mode(zwayland::server::Client&, zwayland::server::Resource& resource,
                     std::uint32_t mode) {
    if (mode > protocol::ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER) {
      resource.post_error(protocol::WL_DISPLAY_ERROR_INVALID_METHOD, "invalid decoration mode");
      return;
    }
    protocol::org_kde_kwin_server_decoration_send_mode(
        resource, protocol::ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER);
  }
};

struct ManagerHandler {
  void create(zwayland::server::Client& client, zwayland::server::Resource& manager,
               std::uint32_t id, zwayland::server::Resource* surface) {
    if (compositor_surface_from_resource(&client, surface) == nullptr) {
      manager.post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "decoration requires a live wl_surface");
      return;
    }
    auto* resource = client.create_resource(&protocol::org_kde_kwin_server_decoration_interface, id, 1);
    if (resource == nullptr) { client.post_no_memory(); return; }
    resource->set_handler(protocol::org_kde_kwin_server_decoration_handler(DecorationHandler{}));
    protocol::org_kde_kwin_server_decoration_send_mode(
        *resource, protocol::ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER);
  }
};
}  // namespace

void bind_kde_decoration_manager(zwayland::server::Client* client, void*, std::uint32_t,
                                 std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::org_kde_kwin_server_decoration_manager_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_handler(protocol::org_kde_kwin_server_decoration_manager_handler(ManagerHandler{}));
  protocol::org_kde_kwin_server_decoration_manager_send_default_mode(
      *resource, protocol::ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
}
}  // namespace zwwm::detail
