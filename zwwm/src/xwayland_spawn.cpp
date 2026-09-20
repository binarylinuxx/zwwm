#include "zwwm/xwayland_spawn.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace zwwm {
namespace {

constexpr int kMaximumDisplay = 32;

std::string find_on_path(std::string_view name) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) return {};
  std::string_view remaining(path);
  while (true) {
    const auto separator = remaining.find(':');
    const auto directory = separator == std::string_view::npos ? remaining : remaining.substr(0, separator);
    const auto candidate = std::filesystem::path(directory.empty() ? "." : std::string(directory)) / name;
    if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
    if (separator == std::string_view::npos) break;
    remaining.remove_prefix(separator + 1);
  }
  return {};
}

bool ensure_socket_directory() {
  if (::mkdir("/tmp/.X11-unix", 0755) == 0) return true;
  if (errno != EEXIST) return false;
  struct stat status {};
  if (::lstat("/tmp/.X11-unix", &status) != 0 || !S_ISDIR(status.st_mode)) return false;
  if (status.st_uid != 0 && status.st_uid != ::getuid()) return false;
  return (status.st_mode & S_ISVTX) != 0 || (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

int create_socket(const sockaddr_un& address, socklen_t length, const char* path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (path != nullptr) ::unlink(path);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), length) != 0 || ::listen(fd, 1) != 0) {
    if (path != nullptr) ::unlink(path);
    ::close(fd);
    return -1;
  }
  if (path != nullptr) (void)::chmod(path, 0666);
  return fd;
}

bool stale_lock(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buffer[12]{};
  const auto count = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  if (count <= 0) return false;
  char* end = nullptr;
  const long pid = std::strtol(buffer, &end, 10);
  return end != buffer && pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

}  // namespace

std::string find_xwayland_binary() {
  if (const char* override_path = std::getenv("ZWWM_XWAYLAND_BIN");
      override_path != nullptr && *override_path != '\0') {
    return ::access(override_path, X_OK) == 0 ? std::string(override_path) : std::string();
  }
  return find_on_path("Xwayland");
}

bool create_xwayland_display(XwaylandDisplay* display) {
  if (display == nullptr || !ensure_socket_directory()) return false;
  for (int number = 0; number <= kMaximumDisplay; ++number) {
    const std::string lock_path = "/tmp/.X" + std::to_string(number) + "-lock";
    int lock_fd = ::open(lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
    if (lock_fd < 0 && errno == EEXIST && stale_lock(lock_path) && ::unlink(lock_path.c_str()) == 0) {
      --number;
      continue;
    }
    if (lock_fd < 0) continue;

    const std::string socket_path = "/tmp/.X11-unix/X" + std::to_string(number);
    sockaddr_un regular{};
    regular.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(regular.sun_path)) {
      ::close(lock_fd);
      ::unlink(lock_path.c_str());
      continue;
    }
    std::memcpy(regular.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const auto regular_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);

    sockaddr_un abstract{};
    abstract.sun_family = AF_UNIX;
    abstract.sun_path[0] = '\0';
    std::memcpy(abstract.sun_path + 1, socket_path.c_str(), socket_path.size());
    const auto abstract_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + socket_path.size());

    const int abstract_fd = create_socket(abstract, abstract_length, nullptr);
    const int regular_fd = create_socket(regular, regular_length, socket_path.c_str());
    if (abstract_fd < 0 || regular_fd < 0) {
      if (abstract_fd >= 0) ::close(abstract_fd);
      if (regular_fd >= 0) ::close(regular_fd);
      ::unlink(socket_path.c_str());
      ::close(lock_fd);
      ::unlink(lock_path.c_str());
      continue;
    }

    char pid[12]{};
    const int length = std::snprintf(pid, sizeof(pid), "%010d\n", static_cast<int>(::getpid()));
    if (length != 11 || ::write(lock_fd, pid, 11) != 11) {
      ::close(abstract_fd);
      ::close(regular_fd);
      ::unlink(socket_path.c_str());
      ::close(lock_fd);
      ::unlink(lock_path.c_str());
      continue;
    }
    display->number = number;
    display->lock_fd = lock_fd;
    display->listen_fds = {abstract_fd, regular_fd};
    return true;
  }
  return false;
}

void destroy_xwayland_display(XwaylandDisplay* display) {
  if (display == nullptr) return;
  for (int& fd : display->listen_fds) {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }
  if (display->lock_fd >= 0) ::close(display->lock_fd);
  if (display->number >= 0) {
    ::unlink(("/tmp/.X11-unix/X" + std::to_string(display->number)).c_str());
    ::unlink(("/tmp/.X" + std::to_string(display->number) + "-lock").c_str());
  }
  display->lock_fd = -1;
  display->number = -1;
}

pid_t spawn_xwayland(const std::string& binary, const XwaylandDisplay& display,
                     int wayland_fd, int wm_fd, int display_fd) {
  if (binary.empty() || display.number < 0 || display.listen_fds[0] < 0 ||
      display.listen_fds[1] < 0 || wayland_fd < 0 || wm_fd < 0 || display_fd < 0) return -1;
  const pid_t child = ::fork();
  if (child != 0) return child;

  for (const int fd : {display.listen_fds[0], display.listen_fds[1], wayland_fd, wm_fd, display_fd}) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) != 0) ::_exit(126);
  }
  const std::string display_name = ":" + std::to_string(display.number);
  const std::string listen_one = std::to_string(display.listen_fds[0]);
  const std::string listen_two = std::to_string(display.listen_fds[1]);
  const std::string wayland_socket = std::to_string(wayland_fd);
  const std::string wm_socket = std::to_string(wm_fd);
  const std::string ready_socket = std::to_string(display_fd);
  (void)::setenv("WAYLAND_SOCKET", wayland_socket.c_str(), 1);
  ::execl(binary.c_str(), binary.c_str(), display_name.c_str(), "-rootless", "-core", "-noreset",
          "-listenfd", listen_one.c_str(), "-listenfd", listen_two.c_str(),
          "-displayfd", ready_socket.c_str(), "-wm", wm_socket.c_str(), nullptr);
  ::_exit(127);
}

}  // namespace zwwm
