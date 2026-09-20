#include "../src/compositor_server_internal.hpp"

#include <zwayland/server/display.hpp>
#include <zwayland/wire/connection.hpp>
#include <zwayland/wire/message.hpp>

#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace {
namespace protocol = zwayland::generated;

void require(bool condition) {
  if (!condition) std::_Exit(EXIT_FAILURE);
}

struct Fixture {
  zwayland::server::Display display{
      "/tmp/zwwm-surface-lifetime-" + std::to_string(getpid())};
  zwwm::CompositorServer compositor{&display, std::make_shared<zwwm::RuntimeConfig>()};
  std::unique_ptr<zwayland::wire::Connection> peer;
  zwayland::server::Client* client = nullptr;

  Fixture() {
    int sockets[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    peer = std::make_unique<zwayland::wire::Connection>(sockets[0]);
    client = display.add_client(sockets[1]);
    for (const auto& global : display.globals()) {
      if (std::string_view(global.interface->name) == "wl_compositor")
        global.bind(*client, 4, 2);
      if (std::string_view(global.interface->name) == "wl_subcompositor")
        global.bind(*client, 1, 3);
    }
    require(client->find_resource(2) != nullptr && client->find_resource(3) != nullptr);
  }

  ~Fixture() { client->destroy(); }

  void request(std::uint32_t object, std::uint32_t opcode,
               std::initializer_list<std::uint32_t> arguments = {}) {
    zwayland::wire::MessageBuilder message(object);
    message.set_opcode(opcode);
    for (const auto value : arguments) message.append_uint(value);
    peer->queue(message.finish(), message.take_fds());
    require(peer->flush() && client->dispatch_available() && !client->dead());
  }

  zwwm::detail::SurfaceState* surface(std::uint32_t id) {
    auto* resource = client->find_resource(id);
    require(resource != nullptr);
    auto* state = resource->data<zwwm::detail::SurfaceState>();
    require(state != nullptr);
    return state;
  }
};
}

int main() {
  Fixture f;
  for (int iteration = 0; iteration < 1000; ++iteration) {
    f.request(2, protocol::ZWAYLAND_WL_COMPOSITOR_REQUEST_CREATE_SURFACE_OPCODE, {10});
    f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_FRAME_OPCODE, {5});
    f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_FRAME_OPCODE, {6});

    f.compositor.notify_frame_presented();
    require(f.surface(10)->pending_frame_callbacks.size() == 2);
    require(f.surface(10)->frame_callbacks.empty());
    f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_COMMIT_OPCODE);

    // Reused callback IDs can precede the surface in client teardown order.
    f.client->destroy_resource(5);
    require(f.surface(10)->frame_callbacks.size() == 1);
    f.compositor.notify_frame_presented();
    require(f.surface(10)->frame_callbacks.empty());
    require(f.client->find_resource(6) == nullptr);

    f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_FRAME_OPCODE, {5});
    f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_FRAME_OPCODE, {6});
    int presented_while_destroying = 0;
    // Surface removal synchronously invokes the backend; it can present a frame.
    struct Presentation {
      Fixture* fixture;
      int* count;
    } presentation{&f, &presented_while_destroying};
    f.compositor.set_surface_commit_observer(
        [](void* data, const zwwm::ShmBufferView&) {
          auto& state = *static_cast<Presentation*>(data);
          ++*state.count;
          state.fixture->compositor.notify_frame_presented();
        }, &presentation);
    f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_DESTROY_OPCODE);
    f.compositor.set_surface_commit_observer(nullptr, nullptr);
    require(presented_while_destroying > 0);
    require(f.client->find_resource(5) == nullptr && f.client->find_resource(6) == nullptr);

    f.request(2, protocol::ZWAYLAND_WL_COMPOSITOR_REQUEST_CREATE_SURFACE_OPCODE, {10});
    f.request(2, protocol::ZWAYLAND_WL_COMPOSITOR_REQUEST_CREATE_SURFACE_OPCODE, {11});
    f.request(3, protocol::ZWAYLAND_WL_SUBCOMPOSITOR_REQUEST_GET_SUBSURFACE_OPCODE, {12, 11, 10});
    // Both destruction orders are legal; the surviving role must be inert.
    f.request(11, protocol::ZWAYLAND_WL_SURFACE_REQUEST_DESTROY_OPCODE);
    require(f.client->find_resource(12)->data<zwwm::detail::SurfaceState>() == nullptr);
    f.request(12, protocol::ZWAYLAND_WL_SUBSURFACE_REQUEST_SET_POSITION_OPCODE, {1, 2});
    f.request(12, protocol::ZWAYLAND_WL_SUBSURFACE_REQUEST_DESTROY_OPCODE);
    require(f.surface(10)->children.empty());

    f.request(2, protocol::ZWAYLAND_WL_COMPOSITOR_REQUEST_CREATE_SURFACE_OPCODE, {11});
    f.request(3, protocol::ZWAYLAND_WL_SUBCOMPOSITOR_REQUEST_GET_SUBSURFACE_OPCODE, {12, 11, 10});
    f.request(12, protocol::ZWAYLAND_WL_SUBSURFACE_REQUEST_DESTROY_OPCODE);
    require(f.surface(11)->subsurface == nullptr && f.surface(11)->parent == nullptr);
    f.request(11, protocol::ZWAYLAND_WL_SURFACE_REQUEST_DESTROY_OPCODE);
    f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_DESTROY_OPCODE);
    f.client->flush();
    (void)f.peer->read_available();
    while (auto message = f.peer->next_message()) f.peer->consume(message->size, 0);
  }

  f.request(2, protocol::ZWAYLAND_WL_COMPOSITOR_REQUEST_CREATE_SURFACE_OPCODE, {10});
  f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_FRAME_OPCODE, {5});
  f.request(10, protocol::ZWAYLAND_WL_SURFACE_REQUEST_FRAME_OPCODE, {20});
  f.client->destroy();
  require(f.client->find_resource(5) == nullptr && f.client->find_resource(10) == nullptr &&
          f.client->find_resource(20) == nullptr);
}
