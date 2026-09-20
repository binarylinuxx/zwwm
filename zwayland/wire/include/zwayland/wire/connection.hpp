#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "zwayland/wire/message.hpp"

struct iovec;

namespace zwayland::wire {

// Chunked Wayland transport with explicit FD ownership and zero-copy loopback.
class Connection {
 public:
  explicit Connection(int fd);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  [[nodiscard]] int fd() const { return fd_; }

  // Drains available input; false indicates hangup or error.
  [[nodiscard]] bool read_available();

  // Sends all queued messages. Returns false on error.
  [[nodiscard]] bool flush();

  // Takes ownership of the message and FDs until flush.
  void queue(std::vector<std::byte> bytes, std::vector<int> fds);

  // Returned views remain valid until the next read or consume.
  [[nodiscard]] std::optional<Message> next_message();

  // Consumes the previous message; delivered FDs belong to the caller.
  void consume(std::size_t size, std::size_t fds_consumed);

  // Both loopback endpoints must link to each other.
  void link(Connection& peer);
  void set_data_ready(std::function<void()> ready) { data_ready_ = std::move(ready); }
  [[nodiscard]] bool has_input() const { return total_input() > 0; }
  [[nodiscard]] bool has_output() const {
    return !output_chunks_.empty() || !output_fds_.empty();
  }
  [[nodiscard]] bool loopback() const { return loopback_; }
  [[nodiscard]] bool healthy() const {
    return !failed_ && (!loopback_ || (link_alive_ && link_alive_->load()));
  }
  [[nodiscard]] bool peer_closed() const { return peer_closed_; }

 private:
  [[nodiscard]] std::size_t total_input() const;
  // Byte at global position `pos` measured from the first unread byte.
  [[nodiscard]] const std::byte& byte_at(std::size_t pos) const;

  int fd_ = -1;
  bool loopback_ = false;
  Connection* peer_ = nullptr;
  // Avoids dereferencing a destroyed loopback peer.
  std::shared_ptr<std::atomic<bool>> link_alive_;
  std::function<void()> data_ready_;
  bool notifying_data_ready_ = false;
  bool data_ready_pending_ = false;
  bool peer_closed_ = false;
  bool failed_ = false;

  std::deque<std::vector<std::byte>> input_chunks_;
  std::size_t input_start_ = 0;  // offset into input_chunks_.front()
  std::size_t input_bytes_ = 0;
  std::deque<std::vector<std::byte>> output_chunks_;
  std::size_t output_start_ = 0;  // offset into output_chunks_.front()
  std::vector<int> output_fds_;
  std::vector<int> input_fds_;
  std::size_t input_fd_cursor_ = 0;
  std::vector<std::byte> message_scratch_;  // used only for boundary-straddling messages

  void close_output_fds();
  void notify_data_ready();
};

}  // namespace zwayland::wire
