#include "zwayland/server/display.hpp"

#include <wayland-zwayland-server.h>

#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace zwayland::server {

namespace {
namespace protocol = zwayland::generated;
}  // namespace

// ---- Client ----------------------------------------------------------------

Client::Client(Display& display, int fd)
    : display_(display), connection_(std::make_unique<wire::Connection>(fd)) {
  struct ucred credentials {};
  socklen_t size = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0) {
    pid_ = credentials.pid;
    uid_ = credentials.uid;
    gid_ = credentials.gid;
  }
  // Bootstrap the wl_display object at id 1, as libwayland does.
  create_resource(&protocol::wl_display_interface, 1, 1);
}

Client::Client(Display& display, std::unique_ptr<wire::Connection> connection)
    : display_(display), connection_(std::move(connection)) {
  create_resource(&protocol::wl_display_interface, 1, 1);
}

Client::~Client() = default;

bool Client::dispatch_available() {
  const bool got_input = connection_->read_available();
  if (!got_input)
    return connection_->healthy() && !connection_->peer_closed();

  for (;;) {
    auto message = connection_->next_message();
    if (!message) break;

    const auto resource_found = resources_.find(message->object_id);
    if (resource_found == resources_.end()) {
      connection_->consume(message->size, 0);
      post_error(1, 0, "invalid object");
      return false;
    }
    const std::shared_ptr<Resource> resource_owner = resource_found->second;
    Resource* resource = resource_owner.get();

    const Interface* iface = resource->interface;
    const std::uint32_t resource_id = resource->id;
    if (message->opcode >= iface->method_count) {
      connection_->consume(message->size, 0);
      post_error(resource_id, 1, "invalid method");
      return false;
    }

    if (iface->methods[message->opcode].since > resource->version) {
      connection_->consume(message->size, 0);
      post_error(resource_id, 1, "request requires a newer object version");
      return false;
    }

    wire::MessageParser parser(message->body, message->fds);
    try {
      if (resource->handler) {
        resource->handler(*this, *resource, message->opcode, parser);
      } else if (auto* handler = display_.handler_for(iface)) {
        (*handler)(*this, *resource, message->opcode, parser);
      } else {
        throw std::runtime_error("object has no request handler");
      }
      if (!parser.at_end()) throw std::runtime_error("request has trailing data");
    } catch (const std::exception& exception) {
      connection_->consume(message->size, parser.consumed_fds());
      post_error(resource_id, 1, exception.what());
      return false;
    }
    connection_->consume(message->size, parser.consumed_fds());
    if (dead_) return false;
  }
  return connection_->healthy() && !connection_->peer_closed();
}

void Client::flush() { (void)connection_->flush(); }

std::uint32_t Client::alloc_id() {
  for (std::uint32_t attempts = 0; attempts < 0x01000000U; ++attempts) {
    if (next_server_id_ < 0xff000000U) next_server_id_ = 0xff000000U;
    const std::uint32_t id = next_server_id_++;
    if (!resources_.contains(id)) return id;
  }
  return 0;
}

Resource* Client::create_resource(const Interface* interface, std::uint32_t id,
                                   std::uint32_t version, std::any userdata) {
  if (id == 0) id = alloc_id();
  if (interface == nullptr || id == 0 || version == 0) return nullptr;
  auto resource = std::make_shared<Resource>();
  resource->id = id;
  resource->version = std::min(version, interface->version);
  resource->interface = interface;
  resource->userdata = std::move(userdata);
  resource->display = &display_;
  resource->client = this;
  Resource* result = resource.get();
  const bool inserted = resources_.emplace(id, resource).second;
  if (!inserted) return nullptr;
  return result;
}

bool Client::valid_new_id(std::uint32_t id) const {
  return id > 0 && id < 0xff000000U && !resources_.contains(id);
}

Resource* Client::find_resource(std::uint32_t id) {
  auto it = resources_.find(id);
  return it == resources_.end() ? nullptr : it->second.get();
}

