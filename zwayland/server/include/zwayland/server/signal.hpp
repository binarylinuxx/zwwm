#pragma once

#include <cstddef>
#include <algorithm>
#include <vector>
#include <utility>

namespace zwayland::server {

// Listener modeled on `struct wl_listener`. The notify callback receives a
// pointer to this listener and an opaque data pointer supplied by `Signal::emit`.
struct Listener {
  void (*notify)(Listener* self, void* data) = nullptr;
};

// A signal modeled on `struct wl_signal`. Listeners are plain objects whose
// address is registered; they must outlive the signal or be removed first.
class Signal {
 public:
  void add(Listener& l) {
    if (std::find(listeners_.begin(), listeners_.end(), &l) == listeners_.end())
      listeners_.push_back(&l);
  }

  void remove(Listener& l) {
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
      if (*it == &l) {
        listeners_.erase(it);
        return;
      }
    }
  }

  // Notify every registered listener. The listener list is copied first so a
  // listener may safely add or remove other listeners (including itself)
  // without invalidating the iteration.
  void emit(void* data) {
    auto snapshot = listeners_;
    for (Listener* l : snapshot) {
      if (l != nullptr &&
          std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end() &&
          l->notify != nullptr)
        l->notify(l, data);
    }
  }

  void emit_final(void* data) {
    auto snapshot = std::move(listeners_);
    listeners_.clear();
    for (Listener* listener : snapshot) {
      if (listener != nullptr && listener->notify != nullptr)
        listener->notify(listener, data);
    }
  }

 private:
  std::vector<Listener*> listeners_;
};

}  // namespace zwayland::server
