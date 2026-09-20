#pragma once

#include <cstdint>
#include <string_view>

namespace zwayland::server {

// Modern replacement for `struct wl_interface` / `struct wl_message`. The
// scanner will eventually emit these directly; for now the core defines the
// built-in wl_display / wl_registry / wl_callback metadata below.
struct Message {
  const char* name;
  const char* signature;  // libwayland-style: "u?o", "n", "usun", ...
  std::uint32_t since = 1;
};

struct Interface {
  const char* name;
  std::uint32_t version;
  const Message* methods;
  std::uint32_t method_count;
  const Message* events;
  std::uint32_t event_count;
};

}  // namespace zwayland::server