void Client::destroy_resource(std::uint32_t id) {
  auto it = resources_.find(id);
  if (it == resources_.end()) return;
  const std::shared_ptr<Resource> owner = it->second;
  Resource& resource = *owner;
  resource.destroy_signal.emit_final(&resource);
  if (resource.destroy_handler) resource.destroy_handler(resource);
  if (id < 0xff000000u && !dead_) {
    wire::MessageBuilder builder(1);  // wl_display.delete_id
    builder.set_opcode(1);
    builder.append_uint(id);
    connection_->queue(builder.finish(), builder.take_fds());
    display_.client_output_changed(*this);
  }
  resources_.erase(it);
}

void Client::send_event([[maybe_unused]] std::uint32_t object_id,
                        [[maybe_unused]] std::uint32_t opcode,
                        std::vector<std::byte> bytes, std::vector<int> fds) {
  if (dead_) return;
  connection_->queue(std::move(bytes), std::move(fds));
  display_.client_output_changed(*this);
}

void Client::post_error(std::uint32_t code, std::string_view message) {
  post_error(1, code, message);
}

void Client::post_error(std::uint32_t object_id, std::uint32_t code,
                        std::string_view message) {
  if (dead_) return;
  dead_ = true;
  Resource* display_resource = find_resource(1);
  if (display_resource == nullptr) return;
  wire::MessageBuilder builder(1);
  builder.set_opcode(0);  // wl_display.error
  builder.append_object(object_id);
  builder.append_uint(code);
  builder.append_string(message);
  connection_->queue(builder.finish(), builder.take_fds());
  (void)connection_->flush();
}

void Client::post_no_memory() { post_error(2, "out of memory"); }

void Client::destroy() {
  if (destroying_) return;
  destroying_ = true;
  destroy_signal.emit_final(this);
  // Emit each resource's destroy_signal, then remove the resources. Copy the
  // ids first because destroying a resource erases it from the map.
  std::vector<std::uint32_t> ids;
  ids.reserve(resources_.size());
  for (const auto& kv : resources_) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end());
  for (std::uint32_t id : ids) {
    if (resources_.count(id)) destroy_resource(id);
  }
  flush();
  dead_ = true;
  display_.schedule_remove_client(this);
}

void Resource::destroy() {
  if (client != nullptr) client->destroy_resource(id);
}

void Resource::post_error(std::uint32_t code, std::string_view message) {
  if (client != nullptr) client->post_error(id, code, message);
}

// ---- Display --------------------------------------------------------------

