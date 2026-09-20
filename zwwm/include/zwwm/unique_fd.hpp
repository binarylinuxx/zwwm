#pragma once

#include <unistd.h>

#include <utility>

namespace zwwm {

class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) (void)close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace zwwm
