#include "clipboard.hpp"

#include <wayland-zwayland-client.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <stdexcept>

namespace zterm {
namespace p = zwayland::generated;
using Proxy = zwayland::client::Proxy;
using Parser = zwayland::wire::MessageParser;
using Fd = zwayland::wire::FileDescriptor;
namespace {
constexpr std::array<const char*, 3> types{"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};
bool text_type(std::string_view value) { return std::find(types.begin(), types.end(), value) != types.end(); }
}

Clipboard::Clipboard(zwayland::client::Display& display, Proxy& manager, Proxy& seat)
    : display_(display), manager_(manager) {
  device_ = display.find_proxy(p::wl_data_device_manager_get_data_device(display, manager.id, &seat));
  device_->set_observer([this](Proxy&, std::uint32_t opcode, Parser& parser) {
    if (opcode == 0) {
      const auto id = parser.read_new_id();
      auto* proxy = display_.create_proxy(&p::wl_data_offer_interface, id, device_->version);
      if (!proxy) throw std::runtime_error("invalid clipboard offer");
      offers_.emplace(id, Offer{proxy, {}});
      proxy->set_observer([this, id](Proxy&, std::uint32_t event, Parser& data) {
        if (event == 0) {
          const auto mime = data.read_string();
          if (text_type(mime)) if (auto offer = offers_.find(id); offer != offers_.end()) offer->second.types.emplace_back(mime);
        } else data.discard("u");
      });
    } else if (opcode == 5) {
      selection_ = parser.read_object();
      for (auto it = offers_.begin(); it != offers_.end();) {
        if (it->first == selection_) { ++it; continue; }
        p::wl_data_offer_destroy(display_, it->first); it = offers_.erase(it);
      }
    } else if (opcode == 1) parser.discard("uoff?o");
    else if (opcode == 3) parser.discard("uff");
  });
}

void Clipboard::copy(std::string text, std::uint32_t serial) {
  if (text.empty() || serial == 0) return;
  auto contents = std::make_shared<const std::string>(std::move(text));
  const auto source = p::wl_data_device_manager_create_data_source(display_, manager_.id);
  auto* proxy = display_.find_proxy(source);
  proxy->set_observer([this, contents](Proxy& object, std::uint32_t opcode, Parser& parser) {
    if (opcode == 0) parser.discard("?s");
    else if (opcode == 1) {
      const auto mime = parser.read_string(); Fd fd(parser.read_fd());
      if (!text_type(mime) || outgoing_.size() >= 32) return;
      const int flags = fcntl(fd.get(), F_GETFL);
      if (flags < 0 || fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) != 0) return;
      outgoing_.push_back({std::move(fd), contents, 0});
    } else if (opcode == 2) p::wl_data_source_destroy(display_, object.id);
    else if (opcode == 5) parser.discard("u");
  });
  for (const auto mime : types) p::wl_data_source_offer(display_, source, mime);
  p::wl_data_device_set_selection(display_, device_->id, proxy, serial);
}

void Clipboard::paste() {
  const auto found = offers_.find(selection_);
  if (found == offers_.end()) return;
  const auto& offer = found->second;
  const char* mime = nullptr;
  for (const auto type : types) if (std::find(offer.types.begin(), offer.types.end(), type) != offer.types.end()) { mime = type; break; }
  if (!mime) return;
  int pipes[2];
  if (pipe2(pipes, O_CLOEXEC) != 0) return;
  if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) != 0) { close(pipes[0]); close(pipes[1]); return; }
  incoming_ = Fd(pipes[0]); Fd writer(pipes[1]); pasted_.clear();
  p::wl_data_offer_receive(display_, offer.proxy->id, mime, writer.get());
}

void Clipboard::poll_descriptors(std::vector<pollfd>& descriptors) const {
  if (incoming_.get() >= 0) descriptors.push_back({incoming_.get(), POLLIN, 0});
  for (const auto& transfer : outgoing_) descriptors.push_back({transfer.fd.get(), POLLOUT, 0});
}

void Clipboard::dispatch(std::span<const pollfd> descriptors) {
  for (const auto& descriptor : descriptors) {
    if (!descriptor.revents) continue;
    if (descriptor.fd == incoming_.get()) {
      std::array<char, 16384> buffer{};
      for (unsigned count = 0; count < 64; ++count) {
        const auto size = read(incoming_.get(), buffer.data(), buffer.size());
        if (size > 0) {
          if (pasted_.size() + size > 4 * 1024 * 1024 - 12) {
            incoming_ = Fd(); pasted_.clear(); std::fputs("zterm: paste exceeds 4 MiB\n", stderr); break;
          }
          pasted_.append(buffer.data(), size);
        } else if (size == 0) {
          incoming_ = Fd();
          if (received) received(std::move(pasted_));
          pasted_.clear(); break;
        } else if (errno == EINTR) continue;
        else if (errno == EAGAIN) break;
        else { incoming_ = Fd(); pasted_.clear(); break; }
      }
    }
    for (auto& transfer : outgoing_) if (descriptor.fd == transfer.fd.get()) {
      if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) { transfer.fd = Fd(); continue; }
      const auto size = write(transfer.fd.get(), transfer.text->data() + transfer.offset,
                               std::min(size_t{16384}, transfer.text->size() - transfer.offset));
      if (size > 0) transfer.offset += size;
      if (transfer.offset == transfer.text->size() || (size < 0 && errno != EAGAIN && errno != EINTR)) transfer.fd = Fd();
    }
  }
  std::erase_if(outgoing_, [](const Transfer& transfer) { return transfer.fd.get() < 0; });
}

}  // namespace zterm
