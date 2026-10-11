#pragma once
#include <functional>
#include <memory>
#include <string>
#include "zwwm/plugin.hpp"

namespace zwayland::server { class Display; }
namespace zwwm {
class RuntimeConfig;
class CompositorServer;
class WindowManagerProtocol {
 public:
  using Current = std::function<std::shared_ptr<const RuntimeConfig>()>;
  using Apply = std::function<std::string(std::shared_ptr<const RuntimeConfig>)>;
  WindowManagerProtocol(zwayland::server::Display* display, CompositorServer* compositor, Current current, Apply apply);
  ~WindowManagerProtocol();
  WindowManagerProtocol(const WindowManagerProtocol&) = delete;
  WindowManagerProtocol& operator=(const WindowManagerProtocol&) = delete;
  void config_changed();
  void emit(const std::string& event);
  void gesture(const ZwwmPluginGesture& gesture);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace zwwm
