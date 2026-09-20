#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>

#include "zwayland/client/display.hpp"
#include "zwayland/loopback.hpp"
#include "zwayland/server/display.hpp"
#include "zwayland/wire/message.hpp"

// Minimal protocol used only to measure loopback throughput.
namespace {

const zwayland::server::Message kTestMethods[] = {{"ping", "u"}};
const zwayland::server::Message kTestEvents[] = {{"pong", "u"}};
const zwayland::server::Interface kTestInterface{"zwayland_bench", 1, kTestMethods, 1,
                                                kTestEvents, 1};

const zwayland::client::Message kClientMethods[] = {{"ping", "u"}};
const zwayland::client::Message kClientEvents[] = {{"pong", "u"}};
const zwayland::client::Interface kClientTestInterface{"zwayland_bench", 1, kClientMethods, 1,
                                                      kClientEvents, 1};

const zwayland::client::Message kRegMethods[] = {{"bind", "usun"}};
const zwayland::client::Message kRegEvents[] = {{"global", "usu"}, {"global_remove", "u"}};
const zwayland::client::Interface kRegInterface{"wl_registry", 1, kRegMethods, 1, kRegEvents, 2};

}  // namespace

int main() {
  zwayland::server::Display server("zwwm-bench");
  int server_pings = 0;
  server.set_handler(&kTestInterface,
                     [&](zwayland::server::Client& client, zwayland::server::Resource&,
                        std::uint32_t, zwayland::wire::MessageParser& parser) {
                       const std::uint32_t v = parser.read_uint();
                       ++server_pings;
                       zwayland::wire::MessageBuilder b(2);
                       b.set_opcode(0);
                       b.append_uint(v);
                       client.send_event(2, 0, b.finish(), b.take_fds());
                     });
  server.add_global(&kTestInterface, 1, [](zwayland::server::Client& client, std::uint32_t,
                                           std::uint32_t id) {
    client.create_resource(&kTestInterface, id);
  });

  zwayland::server::Client* sc = nullptr;
  auto client = zwayland::connect_loopback(server, sc);
  (void)client->display_proxy();

  const std::uint32_t regId = 3;
  const std::uint32_t testId = 2;
  client->create_proxy(&kRegInterface, regId);
  client->create_proxy(&kClientTestInterface, testId);

  std::uint64_t sink = 0;
  client->set_observer(&kClientTestInterface,
                       [&](zwayland::client::Proxy&, std::uint32_t,
                           zwayland::wire::MessageParser& parser) {
                         sink += parser.read_uint();
                       });
  client->set_observer(&kRegInterface,
                       [&](zwayland::client::Proxy&, std::uint32_t op,
                           zwayland::wire::MessageParser& parser) {
                         if (op != 0) return;
                         const std::uint32_t name = parser.read_uint();
                         const std::string iface = std::string(parser.read_string());
                         const std::uint32_t version = parser.read_uint();
                         if (iface != "zwayland_bench") return;
                         zwayland::wire::MessageBuilder b(regId);
                         b.set_opcode(0);  // bind
                         b.append_uint(name);
                         b.append_string(iface);
                         b.append_uint(version);
                         b.append_new_id(testId);
                         client->send_request(regId, 0, b.finish(), b.take_fds());
                       });

  // Bind zwayland_bench to testId so the round-trip is real.
  zwayland::wire::MessageBuilder gr(1);
  gr.set_opcode(1);  // get_registry
  gr.append_new_id(regId);
  client->send_request(1, 1, gr.finish(), gr.take_fds());
  client->flush();
  client->flush();
  if (sc->find_resource(testId) == nullptr) {
    std::fprintf(stderr, "bind did not create resource %u\n", testId);
    return 1;
  }

  const std::uint32_t kIterations = 200000;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < kIterations; ++i) {
    zwayland::wire::MessageBuilder ping(testId);
    ping.set_opcode(0);
    ping.append_uint(i);
    client->send_request(testId, 0, ping.finish(), ping.take_fds());
    client->flush();
  }
  const auto end = std::chrono::steady_clock::now();

  const double seconds = std::chrono::duration<double>(end - start).count();
  std::printf("loopback round-trips: %u in %.3f s => %.0f/sec (handled=%d sink=%llu)\n",
              kIterations, seconds, kIterations / seconds, server_pings,
              static_cast<unsigned long long>(sink));
  return server_pings == static_cast<int>(kIterations) && sink != 0 ? 0 : 1;
}
