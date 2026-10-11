#pragma once
#include <memory>
namespace zwayland::server { class Display; }
namespace zwwm {
class CompositorServer;
class LayoutManagerProtocol {
 public:
  LayoutManagerProtocol(zwayland::server::Display* display, CompositorServer* compositor);
  ~LayoutManagerProtocol();
  LayoutManagerProtocol(const LayoutManagerProtocol&) = delete;
  LayoutManagerProtocol& operator=(const LayoutManagerProtocol&) = delete;
  void invalidate();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace zwwm
