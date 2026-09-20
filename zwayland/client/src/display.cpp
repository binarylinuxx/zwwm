#include "zwayland/client/display.hpp"

#include "zwayland/client/core.hpp"

#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace zwayland::client {

namespace {

int connect_to_socket(const std::string& name) {
  const char* environment = std::getenv("WAYLAND_DISPLAY");
  const std::string socket_name = name.empty() ?
      (environment == nullptr || environment[0] == '\0' ? "wayland-0" : environment) : name;
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (socket_name.front() != '/' &&
      (runtime == nullptr || runtime[0] != '/')) return -1;
  const std::string path = socket_name.front() == '/'
                               ? socket_name
                               : std::string(runtime) + "/" + socket_name;
  if (path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  const socklen_t address_size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + path.size() + 1U);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), address_size) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

Display::Display(int fd)
    : connection_(std::make_unique<wire::Connection>(fd)) {
  auto display = std::make_shared<Proxy>();
  display->id = 1;
  display->interface = &core::display_interface;
  display->display = this;
  proxies_.emplace(1, std::move(display));
}

Display::Display(std::unique_ptr<wire::Connection> connection)
    : connection_(std::move(connection)) {
  auto display = std::make_shared<Proxy>();
  display->id = 1;
  display->interface = &core::display_interface;
  display->display = this;
  proxies_.emplace(1, std::move(display));
}

Display::~Display() = default;

std::unique_ptr<Display> Display::connect(const std::string& name) {
  if (const char* socket = std::getenv("WAYLAND_SOCKET"); socket != nullptr) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(socket, &end, 10);
    if (errno != 0 || end == socket || *end != '\0' || value < 0 ||
        value > std::numeric_limits<int>::max()) return nullptr;
    const int fd = static_cast<int>(value);
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return nullptr;
    unsetenv("WAYLAND_SOCKET");
    return std::unique_ptr<Display>(new Display(fd));
  }
  int fd = connect_to_socket(name);
  if (fd < 0) return nullptr;
  return std::unique_ptr<Display>(new Display(fd));
}

std::unique_ptr<Display> Display::adopt(std::unique_ptr<wire::Connection> connection) {
  return std::unique_ptr<Display>(new Display(std::move(connection)));
}

void Display::set_observer(const Interface* interface, EventObserver observer) {
  observers_[interface] = std::make_shared<EventObserver>(std::move(observer));
}

bool Display::dispatch() {
  if (!flush()) return false;
  if (fd() >= 0) {
    pollfd descriptor{fd(), static_cast<short>(POLLIN |
        (connection_->has_output() ? POLLOUT : 0)), 0};
    int result = 0;
    do {
      result = poll(&descriptor, 1, -1);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0)
      return false;
    if ((descriptor.revents & POLLIN) == 0 &&
        (descriptor.revents & POLLHUP) != 0) return false;
    if ((descriptor.revents & POLLOUT) != 0 && !flush()) return false;
  }
  return dispatch_pending();
}

bool Display::dispatch_pending() {
  const bool got_input = connection_->read_available();
  if (!got_input)
    return connection_->healthy() && !connection_->peer_closed() &&
           !protocol_error_;
  const bool dispatched = dispatch_messages();
  return dispatched && connection_->healthy() && !connection_->peer_closed() &&
         !protocol_error_;
}

