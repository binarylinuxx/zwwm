#pragma once

#include <cstddef>
#include <cstdint>

// Plugins are C++ shared libraries exporting zwwm_plugin_init with C linkage.
// This C-compatible boundary keeps the ABI independent of the compositor's C++ classes.
inline constexpr std::uint32_t ZWWM_PLUGIN_ABI_VERSION = 2;

enum class ZwwmGestureKind : std::uint32_t { swipe = 1, pinch = 2, hold = 3 };
enum class ZwwmGesturePhase : std::uint32_t { begin = 1, update = 2, end = 3 };

struct ZwwmPluginGesture {
  ZwwmGestureKind kind;
  ZwwmGesturePhase phase;
  std::uint32_t fingers;
  std::uint64_t time_usec;
  double dx;
  double dy;
  double scale;
  double angle_delta;
  bool cancelled;
  const char* output;  // Connector name, valid only during this callback.
  const char* device;  // libinput device sysname, valid only during this callback.
};

using ZwwmPluginAction = bool (*)(void* userdata, const char* argument, char* error,
                                  std::size_t error_capacity);

struct ZwwmPluginHost {
  std::uint32_t abi_version;
  void* context;
  bool (*register_action)(void* context, const char* name, ZwwmPluginAction action, void* userdata);
  bool (*dispatch_action)(void* context, const char* name, const char* argument);
};

struct ZwwmPlugin {
  std::uint32_t abi_version;
  void* userdata;
  void (*on_event)(void* userdata, const char* event);
  void (*shutdown)(void* userdata);
  const char* name;  // Unique key in plugin-settings; copied after initialization.
  void (*on_gesture)(void* userdata, const ZwwmPluginGesture* gesture);
  bool (*validate_config)(void* userdata, const char* json, char* error,
                          std::size_t error_capacity);
  void (*on_configure)(void* userdata, const char* json);
};

extern "C" {
// Return false if initialization fails. Fill the descriptor on success.
using ZwwmPluginInit = bool (*)(const ZwwmPluginHost* host, ZwwmPlugin* plugin);
}
