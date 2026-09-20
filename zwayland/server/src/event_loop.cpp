#include "zwayland/server/event_loop.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace zwayland::server {

EventLoop::EventLoop() {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1 failed");
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wake_fd_ >= 0) {
    epoll_event wev{};
    wev.events = EPOLLIN;
    wev.data.fd = wake_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &wev);
  }
}

EventLoop::~EventLoop() {
  for (const auto& [handle, timer] : timers_) {
    (void)handle;
    close(timer.fd);
  }
  for (int fd : owned_fds_) close(fd);
  if (epoll_fd_ >= 0) close(epoll_fd_);
  if (wake_fd_ >= 0) close(wake_fd_);
}

void EventLoop::wake() {
  if (wake_fd_ < 0) return;
  std::uint64_t one = 1;
  [[maybe_unused]] ssize_t n = write(wake_fd_, &one, sizeof(one));
}

int EventLoop::add_fd(int fd, int mask, std::function<bool(int, int)> cb) {
  if (fds_.contains(fd)) throw std::runtime_error("event source already exists");
  epoll_event ev{};
  ev.events = static_cast<std::uint32_t>(mask);
  ev.data.fd = fd;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
    throw std::runtime_error("epoll add failed");
  fds_[fd] = std::make_shared<FdSource>(FdSource{fd, mask, std::move(cb)});
  return fd;
}

void EventLoop::update_fd(int fd, int mask) {
  auto it = fds_.find(fd);
  if (it == fds_.end()) return;
  it->second->mask = mask;
  epoll_event ev{};
  ev.events = static_cast<std::uint32_t>(mask);
  ev.data.fd = fd;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
    throw std::runtime_error("epoll update failed");
}

int EventLoop::add_idle(std::function<void()> cb) {
  int handle = next_handle_++;
  idles_[handle] = std::move(cb);
  wake();
  return handle;
}

int EventLoop::add_timer(std::uint32_t ms, std::function<void()> cb) {
  const int handle = add_timer(std::move(cb));
  timers_.at(handle).one_shot = true;
  try {
    update_timer(handle, ms);
  } catch (...) {
    remove(handle);
    throw;
  }
  return handle;
}

int EventLoop::add_timer(std::function<void()> cb) {
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (tfd < 0) throw std::runtime_error("timerfd_create failed");

  int handle = next_handle_++;
  timers_[handle] = TimerSource{tfd, std::move(cb), false};
  timer_fd_to_handle_[tfd] = handle;
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = tfd;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tfd, &ev) < 0) {
    timer_fd_to_handle_.erase(tfd);
    timers_.erase(handle);
    close(tfd);
    throw std::runtime_error("epoll timer add failed");
  }
  return handle;
}

void EventLoop::update_timer(int handle, std::uint32_t ms) {
  const auto found = timers_.find(handle);
  if (found == timers_.end()) return;
  struct itimerspec spec {};
  spec.it_value.tv_sec = static_cast<std::time_t>(ms / 1000);
  spec.it_value.tv_nsec = static_cast<long>((ms % 1000) * 1000000);
  if (timerfd_settime(found->second.fd, 0, &spec, nullptr) < 0)
    throw std::runtime_error("timerfd_settime failed");
}

int EventLoop::add_signal(int signal_number, std::function<bool(int)> cb) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, signal_number);
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0)
    throw std::runtime_error("sigprocmask failed");
  const int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (fd < 0) throw std::runtime_error("signalfd failed");
  try {
    add_fd(fd, EPOLLIN, [signal_number, cb = std::move(cb)](int source_fd, int) {
    struct signalfd_siginfo info {};
    while (read(source_fd, &info, sizeof(info)) == sizeof(info)) {
      if (!cb(signal_number)) return false;
    }
    return true;
    });
  } catch (...) {
    close(fd);
    throw;
  }
  owned_fds_.insert(fd);
  return fd;
}

void EventLoop::remove(int handle) {
  if (fds_.count(handle)) {
    remove_fd_locked(handle);
    return;
  }
  auto tit = timers_.find(handle);
  if (tit != timers_.end()) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, tit->second.fd, nullptr);
    close(tit->second.fd);
    timer_fd_to_handle_.erase(tit->second.fd);
    timers_.erase(tit);
    return;
  }
  idles_.erase(handle);
}

void EventLoop::remove_fd_locked(int fd) {
  auto it = fds_.find(fd);
  if (it == fds_.end()) return;
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  fds_.erase(it);
  if (owned_fds_.erase(fd) != 0) close(fd);
}

void EventLoop::run() {
  running_.store(true);
  std::vector<epoll_event> events(16);
  struct ReadySource {
    std::shared_ptr<FdSource> fd;
    int timer_handle = -1;
  };
  std::vector<ReadySource> ready(events.size());
  while (running_.load()) {
    if (!idles_.empty()) {
      std::vector<int> handles;
      handles.reserve(idles_.size());
      for (auto& kv : idles_) handles.push_back(kv.first);
      for (int h : handles) {
        auto it = idles_.find(h);
        if (it == idles_.end()) continue;
        auto cb = std::move(it->second);
        idles_.erase(it);
        cb();
        if (!running_.load()) return;
      }
    }

    int n = epoll_wait(epoll_fd_, events.data(),
                       static_cast<int>(events.size()), -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    // Capture registration identities before any callback can close an FD and
    // register a different source under the same number within this batch.
    for (int i = 0; i < n; ++i) {
      ready[i] = {};
      const int fd = events[i].data.fd;
      if (auto it = fds_.find(fd); it != fds_.end()) ready[i].fd = it->second;
      if (auto it = timer_fd_to_handle_.find(fd); it != timer_fd_to_handle_.end())
        ready[i].timer_handle = it->second;
    }

    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      if (fd == wake_fd_) {
        std::uint64_t v = 0;
        while (read(wake_fd_, &v, sizeof(v)) > 0) {
        }
        continue;
      }

      const int handle = ready[i].timer_handle;
      if (handle >= 0) {
        auto it = timers_.find(handle);
        if (it == timers_.end() || it->second.fd != fd) continue;
        std::uint64_t v = 0;
        ssize_t count;
        do { count = read(fd, &v, sizeof(v)); } while (count < 0 && errno == EINTR);
        if (count != sizeof(v)) continue;
        auto cb = it->second.cb;
        if (it->second.one_shot) remove(handle);
        cb();
        continue;
      }

      auto fit = fds_.find(fd);
      if (fit != fds_.end() && fit->second == ready[i].fd) {
        // Keep the source alive while its callback runs: callbacks are allowed
        // to remove their own source and destroy its map entry.
        auto source = fit->second;
        if (!source->cb(fd, static_cast<int>(events[i].events))) {
          fit = fds_.find(fd);
          if (fit != fds_.end() && fit->second == source) remove_fd_locked(fd);
        }
      }
    }

    for (auto& source : ready) source = {};
    if (!running_.load()) break;
  }
}

void EventLoop::stop() {
  running_.store(false);
  wake();
}

}  // namespace zwayland::server
