#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct epoll_event;

namespace zwayland::server {

// A minimal epoll-backed event loop supporting file-descriptor sources, one-shot
// idle callbacks, and one-shot timer sources. Handles are opaque ints; fd
// sources are keyed by their fd while idle/timer sources use a disjoint handle
// range to avoid collisions.
class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // fd source: `cb` returns false to have the source removed. `mask` is the
  // epoll event mask (e.g. EPOLLIN). Returns the fd as the handle.
  int add_fd(int fd, int mask, std::function<bool(int fd, int mask)> cb);
  void update_fd(int fd, int mask);

  // Runs `cb` once on the next loop iteration. Wakes a blocked loop.
  int add_idle(std::function<void()> cb);

  // Runs `cb` once roughly `ms` milliseconds from now (timerfd-backed).
  // Returns a handle usable with remove().
  int add_timer(std::uint32_t ms, std::function<void()> cb);
  // Creates a persistent, initially disarmed timer. Its callback may re-arm it
  // with update_timer(), matching Wayland event-source timer semantics.
  int add_timer(std::function<void()> cb);
  void update_timer(int handle, std::uint32_t ms);

  int add_signal(int signal_number, std::function<bool(int)> cb);

  // Removes a source by its handle (fd, idle, or timer).
  void remove(int handle);

  void run();
  void stop();

 private:
  void wake();
  void remove_fd_locked(int fd);

  int epoll_fd_ = -1;
  int wake_fd_ = -1;
  std::atomic<bool> running_{false};

  struct FdSource {
    int fd;
    int mask;
    std::function<bool(int, int)> cb;
  };
  struct TimerSource {
    int fd;
    std::function<void()> cb;
    bool one_shot = false;
  };

  std::unordered_map<int, std::shared_ptr<FdSource>> fds_;  // keyed by fd
  std::unordered_set<int> owned_fds_;
  std::unordered_map<int, TimerSource> timers_;  // keyed by handle
  std::unordered_map<int, int> timer_fd_to_handle_;
  std::unordered_map<int, std::function<void()>> idles_;  // keyed by handle

  int next_handle_ = 0x10000000;
};

}  // namespace zwayland::server