bool Display::dispatch_messages() {
  for (;;) {
    auto message = connection_->next_message();
    if (!message) break;

    const auto proxy_found = proxies_.find(message->object_id);
    if (proxy_found == proxies_.end()) {
      const auto zombie = zombies_.find(message->object_id);
      if (zombie != zombies_.end() && message->opcode < zombie->second->event_count) {
        wire::MessageParser parser(message->body, message->fds);
        try {
          parser.discard(zombie->second->events[message->opcode].signature);
        } catch (const std::exception&) {
          connection_->consume(message->size, parser.consumed_fds());
          return false;
        }
        connection_->consume(message->size, parser.consumed_fds());
      } else {
        connection_->consume(message->size, 0);
      }
      continue;
    }
    const std::shared_ptr<Proxy> proxy_owner = proxy_found->second;
    Proxy* proxy = proxy_owner.get();
    const Interface* iface = proxy->interface;
    if (message->opcode >= iface->event_count) {
      connection_->consume(message->size, 0);
      return false;
    }
    if (iface->events[message->opcode].since > proxy->version) {
      connection_->consume(message->size, 0);
      return false;
    }
    wire::MessageParser parser(message->body, message->fds);
    try {
      if (iface == &core::display_interface && message->opcode == 0) {
        error_object_id_ = parser.read_object();
        error_code_ = parser.read_uint();
        error_message_ = std::string(parser.read_string());
        protocol_error_ = true;
      } else if (iface == &core::display_interface && message->opcode == 1) {
        const std::uint32_t id = parser.read_uint();
        if (zombies_.erase(id) == 0 && find_proxy(id) != nullptr)
          deleted_ids_.insert(id);
      } else if (proxy->observer) {
        proxy->observer(*proxy, message->opcode, parser);
      } else if (auto it = observers_.find(iface); it != observers_.end()) {
        const std::shared_ptr<EventObserver> observer = it->second;
        (*observer)(*proxy, message->opcode, parser);
      } else {
        parser.discard(iface->events[message->opcode].signature);
      }
      if (!parser.at_end()) throw std::runtime_error("event has trailing data");
    } catch (const std::exception&) {
      connection_->consume(message->size, parser.consumed_fds());
      return false;
    }
    connection_->consume(message->size, parser.consumed_fds());
  }
  return connection_->healthy() && !protocol_error_;
}

bool Display::roundtrip() {
  const std::uint32_t callback_id = alloc_id();
  Proxy* callback = create_proxy(&core::callback_interface, callback_id);
  if (callback == nullptr) return false;

  bool done = false;
  callback->set_observer([&done](Proxy&, std::uint32_t opcode,
                                 wire::MessageParser& parser) {
    if (opcode == 0) {
      (void)parser.read_uint();
      done = true;
    }
  });

  wire::MessageBuilder request(display_proxy()->id);
  request.set_opcode(0);
  request.append_new_id(callback_id);
  send_request(display_proxy()->id, 0, request.finish(), request.take_fds());
  (void)flush();
  while (!done && dispatch()) {}

  if (find_proxy(callback_id) != nullptr) destroy_proxy(callback_id);
  return done;
}

bool Display::flush() { return connection_->flush(); }

std::uint32_t Display::alloc_id() {
  for (std::uint32_t attempts = 0; attempts < 0xfefffffeU; ++attempts) {
    if (next_id_ < 2 || next_id_ >= 0xff000000U) next_id_ = 2;
    const std::uint32_t id = next_id_++;
    if (!proxies_.contains(id) && !zombies_.contains(id) &&
        !deleted_ids_.contains(id)) return id;
  }
  return 0;
}

Proxy* Display::create_proxy(const Interface* interface, std::uint32_t id,
                              std::uint32_t version, std::any userdata) {
  if (interface == nullptr || id == 0 || version == 0 ||
      zombies_.contains(id) || deleted_ids_.contains(id)) return nullptr;
  auto proxy = std::make_shared<Proxy>();
  proxy->id = id;
  proxy->version = std::min(version, interface->version);
  proxy->interface = interface;
  proxy->display = this;
  proxy->userdata = std::move(userdata);
  Proxy* result = proxy.get();
  const bool inserted = proxies_.emplace(id, proxy).second;
  if (!inserted) return nullptr;
  return result;
}

Proxy* Display::find_proxy(std::uint32_t id) {
  auto it = proxies_.find(id);
  return it == proxies_.end() ? nullptr : it->second.get();
}

void Display::destroy_proxy(std::uint32_t id) {
  const auto found = proxies_.find(id);
  if (found == proxies_.end() || id == 1) return;
  if (id < 0xff000000U) {
    if (deleted_ids_.erase(id) == 0)
      zombies_[id] = found->second->interface;
  }
  proxies_.erase(found);
}

void Display::discard_proxy(std::uint32_t id) {
  if (id != 1) proxies_.erase(id);
}

void Display::send_request([[maybe_unused]] std::uint32_t object_id,
                             std::uint32_t opcode,
                             std::vector<std::byte> bytes, std::vector<int> fds) {
  Proxy* proxy = find_proxy(object_id);
  if (protocol_error_ || proxy == nullptr)
    throw std::runtime_error("request targets an invalid proxy");
  if (opcode >= proxy->interface->method_count ||
      proxy->interface->methods[opcode].since > proxy->version)
    throw std::runtime_error("request is not supported by this proxy version");
  connection_->queue(std::move(bytes), std::move(fds));
}

}  // namespace zwayland::client
