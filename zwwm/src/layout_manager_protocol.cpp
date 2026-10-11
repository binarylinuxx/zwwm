#include "zwwm/layout_manager_protocol.hpp"
#include "zwwm/compositor_server.hpp"
#include <zwwm-layout-manager-v1-zwayland-server.h>
#include <algorithm>
#include <map>
#include <limits>

namespace protocol = zwayland::generated;
namespace zwwm {
namespace {
std::uint32_t high(std::uint64_t id) { return static_cast<std::uint32_t>(id >> 32U); }
std::uint32_t low(std::uint64_t id) { return static_cast<std::uint32_t>(id); }
std::uint64_t join(std::uint32_t hi, std::uint32_t lo) { return (static_cast<std::uint64_t>(hi) << 32U) | lo; }
bool same_rect(renderer::Rect a, renderer::Rect b) {
  return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width && a.size.height == b.size.height;
}
bool same_window(const ManagedWindowInfo& a, const ManagedWindowInfo& b) {
  return a.id == b.id && a.parent == b.parent && a.state == b.state && a.app_id == b.app_id && a.title == b.title &&
         a.min_width == b.min_width && a.min_height == b.min_height && a.max_width == b.max_width && a.max_height == b.max_height;
}
}  // namespace

struct LayoutManagerProtocol::Impl {
  struct Slot {
    std::uint32_t serial = 0;
    renderer::Rect area;
    std::vector<ManagedWindowInfo> windows;
    std::vector<layout::Placement> accepted, draft;
    std::string error;
    bool committed = false, force = false;
  };
  zwayland::server::Display* display;
  CompositorServer* compositor;
  std::uint32_t global = 0, serial = 0;
  zwayland::server::Resource* provider = nullptr;
  std::vector<zwayland::server::Resource*> resources;
  std::map<std::pair<std::uint64_t, std::uint8_t>, Slot> slots;
  int pending = -1;

