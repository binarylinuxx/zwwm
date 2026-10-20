#include "connection.hpp"

#include <zwayland/client/core.hpp>
#include <zwwm-client-connection-v1-zwayland-client.h>
#include <cstdlib>
#include <stdexcept>

namespace zterm {
namespace p = zwayland::generated;
using Proxy = zwayland::client::Proxy;
using Parser = zwayland::wire::MessageParser;

ClientConnection::ClientConnection(bool required) : display_(zwayland::client::Display::connect()) {
  if (!display_) throw std::runtime_error("cannot connect to the Wayland compositor");
  auto* registry = zwayland::client::core::get_registry(*display_);
  struct State { Proxy* manager = nullptr; };
  auto state = std::make_shared<State>();
  registry->set_observer([state](Proxy& registry, std::uint32_t opcode, Parser& parser) {
    const auto name = parser.read_uint();
    if (opcode != 0) return;
    const auto interface = parser.read_string(); const auto version = parser.read_uint();
    if (interface == "zwwm_client_connection_manager_v1" && version >= 1 && !state->manager)
      state->manager = zwayland::client::core::bind(*registry.display, registry, name, p::zwwm_client_connection_manager_v1_interface, 1);
  });
  if (!display_->roundtrip()) throw std::runtime_error("client connection discovery failed");
  manager_ = state->manager;
  if (required && !manager_) throw std::runtime_error("zwwm-client-connection-v1 is unavailable on this compositor");
}

zwayland::wire::FileDescriptor ClientConnection::create_wayland() {
  if (!manager_) throw std::runtime_error("native client connections are unavailable");
  struct Result { zwayland::wire::FileDescriptor socket; std::string error; };
  auto result = std::make_shared<Result>();
  const auto id = p::zwwm_client_connection_manager_v1_create_connection(*display_, manager_->id);
  display_->find_proxy(id)->set_observer([result](Proxy&, std::uint32_t opcode, Parser& parser) {
    if (opcode == 0) result->socket = zwayland::wire::FileDescriptor(parser.read_fd());
    else { (void)parser.read_uint(); result->error = parser.read_string(); }
  });
  const bool received = display_->roundtrip();
  p::zwwm_client_connection_v1_destroy(*display_, id);
  if (!received || result->socket.get() < 0)
    throw std::runtime_error(result->error.empty() ? "native client connection failed" : result->error);
  return std::move(result->socket);
}

std::unique_ptr<zwayland::client::Display> connect_display(bool standalone) {
  if (standalone || std::getenv("WAYLAND_SOCKET")) return zwayland::client::Display::connect();
  ClientConnection connection;
  if (!connection.manager()) return connection.take_display();
  auto socket = connection.create_wayland();
  auto transport = std::make_unique<zwayland::wire::Connection>(socket.release());
  return zwayland::client::Display::adopt(std::move(transport));
}

}  // namespace zterm
