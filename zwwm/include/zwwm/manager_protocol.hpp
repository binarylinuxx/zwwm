#pragma once


#include <zwayland/server/display.hpp>
#include <functional>
#include <string>
namespace zwwm {

class CompositorServer;

class ManagerProtocol {
 public:
  using Reload = std::function<std::string()>;
  using RebuildShaders = std::function<std::string()>;

  ManagerProtocol(zwayland::server::Display* display, CompositorServer* compositor, Reload reload,
                  RebuildShaders rebuild_shaders);
  ~ManagerProtocol();

  ManagerProtocol(const ManagerProtocol&) = delete;
  ManagerProtocol& operator=(const ManagerProtocol&) = delete;

  void emit(const std::string& event);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace zwwm
