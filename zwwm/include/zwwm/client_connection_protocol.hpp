#pragma once

#include <memory>
#include <zwayland/server/display.hpp>

namespace zwwm {

class ClientConnectionProtocol {
 public:
  explicit ClientConnectionProtocol(zwayland::server::Display* display);
  ~ClientConnectionProtocol();
  ClientConnectionProtocol(const ClientConnectionProtocol&) = delete;
  ClientConnectionProtocol& operator=(const ClientConnectionProtocol&) = delete;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace zwwm