Display::Display(const std::string& socket_name) {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if ((socket_name.empty() || socket_name.front() != '/') &&
      (runtime == nullptr || runtime[0] != '/'))
    throw std::runtime_error("XDG_RUNTIME_DIR is not set");

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) throw std::runtime_error("cannot create socket");

  const auto bind_socket = [&](const std::string& candidate) {
    const std::string path = candidate.front() == '/'
                                 ? candidate
                                 : std::string(runtime) + "/" + candidate;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return false;
    const std::string lock_path = path + ".lock";
    if (lock_path.size() >= sizeof(sockaddr_un::sun_path) + 5U) return false;
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR |
                                                   O_NOFOLLOW,
                             S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (lock_fd < 0) return false;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
      close(lock_fd);
      return false;
    }
    struct stat socket_stat {};
    if (lstat(path.c_str(), &socket_stat) == 0) {
      if ((socket_stat.st_mode & (S_IWUSR | S_IWGRP)) == 0 ||
          unlink(path.c_str()) < 0) {
        unlink(lock_path.c_str());
        close(lock_fd);
        return false;
      }
    } else if (errno != ENOENT) {
      unlink(lock_path.c_str());
      close(lock_fd);
      return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    const socklen_t address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1U);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), address_size) < 0) {
      unlink(lock_path.c_str());
      close(lock_fd);
      return false;
    }
    socket_path_ = path;
    lock_path_ = lock_path;
    lock_fd_ = lock_fd;
    return true;
  };
  if (socket_name.empty()) {
    for (std::uint32_t index = 0; index != 32; ++index) {
      const std::string candidate = "wayland-" + std::to_string(index);
      if (bind_socket(candidate)) {
        socket_name_ = candidate;
        break;
      }
    }
  } else if (bind_socket(socket_name)) {
    socket_name_ = socket_name;
  }
  const auto cleanup_socket = [this]() {
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
    if (!socket_path_.empty()) unlink(socket_path_.c_str());
    if (!lock_path_.empty()) unlink(lock_path_.c_str());
    if (lock_fd_ >= 0) {
      close(lock_fd_);
      lock_fd_ = -1;
    }
  };
  if (socket_name_.empty()) {
    cleanup_socket();
    throw std::runtime_error("cannot bind Wayland socket");
  }
  if (listen(listen_fd_, 128) < 0) {
    cleanup_socket();
    throw std::runtime_error("cannot listen");
  }

  try {
    loop_.add_fd(listen_fd_, EPOLLIN, [this](int, int) {
      accept_client();
      return true;
    });
  } catch (...) {
    cleanup_socket();
    throw;
  }

  // Register built-in core handlers.
  set_handler(&protocol::wl_display_interface, [this](Client& client, Resource&, uint32_t opcode,
                                        wire::MessageParser& parser) {
    if (opcode == 0) {  // sync
      const uint32_t id = parser.read_new_id();
      Resource* cb = client.create_resource(&protocol::wl_callback_interface, id, 1);
      if (cb == nullptr) {
        client.post_no_memory();
        return;
      }
      wire::MessageBuilder done(id);
      done.set_opcode(0);
      done.append_uint(next_serial_);
      client.send_event(id, 0, done.finish(), done.take_fds());
      cb->destroy();
    } else if (opcode == 1) {  // get_registry
      const uint32_t id = parser.read_new_id();
      Resource* registry = client.create_resource(&protocol::wl_registry_interface, id, 1);
      if (registry == nullptr) {
        client.post_no_memory();
        return;
      }
      client.registry_ids.insert(id);
      registry->set_destroy_handler([&client, id](Resource&) {
        client.registry_ids.erase(id);
      });
      notify_registry(client, id);
    }
  });

  set_handler(&protocol::wl_registry_interface, [this](Client& client, Resource& registry, uint32_t,
                                         wire::MessageParser& parser) {
    const uint32_t name = parser.read_uint();
    const std::string iface = std::string(parser.read_string());
    const uint32_t version = parser.read_uint();
    const uint32_t id = parser.read_new_id();
    for (const auto& global : globals_) {
      if (global.name != name) continue;
      if (!global_visible(client, global)) break;
      if (global.interface->name != iface) {
        registry.post_error(0, "interface mismatch for global");
        return;
      }
      if (version == 0 || global.version < version) {
        registry.post_error(0, "invalid version for global");
        return;
      }
      if (global.bind) global.bind(client, version, id);
      return;
    }
    registry.post_error(0, "unknown global");
  });
}

Display::~Display() {
  destroy_signal.emit_final(this);
  for (auto& client : clients_) client->destroy();
  clients_.clear();
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    if (!socket_path_.empty()) unlink(socket_path_.c_str());
  }
  if (!lock_path_.empty()) unlink(lock_path_.c_str());
  if (lock_fd_ >= 0) close(lock_fd_);
}

Client* Display::add_loopback_client(std::unique_ptr<wire::Connection> connection) {
  auto client = std::make_unique<Client>(*this, std::move(connection));
  Client* raw = client.get();
  // Synchronous in-process dispatch: when the peer pushes data, process it and
  // flush any replies back through the same link.
  raw->connection().set_data_ready([raw]() {
    if (raw->dispatch_available()) raw->flush();
    else raw->destroy();
  });
  clients_.push_back(std::move(client));
  return raw;
}

Client* Display::add_client(int fd) {
  auto client = std::make_unique<Client>(*this, fd);
  Client* raw = client.get();
  loop_.add_fd(fd, EPOLLIN, [this, raw](int source_fd, int mask) {
    if ((mask & (EPOLLERR | EPOLLHUP)) != 0 ||
        ((mask & EPOLLIN) != 0 && !raw->dispatch_available())) {
      raw->destroy();
      return false;
    }
    raw->flush();
    loop_.update_fd(source_fd,
                    EPOLLIN | (raw->connection().has_output() ? static_cast<std::uint32_t>(EPOLLOUT) : 0U));
    return true;
  });
  clients_.push_back(std::move(client));
  return raw;
}

