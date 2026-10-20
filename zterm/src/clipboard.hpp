#pragma once

#include <zwayland/client/display.hpp>
#include <poll.h>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace zterm {

class Clipboard {
 public:
  Clipboard(zwayland::client::Display& display, zwayland::client::Proxy& manager, zwayland::client::Proxy& seat);
  void copy(std::string text, std::uint32_t serial);
  void paste();
  void poll_descriptors(std::vector<pollfd>& descriptors) const;
  void dispatch(std::span<const pollfd> descriptors);
  std::function<void(std::string)> received;

 private:
  struct Offer { zwayland::client::Proxy* proxy; std::vector<std::string> types; };
  struct Transfer {
    zwayland::wire::FileDescriptor fd;
    std::shared_ptr<const std::string> text;
    size_t offset = 0;
  };
  zwayland::client::Display& display_;
  zwayland::client::Proxy &manager_, *device_ = nullptr;
  std::unordered_map<std::uint32_t, Offer> offers_;
  std::uint32_t selection_ = 0;
  std::vector<Transfer> outgoing_;
  zwayland::wire::FileDescriptor incoming_;
  std::string pasted_;
};

}  // namespace zterm