  void failed(zwayland::server::Resource& resource, std::uint32_t serial, const char* message) {
    protocol::zwwm_layout_provider_v1_send_failed(resource, serial, message);
  }
  Slot* find(std::uint32_t requested) {
    for (auto& [key, slot] : slots) {
      if (slot.serial != requested) continue;
      const auto output = compositor->output_info(OutputId{key.first});
      const auto tags = compositor->tags();
      if (!output || std::none_of(tags.begin(), tags.end(), [&](const auto& tag) {
        return tag.output.value == key.first && tag.active == key.second;
      })) return nullptr;
      const auto current = compositor->managed_windows();
      for (const auto& saved : slot.windows) {
        const auto found = std::find_if(current.begin(), current.end(), [&](const auto& window) { return window.id == saved.id; });
        if (found == current.end() || found->output.value != key.first || found->tag != key.second ||
            !same_window(saved, *found)) return nullptr;
      }
      return &slot;
    }
    return nullptr;
  }
  std::vector<layout::Placement> arrange(OutputId output, std::uint8_t tag, renderer::Rect area,
                                        std::span<const std::uint64_t> ids) {
    auto& slot = slots[{output.value, tag}];
    const auto all = compositor->managed_windows();
    std::vector<ManagedWindowInfo> windows;
    for (const auto id : ids) {
      const auto found = std::find_if(all.begin(), all.end(), [id](const auto& item) { return item.id == id; });
      if (found != all.end()) windows.push_back(*found);
    }
    const bool changed = slot.serial == 0 || slot.force || !same_rect(slot.area, area) || windows.size() != slot.windows.size() ||
        !std::equal(windows.begin(), windows.end(), slot.windows.begin(), same_window);
    if (changed) {
      if (++serial == 0) ++serial;
      slot.serial = serial; slot.area = area; slot.windows = windows;
      slot.draft.clear(); slot.error.clear(); slot.committed = false; slot.force = false;
      protocol::zwwm_layout_provider_v1_send_request(*provider, serial, high(output.value), low(output.value), tag,
          area.origin.x, area.origin.y, area.size.width, area.size.height, windows.size());
      for (const auto& window : windows)
        protocol::zwwm_layout_provider_v1_send_window(*provider, serial, high(window.id), low(window.id), window.state,
            window.app_id.c_str(), window.title.c_str(), high(window.parent), low(window.parent),
            window.bounds.origin.x, window.bounds.origin.y,
            window.bounds.size.width, window.bounds.size.height, window.min_width, window.min_height, window.max_width, window.max_height);
      protocol::zwwm_layout_provider_v1_send_done(*provider, serial);
    }
    std::vector<layout::Placement> placements;
    for (const auto& window : windows) {
      const auto accepted = std::find_if(slot.accepted.begin(), slot.accepted.end(), [&](const auto& item) { return item.id == window.id; });
      auto bounds = window.bounds;
      if (accepted != slot.accepted.end()) bounds = accepted->bounds;
      else if (bounds.empty()) {
        const auto width = std::max(1U, area.size.width / 2), height = std::max(1U, area.size.height / 2);
        bounds = {{area.origin.x + static_cast<std::int32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(area.size.width) - width) / 2),
                   area.origin.y + static_cast<std::int32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(area.size.height) - height) / 2)}, {width, height}};
      }
      placements.push_back({window.id, bounds});
    }
    return placements;
  }
  struct ProviderHandler {
    void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
    void request_layout(zwayland::server::Client&, zwayland::server::Resource& resource) {
      auto* self = resource.data<Impl>();
      if (self->provider != &resource) return;
      for (auto& [key, slot] : self->slots) { (void)key; slot.force = true; }
      self->compositor->reconfigure_layout();
    }
    void place(zwayland::server::Client&, zwayland::server::Resource& resource,
                std::uint32_t serial, std::uint32_t hi, std::uint32_t lo,
                std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height) {
      auto* self = resource.data<Impl>();
      if (self->provider != &resource) return;
      auto* slot = self->find(serial);
      if (slot == nullptr || slot->committed) return;
      const auto id = join(hi, lo);
      if (width == 0 || height == 0 || width > 32768 || height > 32768 ||
          static_cast<std::int64_t>(x) + width > std::numeric_limits<std::int32_t>::max() ||
          static_cast<std::int64_t>(y) + height > std::numeric_limits<std::int32_t>::max() ||
          std::none_of(slot->windows.begin(), slot->windows.end(), [id](const auto& item) { return item.id == id; }) ||
          std::any_of(slot->draft.begin(), slot->draft.end(), [id](const auto& item) { return item.id == id; })) {
        slot->error = "invalid or duplicate placement"; return;
      }
      slot->draft.push_back({id, {{x, y}, {width, height}}});
    }
    void commit(zwayland::server::Client&, zwayland::server::Resource& resource,
                 std::uint32_t serial, std::uint32_t animate) {
      auto* self = resource.data<Impl>();
      auto* slot = self->find(serial);
      if (self->provider != &resource || slot == nullptr || slot->committed) {
        self->failed(resource, serial, "stale or already committed layout request"); return;
      }
      if (animate > 1 || !slot->error.empty() || slot->draft.size() != slot->windows.size()) {
        self->failed(resource, serial, slot->error.empty() ? "batch must place every window; animate must be 0 or 1" : slot->error.c_str());
        return;
      }
      slot->accepted = slot->draft; slot->committed = true;
      self->compositor->config_batch(false);
      self->compositor->reconfigure_layout(animate != 0);
      self->compositor->config_batch(true);
      protocol::zwwm_layout_provider_v1_send_applied(resource, serial);
    }
  };
  struct ManagerHandler {
    void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
    void create_provider(zwayland::server::Client& client, zwayland::server::Resource& manager, std::uint32_t id) {
      auto* self = manager.data<Impl>();
      auto* resource = client.create_resource(&protocol::zwwm_layout_provider_v1_interface, id, 1);
      if (resource == nullptr) { client.post_no_memory(); return; }
      self->resources.push_back(resource);
      resource->set_data(self);
      resource->set_handler(protocol::zwwm_layout_provider_v1_handler(ProviderHandler{}));
      resource->set_destroy_handler([self](zwayland::server::Resource& resource) {
        std::erase(self->resources, &resource);
        if (self->provider != &resource) return;
        self->provider = nullptr;
        self->slots.clear();
        self->compositor->set_layout_provider({});
      });
      if (self->provider != nullptr) { self->failed(*resource, 0, "another layout provider is active"); return; }
      self->provider = resource;
      self->compositor->set_layout_provider([self](OutputId output, std::uint8_t tag, renderer::Rect area,
                                                   std::span<const std::uint64_t> ids) {
        return self->arrange(output, tag, area, ids);
      });
    }
  };
};

LayoutManagerProtocol::LayoutManagerProtocol(zwayland::server::Display* display, CompositorServer* compositor)
    : impl_(std::make_unique<Impl>()) {
  impl_->display = display; impl_->compositor = compositor;
  impl_->global = display->add_global(&protocol::zwwm_layout_manager_v1_interface, 1,
      [self = impl_.get()](zwayland::server::Client& client, std::uint32_t, std::uint32_t id) {
        auto* resource = client.create_resource(&protocol::zwwm_layout_manager_v1_interface, id, 1);
        if (resource == nullptr) { client.post_no_memory(); return; }
        self->resources.push_back(resource); resource->set_data(self);
        resource->set_handler(protocol::zwwm_layout_manager_v1_handler(Impl::ManagerHandler{}));
        resource->set_destroy_handler([self](zwayland::server::Resource& resource) { std::erase(self->resources, &resource); });
      });
}
LayoutManagerProtocol::~LayoutManagerProtocol() {
  if (impl_->pending >= 0) impl_->display->event_loop().remove(impl_->pending);
  impl_->display->destroy_global(impl_->global);
  while (!impl_->resources.empty()) impl_->resources.back()->destroy();
}
void LayoutManagerProtocol::invalidate() {
  if (impl_->provider == nullptr || impl_->pending >= 0) return;
  impl_->pending = impl_->display->add_idle([self = impl_.get()] {
    self->pending = -1;
    if (self->provider != nullptr) self->compositor->reconfigure_layout();
  });
}
}  // namespace zwwm
