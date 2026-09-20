#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace zwayland::wire {

// Wayland signature types: i, u, f, s, o, n, a, h.
enum class ArgType { int32, uint32, fixed, string, object, new_id, array, fd };

struct ArgSpec {
  ArgType type{};
  bool nullable = false;
};

// `?` marks the immediately following argument nullable.
[[nodiscard]] std::vector<ArgSpec> parse_signature(std::string_view signature);

// Fixed-point helpers: wire stores fixed as signed 23.8 (1/256) integers.
[[nodiscard]] std::int32_t to_fixed(double value);
[[nodiscard]] double from_fixed(std::int32_t raw);

// Rejects raw pointers to keep nullptr out of string_view construction.
class NullableString {
 public:
  NullableString() = default;
  NullableString(std::nullopt_t) {}
  NullableString(std::string_view value) : value_(value) {}
  NullableString(std::optional<std::string_view> value) : value_(value) {}
  NullableString(const char*) = delete;

  [[nodiscard]] explicit operator bool() const { return value_.has_value(); }
  [[nodiscard]] std::string_view operator*() const { return *value_; }

 private:
  std::optional<std::string_view> value_;
};

static_assert(!std::is_constructible_v<NullableString, const char*>);
static_assert(std::is_constructible_v<NullableString, std::string_view>);
static_assert(std::is_constructible_v<NullableString, std::nullopt_t>);

// Builds one message; FDs remain out-of-band for SCM_RIGHTS.
class MessageBuilder {
 public:
  explicit MessageBuilder(std::uint32_t object_id);
  ~MessageBuilder();

  MessageBuilder(const MessageBuilder&) = delete;
  MessageBuilder& operator=(const MessageBuilder&) = delete;

  void set_opcode(std::uint32_t opcode);
  void append_int(std::int32_t value);
  void append_uint(std::uint32_t value);
  void append_fixed(double value);
  void append_string(std::string_view value);   // empty string != null
  void append_null_string();
  void append_object(std::uint32_t id);          // 0 == null object
  void append_new_id(std::uint32_t id);
  void append_array(std::span<const std::byte> data);
  void add_fd(int fd);

  // Serializes the aligned message and transfers it to the caller.
  [[nodiscard]] std::vector<std::byte> finish();
  [[nodiscard]] std::vector<int> take_fds();

 private:
  std::vector<std::byte> buffer_;
  std::vector<int> fds_;
  std::uint32_t opcode_ = 0;
  void pad_to_alignment();
};

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor();
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept;
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;

  [[nodiscard]] int get() const { return fd_; }
  [[nodiscard]] int release();

 private:
  int fd_ = -1;
};

// Decoded header with non-owning body and FD views.
struct Message {
  std::uint32_t object_id = 0;
  std::uint32_t opcode = 0;
  std::uint32_t size = 0;
  std::span<const std::byte> body;
  std::span<const int> fds;
};

class MessageParser {
 public:
  MessageParser(std::span<const std::byte> body, std::span<const int> fds);

  [[nodiscard]] bool at_end() const;
  [[nodiscard]] std::int32_t read_int();
  [[nodiscard]] std::uint32_t read_uint();
  [[nodiscard]] double read_fixed();
  // Check is_null_string() before reading a nullable string.
  [[nodiscard]] bool is_null_string();
  [[nodiscard]] std::string_view read_string();
  [[nodiscard]] std::uint32_t read_object();
  [[nodiscard]] std::uint32_t read_new_id();
  [[nodiscard]] std::span<const std::byte> read_array();
  [[nodiscard]] int read_fd();
  void discard(std::string_view signature);
  void skip(const ArgSpec& spec);

  // Used by Connection to advance its FD cursor.
  [[nodiscard]] std::size_t consumed_fds() const { return fd_pos_; }

 private:
  std::span<const std::byte> body_;
  std::size_t pos_ = 0;
  std::span<const int> fds_;
  std::size_t fd_pos_ = 0;

  void ensure(std::size_t bytes);
  template <typename T>
  T read_raw();
  void consume_fd();
};

}  // namespace zwayland::wire
