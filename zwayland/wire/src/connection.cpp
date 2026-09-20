#include "zwayland/wire/connection.hpp"

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <iterator>
#include <utility>

namespace zwayland::wire {

namespace {

constexpr std::size_t kRecvChunk = 16384;
constexpr std::size_t kMaxControlBytes = 4096;
constexpr std::uint32_t kMaxMessageSize = 4096 * 16;
constexpr std::size_t kMaxBufferedInput = 16 * 1024 * 1024;
constexpr std::size_t kMaxIovecsPerSend = 64;
constexpr std::size_t kMaxFdsPerSend = 28;

}  // namespace

Connection::Connection(int fd) : fd_(fd) {}

void Connection::link(Connection& peer) {
  loopback_ = true;
  peer_ = &peer;
  if (!link_alive_) link_alive_ = std::make_shared<std::atomic<bool>>(true);
  peer.link_alive_ = link_alive_;
}

Connection::~Connection() {
  close_output_fds();
  // Any FDs not yet delivered to a handler are still ours to close.
  for (std::size_t i = input_fd_cursor_; i < input_fds_.size(); ++i) close(input_fds_[i]);
  if (link_alive_) link_alive_->store(false);
  if (fd_ >= 0) close(fd_);
}

void Connection::close_output_fds() {
  for (int fd : output_fds_) close(fd);
  output_fds_.clear();
}

void Connection::notify_data_ready() {
  if (!data_ready_) return;
  if (notifying_data_ready_) {
    data_ready_pending_ = true;
    return;
  }

  notifying_data_ready_ = true;
  do {
    data_ready_pending_ = false;
    data_ready_();
  } while (data_ready_pending_);
  notifying_data_ready_ = false;
}

std::size_t Connection::total_input() const {
  return input_bytes_;
}

const std::byte& Connection::byte_at(std::size_t pos) const {
  pos += input_start_;
  for (const auto& chunk : input_chunks_) {
    if (pos < chunk.size()) return chunk[pos];
    pos -= chunk.size();
  }
  static const std::byte zero{};
  return zero;
}

bool Connection::read_available() {
  if (loopback_) return link_alive_ && link_alive_->load() && has_input();

  bool got_any = false;
  for (;;) {
    std::vector<std::byte> chunk(kRecvChunk);
    std::byte control[kMaxControlBytes];

    struct iovec iov {
      chunk.data(), chunk.size()
    };
    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    const ssize_t n = recvmsg(fd_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0) {
      failed_ = true;
      break;
    }
    if (n == 0) {
      peer_closed_ = true;
      break;
    }
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
        continue;
      const std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
      for (std::size_t i = 0; i < count; ++i) input_fds_.push_back(fds[i]);
    }
    if ((msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
      failed_ = true;
      break;
    }
    chunk.resize(static_cast<std::size_t>(n));
    input_bytes_ += static_cast<std::size_t>(n);
    input_chunks_.push_back(std::move(chunk));
    got_any = true;
    if (input_bytes_ > kMaxBufferedInput) {
      failed_ = true;
      break;
    }
    // Drain the socket fully so we do one dispatch pass per wakeup.
  }
  return got_any;
}

bool Connection::flush() {
  if (loopback_) {
    if (!link_alive_ || !link_alive_->load() || peer_ == nullptr) {
      failed_ = true;
      return false;
    }
    if (output_chunks_.empty() && output_fds_.empty()) return true;
    // Zero-copy hand-off: splice the queued chunks straight into the peer's
    // input. FDs are NOT closed: ownership transfers within the process.
    for (const auto& chunk : output_chunks_) peer_->input_bytes_ += chunk.size();
    peer_->input_chunks_.insert(
        peer_->input_chunks_.end(),
        std::make_move_iterator(output_chunks_.begin()),
        std::make_move_iterator(output_chunks_.end()));
    output_chunks_.clear();
    peer_->input_fds_.insert(
        peer_->input_fds_.end(),
        std::make_move_iterator(output_fds_.begin()),
        std::make_move_iterator(output_fds_.end()));
    output_fds_.clear();
    peer_->notify_data_ready();
    return true;
  }

  while (!output_chunks_.empty()) {
    // Bound each gather operation below IOV_MAX without allocating an iovec
    // vector for every flush.
    std::array<struct iovec, kMaxIovecsPerSend> iovs{};
    std::size_t iov_count = 0;
    for (const auto& chunk : output_chunks_) {
      const std::size_t offset = iov_count == 0 ? output_start_ : 0;
      iovs[iov_count++] = {
          const_cast<std::byte*>(chunk.data() + offset), chunk.size() - offset};
      if (iov_count == iovs.size()) break;
    }
    // SCM_RIGHTS needs at least one payload byte. Keep bytes queued when more
    // descriptors remain so every descriptor batch has a byte to accompany it.
    if (output_fds_.size() > kMaxFdsPerSend) {
      iovs[0].iov_len = 1;
      iov_count = 1;
    }

    alignas(struct cmsghdr) std::byte control[kMaxControlBytes]{};
    struct msghdr msg {};
    msg.msg_iov = iovs.data();
    msg.msg_iovlen = iov_count;

    const std::size_t fd_count = std::min(output_fds_.size(), kMaxFdsPerSend);
    if (fd_count > 0) {
      const std::size_t needed = CMSG_SPACE(fd_count * sizeof(int));
      if (needed > sizeof(control)) {
        failed_ = true;
        return false;
      }
      msg.msg_control = control;
      msg.msg_controllen = needed;
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(fd_count * sizeof(int));
      std::memcpy(CMSG_DATA(cmsg), output_fds_.data(), fd_count * sizeof(int));
    }

    ssize_t n;
    do {
      n = sendmsg(fd_, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    if (n <= 0) {
      failed_ = true;
      return false;
    }

    // SCM_RIGHTS duplicates descriptors into the receiver. Close the sent
    // descriptors now; the connection owned them after queue().
    for (std::size_t i = 0; i < fd_count; ++i) close(output_fds_[i]);
    output_fds_.erase(output_fds_.begin(), output_fds_.begin() + fd_count);

    std::size_t sent = static_cast<std::size_t>(n);
    while (!output_chunks_.empty()) {
      const std::size_t remaining = output_chunks_.front().size() - output_start_;
      if (sent < remaining) {
        output_start_ += sent;
        break;
      }
      sent -= remaining;
      output_chunks_.pop_front();
      output_start_ = 0;
      if (sent == 0) break;
    }
  }
  return true;
}

void Connection::queue(std::vector<std::byte> bytes, std::vector<int> fds) {
  // Reserve descriptor storage before publishing the byte chunk so allocation
  // failure cannot leave half of a message queued.
  try {
    output_fds_.reserve(output_fds_.size() + fds.size());
    output_chunks_.push_back(std::move(bytes));
    output_fds_.insert(output_fds_.end(), fds.begin(), fds.end());
  } catch (...) {
    for (int fd : fds) close(fd);
    throw;
  }
}

std::optional<Message> Connection::next_message() {
  const std::size_t avail = total_input();
  if (avail < 8) return std::nullopt;

  std::uint32_t header = 0;
  for (std::size_t i = 0; i < 4; ++i)
    header |= static_cast<std::uint32_t>(byte_at(4 + i)) << (8 * i);
  const std::uint32_t size = header >> 16;
  if (size < 8 || size > kMaxMessageSize || size % 4 != 0) {
    failed_ = true;
    return std::nullopt;
  }
  if (avail < size) return std::nullopt;

  std::uint32_t object_id = 0;
  for (std::size_t i = 0; i < 4; ++i)
    object_id |= static_cast<std::uint32_t>(byte_at(i)) << (8 * i);

  Message message;
  message.object_id = object_id;
  message.opcode = header & 0xffff;
  message.size = size;

  const std::size_t body_size = size - 8;
  const std::size_t front_remaining =
      input_chunks_.empty() ? 0 : input_chunks_.front().size() - input_start_;
  if (size <= front_remaining) {
    // Fast path: whole message lives in the front chunk -- read a direct span.
    message.body = std::span<const std::byte>(
        input_chunks_.front().data() + input_start_ + 8, body_size);
  } else {
    // Boundary-straddling message: linearize into scratch (rare).
    message_scratch_.resize(body_size);
    for (std::size_t i = 0; i < body_size; ++i)
      message_scratch_[i] = byte_at(8 + i);
    message.body = std::span<const std::byte>(message_scratch_.data(), body_size);
  }

  const std::size_t fd_count = input_fds_.size() - input_fd_cursor_;
  message.fds = std::span<const int>(input_fds_.data() + input_fd_cursor_, fd_count);
  return message;
}

void Connection::consume(std::size_t size, std::size_t fds_consumed) {
  input_bytes_ -= size;
  input_start_ += size;
  input_fd_cursor_ += fds_consumed;
  while (!input_chunks_.empty() && input_start_ >= input_chunks_.front().size()) {
    input_start_ -= input_chunks_.front().size();
    input_chunks_.pop_front();
  }
  if (input_chunks_.empty()) input_start_ = 0;
  if (input_fd_cursor_ >= input_fds_.size()) {
    input_fds_.clear();
    input_fd_cursor_ = 0;
  }
}

}  // namespace zwayland::wire