std::uint32_t Display::add_global(const Interface* interface,
                                   std::uint32_t version, BindHandler bind) {
  if (interface == nullptr || version == 0)
    throw std::invalid_argument("global requires an interface and version");
  Global global;
  global.interface = interface;
  if (version > interface->version)
    throw std::invalid_argument("global version exceeds interface version");
  if (next_global_name_ == std::numeric_limits<std::uint32_t>::max())
    throw std::overflow_error("Wayland global name space is exhausted");
  global.version = version;
  global.name = next_global_name_++;
  global.bind = std::move(bind);
  globals_.push_back(global);
  const Global announced = globals_.back();
  for (auto& client : clients_) notify_global(*client, announced);
  return global.name;
}

void Display::set_handler(const Interface* interface, RequestHandler handler) {
  handlers_[interface] = std::move(handler);
}

RequestHandler* Display::handler_for(const Interface* interface) {
  auto it = handlers_.find(interface);
  return it == handlers_.end() ? nullptr : &it->second;
}

void Display::notify_registry(Client& client, std::uint32_t registry_id) {
  for (const auto& global : globals_) {
    if (global.removed || !global_visible(client, global)) continue;
    wire::MessageBuilder builder(registry_id);
    builder.set_opcode(0);  // wl_registry.global
    builder.append_uint(global.name);
    builder.append_string(global.interface->name);
    builder.append_uint(global.version);
    client.send_event(registry_id, 0, builder.finish(), builder.take_fds());
  }
}

void Display::notify_global(Client& client, const Global& global) {
  if (!global_visible(client, global)) return;
  for (const std::uint32_t registry_id : client.registry_ids) {
    wire::MessageBuilder builder(registry_id);
    builder.set_opcode(0);  // wl_registry.global
    builder.append_uint(global.name);
    builder.append_string(global.interface->name);
    builder.append_uint(global.version);
    client.send_event(registry_id, 0, builder.finish(), builder.take_fds());
  }
}

void Display::accept_client() {
  int client_fd = accept4(listen_fd_, nullptr, nullptr,
                          SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (client_fd < 0) return;
  add_client(client_fd);
}

void Display::remove_client(Client* client) {
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if (it->get() == client) {
      int fd = client->fd();
      loop_.remove(fd);
      clients_.erase(it);
      return;
    }
  }
}

void Display::schedule_remove_client(Client* client) {
  loop_.add_idle([this, client]() { remove_client(client); });
}

void Display::client_output_changed(Client& client) {
  if (client.connection().loopback()) {
    client.flush();
    return;
  }
  if (client.fd() >= 0)
    loop_.update_fd(client.fd(), EPOLLIN | EPOLLOUT);
}

void Display::run() { loop_.run(); }

void Display::flush_clients() {
  for (auto& client : clients_) {
    client->flush();
    if (client->fd() >= 0)
      loop_.update_fd(client->fd(), EPOLLIN |
          (client->connection().has_output() ? static_cast<std::uint32_t>(EPOLLOUT) : 0U));
  }
}

void Display::set_global_filter(
    std::function<bool(const Client&, const Global&)> filter) {
  global_filter_ = std::move(filter);
}

bool Display::global_visible(const Client& client, const Global& global) const {
  return !global_filter_ || global_filter_(client, global);
}

void Display::remove_global(std::uint32_t name) {
  auto global = std::find_if(globals_.begin(), globals_.end(),
                             [name](const Global& item) { return item.name == name; });
  if (global == globals_.end() || global->removed) return;
  global->removed = true;
  const Global removed = *global;
  for (auto& client : clients_) {
    if (!global_visible(*client, removed)) continue;
    for (const std::uint32_t registry_id : client->registry_ids) {
      wire::MessageBuilder builder(registry_id);
      builder.set_opcode(1);  // wl_registry.global_remove
      builder.append_uint(name);
      client->send_event(registry_id, 1, builder.finish(), builder.take_fds());
    }
  }
}

void Display::destroy_global(std::uint32_t name) {
  remove_global(name);
  std::erase_if(globals_, [name](const Global& global) {
    return global.name == name;
  });
}

void Display::stop() { loop_.stop(); }

std::uint32_t Display::next_serial() { return ++next_serial_; }

}  // namespace zwayland::server
