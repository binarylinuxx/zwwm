#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>

#include "zwayland/loopback.hpp"
#include "zwayland/wire/message.hpp"

namespace {

// Server-side metadata for a tiny round-trip interface.
const zwayland::server::Message kTestMethods[] = {{"ping", "u"}};
const zwayland::server::Message kTestEvents[] = {{"pong", "u"}};
const zwayland::server::Interface kTestInterface{"zwayland_test", 1, kTestMethods, 1,
                                                kTestEvents, 1};

// Client-side metadata (separate instances; only the name/signature matter).
const zwayland::client::Message kRegMethods[] = {{"bind", "usun"}};
const zwayland::client::Message kRegEvents[] = {{"global", "usu"}, {"global_remove", "u"}};
const zwayland::client::Interface kRegistryInterface{"wl_registry", 1, kRegMethods, 1,
                                                    kRegEvents, 2};

const zwayland::client::Message kCTestMethods[] = {{"ping", "u"}};
const zwayland::client::Message kCTestEvents[] = {{"pong", "u"}};
const zwayland::client::Interface kCTestInterface{"zwayland_test", 1, kCTestMethods, 1,
                                                 kCTestEvents, 1};

}  // namespace

int main() {
  zwayland::server::Display server("zwwm-loopback-test");
  server.set_handler(&kTestInterface, [](zwayland::server::Client& client,
                                         zwayland::server::Resource& resource,
                                         std::uint32_t, zwayland::wire::MessageParser& parser) {
    const std::uint32_t value = parser.read_uint();
    zwayland::wire::MessageBuilder reply(resource.id);
    reply.set_opcode(0);  // pong
    reply.append_uint(value + 1);
    client.send_event(resource.id, 0, reply.finish(), reply.take_fds());
  });
  server.add_global(&kTestInterface, 1, [](zwayland::server::Client& client,
                                           std::uint32_t, std::uint32_t id) {
    client.create_resource(&kTestInterface, id);
  });

  zwayland::server::Client* sc = nullptr;
  auto client = zwayland::connect_loopback(server, sc);

  // Loopback must be socketless.
  assert(client->fd() == -1);
  assert(sc->fd() == -1);

  const std::uint32_t regId = client->alloc_id();
  client->create_proxy(&kRegistryInterface, regId);

  std::uint32_t testId = 0;
  client->set_observer(&kRegistryInterface,
                       [&](zwayland::client::Proxy&, std::uint32_t op,
                           zwayland::wire::MessageParser& parser) {
                         if (op != 0) return;  // global
                         const std::uint32_t name = parser.read_uint();
                         const std::string iface = std::string(parser.read_string());
                         const std::uint32_t version = parser.read_uint();
                         if (iface != "zwayland_test") return;
                         testId = client->alloc_id();
                         client->create_proxy(&kCTestInterface, testId);
                         zwayland::wire::MessageBuilder bind(regId);
                          bind.set_opcode(0);  // bind
                         bind.append_uint(name);
                         bind.append_string(iface);
                         bind.append_uint(version);
                         bind.append_new_id(testId);
                          client->send_request(regId, 0, bind.finish(), bind.take_fds());
                       });

  int pong = -1;
  client->set_observer(&kCTestInterface,
                       [&](zwayland::client::Proxy&, std::uint32_t op,
                           zwayland::wire::MessageParser& parser) {
                         if (op == 0) pong = static_cast<int>(parser.read_uint());
                       });

  // get_registry -> server replies with global -> observer binds.
  {
    zwayland::wire::MessageBuilder get(1);
    get.set_opcode(1);  // get_registry
    get.append_new_id(regId);
    client->send_request(1, 1, get.finish(), get.take_fds());
  }
  try { client->flush(); } catch (const std::exception& e) {
    std::fprintf(stderr, "flush1 failed: %s\n", e.what()); throw;
  }  // delivers get_registry; nested dispatch queues bind
  try { client->flush(); } catch (const std::exception& e) {
    std::fprintf(stderr, "flush2 failed: %s\n", e.what()); throw;
  }  // delivers bind; server creates the test resource

  // ping -> server replies pong(value+1).
  {
    zwayland::wire::MessageBuilder ping(testId);
    ping.set_opcode(0);  // ping
    ping.append_uint(42);
    client->send_request(testId, 0, ping.finish(), ping.take_fds());
  }
  try { client->flush(); } catch (const std::exception& e) {
    std::fprintf(stderr, "flush3 failed: %s\n", e.what()); throw;
  }

  assert(pong == 43);
  return 0;
}
