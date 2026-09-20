#pragma once

#include "zwayland/client/display.hpp"
#include "zwayland/server/display.hpp"
#include "zwayland/wire/connection.hpp"

#include <memory>

namespace zwayland {

// Wires an in-process client Display to `server` over an in-memory loopback
// transport: messages skip the socket entirely. Marshal-time FD duplicates are
// transferred directly to the peer rather than duplicated again by SCM_RIGHTS. Returns the
// client; `*server_client` receives the adopted server-side Client.
//
// Delivery is synchronous: a client flush runs the server handler (and any
// resulting events) inline on the calling thread, so no event loop is required
// for either side. Destroying the returned client severs the link.
inline std::unique_ptr<client::Display> connect_loopback(
    server::Display& server, server::Client*& server_client) {
  auto server_conn = std::make_unique<wire::Connection>(-1);
  auto client_conn = std::make_unique<wire::Connection>(-1);
  server_conn->link(*client_conn);
  client_conn->link(*server_conn);

  server::Client* sc = server.add_loopback_client(std::move(server_conn));
  server_client = sc;

  auto client = client::Display::adopt(std::move(client_conn));
  client->connection().set_data_ready(
      [client = client.get()]() { (void)client->dispatch(); });
  return client;
}

}  // namespace zwayland
