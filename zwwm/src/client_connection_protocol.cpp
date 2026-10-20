#include "zwwm/client_connection_protocol.hpp"
#include "zwwm/unique_fd.hpp"
#include <zwwm-client-connection-v1-zwayland-server.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>
#include <sys/socket.h>
#include <unordered_map>
#include <vector>

namespace zwwm {
namespace p = zwayland::generated;
using Client = zwayland::server::Client;
using Resource = zwayland::server::Resource;

struct ClientConnectionProtocol::Impl {
  struct Daemon { Impl* owner; Resource* resource; std::string name; bool registered = false; };
  zwayland::server::Display* display;
  std::uint32_t global = 0;
  std::vector<Resource*> managers;
  std::vector<Daemon*> daemons;
  std::unordered_map<std::string, Daemon*> names;

  struct Destroy {
    void destroy(Client&, Resource& resource) { resource.destroy(); }
  };
  static bool valid_name(const std::string& name) {
    return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
  }
  static Impl* owner(Resource& resource) {
    auto* self = resource.data<Impl>();
    if (!self) resource.post_error(0, "client connection service is unavailable");
    return self;
  }
  Resource* connection(Client& client, std::uint32_t id) {
    auto* resource = client.create_resource(&p::zwwm_client_connection_v1_interface, id, 1);
    if (!resource) { client.post_no_memory(); return nullptr; }
    resource->set_handler(p::zwwm_client_connection_v1_handler(Destroy{}));
    return resource;
  }
  static bool sockets(Resource& result, UniqueFd& first, UniqueFd& second) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) {
      p::zwwm_client_connection_v1_send_failed(result, p::ZWWM_CLIENT_CONNECTION_MANAGER_V1_FAILURE_CONNECTION_FAILED, std::strerror(errno));
      return false;
    }
    first = UniqueFd(pair[0]); second = UniqueFd(pair[1]); return true;
  }
  void create_connection(Client& client, std::uint32_t id) {
    auto* result = connection(client, id); if (!result) return;
    UniqueFd server, peer;
    if (!sockets(*result, server, peer)) return;
    display->add_client(server.release());
    p::zwwm_client_connection_v1_send_connected(*result, peer.get());
  }
  void register_daemon(Client& client, std::uint32_t id, const std::string& name) {
    auto* resource = client.create_resource(&p::zwwm_client_daemon_v1_interface, id, 1);
    auto* daemon = resource ? new (std::nothrow) Daemon{this,resource,name,false} : nullptr;
    if (!resource || !daemon) { if (resource) resource->destroy(); client.post_no_memory(); return; }
    daemons.push_back(daemon);
    resource->set_data(daemon); resource->set_handler(p::zwwm_client_daemon_v1_handler(Destroy{}));
    resource->set_destroy_handler([](Resource& object) {
      auto* state = object.data<Daemon>();
      if (state->owner) {
        if (state->registered) state->owner->names.erase(state->name);
        std::erase(state->owner->daemons, state);
      }
      delete state;
    });
    if (!valid_name(name)) p::zwwm_client_daemon_v1_send_failed(*resource, p::ZWWM_CLIENT_CONNECTION_MANAGER_V1_FAILURE_INVALID_NAME, "invalid daemon name");
    else if (names.contains(name)) p::zwwm_client_daemon_v1_send_failed(*resource, p::ZWWM_CLIENT_CONNECTION_MANAGER_V1_FAILURE_ALREADY_REGISTERED, "daemon name is already registered");
    else { names.emplace(name, daemon); daemon->registered = true; p::zwwm_client_daemon_v1_send_ready(*resource); }
  }
  void connect_daemon(Client& client, std::uint32_t id, const std::string& name) {
    auto* result = connection(client, id); if (!result) return;
    const auto found = names.find(name);
    if (found == names.end()) {
      p::zwwm_client_connection_v1_send_failed(*result, p::ZWWM_CLIENT_CONNECTION_MANAGER_V1_FAILURE_UNAVAILABLE, "daemon is not registered"); return;
    }
    UniqueFd daemon, peer;
    if (!sockets(*result, daemon, peer)) return;
    p::zwwm_client_daemon_v1_send_connection(*found->second->resource, daemon.get());
    p::zwwm_client_connection_v1_send_connected(*result, peer.get());
  }
  struct Manager {
    void destroy(Client&, Resource& resource) { resource.destroy(); }
    void create_connection(Client& client, Resource& resource, std::uint32_t id) {
      if (auto* self = owner(resource)) self->create_connection(client, id);
    }
    void register_daemon(Client& client, Resource& resource, std::uint32_t id, const std::string& name) {
      if (auto* self = owner(resource)) self->register_daemon(client, id, name);
    }
    void connect_daemon(Client& client, Resource& resource, std::uint32_t id, const std::string& name) {
      if (auto* self = owner(resource)) self->connect_daemon(client, id, name);
    }
  };
};

ClientConnectionProtocol::ClientConnectionProtocol(zwayland::server::Display* display) : impl_(std::make_unique<Impl>()) {
  if (!display) throw std::invalid_argument("client connection protocol requires a display");
  impl_->display = display;
  impl_->global = display->add_global(&p::zwwm_client_connection_manager_v1_interface, 1,
    [self = impl_.get()](Client& client, std::uint32_t version, std::uint32_t id) {
      auto* resource = client.create_resource(&p::zwwm_client_connection_manager_v1_interface, id, version);
      if (!resource) { client.post_no_memory(); return; }
      self->managers.push_back(resource); resource->set_data(self);
      resource->set_handler(p::zwwm_client_connection_manager_v1_handler(Impl::Manager{}));
      resource->set_destroy_handler([](Resource& object) { if (auto* owner = object.data<Impl>()) std::erase(owner->managers, &object); });
    });
}

ClientConnectionProtocol::~ClientConnectionProtocol() {
  for (auto* resource : impl_->managers) resource->set_data(nullptr);
  for (auto* daemon : impl_->daemons) daemon->owner = nullptr;
  impl_->display->destroy_global(impl_->global);
}

}  // namespace zwwm
