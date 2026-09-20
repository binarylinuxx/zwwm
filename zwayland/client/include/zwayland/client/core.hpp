#pragma once

#include <cstdint>
#include <algorithm>
#include <utility>

#include <wayland-zwayland-client.h>

#include "zwayland/client/display.hpp"

namespace zwayland::client::core {

inline const Interface& display_interface = generated::wl_display_interface;
inline const Interface& registry_interface = generated::wl_registry_interface;
inline const Interface& callback_interface = generated::wl_callback_interface;

inline Proxy* get_registry(Display& display) {
  const std::uint32_t id = generated::wl_display_get_registry(
      display, display.display_proxy()->id);
  return display.find_proxy(id);
}

inline Proxy* bind(Display& display, Proxy& registry, std::uint32_t name,
                   const Interface& interface, std::uint32_t version) {
  const std::uint32_t id = generated::wl_registry_bind(
      display, registry.id, interface, version, name);
  Proxy* proxy = display.find_proxy(id);
  if (proxy != nullptr) proxy->version = std::min(version, interface.version);
  return proxy;
}

template <typename Handler>
void observe_registry(Display& display, Handler&& handler) {
  generated::wl_registry_observe(display, std::forward<Handler>(handler));
}

}  // namespace zwayland::client::core
