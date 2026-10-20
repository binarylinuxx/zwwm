#pragma once

#include <memory>
#include <zwayland/client/display.hpp>

namespace zterm {

class ClientConnection {
 public:
  explicit ClientConnection(bool required = false);
  zwayland::wire::FileDescriptor create_wayland();
  zwayland::client::Display& display() { return *display_; }
  zwayland::client::Proxy* manager() const { return manager_; }
  std::unique_ptr<zwayland::client::Display> take_display() { return std::move(display_); }
 private:
  std::unique_ptr<zwayland::client::Display> display_;
  zwayland::client::Proxy* manager_ = nullptr;
};

std::unique_ptr<zwayland::client::Display> connect_display(bool standalone);
int run_daemon();

}  // namespace zterm
