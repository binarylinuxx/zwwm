#include <zwayland/loopback.hpp>
#include <zwayland/server/display.hpp>
#include <zwayland/server/signal.hpp>
#include <zwayland/wire/message.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

namespace {

// Server-side metadata for a tiny interface exercised by the core test.
const zwayland::server::Message kCoreMethods[] = {{"noop", ""}};
const zwayland::server::Message* kCoreEvents = nullptr;
const zwayland::server::Interface kCoreInterface{"zwwm_core_test", 1, kCoreMethods, 1,
                                                kCoreEvents, 0};

// Client-side metadata mirrored from the server for the loopback transport.
const zwayland::client::Message kCRegMethods[] = {{"bind", "usun"}};
const zwayland::client::Message kCRegEvents[] = {{"global", "usu"}, {"global_remove", "u"}};
const zwayland::client::Interface kCRegInterface{"wl_registry", 1, kCRegMethods, 1,
                                                kCRegEvents, 2};

const zwayland::client::Message kCTestMethods[] = {{"noop", ""}};
const zwayland::client::Message* kCTestEvents = nullptr;
const zwayland::client::Interface kCTestInterface{"zwwm_core_test", 1, kCTestMethods, 1,
                                                  kCTestEvents, 0};

bool g_resource_destroyed = false;
bool g_client_destroyed = false;

void on_resource_destroy(zwayland::server::Listener*, void* data) {
  (void)data;  // data is the Resource* being destroyed
  g_resource_destroyed = true;
}

void on_client_destroy(zwayland::server::Listener*, void* data) {
  (void)data;  // data is the Client* being destroyed
  g_client_destroyed = true;
}

}  // namespace

int main() {
  using namespace zwayland;
  server::Display display("zwwm-core-test");

  // 1. next_serial returns monotonically increasing values.
  assert(display.next_serial() == 1);
  assert(display.next_serial() == 2);
  assert(display.next_serial() == 3);

  server::Listener res_listener;
  res_listener.notify = &on_resource_destroy;

  server::Resource* the_resource = nullptr;

  display.set_handler(&kCoreInterface,
                      [](server::Client&, server::Resource&, std::uint32_t,
                         wire::MessageParser&) {});
  display.add_global(&kCoreInterface, 1,
                     [&](server::Client& client, std::uint32_t, std::uint32_t id) {
                       server::Resource* r = client.create_resource(
                           &kCoreInterface, id, 1, std::any(12345));
                       the_resource = r;
                       r->destroy_signal.add(res_listener);
                     });

  // 2. Loopback bind; the resource is created with userdata and a destroy
  // listener registered on its destroy_signal.
  server::Client* sc = nullptr;
  auto client = connect_loopback(display, sc);
  assert(client->fd() == -1);
  assert(sc->fd() == -1);

  const std::uint32_t reg_id = client->alloc_id();
  client->create_proxy(&kCRegInterface, reg_id);

  std::uint32_t test_id = 0;
  client->set_observer(&kCRegInterface,
                       [&](client::Proxy&, std::uint32_t op,
                           wire::MessageParser& parser) {
                         if (op != 0) return;  // global
                         const std::uint32_t name = parser.read_uint();
                         const std::string iface = std::string(parser.read_string());
                         const std::uint32_t version = parser.read_uint();
                         if (iface != "zwwm_core_test") return;
                         test_id = client->alloc_id();
                         client->create_proxy(&kCTestInterface, test_id);
                         wire::MessageBuilder bind(reg_id);
                          bind.set_opcode(0);  // bind
                         bind.append_uint(name);
                         bind.append_string(iface);
                         bind.append_uint(version);
                         bind.append_new_id(test_id);
                          client->send_request(reg_id, 0, bind.finish(),
                                              bind.take_fds());
                       });

  {
    wire::MessageBuilder get(1);
    get.set_opcode(1);  // get_registry
    get.append_new_id(reg_id);
    client->send_request(1, 1, get.finish(), get.take_fds());
  }
  client->flush();
  client->flush();

  assert(the_resource != nullptr);
  assert(std::any_cast<int>(*the_resource->get_user_data()) == 12345);
  assert(std::strcmp(the_resource->get_class(), "zwwm_core_test") == 0);
  assert(the_resource->get_client() == sc);

  // 3. Destroying the client fires the resource destroy_signal and the client
  // destroy_signal.
  server::Listener cl_listener;
  cl_listener.notify = &on_client_destroy;
  sc->add_destroy_listener(cl_listener);

  sc->destroy();

  assert(g_resource_destroyed);
  assert(g_client_destroyed);

  // 4. Idle and timer sources both fire.
  std::atomic<bool> idle_fired{false};
  std::atomic<bool> timer_fired{false};
  display.add_idle([&]() { idle_fired.store(true); });
  display.add_timer(50, [&]() {
    timer_fired.store(true);
    display.stop();
  });

  std::thread loop([&]() { display.run(); });
  // The timer callback stops the loop; wait for completion with a timeout.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!timer_fired.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  loop.join();

  assert(idle_fired.load());
  assert(timer_fired.load());

  return 0;
}
