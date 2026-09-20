#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zwayland/wire/connection.hpp"
#include "zwayland/wire/message.hpp"

namespace zwayland::client {

// Client-side protocol metadata (mirrors the server's built-in definitions).
struct Message {
  const char* name;
  const char* signature;
  std::uint32_t since = 1;
};

struct Interface {
  const char* name;
  std::uint32_t version;
  const Message* methods;
  std::uint32_t method_count;
  const Message* events;
  std::uint32_t event_count;
};

// A client-side proxy for a protocol object. userdata is type-erased.
class Display;
struct Proxy;

using EventObserver =
    std::function<void(Proxy& proxy, std::uint32_t opcode,
                       wire::MessageParser& parser)>;

struct Proxy {
  std::uint32_t id = 0;
  std::uint32_t version = 1;
  const Interface* interface = nullptr;
  Display* display = nullptr;
  std::any userdata;
  EventObserver observer;

  template <typename T>
  [[nodiscard]] T* data() const {
    const auto value = std::any_cast<T*>(&userdata);
    return value == nullptr ? nullptr : *value;
  }

  template <typename T>
  void set_data(T* value) { userdata = value; }

  void set_observer(EventObserver value) { observer = std::move(value); }
};

// Invoked for every event once the proxy and its interface are resolved.

class Display {
 public:
  // Connects to $WAYLAND_DISPLAY (or "wayland-0"). Returns nullptr on failure.
  static std::unique_ptr<Display> connect(const std::string& name = {});

  // Adopts an already-linked Connection (e.g. an in-process loopback peer)
  // instead of opening a socket.
  static std::unique_ptr<Display> adopt(std::unique_ptr<wire::Connection> connection);

  ~Display();

  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  [[nodiscard]] int fd() const { return connection_->fd(); }
  [[nodiscard]] wire::Connection& connection() { return *connection_; }

  // Registers the event observer for `interface`.
  void set_observer(const Interface* interface, EventObserver observer);

  // Reads from the socket and dispatches complete messages. Returns false on
  // hangup/error.
  [[nodiscard]] bool dispatch();

  // Dispatches bytes already available on the socket without blocking. Event
  // loop integrations call this after their fd becomes readable.
  [[nodiscard]] bool dispatch_pending();

  // Sends wl_display.sync and dispatches until its callback completes.
  [[nodiscard]] bool roundtrip();

  // Flushes outgoing requests.
  bool flush();
  [[nodiscard]] bool wants_write() const { return connection_->has_output(); }
  [[nodiscard]] bool has_error() const { return protocol_error_; }
  [[nodiscard]] std::uint32_t error_code() const { return error_code_; }
  [[nodiscard]] std::uint32_t error_object_id() const { return error_object_id_; }
  [[nodiscard]] const std::string& error_message() const { return error_message_; }

  // Allocates a client-side object id (skips the bootstrap display id 1).
  [[nodiscard]] std::uint32_t alloc_id();

  Proxy* create_proxy(const Interface* interface, std::uint32_t id,
                       std::uint32_t version = 1, std::any userdata = {});
  [[nodiscard]] Proxy* find_proxy(std::uint32_t id);
  void destroy_proxy(std::uint32_t id);
  // Rolls back a proxy whose constructor request was not queued.
  void discard_proxy(std::uint32_t id);

  // Queues a request addressed to `object_id`.
  void send_request(std::uint32_t object_id, std::uint32_t opcode,
                    std::vector<std::byte> bytes, std::vector<int> fds = {});

  // Convenience: the bootstrap wl_display proxy (id 1).
  [[nodiscard]] Proxy* display_proxy() { return find_proxy(1); }

 private:
  explicit Display(int fd);
  explicit Display(std::unique_ptr<wire::Connection> connection);
  [[nodiscard]] bool dispatch_messages();

  std::unique_ptr<wire::Connection> connection_;
  std::unordered_map<std::uint32_t, std::shared_ptr<Proxy>> proxies_;
  std::unordered_map<std::uint32_t, const Interface*> zombies_;
  std::unordered_set<std::uint32_t> deleted_ids_;
  std::unordered_map<const Interface*, std::shared_ptr<EventObserver>> observers_;
  std::uint32_t next_id_ = 2;
  bool protocol_error_ = false;
  std::uint32_t error_code_ = 0;
  std::uint32_t error_object_id_ = 0;
  std::string error_message_;
};

}  // namespace zwayland::client
