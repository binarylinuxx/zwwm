#include "zwayland/wire/message.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace zwayland::wire {

std::int32_t to_fixed(double value) {
  if (!std::isfinite(value)) return 0;
  const double scaled = std::round(value * 256.0);
  return static_cast<std::int32_t>(std::clamp(
      scaled, static_cast<double>(std::numeric_limits<std::int32_t>::min()),
      static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

double from_fixed(std::int32_t raw) {
  return static_cast<double>(raw) / 256.0;
}

namespace {

ArgType type_from_letter(char c) {
  switch (c) {
    case 'i': return ArgType::int32;
    case 'u': return ArgType::uint32;
    case 'f': return ArgType::fixed;
    case 's': return ArgType::string;
    case 'o': return ArgType::object;
    case 'n': return ArgType::new_id;
    case 'a': return ArgType::array;
    case 'h': return ArgType::fd;
    default: throw std::runtime_error("invalid argument type letter");
  }
}

}  // namespace

std::vector<ArgSpec> parse_signature(std::string_view signature) {
  std::vector<ArgSpec> specs;
  bool nullable = false;
  for (char c : signature) {
    if (c >= '0' && c <= '9') continue;
    if (c == '?') {
      nullable = true;
      continue;
    }
    ArgSpec spec{type_from_letter(c), nullable};
    specs.push_back(spec);
    nullable = false;
  }
  if (nullable) throw std::runtime_error("dangling nullable marker in signature");
  return specs;
}

MessageBuilder::MessageBuilder(std::uint32_t object_id) {
  // Reserve header; filled in finish().
  buffer_.resize(8);
  std::memcpy(buffer_.data(), &object_id, sizeof(object_id));
}

MessageBuilder::~MessageBuilder() {
  for (int fd : fds_) close(fd);
}

void MessageBuilder::set_opcode(std::uint32_t opcode) { opcode_ = opcode; }

void MessageBuilder::pad_to_alignment() {
  while (buffer_.size() % 4 != 0) buffer_.push_back(std::byte{0});
}

void MessageBuilder::append_int(std::int32_t value) {
  pad_to_alignment();
  const auto raw = static_cast<std::uint32_t>(value);
  buffer_.insert(buffer_.end(), reinterpret_cast<const std::byte*>(&raw),
                 reinterpret_cast<const std::byte*>(&raw) + 4);
}

void MessageBuilder::append_uint(std::uint32_t value) {
  pad_to_alignment();
  buffer_.insert(buffer_.end(), reinterpret_cast<const std::byte*>(&value),
                 reinterpret_cast<const std::byte*>(&value) + 4);
}

void MessageBuilder::append_fixed(double value) { append_int(to_fixed(value)); }

void MessageBuilder::append_string(std::string_view value) {
  if (value.size() >= std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("string is too large for the Wayland wire format");
  if (value.find('\0') != std::string_view::npos)
    throw std::invalid_argument("Wayland strings cannot contain embedded NUL bytes");
  pad_to_alignment();
  const std::uint32_t length = static_cast<std::uint32_t>(value.size()) + 1;
  append_uint(length);
  if (!value.empty())
    buffer_.insert(buffer_.end(), reinterpret_cast<const std::byte*>(value.data()),
                    reinterpret_cast<const std::byte*>(value.data()) + value.size());
  buffer_.push_back(std::byte{0});
  pad_to_alignment();
}

void MessageBuilder::append_null_string() { append_uint(0); }

void MessageBuilder::append_object(std::uint32_t id) { append_uint(id); }

void MessageBuilder::append_new_id(std::uint32_t id) { append_uint(id); }

void MessageBuilder::append_array(std::span<const std::byte> data) {
  if (data.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("array is too large for the Wayland wire format");
  pad_to_alignment();
  append_uint(static_cast<std::uint32_t>(data.size()));
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  pad_to_alignment();
}

void MessageBuilder::add_fd(int fd) {
  const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (copy < 0) throw std::runtime_error("could not duplicate Wayland FD argument");
  fds_.push_back(copy);
}

std::vector<std::byte> MessageBuilder::finish() {
  pad_to_alignment();
  if (buffer_.size() > 0xffffU || opcode_ > 0xffffU)
    throw std::length_error("Wayland message exceeds its 16-bit header fields");
  const std::uint32_t size = static_cast<std::uint32_t>(buffer_.size());
  const std::uint32_t header = (size << 16) | (opcode_ & 0xffff);
  std::memcpy(buffer_.data() + 4, &header, sizeof(header));
  return std::move(buffer_);
}

std::vector<int> MessageBuilder::take_fds() {
  std::vector<int> result;
  result.swap(fds_);
  return result;
}

FileDescriptor::~FileDescriptor() {
  if (fd_ >= 0) close(fd_);
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(other.release()) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  if (this == &other) return *this;
  if (fd_ >= 0) close(fd_);
  fd_ = other.release();
  return *this;
}

int FileDescriptor::release() {
  const int result = fd_;
  fd_ = -1;
  return result;
}

MessageParser::MessageParser(std::span<const std::byte> body,
                             std::span<const int> fds)
    : body_(body), fds_(fds) {}

bool MessageParser::at_end() const { return pos_ >= body_.size(); }

void MessageParser::ensure(std::size_t bytes) {
  if (pos_ + bytes > body_.size())
    throw std::runtime_error("message truncated while parsing argument");
}

template <typename T>
T MessageParser::read_raw() {
  ensure(sizeof(T));
  T value;
  std::memcpy(&value, body_.data() + pos_, sizeof(T));
  pos_ += sizeof(T);
  return value;
}

void MessageParser::consume_fd() {
  if (fd_pos_ >= fds_.size())
    throw std::runtime_error("message expected an FD but none were received");
  ++fd_pos_;
}

std::int32_t MessageParser::read_int() { return read_raw<std::int32_t>(); }

std::uint32_t MessageParser::read_uint() { return read_raw<std::uint32_t>(); }

double MessageParser::read_fixed() { return from_fixed(read_raw<std::int32_t>()); }

bool MessageParser::is_null_string() {
  ensure(4);
  std::uint32_t length;
  std::memcpy(&length, body_.data() + pos_, 4);
  return length == 0;
}

std::string_view MessageParser::read_string() {
  const std::uint32_t length = read_raw<std::uint32_t>();
  if (length == 0) return {};
  // length includes the trailing NUL.
  ensure(length);
  const char* begin = reinterpret_cast<const char*>(body_.data() + pos_);
  if (begin[length - 1] != '\0')
    throw std::runtime_error("Wayland string is not NUL terminated");
  if (std::memchr(begin, '\0', length - 1) != nullptr)
    throw std::runtime_error("Wayland string contains an embedded NUL byte");
  std::string_view view(begin, length - 1);
  pos_ += length;
  // Align after the full length (including the NUL already counted).
  const std::size_t aligned = (pos_ + 3U) & ~std::size_t{3U};
  ensure(aligned - pos_);
  pos_ = aligned;
  return view;
}

std::uint32_t MessageParser::read_object() { return read_raw<std::uint32_t>(); }

std::uint32_t MessageParser::read_new_id() { return read_raw<std::uint32_t>(); }

std::span<const std::byte> MessageParser::read_array() {
  const std::uint32_t length = read_raw<std::uint32_t>();
  ensure(length);
  std::span<const std::byte> view(body_.data() + pos_, length);
  pos_ += length;
  const std::size_t aligned = (pos_ + 3U) & ~std::size_t{3U};
  ensure(aligned - pos_);
  pos_ = aligned;
  return view;
}

int MessageParser::read_fd() {
  if (fd_pos_ >= fds_.size())
    throw std::runtime_error("message expected an FD but none were received");
  return fds_[fd_pos_++];
}

void MessageParser::discard(std::string_view signature) {
  for (char type : signature) {
    if (type == '?' || (type >= '0' && type <= '9')) continue;
    switch (type) {
      case 'i': (void)read_int(); break;
      case 'u': (void)read_uint(); break;
      case 'f': (void)read_fixed(); break;
      case 's': (void)read_string(); break;
      case 'o': (void)read_object(); break;
      case 'n': (void)read_new_id(); break;
      case 'a': (void)read_array(); break;
      case 'h': close(read_fd()); break;
      default: throw std::runtime_error("invalid Wayland signature");
    }
  }
}

void MessageParser::skip(const ArgSpec& spec) {
  switch (spec.type) {
    case ArgType::int32:
    case ArgType::uint32:
    case ArgType::fixed:
    case ArgType::object:
    case ArgType::new_id:
      read_raw<std::uint32_t>();
      break;
    case ArgType::string:
      (void)read_string();
      break;
    case ArgType::array:
      (void)read_array();
      break;
    case ArgType::fd:
      close(read_fd());
      break;
  }
}

}  // namespace zwayland::wire
