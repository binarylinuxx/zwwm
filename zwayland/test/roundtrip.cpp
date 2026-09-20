#include <zwayland/client/display.hpp>
#include <zwayland/server/display.hpp>
#include <zwayland/wire/message.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unistd.h>

// Built-in metadata mirrored from the server for the client side. The wire
// protocol only cares about opcodes and signatures, so names just need to
// match for readability.
namespace {

const zwayland::client::Message kCallbackEvents[] = {{"done", "u"}};
const zwayland::client::Interface kCallbackInterface{"wl_callback", 1, nullptr, 0,
                                                     kCallbackEvents, 1};

}  // namespace

int main() {
  using namespace zwayland;

  server::Display display("wayland-zwtest");

  std::atomic<bool> done{false};
  std::thread server([&] { display.run(); });

  // Give the server a moment to enter its event loop.
  usleep(20000);

  auto client = client::Display::connect("wayland-zwtest");
  assert(client != nullptr);

  client->set_observer(&kCallbackInterface, [&](client::Proxy&, std::uint32_t opcode,
                                                wire::MessageParser& parser) {
    if (opcode == 0) {  // wl_callback.done
      (void)parser.read_uint();
      done.store(true);
    }
  });

  // wl_display.sync(new_id callback)
  const std::uint32_t cb_id = client->alloc_id();
  client->create_proxy(&kCallbackInterface, cb_id);
  wire::MessageBuilder request(1);
  request.set_opcode(0);
  request.append_new_id(cb_id);
  client->send_request(1, 0, request.finish(), request.take_fds());
  client->flush();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done.load() &&
         std::chrono::steady_clock::now() < deadline) {
    (void)client->dispatch();
    client->flush();
    usleep(1000);
  }

  assert(done.load() && "sync roundtrip did not complete");

  display.stop();
  server.join();
  return 0;
}
