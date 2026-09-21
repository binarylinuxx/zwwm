#include "zwwm/compositor_server.hpp"
#include "compositor_server_internal.hpp"
#ifdef ZWWM_XWAYLAND
#include "compositor_server_xwayland_internal.hpp"
#endif

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <xdg-shell-zwayland-server.h>
#include <xdg-decoration-zwayland-server.h>
#include <xdg-dialog-zwayland-server.h>
#include <linux-dmabuf-zwayland-server.h>
#include <cursor-shape-zwayland-server.h>
#include <pointer-constraints-zwayland-server.h>
#include <relative-pointer-zwayland-server.h>
#include <viewporter-zwayland-server.h>
#include <fractional-scale-zwayland-server.h>
#include <session-lock-zwayland-server.h>
#include <zwwm-tags-unstable-v1-zwayland-server.h>
#include <zwwm-data-control-v1-zwayland-server.h>
#include <wlr-data-control-unstable-v1-zwayland-server.h>
#include <wlr-layer-shell-unstable-v1-zwayland-server.h>
#include <xwlr-layer-shell-v1-zwayland-server.h>
#ifdef ZWWM_XWAYLAND
#include <xwayland-shell-zwayland-server.h>
#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/xfixes.h>
#include "zwwm/xwayland_spawn.hpp"
#endif
#include <xkbcommon/xkbcommon.h>
#include <libdrm/drm_fourcc.h>
#include <linux/input-event-codes.h>

#include "zwwm/layout/master_stack.hpp"
#include "zwwm/runtime_config.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>

namespace protocol = zwayland::generated;

namespace zwwm {
namespace detail {
constexpr std::uint32_t kCompositorVersion = 4, kXdgVersion = 3;
constexpr std::uint32_t kOutputVersion = 4, kSubcompositorVersion = 1;
constexpr std::uint32_t kDataDeviceManagerVersion = 3;
constexpr std::uint32_t kDecorationVersion = 2;
constexpr std::int32_t kOutputWidth = 960, kOutputHeight = 540;
void notify_surface(SurfaceState* surface);
void end_interactive(SeatState* seat);
void set_keyboard_focus(SeatState* seat, SurfaceState* next);
void selection_focus_changed(SeatState* seat, zwayland::server::Client* old_client, zwayland::server::Client* new_client);
void region_subtract(RegionState* region, Rect cut);
void configure_layout(Observer* observer, XdgSurfaceState* candidate);
#ifdef ZWWM_XWAYLAND
void focus_xwayland_if_needed(SurfaceState* surface);
#endif
ProtocolGlobals& ProtocolGlobals::operator=(zwayland::server::Display* next) {
    display = next;
    viewporter = next->add_global(&protocol::wp_viewporter_interface, 1, [](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_viewporter)(&client, nullptr, bound_version, id); });
    fractional_scale_manager = next->add_global(&protocol::wp_fractional_scale_manager_v1_interface, 1, [data = observer](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_fractional_scale_manager)(&client, data, bound_version, id); });
    layer_shell = next->add_global(&protocol::zwlr_layer_shell_v1_interface, 4, [data = observer](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_layer_shell)(&client, data, bound_version, id); });
    xwlr_layer_shell = next->add_global(&protocol::xwlr_layer_shell_v1_interface, 1, [data = observer](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_xwlr_layer_shell)(&client, data, bound_version, id); });
    relative_pointer_manager = next->add_global(&protocol::zwp_relative_pointer_manager_v1_interface, 1, [data = observer->seat](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_relative_pointer_manager)(&client, data, bound_version, id); });
    idle_notifier = next->add_global(&protocol::ext_idle_notifier_v1_interface, 2, [data = observer->seat](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_idle_notifier)(&client, data, bound_version, id); });
    return *this;
}
ProtocolGlobals::~ProtocolGlobals() {
    if (display == nullptr) return;
    if (idle_notifier != 0) display->destroy_global(idle_notifier);
    if (relative_pointer_manager != 0) display->destroy_global(relative_pointer_manager);
    if (xwlr_layer_shell != 0) display->destroy_global(xwlr_layer_shell);
    if (layer_shell != 0) display->destroy_global(layer_shell);
    if (fractional_scale_manager != 0) display->destroy_global(fractional_scale_manager);
    if (viewporter != 0) display->destroy_global(viewporter);
}
#ifdef ZWWM_XWAYLAND
void focus_xwayland_if_needed(SurfaceState* surface) {
  auto* observer = surface == nullptr ? nullptr : surface->observer;
  if (observer != nullptr && observer->seat != nullptr && observer->seat->keyboard_focus == nullptr)
    set_keyboard_focus(observer->seat, surface);
}
#endif
SurfaceState::~SurfaceState() {
  if (lock_surface != nullptr) lock_surface->surface = nullptr;
#ifdef ZWWM_XWAYLAND
  const bool xwayland_was_mapped = xwayland_surface != nullptr && xwayland_surface->window != nullptr &&
                                   xwayland_surface->window->mapped;
  if (xwayland_surface != nullptr) {
    auto* role = xwayland_surface;
    xwayland_surface = nullptr;
    role->surface = nullptr;
    if (role->window != nullptr) { role->window->surface = nullptr; role->window = nullptr; }
    if (role->resource == nullptr) {
      if (role->runtime != nullptr) role->runtime->remove_role(role);
      else delete role;
    }
  }
  if (xwayland_was_mapped && observer != nullptr) configure_layout(observer, nullptr);
  if (xwayland_was_mapped && observer != nullptr && observer->seat != nullptr &&
      observer->seat->keyboard_focus == nullptr && observer->surfaces != nullptr) {
    for (auto it = observer->surfaces->rbegin(); it != observer->surfaces->rend(); ++it) {
      auto* role = (*it)->xwayland_surface;
      if ((*it)->parent == nullptr && role != nullptr && role->window != nullptr && role->window->mapped &&
          !role->window->override_redirect) {
        set_keyboard_focus(observer->seat, *it);
        break;
      }
    }
  }
#endif
  if (observer != nullptr && observer->seat != nullptr && observer->seat->cursor_hidden_for_lock &&
      std::none_of(observer->seat->constraints.begin(), observer->seat->constraints.end(),
                   [](const PointerConstraint* constraint) { return constraint->active && constraint->locked; })) {
    observer->seat->cursor_hidden_for_lock = false;
    apply_cursor_shape(observer->seat);
  }
  *alive = false;
}
struct XdgSurfaceState {
  zwayland::server::Resource* resource = nullptr;
  SurfaceState* surface = nullptr;
  zwayland::server::Resource* toplevel = nullptr;
  PopupState* popup = nullptr;
  zwayland::server::Resource* decoration = nullptr;
  DialogState* dialog = nullptr;
  XdgSurfaceState* transient_parent = nullptr;
  OutputId output;
  std::vector<std::uint32_t> serials;
  Rect tile_bounds{0, 0, 0, 0};
  Rect content_bounds{0, 0, 0, 0};
  Rect window_geometry{0, 0, 0, 0};
  Rect floating_bounds{0, 0, 0, 0};
  Rect last_floating_bounds{0, 0, 0, 0};
  CanvasBounds canvas_bounds;
  bool floating = false;
  bool fullscreen = false;
  bool restore_floating = false;
  bool rule_applied = false;
  std::uint32_t background_blur_radius = 0;
  bool glass = false;
  float rule_opacity = 1.0F;
  std::string window_shader;
  std::string border_shader;
  std::uint8_t tag = 1;
  std::int32_t min_width = 0, min_height = 0, max_width = 0, max_height = 0;
  std::int32_t pending_min_width = 0, pending_min_height = 0;
  std::int32_t pending_max_width = 0, pending_max_height = 0;
  std::int32_t configured_width = 0, configured_height = 0;
  std::uint32_t configured_states = 0;
  std::uint32_t last_sent_serial = 0;
  std::uint32_t last_acked_serial = 0;
  std::uint32_t size_configure_serial = 0;
  std::int32_t accepted_width = 0, accepted_height = 0;
  std::uint64_t commit_count = 0, ack_commit_count = 0, size_ack_commit_count = 0;
  std::uint32_t decoration_preference = 0;
  bool mapped = false;
  bool ever_committed = false;
  bool has_acked_configure = false;
  bool size_configure_acked = false;
  bool content_ready = false;
  bool window_geometry_set = false;
  std::string title;
  std::string app_id;
};
struct DialogState { zwayland::server::Resource* resource = nullptr; XdgSurfaceState* xdg = nullptr; bool modal = false; };
struct PopupState { zwayland::server::Resource* resource = nullptr; XdgSurfaceState* xdg = nullptr; XdgSurfaceState* parent = nullptr; SurfaceState* layer_parent = nullptr; PositionerState positioner; Rect last_geometry{0, 0, 0, 0}; bool has_geometry = false; bool grabbed = false; bool dismissed = false; };
std::uint32_t timestamp_ms() { static std::uint32_t last = 0; const auto n = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); return last = n <= last ? last + 1 : n; }
SurfaceState* root(SurfaceState* s) { while (s != nullptr && s->parent != nullptr) s = s->parent; return s; }
bool belongs_to_popup(const SurfaceState* surface) {
  for (auto* item = surface; item != nullptr; item = item->parent)
    if (item->xdg_surface != nullptr && item->xdg_surface->popup != nullptr) return true;
  return false;
}
Rect effective_window_geometry(const SurfaceState* surface);
void absolute_position(const SurfaceState* s, std::int32_t* x, std::int32_t* y) {
  *x = 0; *y = 0;
  for (; s != nullptr; s = s->parent) {
    *x += s->x; *y += s->y;
    if (s->xdg_surface != nullptr && s->xdg_surface->popup != nullptr) {
      const Rect geometry = effective_window_geometry(s);
      *x -= geometry.x; *y -= geometry.y;
    }
  }
}
void surface_local_from_global(const SurfaceState* surface, std::int32_t global_x, std::int32_t global_y,
                               std::int32_t* local_x, std::int32_t* local_y);
void surface_global_from_local(const SurfaceState* surface, std::int32_t local_x, std::int32_t local_y,
                               std::int32_t* global_x, std::int32_t* global_y);
void append_state(std::vector<std::uint32_t>& states, std::uint32_t state) { states.push_back(state); }
constexpr std::uint32_t kActivatedState = 1U;
constexpr std::uint32_t kManagedTiledStates = 2U;
constexpr std::uint32_t kFullscreenState = 4U;
constexpr std::uint32_t kSuppressClientDecorations = 8U;
bool same_rect(const Rect& left, const Rect& right) {
  return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}
void notify_surface(SurfaceState* s);
void notify_surface_tree(SurfaceState* s);
void send_popup_configure(PopupState* popup, bool force = false,
                          std::optional<std::uint32_t> reposition_token = std::nullopt);
void activate_popup_grab(PopupState* popup);
Rect surface_local_bounds(const SurfaceState* surface);
void popup_done(PopupState* popup);
void set_keyboard_focus(SeatState* seat, SurfaceState* next);
void refresh_layer_keyboard_focus(Observer* observer);
void set_pointer_focus(SeatState* seat, SurfaceState* next, std::int32_t x, std::int32_t y);
void send_pointer_frame(SeatState* seat, zwayland::server::Client* client);
void refresh_tree_stacking(SurfaceState* surface);
std::uint64_t next_root_order() { static std::uint64_t next = 1; return next++; }
void raise_root(SurfaceState* surface) {
  auto* top = root(surface);
  if (top == nullptr || top->observer == nullptr || top->observer->surfaces == nullptr) return;
  std::uint64_t highest = top->root_order;
  for (const auto* candidate : *top->observer->surfaces)
    if (candidate->parent == nullptr) highest = std::max(highest, candidate->root_order);
  if (top->root_order == highest) return;
  top->root_order = next_root_order();
  notify_surface_tree(top);
}
void configure_layer_surface(LayerSurfaceState* layer, Rect area);
Rect arrange_layers(Observer* observer, OutputId output);
OutputState* output_state(Observer* observer, OutputId id) {
  if (observer == nullptr || observer->outputs == nullptr || !id) return nullptr;
  const auto found = std::find_if(observer->outputs->begin(), observer->outputs->end(),
                                  [id](const auto& output) { return output->info.id == id && !output->retired; });
  return found == observer->outputs->end() ? nullptr : found->get();
}
const OutputConfig& output_config(const Observer* observer, OutputId id) {
  if (observer != nullptr && observer->config != nullptr && observer->outputs != nullptr) {
    const auto output = std::find_if(observer->outputs->begin(), observer->outputs->end(), [id](const auto& item) {
      return !id || item->info.id == id;
    });
    if (output != observer->outputs->end()) return observer->config->output_for((*output)->info.connector);
  }
  static const OutputConfig defaults;
  return observer == nullptr || observer->config == nullptr ? defaults : observer->config->output;
}
OutputId surface_output(const SurfaceState* surface) {
  if (surface == nullptr) return {};
  if (surface->layer_surface != nullptr) return surface->layer_surface->output;
  if (surface->lock_surface != nullptr) return surface->lock_surface->output;
  if (surface->xdg_surface != nullptr) return surface->xdg_surface->output;
#ifdef ZWWM_XWAYLAND
  if (surface->xwayland_surface != nullptr) return surface->xwayland_surface->output;
#endif
  return {};
}
void reorder_layer_roots(Observer* observer, OutputId output, std::uint32_t bucket) {
  auto* state = output_state(observer, output);
  if (state == nullptr || bucket >= state->layer_roots.size()) return;
  auto& roots = state->layer_roots[bucket];
  std::stable_sort(roots.begin(), roots.end(), [](const SurfaceState* left, const SurfaceState* right) {
    const auto* lhs = left->layer_surface;
    const auto* rhs = right->layer_surface;
    const auto lp = lhs != nullptr && lhs->xwlr != nullptr ? lhs->xwlr->current_priority : 0;
    const auto rp = rhs != nullptr && rhs->xwlr != nullptr ? rhs->xwlr->current_priority : 0;
    if (lp != rp) return lp < rp;
    return lhs->creation < rhs->creation;
  });
}
std::uint32_t reservation_edge(const LayerSurfaceState* layer) {
  if (layer->xwlr != nullptr && layer->xwlr->current_edge != 0) return layer->xwlr->current_edge;
  const auto anchor = layer->current.anchor;
  constexpr auto top = protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
  constexpr auto bottom = protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
  constexpr auto left = protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
  constexpr auto right = protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
  if (anchor == top || anchor == (top | left | right)) return top;
  if (anchor == bottom || anchor == (bottom | left | right)) return bottom;
  if (anchor == left || anchor == (left | top | bottom)) return left;
  if (anchor == right || anchor == (right | top | bottom)) return right;
  return 0;
}
void send_configure(XdgSurfaceState* x, std::int32_t width, std::int32_t height,
                      std::uint32_t states, bool force = false) {
  if (x == nullptr || x->toplevel == nullptr || width < 0 || height < 0 ||
      (!force && x->configured_width == width && x->configured_height == height &&
       x->configured_states == states)) return;
  std::vector<std::uint32_t> state_array;
  if ((states & kActivatedState) != 0) append_state(state_array, protocol::XDG_TOPLEVEL_STATE_ACTIVATED);
  if ((states & kFullscreenState) != 0) append_state(state_array, protocol::XDG_TOPLEVEL_STATE_FULLSCREEN);
  // GTK uses this state to suppress client-side rounded edges.
  if ((states & (kManagedTiledStates | kSuppressClientDecorations)) != 0)
    append_state(state_array, protocol::XDG_TOPLEVEL_STATE_MAXIMIZED);
  if ((states & kManagedTiledStates) != 0 && x->toplevel->version >= 2) {
    append_state(state_array, protocol::XDG_TOPLEVEL_STATE_TILED_LEFT);
    append_state(state_array, protocol::XDG_TOPLEVEL_STATE_TILED_RIGHT);
    append_state(state_array, protocol::XDG_TOPLEVEL_STATE_TILED_TOP);
    append_state(state_array, protocol::XDG_TOPLEVEL_STATE_TILED_BOTTOM);
  }
  protocol::xdg_toplevel_send_configure(*x->toplevel, width, height, std::as_bytes(std::span(state_array)));
  const auto serial = x->resource->display->next_serial();
  x->serials.push_back(serial);
  protocol::xdg_surface_send_configure(*x->resource, serial);
  if (x->configured_width != width || x->configured_height != height) {
    x->size_configure_serial = serial;
    x->size_configure_acked = false;
  }
  x->last_sent_serial = serial;
  x->configured_width = width;
  x->configured_height = height;
  x->configured_states = states;
}
bool glob_match(std::string_view pattern, std::string_view value) {
  std::size_t p = 0, v = 0, star = std::string_view::npos, retry = 0;
  while (v < value.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) { ++p; ++v; continue; }
    if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = v; continue; }
    if (star == std::string_view::npos) return false;
    p = star + 1; v = ++retry;
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}
bool visible_xdg(const Observer* observer, const XdgSurfaceState* x) {
  if (observer == nullptr || x == nullptr || !x->mapped) return false;
  auto* output = output_state(const_cast<Observer*>(observer), x->output);
  return output != nullptr && x->tag == output->active_tag;
}
bool focusable_toplevel(const Observer* observer, const SurfaceState* surface,
                        const SurfaceState* excluded = nullptr) {
  if (observer == nullptr || surface == nullptr || surface == excluded || surface->parent != nullptr) return false;
  if (surface->xdg_surface != nullptr && surface->xdg_surface->toplevel != nullptr)
    return visible_xdg(observer, surface->xdg_surface);
#ifdef ZWWM_XWAYLAND
  if (surface->xwayland_surface != nullptr && surface->xwayland_surface->window != nullptr &&
      surface->xwayland_surface->window->mapped) {
    const auto* output = output_state(const_cast<Observer*>(observer), surface->xwayland_surface->output);
    return output != nullptr && surface->xwayland_surface->tag == output->active_tag;
  }
#endif
  return false;
}
SurfaceState* focus_fallback(Observer* observer, SurfaceState* excluded = nullptr) {
  if (observer == nullptr || observer->seat == nullptr || observer->surfaces == nullptr) return nullptr;
  if (focusable_toplevel(observer, observer->seat->regular_focus, excluded))
    return observer->seat->regular_focus;
  for (auto it = observer->surfaces->rbegin(); it != observer->surfaces->rend(); ++it)
    if (focusable_toplevel(observer, *it, excluded)) return *it;
  return nullptr;
}
Rect output_work(Observer* observer, OutputId id, bool full = false) {
  auto* output = output_state(observer, id);
  if (output == nullptr) return {0, 0, 1, 1};
  if (!full) return arrange_layers(observer, id);
  return {output->info.logical_x, output->info.logical_y, static_cast<std::int32_t>(output->info.logical_width),
          static_cast<std::int32_t>(output->info.logical_height)};
}
bool endless_canvas(const Observer* observer) {
  return observer != nullptr && observer->config != nullptr &&
         observer->config->layout.kind == LayoutConfig::Kind::endless_canvas;
}
CanvasViewport* canvas_viewport(Observer* observer, OutputId id, std::uint8_t tag) {
  auto* output = output_state(observer, id);
  return output == nullptr || tag < 1 || tag > 9 ? nullptr : &output->canvas_viewports[tag - 1];
}
std::pair<std::int64_t, std::int64_t> snap_canvas_window(
    Observer* observer, SurfaceState* moving, OutputId output, std::uint8_t tag,
    const CanvasViewport& viewport, std::int64_t x, std::int64_t y,
    std::int32_t width, std::int32_t height) {
  if (observer == nullptr || observer->surfaces == nullptr || observer->config == nullptr)
    return {x, y};
  std::vector<CanvasBounds> candidates;
  for (auto* surface : *observer->surfaces) {
    if (surface == nullptr || surface == moving || surface->parent != nullptr) continue;
    if (auto* candidate = surface->xdg_surface;
        candidate != nullptr && candidate->toplevel != nullptr && candidate->mapped &&
        !candidate->fullscreen && candidate->output == output && candidate->tag == tag) {
      candidates.push_back(candidate->canvas_bounds);
      continue;
    }
#ifdef ZWWM_XWAYLAND
    if (auto* candidate = surface->xwayland_surface;
        candidate != nullptr && candidate->window != nullptr && candidate->window->mapped &&
        !candidate->fullscreen && candidate->output == output && candidate->tag == tag)
      candidates.push_back(candidate->canvas_bounds);
#endif
  }
  return layout::snap_canvas_window(x, y, width, height, candidates,
                                    observer->config->layout.inner_gap, viewport);
}
void zoom_canvas(Observer* observer, SeatState* seat, bool zoom_in) {
  if (observer == nullptr || seat == nullptr) return;
  auto* output = output_state(observer, observer->active_output);
  if (output == nullptr) return;
  auto& viewport = output->canvas_viewports[output->active_tag - 1];
  const Rect work = output_work(observer, output->info.id);
  const double center_x = viewport.x + work.width / (2.0 * viewport.scale);
  const double center_y = viewport.y + work.height / (2.0 * viewport.scale);
  const double minimum = observer->config->layout.min_zoom_per_mille / 1000.0;
  const double maximum = observer->config->layout.max_zoom_per_mille / 1000.0;
  const double scale = std::clamp(viewport.scale * (zoom_in ? 1.1 : 1.0 / 1.1), minimum, maximum);
  if (scale == viewport.scale) return;
  viewport.scale = scale;
  viewport.x = center_x - work.width / (2.0 * scale);
  viewport.y = center_y - work.height / (2.0 * scale);
  seat->canvas_zooming = true;
  configure_layout(observer);
  seat->canvas_zooming = false;
}
Rect clamp_window(Rect rect, Rect area) {
  rect.width = std::clamp(rect.width, 1, std::max(1, area.width));
  rect.height = std::clamp(rect.height, 1, std::max(1, area.height));
  rect.x = std::clamp(rect.x, area.x, area.x + area.width - rect.width);
  rect.y = std::clamp(rect.y, area.y, area.y + area.height - rect.height);
  return rect;
}
Rect position_window(Rect rect, Rect area) {
  rect.x = rect.width > area.width ? area.x : std::clamp(rect.x, area.x, area.x + area.width - rect.width);
  rect.y = rect.height > area.height ? area.y : std::clamp(rect.y, area.y, area.y + area.height - rect.height);
  return rect;
}
Rect constrain_xdg_window(const RuntimeConfig& config, const XdgSurfaceState& x, Rect bounds) {
  bounds.width = std::max(1, bounds.width);
  bounds.height = std::max(1, bounds.height);
  const auto content = config.content_bounds(
      {{bounds.x, bounds.y}, {static_cast<std::uint32_t>(bounds.width), static_cast<std::uint32_t>(bounds.height)}});
  auto width = static_cast<std::int32_t>(content.size.width);
  auto height = static_cast<std::int32_t>(content.size.height);
  if (x.max_width > 0) width = std::min(width, x.max_width);
  if (x.max_height > 0) height = std::min(height, x.max_height);
  width = std::max({1, width, x.min_width});
  height = std::max({1, height, x.min_height});
  bounds.width += width - static_cast<std::int32_t>(content.size.width);
  bounds.height += height - static_cast<std::int32_t>(content.size.height);
  return bounds;
}
bool portal_surface(const SurfaceState* surface) {
  if (surface == nullptr || surface->resource == nullptr || surface->observer == nullptr ||
      surface->observer->portal_pid <= 0) return false;
  return surface->resource->client->pid() == surface->observer->portal_pid;
}
void apply_window_rules(XdgSurfaceState* x) {
  if (x == nullptr || x->rule_applied || x->surface == nullptr || x->surface->observer == nullptr) return;
  auto* observer = x->surface->observer;
  x->background_blur_radius = 0;
  x->glass = false;
  x->rule_opacity = 1.0F;
  x->window_shader = observer->config->window_shader;
  x->border_shader = observer->config->border_shader;
  const bool fixed_size = x->min_width > 0 && x->min_height > 0 &&
      ((x->max_width > 0 && x->min_width == x->max_width) ||
       (x->max_height > 0 && x->min_height == x->max_height));
  x->floating = x->transient_parent != nullptr || fixed_size ||
                (x->dialog != nullptr && x->dialog->modal);
  const WindowRule* size_rule = nullptr;
  for (const auto& rule : observer->config->window_rules) {
    if ((!rule.app_id.empty() && !glob_match(rule.app_id, x->app_id)) ||
        (!rule.title.empty() && !glob_match(rule.title, x->title))) continue;
    if (rule.floating) x->floating = *rule.floating;
    if (rule.width != 0 && rule.height != 0) size_rule = &rule;
    if (rule.opacity) x->rule_opacity = *rule.opacity;
    if (!rule.window_shader.empty()) x->window_shader = rule.window_shader;
    if (!rule.border_shader.empty()) x->border_shader = rule.border_shader;
  }
  x->background_blur_radius = static_cast<std::uint32_t>(std::clamp(
      observer->config->shader_integer(x->window_shader, "blur_radius"), 0, 64));
  if (size_rule != nullptr) {
    const Rect work = output_work(observer, x->output);
    const auto width = size_rule->width_percent ? work.width * static_cast<std::int32_t>(size_rule->width) / 100 : static_cast<std::int32_t>(size_rule->width);
    const auto height = size_rule->height_percent ? work.height * static_cast<std::int32_t>(size_rule->height) / 100 : static_cast<std::int32_t>(size_rule->height);
    x->floating_bounds = clamp_window({work.x + (work.width - width) / 2, work.y + (work.height - height) / 2, width, height}, work);
  }
  if (portal_surface(x->surface)) x->floating = true;
  x->rule_applied = true;
}
bool has_horizontal(std::uint32_t value, bool left) { return value == (left ? protocol::XDG_POSITIONER_ANCHOR_LEFT : protocol::XDG_POSITIONER_ANCHOR_RIGHT) || value == (left ? protocol::XDG_POSITIONER_ANCHOR_TOP_LEFT : protocol::XDG_POSITIONER_ANCHOR_TOP_RIGHT) || value == (left ? protocol::XDG_POSITIONER_ANCHOR_BOTTOM_LEFT : protocol::XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT); }
bool has_vertical(std::uint32_t value, bool top) { return value == (top ? protocol::XDG_POSITIONER_ANCHOR_TOP : protocol::XDG_POSITIONER_ANCHOR_BOTTOM) || value == (top ? protocol::XDG_POSITIONER_ANCHOR_TOP_LEFT : protocol::XDG_POSITIONER_ANCHOR_BOTTOM_LEFT) || value == (top ? protocol::XDG_POSITIONER_ANCHOR_TOP_RIGHT : protocol::XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT); }
Rect popup_geometry(const PositionerState& p, Rect bounds) {
  std::int64_t constraint_x = bounds.x, constraint_y = bounds.y;
  std::int64_t constraint_width = bounds.width, constraint_height = bounds.height;
  if (constraint_width > 8) { constraint_x += 4; constraint_width -= 8; }
  if (constraint_height > 8) { constraint_y += 4; constraint_height -= 8; }
  const std::int64_t constraint_right = constraint_x + constraint_width;
  const std::int64_t constraint_bottom = constraint_y + constraint_height;
  std::int64_t width = p.width, height = p.height;
  const bool anchor_left = has_horizontal(p.anchor, true), anchor_right = has_horizontal(p.anchor, false);
  const bool anchor_top = has_vertical(p.anchor, true), anchor_bottom = has_vertical(p.anchor, false);
  bool gravity_left = has_horizontal(p.gravity, true), gravity_right = has_horizontal(p.gravity, false);
  bool gravity_top = has_vertical(p.gravity, true), gravity_bottom = has_vertical(p.gravity, false);
  std::int64_t anchor_x = anchor_left ? p.anchor_rect.x : anchor_right ? p.anchor_rect.x + p.anchor_rect.width : p.anchor_rect.x + p.anchor_rect.width / 2;
  std::int64_t anchor_y = anchor_top ? p.anchor_rect.y : anchor_bottom ? p.anchor_rect.y + p.anchor_rect.height : p.anchor_rect.y + p.anchor_rect.height / 2;
  const auto effective_x = [&](bool left, bool right, std::int64_t anchor) { return left ? anchor - width : right ? anchor : anchor - width / 2; };
  const auto effective_y = [&](bool top, bool bottom, std::int64_t anchor) { return top ? anchor - height : bottom ? anchor : anchor - height / 2; };
  const auto remaining_width = [&](std::int64_t origin) {
    std::int64_t remaining = p.width;
    if (origin < constraint_x) { remaining -= constraint_x - origin; origin = constraint_x; }
    if (origin + remaining > constraint_right) remaining -= origin + remaining - constraint_right;
    return std::pair{origin, remaining};
  };
  const auto remaining_height = [&](std::int64_t origin) {
    std::int64_t remaining = p.height;
    if (origin < constraint_y) { remaining -= constraint_y - origin; origin = constraint_y; }
    if (origin + remaining > constraint_bottom) remaining -= origin + remaining - constraint_bottom;
    return std::pair{origin, remaining};
  };
  std::int64_t x = effective_x(gravity_left, gravity_right, anchor_x);
  std::int64_t y = effective_y(gravity_top, gravity_bottom, anchor_y);
  if ((p.adjustment & protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) != 0 &&
      ((gravity_left && x + p.offset_x < constraint_x) ||
       (gravity_right && x + p.offset_x + width > constraint_right))) {
    const bool new_left = gravity_right, new_right = gravity_left;
    const std::int64_t new_anchor = anchor_left ? p.anchor_rect.x + p.anchor_rect.width : anchor_right ? p.anchor_rect.x : anchor_x;
    const std::int64_t candidate = effective_x(new_left, new_right, new_anchor);
    if (remaining_width(candidate).second > remaining_width(x).second) {
      gravity_left = new_left; gravity_right = new_right; anchor_x = new_anchor; x = candidate;
    }
  }
  if ((p.adjustment & protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) != 0 &&
      ((gravity_top && y + p.offset_y < constraint_y) ||
       (gravity_bottom && y + p.offset_y + height > constraint_bottom))) {
    const bool new_top = gravity_bottom, new_bottom = gravity_top;
    const std::int64_t new_anchor = anchor_top ? p.anchor_rect.y + p.anchor_rect.height : anchor_bottom ? p.anchor_rect.y : anchor_y;
    const std::int64_t candidate = effective_y(new_top, new_bottom, new_anchor);
    if (remaining_height(candidate).second > remaining_height(y).second) {
      gravity_top = new_top; gravity_bottom = new_bottom; anchor_y = new_anchor; y = candidate;
    }
  }
  x += p.offset_x; y += p.offset_y;
  if ((p.adjustment & protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X) != 0) {
    if (x + width > constraint_right) x = constraint_right - width;
    if (x < constraint_x) x = constraint_x;
  }
  if ((p.adjustment & protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y) != 0) {
    if (y + height > constraint_bottom) y = constraint_bottom - height;
    if (y < constraint_y) y = constraint_y;
  }
  if ((p.adjustment & protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X) != 0) std::tie(x, width) = remaining_width(x);
  if ((p.adjustment & protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y) != 0) std::tie(y, height) = remaining_height(y);
  return {static_cast<std::int32_t>(std::clamp<std::int64_t>(x, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max())),
          static_cast<std::int32_t>(std::clamp<std::int64_t>(y, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max())),
          static_cast<std::int32_t>(std::max<std::int64_t>(1, width)), static_cast<std::int32_t>(std::max<std::int64_t>(1, height))};
}
Rect popup_constraint_bounds(const PopupState& popup) {
  if (popup.layer_parent != nullptr) {
    const auto* observer = popup.layer_parent->observer;
    const auto* layer = root(popup.layer_parent)->layer_surface;
    auto* output = layer == nullptr ? nullptr : output_state(const_cast<Observer*>(observer), layer->output);
    std::int32_t parent_x = 0, parent_y = 0;
    absolute_position(popup.layer_parent, &parent_x, &parent_y);
    if (output != nullptr) return {output->info.logical_x - parent_x, output->info.logical_y - parent_y,
                                   static_cast<std::int32_t>(output->info.logical_width),
                                   static_cast<std::int32_t>(output->info.logical_height)};
    return {0, 0, observer == nullptr ? kOutputWidth : observer->output_width,
            observer == nullptr ? kOutputHeight : observer->output_height};
  }
  const auto* parent = popup.parent;
  if (parent == nullptr || parent->surface == nullptr) return {0, 0, 1, 1};
  auto* top = root(parent->surface);
  const auto* observer = parent->surface->observer;
  const auto* top_xdg = top == nullptr ? nullptr : top->xdg_surface;
  auto* output = top_xdg == nullptr ? nullptr : output_state(const_cast<Observer*>(observer), top_xdg->output);
  if (top == nullptr || top_xdg == nullptr || output == nullptr) return {0, 0, 1, 1};

  const Rect root_geometry = effective_window_geometry(top);
  const Rect parent_geometry = effective_window_geometry(parent->surface);
  const Rect content = top_xdg->content_bounds;
  if (root_geometry.width <= 0 || root_geometry.height <= 0 || content.width <= 0 || content.height <= 0)
    return {0, 0, 1, 1};

  std::int32_t parent_x = 0, parent_y = 0, root_x = 0, root_y = 0;
  absolute_position(parent->surface, &parent_x, &parent_y);
  absolute_position(top, &root_x, &root_y);
  const auto logical_to_root_x = [&](std::int32_t value) {
    return root_geometry.x + static_cast<std::int64_t>(value - content.x) * root_geometry.width / content.width;
  };
  const auto logical_to_root_y = [&](std::int32_t value) {
    return root_geometry.y + static_cast<std::int64_t>(value - content.y) * root_geometry.height / content.height;
  };
  const std::int64_t parent_window_x = parent_x - root_x + parent_geometry.x;
  const std::int64_t parent_window_y = parent_y - root_y + parent_geometry.y;
  const std::int64_t left = logical_to_root_x(output->info.logical_x) - parent_window_x;
  const std::int64_t top_edge = logical_to_root_y(output->info.logical_y) - parent_window_y;
  const std::int64_t right = logical_to_root_x(output->info.logical_x + static_cast<std::int32_t>(output->info.logical_width)) - parent_window_x;
  const std::int64_t bottom = logical_to_root_y(output->info.logical_y + static_cast<std::int32_t>(output->info.logical_height)) - parent_window_y;
  return {static_cast<std::int32_t>(std::clamp<std::int64_t>(left, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max())),
          static_cast<std::int32_t>(std::clamp<std::int64_t>(top_edge, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max())),
          static_cast<std::int32_t>(std::clamp<std::int64_t>(right - left, 1, std::numeric_limits<std::int32_t>::max())),
          static_cast<std::int32_t>(std::clamp<std::int64_t>(bottom - top_edge, 1, std::numeric_limits<std::int32_t>::max()))};
}
void send_popup_configure(PopupState* popup, bool force,
                          std::optional<std::uint32_t> reposition_token) {
  if (popup == nullptr || popup->dismissed || popup->resource == nullptr || popup->xdg == nullptr ||
      ((popup->parent == nullptr || popup->parent->surface == nullptr) && popup->layer_parent == nullptr)) return;
  const Rect geometry = popup_geometry(popup->positioner, popup_constraint_bounds(*popup));
  if (!force && popup->has_geometry && same_rect(popup->last_geometry, geometry)) return;
  popup->last_geometry = geometry;
  popup->has_geometry = true;
  // xdg_popup coordinates are relative to the parent's window geometry, while
  // surface composition coordinates are relative to its wl_surface origin.
  const Rect parent_geometry = popup->parent == nullptr ? Rect{} : effective_window_geometry(popup->parent->surface);
  popup->xdg->surface->x = popup->parent == nullptr ? geometry.x : parent_geometry.x + geometry.x;
  popup->xdg->surface->y = popup->parent == nullptr ? geometry.y : parent_geometry.y + geometry.y;
  popup->xdg->configured_width = geometry.width; popup->xdg->configured_height = geometry.height;
  protocol::xdg_popup_send_configure(*popup->resource, geometry.x, geometry.y, geometry.width, geometry.height);
  if (reposition_token.has_value())
    protocol::xdg_popup_send_repositioned(*popup->resource, *reposition_token);
  const auto serial = popup->xdg->resource->display->next_serial();
  popup->xdg->serials.push_back(serial); popup->xdg->last_sent_serial = serial;
  protocol::xdg_surface_send_configure(*popup->xdg->resource, serial);
  notify_surface(popup->xdg->surface);
}
std::vector<layout::Placement> root_placements(const Observer* observer, const XdgSurfaceState* candidate = nullptr, Rect area = {0, 0, -1, -1}, OutputId output = {}) {
  std::vector<layout::WindowId> roots;
  if (observer != nullptr && observer->surfaces != nullptr) {
    for (const auto* surface : *observer->surfaces) {
      const auto* x = surface->xdg_surface;
      if (x != nullptr && x->toplevel != nullptr && !x->floating && !x->fullscreen && (!output || x->output == output) &&
          (x == candidate || visible_xdg(observer, x))) roots.push_back(surface->id);
#ifdef ZWWM_XWAYLAND
      const auto* xwayland = surface->xwayland_surface;
      const auto* xwayland_output = xwayland == nullptr ? nullptr :
          output_state(const_cast<Observer*>(observer), xwayland->output);
      if (xwayland != nullptr && xwayland->window != nullptr && xwayland->window->mapped &&
          !xwayland->floating && !xwayland->fullscreen && (!output || xwayland->output == output) &&
          xwayland_output != nullptr && xwayland->tag == xwayland_output->active_tag)
        roots.push_back(surface->id);
#endif
    }
  }
  if (area.width < 0) area = {0, 0, observer->output_width, observer->output_height};
  auto* output_state_value = output_state(const_cast<Observer*>(observer), output);
  if (observer->config->layout.kind == LayoutConfig::Kind::focus_fibonacci && output_state_value != nullptr) {
    auto& leaves = output_state_value->fibonacci_leaves[output_state_value->active_tag - 1];
    const auto focused_id = observer->seat == nullptr || root(observer->seat->toplevel_focus) == nullptr ? 0 :
                            root(observer->seat->toplevel_focus)->id;
    layout::reconcile_fibonacci_leaves(leaves, roots, focused_id);
    const std::int32_t outer_gap = observer->config->layout.smart_gaps && leaves.size() == 1 ? 0 :
                                   static_cast<std::int32_t>(observer->config->layout.outer_gap);
    const std::int32_t inner_gap = static_cast<std::int32_t>(observer->config->layout.inner_gap);
    return layout::arrange_focus_fibonacci(
        leaves, {{area.x, area.y}, {static_cast<std::uint32_t>(area.width),
                                    static_cast<std::uint32_t>(area.height)}},
        outer_gap, inner_gap);
  }
  if (output_state_value == nullptr || (!output_state_value->master_ratio && output_state_value->tile_weights.empty()))
    return observer->config->placements(roots, {{area.x, area.y}, {static_cast<std::uint32_t>(area.width), static_cast<std::uint32_t>(area.height)}});
  layout::MasterStack layout;
  (void)layout.set_master_count(observer->config->layout.master_count);
  (void)layout.set_master_ratio(output_state_value->master_ratio.value_or(
      static_cast<float>(observer->config->layout.master_ratio_percent) / 100.0F));
  (void)layout.set_outer_gap(observer->config->layout.smart_gaps && roots.size() == 1 ? 0 :
                             static_cast<std::int32_t>(observer->config->layout.outer_gap));
  (void)layout.set_inner_gap(static_cast<std::int32_t>(observer->config->layout.inner_gap));
  std::vector<float> weights;
  weights.reserve(roots.size());
  for (const auto id : roots) {
    const auto weight = output_state_value->tile_weights.find(id);
    weights.push_back(weight == output_state_value->tile_weights.end() ? 1.0F : weight->second);
  }
  return layout.arrange(roots, {{area.x, area.y}, {static_cast<std::uint32_t>(area.width), static_cast<std::uint32_t>(area.height)}}, weights);
}
Rect layer_geometry(const LayerState& state, Rect area) {
  const bool left = (state.anchor & protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
  const bool right = (state.anchor & protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;
  const bool top = (state.anchor & protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
  const bool bottom = (state.anchor & protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;
  const auto width = state.width == 0 ? std::max(1, area.width - state.left - state.right) : state.width;
  const auto height = state.height == 0 ? std::max(1, area.height - state.top - state.bottom) : state.height;
  return {left ? area.x + state.left : right ? area.x + area.width - width - state.right : area.x + (area.width - width) / 2,
          top ? area.y + state.top : bottom ? area.y + area.height - height - state.bottom : area.y + (area.height - height) / 2,
          width, height};
}
void arrange_layer_surface(LayerSurfaceState* layer, Rect area) {
  if (layer == nullptr || layer->surface == nullptr) return;
  const Rect geometry = layer_geometry(layer->current, area);
  auto* surface = layer->surface;
  const bool moved = surface->x != geometry.x || surface->y != geometry.y;
  const bool resized = layer->configured_width != geometry.width || layer->configured_height != geometry.height;
  surface->x = geometry.x;
  surface->y = geometry.y;
  if (moved || resized) notify_surface_tree(surface);
  if (layer->configure_requested || resized)
    configure_layer_surface(layer, geometry);
}
Rect arrange_layers(Observer* observer, OutputId output) {
  auto* state = output_state(observer, output);
  if (state == nullptr) return {0, 0, observer->output_width, observer->output_height};
  const auto& info = state->info;
  Rect work{info.logical_x, info.logical_y, static_cast<std::int32_t>(info.logical_width),
            static_cast<std::int32_t>(info.logical_height)};
  const Rect full = work;
  constexpr std::array<std::uint32_t, 4> order{protocol::ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, protocol::ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                                                 protocol::ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, protocol::ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND};
  // Reserve by layer, xwlr priority, then creation order.
  for (const auto bucket : order) {
    reorder_layer_roots(observer, output, bucket);
    for (auto* surface : state->layer_roots[bucket]) {
      auto* layer = surface->layer_surface;
      if (layer == nullptr || !layer->mapped || layer->current.layer != bucket || layer->current.zone <= 0) continue;
      arrange_layer_surface(layer, work);
      const auto edge = reservation_edge(layer);
      const auto zone = layer->current.zone;
      if (edge == protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) { const auto amount = zone + layer->current.top; work.y += amount; work.height = std::max(0, work.height - amount); }
      else if (edge == protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) work.height = std::max(0, work.height - zone - layer->current.bottom);
      else if (edge == protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) { const auto amount = zone + layer->current.left; work.x += amount; work.width = std::max(0, work.width - amount); }
      else if (edge == protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) work.width = std::max(0, work.width - zone - layer->current.right);
    }
  }
  for (const auto bucket : order) for (auto* surface : state->layer_roots[bucket]) {
    auto* layer = surface->layer_surface;
    if (layer == nullptr || layer->current.layer != bucket || (layer->mapped && layer->current.zone > 0)) continue;
    arrange_layer_surface(layer, layer->current.zone < 0 ? full : work);
  }
  return work;
}
void configure_canvas_layout(Observer* observer, XdgSurfaceState* candidate) {
  if (observer == nullptr || observer->surfaces == nullptr) return;
  const auto border = observer->config->border_width();
  for (auto* surface : *observer->surfaces) {
    if (surface == nullptr || surface->parent != nullptr) continue;
    if (auto* x = surface->xdg_surface; x != nullptr && x->toplevel != nullptr) {
      auto* output = output_state(observer, x->output);
      if (output == nullptr || (x != candidate && (!x->mapped || x->tag != output->active_tag))) continue;
      const Rect work = output_work(observer, x->output);
      auto* viewport = canvas_viewport(observer, x->output, x->tag);
      if (viewport == nullptr) continue;
      layout::initialize_canvas_bounds(x->canvas_bounds, x->fullscreen ? Rect{} : x->tile_bounds, work, *viewport,
                               x->window_geometry_set ? x->window_geometry.width : 0,
                               x->window_geometry_set ? x->window_geometry.height : 0, border);
      Rect bounds;
      std::uint32_t states = kSuppressClientDecorations |
          (observer->seat != nullptr && observer->seat->toplevel_focus == surface ? kActivatedState : 0U);
      if (x->fullscreen) {
        bounds = output_work(observer, x->output, true);
        states |= kFullscreenState;
      } else {
        bounds = layout::canvas_view_bounds(x->canvas_bounds, *viewport, work);
      }
      const auto content = x->fullscreen ?
          renderer::Rect{{bounds.x, bounds.y}, {static_cast<std::uint32_t>(bounds.width),
                                                static_cast<std::uint32_t>(bounds.height)}} :
          observer->config->content_bounds({{bounds.x, bounds.y},
                                             {static_cast<std::uint32_t>(bounds.width),
                                              static_cast<std::uint32_t>(bounds.height)}});
      const Rect assigned{content.origin.x, content.origin.y,
                           static_cast<std::int32_t>(content.size.width),
                           static_cast<std::int32_t>(content.size.height)};
      const auto client_content = x->fullscreen ? content : observer->config->content_bounds(
          {{0, 0}, {static_cast<std::uint32_t>(x->canvas_bounds.width),
                    static_cast<std::uint32_t>(x->canvas_bounds.height)}});
      const bool changed = !same_rect(x->tile_bounds, bounds) || !same_rect(x->content_bounds, assigned);
      const bool state_changed = x->configured_states != states;
      x->tile_bounds = bounds;
      x->content_bounds = assigned;
      if (observer->seat == nullptr || !observer->seat->canvas_zooming)
        send_configure(x, static_cast<std::int32_t>(client_content.size.width),
                       static_cast<std::int32_t>(client_content.size.height), states);
      if (changed || state_changed) notify_surface_tree(surface);
      continue;
    }
#ifdef ZWWM_XWAYLAND
    auto* role = surface->xwayland_surface;
    if (role == nullptr || role->window == nullptr || !role->window->mapped) continue;
    auto* output = output_state(observer, role->output);
    if (output == nullptr || role->tag != output->active_tag) continue;
    const Rect work = output_work(observer, role->output);
    auto* viewport = canvas_viewport(observer, role->output, role->tag);
    if (viewport == nullptr) continue;
    const auto requested_width = role->window->requested_bounds.width > 1 ?
        role->window->requested_bounds.width : static_cast<std::int32_t>(role->window->width);
    const auto requested_height = role->window->requested_bounds.height > 1 ?
        role->window->requested_bounds.height : static_cast<std::int32_t>(role->window->height);
    layout::initialize_canvas_bounds(role->canvas_bounds, role->fullscreen ? Rect{} : role->tile_bounds, work, *viewport,
                             requested_width > 1 ? requested_width : 0,
                             requested_height > 1 ? requested_height : 0, border);
    Rect bounds;
    if (role->fullscreen) {
      bounds = output_work(observer, role->output, true);
    } else {
      bounds = layout::canvas_view_bounds(role->canvas_bounds, *viewport, work);
    }
    const auto content = role->fullscreen ?
        renderer::Rect{{bounds.x, bounds.y}, {static_cast<std::uint32_t>(bounds.width),
                                              static_cast<std::uint32_t>(bounds.height)}} :
        observer->config->content_bounds({{bounds.x, bounds.y},
                                           {static_cast<std::uint32_t>(bounds.width),
                                            static_cast<std::uint32_t>(bounds.height)}});
    const Rect assigned{content.origin.x, content.origin.y,
                         static_cast<std::int32_t>(content.size.width),
                         static_cast<std::int32_t>(content.size.height)};
    const auto client_content = role->fullscreen ? content : observer->config->content_bounds(
        {{0, 0}, {static_cast<std::uint32_t>(role->canvas_bounds.width),
                  static_cast<std::uint32_t>(role->canvas_bounds.height)}});
    const bool changed = !same_rect(role->tile_bounds, bounds) || !same_rect(role->content_bounds, assigned);
    role->tile_bounds = bounds;
    role->content_bounds = assigned;
    const bool camera_only = observer->seat != nullptr && observer->seat->canvas_zooming;
    if (changed && !camera_only && role->runtime != nullptr && role->runtime->connection != nullptr) {
      const std::array<std::uint32_t, 4> values{
          static_cast<std::uint32_t>(assigned.x), static_cast<std::uint32_t>(assigned.y),
          client_content.size.width, client_content.size.height};
      role->window->x = static_cast<std::int16_t>(assigned.x);
      role->window->y = static_cast<std::int16_t>(assigned.y);
      role->window->width = static_cast<std::uint16_t>(client_content.size.width);
      role->window->height = static_cast<std::uint16_t>(client_content.size.height);
      surface->x = assigned.x;
      surface->y = assigned.y;
      xcb_configure_window(role->runtime->connection, role->window->id,
          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
          values.data());
      xcb_flush(role->runtime->connection);
    }
    if (changed) notify_surface_tree(surface);
#endif
  }
}
void configure_layout(Observer* observer, XdgSurfaceState* candidate) {
  if (endless_canvas(observer)) {
    configure_canvas_layout(observer, candidate);
    return;
  }
  std::vector<layout::Placement> tiled;
  if (observer->outputs != nullptr && !observer->outputs->empty()) {
    for (const auto& output : *observer->outputs) {
      const auto& info = output->info;
      const Rect area = arrange_layers(observer, info.id);
      auto placements = root_placements(observer, candidate, area, info.id);
      tiled.insert(tiled.end(), placements.begin(), placements.end());
    }
  } else {
    const Rect work = arrange_layers(observer, {});
    tiled = root_placements(observer, candidate, work);
  }
  for (const auto& placement : tiled) {
    const auto surface = std::find_if(observer->surfaces->begin(), observer->surfaces->end(),
                                      [&placement](const SurfaceState* item) { return item->id == placement.id; });
    if (surface == observer->surfaces->end()) continue;
    auto* x = (*surface)->xdg_surface;
#ifdef ZWWM_XWAYLAND
    auto* xwayland = (*surface)->xwayland_surface;
#endif
    if (x == nullptr
#ifdef ZWWM_XWAYLAND
        && xwayland == nullptr
#endif
        ) continue;
    if (candidate != nullptr && !candidate->mapped && x != candidate) continue;
    const Rect slot{placement.bounds.origin.x, placement.bounds.origin.y,
                    static_cast<std::int32_t>(placement.bounds.size.width),
                    static_cast<std::int32_t>(placement.bounds.size.height)};
    // Client size hints constrain floating windows, not managed tiles.
    const Rect tile = slot;
    const auto content = observer->config->content_bounds(
        {{tile.x, tile.y}, {static_cast<std::uint32_t>(tile.width), static_cast<std::uint32_t>(tile.height)}});
    const Rect assigned_content{content.origin.x, content.origin.y,
                                static_cast<std::int32_t>(content.size.width),
                                static_cast<std::int32_t>(content.size.height)};
    const auto width = static_cast<std::int32_t>(content.size.width);
    const auto height = static_cast<std::int32_t>(content.size.height);
    const bool focused = observer->seat != nullptr && observer->seat->toplevel_focus == *surface;
    const auto states = kManagedTiledStates | (focused ? kActivatedState : 0U);
    bool assignment_changed = false;
    bool state_changed = false;
    if (x != nullptr) {
      assignment_changed = !same_rect(x->tile_bounds, tile) || !same_rect(x->content_bounds, assigned_content);
      state_changed = x->configured_states != states;
      x->tile_bounds = tile;
      x->content_bounds = assigned_content;
      send_configure(x, width, height, states);
    }
#ifdef ZWWM_XWAYLAND
    else {
      assignment_changed = !same_rect(xwayland->tile_bounds, tile) ||
                           !same_rect(xwayland->content_bounds, assigned_content);
      xwayland->tile_bounds = tile;
      xwayland->content_bounds = assigned_content;
      auto* window = xwayland->window;
      if (assignment_changed && window != nullptr && xwayland->runtime != nullptr &&
          xwayland->runtime->connection != nullptr) {
        const std::array<std::uint32_t, 4> values{
            static_cast<std::uint32_t>(assigned_content.x), static_cast<std::uint32_t>(assigned_content.y),
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        window->x = static_cast<std::int16_t>(assigned_content.x);
        window->y = static_cast<std::int16_t>(assigned_content.y);
        window->width = static_cast<std::uint16_t>(width);
        window->height = static_cast<std::uint16_t>(height);
        (*surface)->x = assigned_content.x;
        (*surface)->y = assigned_content.y;
        xcb_configure_window(xwayland->runtime->connection, window->id,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            values.data());
        xcb_flush(xwayland->runtime->connection);
      }
    }
#endif
    if (assignment_changed) {
      const auto reconfigure_popups = [&](const auto& self, SurfaceState* parent) -> void {
        for (auto* child : parent->children) if (child->xdg_surface != nullptr && child->xdg_surface->popup != nullptr) {
          auto* popup = child->xdg_surface->popup;
          if (popup->positioner.reactive) send_popup_configure(popup);
          self(self, child);
        }
      };
      reconfigure_popups(reconfigure_popups, *surface);
    }
    if (assignment_changed || state_changed) notify_surface_tree(*surface);
  }
  for (auto* surface : *observer->surfaces) {
    auto* x = surface->xdg_surface;
    if (surface->parent != nullptr || x == nullptr || x->toplevel == nullptr || (!visible_xdg(observer, x) && x != candidate) ||
        (!x->floating && !x->fullscreen)) continue;
    Rect bounds;
    std::uint32_t states = kSuppressClientDecorations |
        (observer->seat != nullptr && observer->seat->toplevel_focus == surface ? kActivatedState : 0U);
    if (x->fullscreen) {
      bounds = output_work(observer, x->output, true);
      states |= kFullscreenState;
    } else {
      const Rect work = output_work(observer, x->output);
      if (x->floating_bounds.width <= 0 || x->floating_bounds.height <= 0) {
        const auto width = x->accepted_width > 1 ? x->accepted_width : std::max(1, work.width / 2);
        const auto height = x->accepted_height > 1 ? x->accepted_height : std::max(1, work.height / 2);
        x->floating_bounds = {work.x + (work.width - width) / 2, work.y + (work.height - height) / 2, width, height};
      }
      bounds = clamp_window(x->floating_bounds, work);
      bounds = constrain_xdg_window(*observer->config, *x, bounds);
      bounds = x->floating_bounds = position_window(bounds, work);
      x->last_floating_bounds = bounds;
    }
    const auto content = x->fullscreen ? renderer::Rect{{bounds.x, bounds.y}, {static_cast<std::uint32_t>(bounds.width), static_cast<std::uint32_t>(bounds.height)}} :
                                        observer->config->content_bounds({{bounds.x, bounds.y}, {static_cast<std::uint32_t>(bounds.width), static_cast<std::uint32_t>(bounds.height)}});
    x->tile_bounds = bounds;
    x->content_bounds = {content.origin.x, content.origin.y, static_cast<std::int32_t>(content.size.width), static_cast<std::int32_t>(content.size.height)};
    send_configure(x, x->content_bounds.width, x->content_bounds.height, states);
    notify_surface_tree(surface);
  }
#ifdef ZWWM_XWAYLAND
  for (auto* surface : *observer->surfaces) {
    auto* role = surface->xwayland_surface;
    if (surface->parent != nullptr || role == nullptr || role->window == nullptr || !role->window->mapped ||
        (!role->floating && !role->fullscreen)) continue;
    auto* role_output = output_state(observer, role->output);
    if (role_output == nullptr || role->tag != role_output->active_tag) continue;
    const Rect work = output_work(observer, role->output, role->fullscreen);
    Rect bounds = role->fullscreen ? work : role->floating_bounds;
    if (bounds.width <= 0 || bounds.height <= 0) {
      const auto width = role->window->requested_bounds.width > 1 ? role->window->requested_bounds.width :
          static_cast<std::int32_t>(role->window->width);
      const auto height = role->window->requested_bounds.height > 1 ? role->window->requested_bounds.height :
          static_cast<std::int32_t>(role->window->height);
      if (width <= 1 || height <= 1) {
        bounds = {work.x + work.width / 4, work.y + work.height / 4,
                  std::max(1, work.width / 2), std::max(1, work.height / 2)};
      } else if (role->window->override_redirect || role->window->position_specified) {
        bounds = {role->window->x, role->window->y, width, height};
      } else {
        bounds = {work.x + (work.width - width) / 2, work.y + (work.height - height) / 2, width, height};
      }
    }
    if (!role->fullscreen) bounds = clamp_window(bounds, work);
    role->floating_bounds = bounds;
    const auto content = role->fullscreen ?
        renderer::Rect{{bounds.x, bounds.y}, {static_cast<std::uint32_t>(bounds.width), static_cast<std::uint32_t>(bounds.height)}} :
        observer->config->content_bounds({{bounds.x, bounds.y}, {static_cast<std::uint32_t>(bounds.width), static_cast<std::uint32_t>(bounds.height)}});
    const Rect assigned{content.origin.x, content.origin.y,
                        static_cast<std::int32_t>(content.size.width), static_cast<std::int32_t>(content.size.height)};
    const bool changed = !same_rect(role->tile_bounds, bounds) || !same_rect(role->content_bounds, assigned);
    role->tile_bounds = bounds;
    role->content_bounds = assigned;
    if (changed && role->runtime != nullptr && role->runtime->connection != nullptr) {
      const std::array<std::uint32_t, 4> values{
          static_cast<std::uint32_t>(assigned.x), static_cast<std::uint32_t>(assigned.y),
          static_cast<std::uint32_t>(assigned.width), static_cast<std::uint32_t>(assigned.height)};
      role->window->x = static_cast<std::int16_t>(assigned.x);
      role->window->y = static_cast<std::int16_t>(assigned.y);
      role->window->width = static_cast<std::uint16_t>(assigned.width);
      role->window->height = static_cast<std::uint16_t>(assigned.height);
      surface->x = assigned.x;
      surface->y = assigned.y;
      xcb_configure_window(role->runtime->connection, role->window->id,
          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
          values.data());
      xcb_flush(role->runtime->connection);
      notify_surface_tree(surface);
    }
  }
#endif
}
void notify_surface(SurfaceState* s) {
  if (s == nullptr || s->observer == nullptr) return;
  if (s->xdg_surface != nullptr && s->observer->toplevel_callback != nullptr) s->observer->toplevel_callback(s->observer->toplevel_data);
  if (s->observer->event_callback != nullptr) s->observer->event_callback(
      s->observer->event_data, root(s)->layer_surface != nullptr ? "layer" : "window");
  if (s->observer->callback == nullptr) return;
  auto* scene_root = root(s);
  if (scene_root->lock_surface != nullptr && scene_root->lock_surface->lock == nullptr) return;
  if (s->observer->seat != nullptr && s->observer->seat->session_lock != nullptr && scene_root->lock_surface == nullptr) return;
  // Ignore late commits to layers detached with a retired output.
  if (scene_root->layer_surface != nullptr && scene_root->layer_surface->closed) return;
  const OutputId output = scene_root->layer_surface != nullptr ? scene_root->layer_surface->output :
                            scene_root->lock_surface != nullptr ? scene_root->lock_surface->output :
                            scene_root->xdg_surface != nullptr ? scene_root->xdg_surface->output :
#ifdef ZWWM_XWAYLAND
                            scene_root->xwayland_surface != nullptr ? scene_root->xwayland_surface->output :
#endif
                            OutputId{};
  if (scene_root->xdg_surface != nullptr && scene_root->xdg_surface->toplevel != nullptr &&
      !visible_xdg(s->observer, scene_root->xdg_surface)) {
    s->observer->callback(s->observer->data, ShmBufferView{.surface_id = s->id,
        .root_surface_id = scene_root->id, .output = output, .window_shader = {}, .border_shader = {}});
    return;
  }
#ifdef ZWWM_XWAYLAND
  if (scene_root->xwayland_surface != nullptr) {
    const auto* role = scene_root->xwayland_surface;
    const auto* owner = output_state(s->observer, role->output);
    if (owner == nullptr || role->tag != owner->active_tag) {
      s->observer->callback(s->observer->data, ShmBufferView{.surface_id = s->id,
          .root_surface_id = scene_root->id, .output = output, .window_shader = {}, .border_shader = {}});
      return;
    }
  }
#endif
  const bool root_mapped = scene_root->xdg_surface != nullptr ? scene_root->xdg_surface->mapped :
                            scene_root->layer_surface != nullptr ? scene_root->layer_surface->mapped :
                            scene_root->lock_surface != nullptr ? scene_root->lock_surface->mapped :
#ifdef ZWWM_XWAYLAND
                            scene_root->xwayland_surface != nullptr && scene_root->xwayland_surface->window != nullptr &&
                                scene_root->xwayland_surface->window->mapped;
#else
                             false;
#endif
#ifdef ZWWM_XWAYLAND
  bool xwayland_content_ready = root_mapped;
  if (auto* role = scene_root->xwayland_surface;
      role != nullptr && role->window != nullptr && !role->window->override_redirect) {
    const Rect geometry = surface_local_bounds(scene_root);
    xwayland_content_ready = root_mapped && scene_root->current_buffer != nullptr &&
        geometry.width == role->content_bounds.width && geometry.height == role->content_bounds.height;
  }
#endif
  if (root_mapped && s->current_buffer != nullptr && output && output != s->entered_output && s->observer->outputs != nullptr) {
    const auto send_membership = [&](OutputId id, bool enter) {
      const auto state = std::find_if(s->observer->outputs->begin(), s->observer->outputs->end(), [&](const auto& item) {
        return item->info.id == id;
      });
      if (state == s->observer->outputs->end()) return;
      const auto client = s->resource->client;
      const auto resource = std::find_if((*state)->resources.begin(), (*state)->resources.end(), [&](zwayland::server::Resource* item) {
        return item->client == client;
      });
      if (resource != (*state)->resources.end()) {
        if (enter) protocol::wl_surface_send_enter(*s->resource, *resource);
        else protocol::wl_surface_send_leave(*s->resource, *resource);
      }
    };
    if (s->entered_output) send_membership(s->entered_output, false);
    send_membership(output, true);
    s->entered_output = output;
  }
  ShmBufferView view{.surface_id = s->id, .root_surface_id = scene_root->id, .output = output,
      .toplevel = (s->xdg_surface != nullptr && s->xdg_surface->toplevel != nullptr)
#ifdef ZWWM_XWAYLAND
          || s->xwayland_surface != nullptr
#endif
      , .fullscreen = scene_root->xdg_surface != nullptr && scene_root->xdg_surface->fullscreen,
      .focused = s->observer->seat != nullptr && s->observer->seat->toplevel_focus == scene_root,
      .window_shader = {}, .border_shader = {}};
  if (auto* layer = root(s)->layer_surface; layer != nullptr) {
    view.layer_surface = true;
    view.layer = layer->current.layer;
    const std::uint64_t rank = layer->current.layer == protocol::ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ? 0ULL :
                               layer->current.layer == protocol::ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM ? 1ULL :
                               layer->current.layer == protocol::ZWLR_LAYER_SHELL_V1_LAYER_TOP ? 3ULL : 4ULL;
    view.scene_rank = static_cast<std::uint32_t>(rank);
    view.layer_priority = layer->xwlr == nullptr ? 0 : layer->xwlr->current_priority;
    view.root_order = layer->creation;
    view.tree_order = s->stack_order;
    const auto effect = s->observer->config->layer_effect(layer->name_space);
    view.compositor_opacity = (layer->xwlr == nullptr ? 1.0F : static_cast<float>(layer->xwlr->current_opacity) / 256.0F) * effect.opacity;
    view.background_blur_radius = s == scene_root ? effect.blur_radius : 0U;
    view.background_blur_ignore_alpha = s == scene_root ? effect.ignore_alpha : 0.0F;
    view.scene_order = layer->creation;
  } else {
    view.scene_rank = 2;
    auto* xdg_root = scene_root->xdg_surface;
    const bool portal_dialog = xdg_root != nullptr && portal_surface(scene_root);
    view.layer_priority = portal_dialog ? 40 : belongs_to_popup(s) ? 30 : xdg_root != nullptr && xdg_root->fullscreen ? 20 :
                           xdg_root != nullptr && xdg_root->floating ? 10 : 0;
    view.root_order = scene_root->root_order;
    view.tree_order = s->stack_order;
    view.scene_order = root(s)->id;
    if (auto* xdg_root = scene_root->xdg_surface; xdg_root != nullptr && xdg_root->toplevel != nullptr) {
      view.compositor_opacity = xdg_root->rule_opacity;
      view.background_blur_radius = s == scene_root ? xdg_root->background_blur_radius : 0U;
      view.glass = s == scene_root && xdg_root->glass;
    }
  }
  view.popup = belongs_to_popup(s);
  if (scene_root->xdg_surface != nullptr && scene_root->xdg_surface->toplevel != nullptr) {
    view.window_shader = scene_root->xdg_surface->window_shader;
    view.border_shader = scene_root->xdg_surface->border_shader;
  } else {
    view.window_shader = s->observer->config->window_shader;
    view.border_shader = s->observer->config->border_shader;
  }
  view.stack_index = static_cast<std::uint32_t>(s->stack_order);
  const auto* seat = s->observer == nullptr ? nullptr : s->observer->seat;
  const bool canvas_interaction = endless_canvas(s->observer) && seat != nullptr &&
      (seat->canvas_panning || seat->interactive != nullptr);
  view.suppress_geometry_animation = view.toplevel && seat != nullptr &&
      (seat->canvas_zooming || (seat->interactive != nullptr && !canvas_interaction));
  view.track_geometry_animation = view.toplevel && canvas_interaction;
  absolute_position(s, &view.x, &view.y);
    if (auto* xdg_root = root(s)->xdg_surface;
        xdg_root != nullptr && xdg_root->toplevel != nullptr) {
      view.assigned_tile_x = xdg_root->tile_bounds.x;
      view.assigned_tile_y = xdg_root->tile_bounds.y;
      view.assigned_tile_width = xdg_root->tile_bounds.width;
      view.assigned_tile_height = xdg_root->tile_bounds.height;
      view.assigned_content_x = xdg_root->content_bounds.x;
      view.assigned_content_y = xdg_root->content_bounds.y;
      view.assigned_content_width = xdg_root->content_bounds.width;
      view.assigned_content_height = xdg_root->content_bounds.height;
      if (endless_canvas(s->observer) && !xdg_root->fullscreen && xdg_root->canvas_bounds.initialized) {
        const auto work = output_work(s->observer, xdg_root->output);
        const auto* viewport = canvas_viewport(s->observer, xdg_root->output, xdg_root->tag);
        if (viewport != nullptr) {
          const auto tile = layout::canvas_intrinsic_bounds(xdg_root->canvas_bounds, *viewport, work);
          const auto content = s->observer->config->content_bounds(
              {{tile.x, tile.y}, {static_cast<std::uint32_t>(tile.width),
                                  static_cast<std::uint32_t>(tile.height)}});
          view.assigned_tile_x = tile.x; view.assigned_tile_y = tile.y;
          view.assigned_tile_width = tile.width; view.assigned_tile_height = tile.height;
          view.assigned_content_x = content.origin.x; view.assigned_content_y = content.origin.y;
          view.assigned_content_width = static_cast<std::int32_t>(content.size.width);
          view.assigned_content_height = static_cast<std::int32_t>(content.size.height);
          view.camera_scale = static_cast<float>(viewport->scale);
          view.camera_center_x = work.x + work.width / 2;
          view.camera_center_y = work.y + work.height / 2;
        }
      }
      view.content_ready = xdg_root->content_ready;
    }
#ifdef ZWWM_XWAYLAND
    else if (auto* role = root(s)->xwayland_surface; role != nullptr && role->window != nullptr) {
      const auto* window = role->window;
      view.assigned_tile_x = role->tile_bounds.x;
      view.assigned_tile_y = role->tile_bounds.y;
      view.assigned_tile_width = role->tile_bounds.width;
      view.assigned_tile_height = role->tile_bounds.height;
      view.assigned_content_x = role->content_bounds.x;
      view.assigned_content_y = role->content_bounds.y;
       view.assigned_content_width = role->content_bounds.width;
       view.assigned_content_height = role->content_bounds.height;
       if (endless_canvas(s->observer) && !role->fullscreen && role->canvas_bounds.initialized) {
         const auto work = output_work(s->observer, role->output);
         const auto* viewport = canvas_viewport(s->observer, role->output, role->tag);
         if (viewport != nullptr) {
           const auto tile = layout::canvas_intrinsic_bounds(role->canvas_bounds, *viewport, work);
           const auto content = s->observer->config->content_bounds(
               {{tile.x, tile.y}, {static_cast<std::uint32_t>(tile.width),
                                   static_cast<std::uint32_t>(tile.height)}});
           view.assigned_tile_x = tile.x; view.assigned_tile_y = tile.y;
           view.assigned_tile_width = tile.width; view.assigned_tile_height = tile.height;
           view.assigned_content_x = content.origin.x; view.assigned_content_y = content.origin.y;
           view.assigned_content_width = static_cast<std::int32_t>(content.size.width);
           view.assigned_content_height = static_cast<std::int32_t>(content.size.height);
           view.camera_scale = static_cast<float>(viewport->scale);
           view.camera_center_x = work.x + work.width / 2;
           view.camera_center_y = work.y + work.height / 2;
         }
       }
       view.window_geometry_width = window->width;
       view.window_geometry_height = window->height;
       view.content_ready = xwayland_content_ready;
      view.layer_priority = role->floating ? 10 : 0;
    }
#endif
    if (s->xdg_surface != nullptr) {
      view.window_geometry_x = s->xdg_surface->window_geometry.x;
      view.window_geometry_y = s->xdg_surface->window_geometry.y;
      view.window_geometry_width = s->xdg_surface->window_geometry.width;
      view.window_geometry_height = s->xdg_surface->window_geometry.height;
    }
    if (root_mapped && s->current_buffer != nullptr &&
        (s->xdg_surface == nullptr || s->xdg_surface->content_ready)
#ifdef ZWWM_XWAYLAND
        && (scene_root->xwayland_surface == nullptr || xwayland_content_ready)
#endif
        ) {
     const auto* b = s->current_buffer;
     if (b->pool != nullptr) view.pixels = static_cast<const std::uint8_t*>(b->pool->mapping) + b->offset;
      view.width = b->width; view.height = b->height; view.stride = b->stride; view.format = b->format;
      view.buffer_generation = s->buffer_generation;
      view.dmabuf = b->dmabuf ? &*b->dmabuf : nullptr;
     const auto& viewport = s->current_viewport;
     const double scale = s->current_buffer_scale;
     const double source_x = viewport.has_source ? (viewport.x) * scale : 0.0;
     const double source_y = viewport.has_source ? (viewport.y) * scale : 0.0;
     const double source_width = viewport.has_source ? (viewport.width) * scale : b->width;
     const double source_height = viewport.has_source ? (viewport.height) * scale : b->height;
     view.logical_width = viewport.has_destination ? viewport.destination_width : viewport.has_source ? static_cast<int>(viewport.width) : b->width / s->current_buffer_scale;
     view.logical_height = viewport.has_destination ? viewport.destination_height : viewport.has_source ? static_cast<int>(viewport.height) : b->height / s->current_buffer_scale;
      view.source_left = static_cast<float>(source_x / b->width); view.source_top = static_cast<float>(source_y / b->height);
      view.source_right = static_cast<float>((source_x + source_width) / b->width); view.source_bottom = static_cast<float>((source_y + source_height) / b->height);
      view.opaque = b->format == protocol::WL_SHM_FORMAT_XRGB8888 || b->format == DRM_FORMAT_XRGB8888;
      if (!view.opaque && view.logical_width > 0 && view.logical_height > 0 && !s->current_opaque_region.empty()) {
        RegionState uncovered{{Rect{0, 0, view.logical_width, view.logical_height}}};
        for (const Rect& rect : s->current_opaque_region) region_subtract(&uncovered, rect);
        view.opaque = uncovered.rects.empty();
      }
   }
  if (!s->pending_buffer_damage.empty()) {
    Rect damage = s->pending_buffer_damage.front();
    for (const auto& item : s->pending_buffer_damage) {
      const auto left = std::min(damage.x, item.x), top = std::min(damage.y, item.y);
      const auto right = std::max(damage.x + damage.width, item.x + item.width), bottom = std::max(damage.y + damage.height, item.y + item.height);
      damage = {left, top, right - left, bottom - top};
    }
    view.damage_x = damage.x; view.damage_y = damage.y; view.damage_width = damage.width; view.damage_height = damage.height;
  }
  s->observer->callback(s->observer->data, view);
  s->pending_buffer_damage.clear();
}
void notify_surface_tree(SurfaceState* s) {
  if (s == nullptr) return;
  notify_surface(s);
  for (auto* child : s->children) notify_surface_tree(child);
}
struct TagsClient {
  Observer* observer = nullptr;
  zwayland::server::Client* client = nullptr;
  zwayland::server::Resource* resource = nullptr;
  std::vector<zwayland::server::Resource*> observed_toplevels;
};
struct TagsState { std::vector<TagsClient*> clients; };

void send_tags(TagsClient* client) {
  if (client == nullptr || client->resource == nullptr) return;
  for (std::uint32_t tag = 1; tag <= 9; ++tag) {
    const std::string name = std::to_string(tag);
    protocol::zwwm_tags_v1_send_tag(*client->resource, tag, name.c_str());
  }
  protocol::zwwm_tags_v1_send_done(*client->resource);
}
void send_active_tags(TagsClient* client) {
  if (client == nullptr || client->resource == nullptr || client->observer == nullptr) return;
  std::vector<std::uint32_t> tags;
  if (auto* output = output_state(client->observer, client->observer->active_output);
      output != nullptr && output->active_tag != 0) tags.push_back(output->active_tag);
  protocol::zwwm_tags_v1_send_active_tags(*client->resource, std::as_bytes(std::span(tags)));
}
void send_toplevel_tags(TagsClient* client, zwayland::server::Resource* toplevel, std::uint8_t tag) {
  if (client == nullptr || client->resource == nullptr || toplevel == nullptr) return;
  std::vector<std::uint32_t> tags;
  if (tag != 0) tags.push_back(tag);
  protocol::zwwm_tags_v1_send_toplevel_tags(*client->resource, toplevel, std::as_bytes(std::span(tags)));
}
void notify_active_tags(Observer* observer) {
  if (observer == nullptr || observer->tags == nullptr) return;
  for (auto* client : observer->tags->clients) send_active_tags(client);
}
void notify_toplevel_tags(Observer* observer, XdgSurfaceState* x) {
  if (observer == nullptr || observer->tags == nullptr || x == nullptr || x->toplevel == nullptr) return;
  for (auto* client : observer->tags->clients)
    if (client->client == x->toplevel->client ||
        std::find(client->observed_toplevels.begin(), client->observed_toplevels.end(), x->toplevel) != client->observed_toplevels.end())
      send_toplevel_tags(client, x->toplevel, x->tag);
}
void notify_toplevel_closed(Observer* observer, zwayland::server::Resource* toplevel) {
  if (observer == nullptr || observer->tags == nullptr || toplevel == nullptr) return;
  for (auto* client : observer->tags->clients) {
    const auto observed = std::find(client->observed_toplevels.begin(), client->observed_toplevels.end(), toplevel);
    if (observed != client->observed_toplevels.end()) {
      protocol::zwwm_tags_v1_send_toplevel_closed(*client->resource, toplevel);
      client->observed_toplevels.erase(observed);
    }
  }
}
// Notify through the surface observer so backends can retain outgoing windows.
void switch_active_tag(Observer* observer, OutputId output_id, std::uint8_t tag) {
  auto* output = output_state(observer, output_id);
  if (output == nullptr || output->active_tag == tag) return;
  const std::int8_t direction = tag > output->active_tag ? 1 : -1;
  output->active_tag = tag;
  if (observer->event_callback != nullptr) observer->event_callback(observer->event_data, "tag");
  if (observer->callback != nullptr)
    observer->callback(observer->data, ShmBufferView{.output = output_id, .tag_transition = true,
                                                       .tag_transition_direction = direction,
                                                       .window_shader = {}, .border_shader = {}});
  SurfaceState* fallback = nullptr;
  for (auto it = observer->surfaces->rbegin(); it != observer->surfaces->rend(); ++it) {
    auto* candidate = (*it)->xdg_surface;
    if ((*it)->parent == nullptr && candidate != nullptr && candidate->toplevel != nullptr &&
        candidate->output == output_id && visible_xdg(observer, candidate)) { fallback = *it; break; }
#ifdef ZWWM_XWAYLAND
    auto* xwayland = (*it)->xwayland_surface;
    if ((*it)->parent == nullptr && xwayland != nullptr && xwayland->window != nullptr &&
        xwayland->window->mapped && xwayland->output == output_id && xwayland->tag == tag) {
      fallback = *it;
      break;
    }
#endif
  }
  if (observer->seat != nullptr && output_id == observer->active_output) set_keyboard_focus(observer->seat, fallback);
  configure_layout(observer);
  for (auto* surface : *observer->surfaces)
    if (surface->parent == nullptr && ((surface->xdg_surface != nullptr && surface->xdg_surface->output == output_id)
#ifdef ZWWM_XWAYLAND
        || (surface->xwayland_surface != nullptr && surface->xwayland_surface->output == output_id)
#endif
        ))
      notify_surface_tree(surface);
  if (observer->callback != nullptr)
    observer->callback(observer->data, ShmBufferView{.output = output_id, .tag_transition = true,
                                                       .tag_transition_ready = true,
                                                       .tag_transition_direction = direction,
                                                       .window_shader = {}, .border_shader = {}});
  notify_active_tags(observer);
}
bool decode_single_tag(zwayland::server::Resource* resource, const std::span<const std::byte> tags, bool allow_empty, std::uint8_t* tag) {
  if (tags.size() % sizeof(std::uint32_t) != 0 || tags.size() > sizeof(std::uint32_t)) {
    resource->post_error(protocol::WL_DISPLAY_ERROR_INVALID_METHOD, "tags must contain at most one uint32 id");
    return false;
  }
  if (tags.empty()) {
    if (!allow_empty) {
      resource->post_error(protocol::WL_DISPLAY_ERROR_INVALID_METHOD, "a toplevel must have one tag");
      return false;
    }
    *tag = 0;
    return true;
  }
  std::uint32_t value = 0;
  std::memcpy(&value, tags.data(), sizeof(value));
  if (value == 0 || value > 9) {
    resource->post_error(protocol::WL_DISPLAY_ERROR_INVALID_METHOD, "unknown tag id");
    return false;
  }
  *tag = static_cast<std::uint8_t>(value);
  return true;
}
void tags_client_destroyed(zwayland::server::Resource* resource) {
  auto* client = resource->data<TagsClient>();
  if (client == nullptr) return;
  if (client->observer != nullptr && client->observer->tags != nullptr)
    std::erase(client->observer->tags->clients, client);
  delete client;
}
struct ZwwmTagsV1KTagsHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void get_toplevel_tags(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* toplevel) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* resource, zwayland::server::Resource* toplevel) {
      auto* state = resource->data<TagsClient>();
      auto* x = toplevel == nullptr ? nullptr : toplevel->data<XdgSurfaceState>();
      if (state == nullptr || x == nullptr || x->toplevel != toplevel || toplevel->client != client) {
        resource->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "toplevel is not owned by this client");
        return;
      }
      if (std::find(state->observed_toplevels.begin(), state->observed_toplevels.end(), toplevel) == state->observed_toplevels.end())
        state->observed_toplevels.push_back(toplevel);
      send_toplevel_tags(state, toplevel, x->tag);
    })(&client, &resource, toplevel);
  }
  void set_active_tags(zwayland::server::Client& client, zwayland::server::Resource& resource, std::span<const std::byte> tags) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, std::span<const std::byte> tags) {
      auto* state = resource->data<TagsClient>();
      std::uint8_t tag = 0;
      if (state != nullptr && decode_single_tag(resource, tags, true, &tag))
        switch_active_tag(state->observer, state->observer->active_output, tag);
    })(&client, &resource, tags);
  }
  void set_toplevel_tags(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* toplevel, std::span<const std::byte> tags) {
    ([](zwayland::server::Client* client, zwayland::server::Resource* resource, zwayland::server::Resource* toplevel, std::span<const std::byte> tags) {
      auto* x = toplevel == nullptr ? nullptr : toplevel->data<XdgSurfaceState>();
      std::uint8_t tag = 0;
      if (x == nullptr || x->toplevel != toplevel || toplevel->client != client) {
        resource->post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "toplevel is not owned by this client");
        return;
      }
      if (!decode_single_tag(resource, tags, false, &tag) || x->tag == tag) return;
      x->tag = tag;
      auto* observer = x->surface == nullptr ? nullptr : x->surface->observer;
      if (observer == nullptr) return;
      configure_layout(observer);
      notify_surface_tree(x->surface);
      notify_toplevel_tags(observer, x);
    })(&client, &resource, toplevel, tags);
  }
};
void bind_tags(zwayland::server::Client* client, void* data, std::uint32_t version, std::uint32_t id) {
  auto* observer = static_cast<Observer*>(data);
  auto* resource = client->create_resource(&protocol::zwwm_tags_v1_interface, id, std::min(version, 1U));
  auto* state = resource == nullptr ? nullptr : new (std::nothrow) TagsClient;
  if (resource == nullptr || state == nullptr) { if (resource != nullptr) resource->destroy(); client->post_no_memory(); return; }
  state->observer = observer; state->client = client; state->resource = resource;
  observer->tags->clients.push_back(state);
  resource->set_data(state); resource->set_handler(protocol::zwwm_tags_v1_handler(ZwwmTagsV1KTagsHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (tags_client_destroyed)(&destroyed); });
  send_tags(state);
  send_active_tags(state);
}
void notify_surface_removed(SurfaceState* s) {
  if (s == nullptr) return;
  auto* observer = s->observer;
  bool layer_was_mapped = false;
  if (s->layer_surface != nullptr) {
    layer_was_mapped = s->layer_surface->mapped;
    if (auto* output = output_state(observer, s->layer_surface->output); output != nullptr)
      std::erase(output->layer_roots[s->layer_surface->current.layer], s);
    s->layer_surface->mapped = false;
    s->layer_surface->surface = nullptr;
  }
  if (observer != nullptr && observer->callback != nullptr)
    observer->callback(observer->data, ShmBufferView{.surface_id = s->id, .root_surface_id = root(s)->id,
                                                       .output = s->entered_output,
                                                       .window_shader = {}, .border_shader = {}});
  if (layer_was_mapped && observer != nullptr) configure_layout(observer);
}
void surface_destroyed(zwayland::server::Resource* resource) {
  auto* surface = resource->data<SurfaceState>();
  if (surface == nullptr) return;
  *surface->alive = false;
  if (surface->subsurface != nullptr) surface->subsurface->userdata.reset();
  const bool was_mapped = surface->xdg_surface != nullptr && surface->xdg_surface->mapped;
  for (auto* child : std::vector<SurfaceState*>(surface->children))
    if (child->xdg_surface != nullptr && child->xdg_surface->popup != nullptr)
      popup_done(child->xdg_surface->popup);
  for (auto* child : surface->children) child->parent = nullptr;
  surface->children.clear();
  if (surface->observer != nullptr && surface->observer->seat != nullptr) {
    auto* seat = surface->observer->seat;
    if (root(seat->interactive) == root(surface)) end_interactive(seat);
    for (auto* constraint : seat->constraints) {
      if (constraint->surface != surface) continue;
      deactivate_constraint(constraint, true);
      constraint->surface = nullptr;
      constraint->defunct = true;
    }
    if (root(seat->keyboard_focus) == root(surface))
      set_keyboard_focus(seat, focus_fallback(surface->observer, root(surface)));
    if (seat->pointer_focus == surface) seat->pointer_focus = nullptr;
    if (seat->hit_target == surface) seat->hit_target = nullptr;
    if (seat->toplevel_focus == surface) seat->toplevel_focus = nullptr;
    if (seat->pointer_grab == surface) seat->pointer_grab = nullptr;
    if (seat->regular_focus == surface) seat->regular_focus = nullptr;
    drag_surface_destroyed(seat, surface);
  }
  if (surface->parent != nullptr) {
    auto* parent = surface->parent;
    std::erase(parent->children, surface);
    surface->parent = nullptr;
    refresh_tree_stacking(parent);
  }
  notify_surface_removed(surface);
  if (surface->observer != nullptr && surface->observer->surfaces != nullptr)
    std::erase(*surface->observer->surfaces, surface);
  if (surface->xdg_surface != nullptr) surface->xdg_surface->surface = nullptr;
  if (was_mapped) configure_layout(surface->observer);
  if (surface->pending_buffer != surface->current_buffer) {
    unref_buffer(surface->pending_buffer);
  }
  if (!surface->current_buffer_released) send_buffer_release(surface->current_buffer);
  unref_buffer(surface->current_buffer);
  while (!surface->pending_frame_callbacks.empty()) surface->pending_frame_callbacks.back()->destroy();
  while (!surface->frame_callbacks.empty()) surface->frame_callbacks.back()->destroy();
  delete surface;
}
void surface_destroy(zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); }
void surface_attach(zwayland::server::Client*, zwayland::server::Resource* r,
                    zwayland::server::Resource* b, std::int32_t, std::int32_t) {
  auto* s = r->data<SurfaceState>();
  auto* next = b == nullptr ? nullptr : b->data<Buffer>();
  if (s->pending_buffer != next) {
    if (s->pending_buffer != s->current_buffer) unref_buffer(s->pending_buffer);
    s->pending_buffer = next;
    // Pending and current share one reference when they name the same buffer.
    if (next != nullptr && next != s->current_buffer) ++next->references;
  }
  s->buffer_changed = true;
}
void surface_damage(zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) { if (w > 0 && h > 0) r->data<SurfaceState>()->pending_surface_damage.push_back({x, y, w, h}); }
void surface_damage_buffer(zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) { if (w > 0 && h > 0) r->data<SurfaceState>()->pending_buffer_damage.push_back({x, y, w, h}); }
void surface_frame(zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t id) {
  auto* cb = c->create_resource(&protocol::wl_callback_interface, id, 1);
  if (cb == nullptr) { c->post_no_memory(); return; }
  auto* surface = r->data<SurfaceState>();
  surface->pending_frame_callbacks.push_back(cb);
  // Client teardown can destroy a callback before its surface (IDs are reused).
  cb->set_destroy_handler([surface](zwayland::server::Resource& callback) {
    std::erase(surface->pending_frame_callbacks, &callback);
    std::erase(surface->frame_callbacks, &callback);
  });
}
bool validate_surface_state(SurfaceState* surface, Buffer* buffer, const ViewportConfig& viewport,
                            std::int32_t scale) {
  if (buffer == nullptr) return true;
  if (buffer->width % scale != 0 || buffer->height % scale != 0) {
    surface->resource->post_error(protocol::WL_SURFACE_ERROR_INVALID_SIZE,
                           "buffer dimensions are not divisible by buffer scale");
    return false;
  }
  if (!viewport.has_source) return true;
  if (!viewport.has_destination &&
      (std::trunc(viewport.width) != viewport.width || std::trunc(viewport.height) != viewport.height)) {
    surface->viewport->resource->post_error(protocol::WP_VIEWPORT_ERROR_BAD_SIZE,
                           "viewport source size must be integral without a destination");
    return false;
  }
  const double max_x = static_cast<double>(buffer->width) / scale;
  const double max_y = static_cast<double>(buffer->height) / scale;
  if (viewport.x + viewport.width > max_x || viewport.y + viewport.height > max_y) {
    surface->viewport->resource->post_error(protocol::WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
                           "viewport source extends outside the buffer");
    return false;
  }
  return true;
}
void surface_commit(zwayland::server::Client*, zwayland::server::Resource* r) {
  auto* s = r->data<SurfaceState>();
  s->ever_committed = true;
  if (s->lock_surface != nullptr) {
    auto* lock = s->lock_surface;
    if (lock->acked == 0) { lock->resource->post_error(protocol::EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK, "lock configure not acknowledged"); return; }
    if (s->pending_buffer == nullptr && s->current_buffer == nullptr) { lock->resource->post_error(protocol::EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER, "lock surface requires a buffer"); return; }
  }
  if (s->layer_surface != nullptr) {
    auto* layer = s->layer_surface;
    const auto& state = layer->pending;
    const auto anchor = state.anchor;
    if ((state.width == 0 && (anchor & (protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) !=
                                 (protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) ||
        (state.height == 0 && (anchor & (protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) !=
                                  (protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | protocol::ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))) {
      s->layer_surface->resource->post_error(protocol::ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
                             "zero layer dimension requires opposite anchors");
      return;
    }
    if (layer->xwlr != nullptr && layer->xwlr->pending_edge != 0 &&
        (state.anchor & layer->xwlr->pending_edge) == 0) {
      layer->xwlr->resource->post_error(protocol::XWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                             "exclusive edge is not anchored by the committed layer state");
      return;
    }
  }
  if (s->layer_surface != nullptr && s->pending_buffer != nullptr && !s->layer_surface->mapped && !s->layer_surface->configured) {
    s->layer_surface->resource->post_error(protocol::ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                           "buffer committed before layer configure was acknowledged");
    return;
  }
  auto* x = s->xdg_surface;
  if (x != nullptr && s->pending_buffer != nullptr && !x->content_ready &&
      !x->has_acked_configure) {
    x->resource->post_error(protocol::XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                           "buffer committed before initial configure was acknowledged");
    return;
  }
  Buffer* next_buffer = s->buffer_changed ? s->pending_buffer : s->current_buffer;
  const bool layer_detach = s->layer_surface != nullptr && s->buffer_changed && next_buffer == nullptr;
  const bool layer_was_mapped = s->layer_surface != nullptr && s->layer_surface->mapped;
  const auto& next_viewport = s->viewport_changed ? s->pending_viewport : s->current_viewport;
  const auto next_scale = s->buffer_scale_changed ? s->pending_buffer_scale : s->current_buffer_scale;
  if (!validate_surface_state(s, next_buffer, next_viewport, next_scale)) return;
  // Frame requests are double-buffered surface state, not immediately eligible
  // for completion by an unrelated output presentation.
  s->frame_callbacks.insert(s->frame_callbacks.end(), s->pending_frame_callbacks.begin(),
                            s->pending_frame_callbacks.end());
  s->pending_frame_callbacks.clear();
  const bool was_mapped = x != nullptr && x->mapped;
  bool constraints_changed = false;
  if (x != nullptr && x->toplevel != nullptr) {
    constraints_changed = x->min_width != x->pending_min_width || x->min_height != x->pending_min_height ||
                          x->max_width != x->pending_max_width || x->max_height != x->pending_max_height;
    x->min_width = x->pending_min_width;
    x->min_height = x->pending_min_height;
    x->max_width = x->pending_max_width;
    x->max_height = x->pending_max_height;
  }
  const bool new_buffer_attached = s->buffer_changed && next_buffer != nullptr;
  Buffer* previous_buffer = nullptr;
  bool previous_buffer_released = true;
  if (s->buffer_changed) {
    if (s->current_buffer != s->pending_buffer) {
      previous_buffer = s->current_buffer;
      previous_buffer_released = s->current_buffer_released;
    }
    s->current_buffer = s->pending_buffer;
    s->current_buffer_released = false;
    ++s->buffer_generation;
    s->buffer_changed = false;
  }
  if (s->viewport_changed) { s->current_viewport = s->pending_viewport; s->viewport_changed = false; }
  if (s->buffer_scale_changed) { s->current_buffer_scale = s->pending_buffer_scale; s->buffer_scale_changed = false; }
  if (s->input_region_changed) {
    s->current_input_region = s->pending_input_region;
    s->current_input_infinite = s->pending_input_infinite;
    s->input_region_changed = false;
  }
  if (s->opaque_region_changed) {
    s->current_opaque_region = s->pending_opaque_region;
    s->opaque_region_changed = false;
  }
  if (s->layer_surface != nullptr) {
    auto* layer = s->layer_surface;
    const auto old_layer = layer->current.layer;
    const bool standard_changed = old_layer != layer->pending.layer || layer->current.anchor != layer->pending.anchor ||
                               layer->current.width != layer->pending.width || layer->current.height != layer->pending.height ||
                               layer->current.zone != layer->pending.zone || layer->current.top != layer->pending.top ||
                               layer->current.right != layer->pending.right || layer->current.bottom != layer->pending.bottom ||
                               layer->current.left != layer->pending.left || layer->current.keyboard != layer->pending.keyboard;
    const bool extension_changed = layer->xwlr != nullptr &&
        (layer->xwlr->current_edge != layer->xwlr->pending_edge ||
         layer->xwlr->current_priority != layer->xwlr->pending_priority ||
         layer->xwlr->current_opacity != layer->xwlr->pending_opacity);
    layer->current = layer->pending;
    if (layer->xwlr != nullptr) {
      layer->xwlr->current_edge = layer->xwlr->pending_edge;
      layer->xwlr->current_priority = layer->xwlr->pending_priority;
      layer->xwlr->current_opacity = layer->xwlr->pending_opacity;
    }
    if (old_layer != layer->current.layer && s->observer != nullptr) {
      if (auto* output = output_state(s->observer, layer->output); output != nullptr) {
        std::erase(output->layer_roots[old_layer], s);
        output->layer_roots[layer->current.layer].push_back(s);
      }
    }
    if ((old_layer != layer->current.layer || extension_changed) && s->observer != nullptr)
      reorder_layer_roots(s->observer, layer->output, layer->current.layer);
    if (layer_detach) {
      layer->mapped = false;
      layer->configured = false;
      layer->configure_requested = false;
      layer->serials.clear();
      layer->last_sent = 0;
      layer->last_acked = 0;
    } else if (!layer->mapped && s->current_buffer != nullptr && layer->configured) {
      layer->mapped = true;
    } else if (!layer->mapped) {
      // An initial (or remap) empty commit asks the arrangement pass for the
      // one configure required before a buffer may be attached.
      layer->configure_requested = true;
    }
    if (standard_changed || extension_changed || layer_was_mapped != layer->mapped || !layer->mapped)
      configure_layout(s->observer);
    if (extension_changed) notify_surface_tree(s);
    if (layer->xwlr != nullptr && layer->current.zone > 0 && layer->xwlr->current_edge == 0 &&
        reservation_edge(layer) == 0) {
      if (!layer->xwlr->ignored_reported || layer->xwlr->reported_anchor != layer->current.anchor ||
          layer->xwlr->reported_zone != layer->current.zone) {
        protocol::xwlr_layer_surface_v1_send_report(*layer->xwlr->resource,
                                           protocol::XWLR_LAYER_SURFACE_V1_REPORT_CODE_EXCLUSIVE_ZONE_IGNORED,
                                           "positive exclusive zone has ambiguous anchors without an explicit edge");
        layer->xwlr->ignored_reported = true;
        layer->xwlr->reported_anchor = layer->current.anchor;
        layer->xwlr->reported_zone = layer->current.zone;
      }
    } else if (layer->xwlr != nullptr) {
      layer->xwlr->ignored_reported = false;
    }
    refresh_layer_keyboard_focus(s->observer);
  }
  if (s->current_buffer != nullptr) {
    const auto* buffer = s->current_buffer;
    const auto& viewport = s->current_viewport;
    const double source_x = viewport.has_source ? (viewport.x) * s->current_buffer_scale : 0.0;
    const double source_y = viewport.has_source ? (viewport.y) * s->current_buffer_scale : 0.0;
    const double source_width = viewport.has_source ? (viewport.width) * s->current_buffer_scale : buffer->width;
    const double source_height = viewport.has_source ? (viewport.height) * s->current_buffer_scale : buffer->height;
    const double logical_width = viewport.has_destination ? viewport.destination_width : source_width / s->current_buffer_scale;
    const double logical_height = viewport.has_destination ? viewport.destination_height : source_height / s->current_buffer_scale;
    for (const Rect& damage : s->pending_surface_damage) {
      const auto left = static_cast<std::int32_t>(std::floor(source_x + damage.x * source_width / logical_width));
      const auto top = static_cast<std::int32_t>(std::floor(source_y + damage.y * source_height / logical_height));
      const auto right = static_cast<std::int32_t>(std::ceil(source_x + (static_cast<std::int64_t>(damage.x) + damage.width) * source_width / logical_width));
      const auto bottom = static_cast<std::int32_t>(std::ceil(source_y + (static_cast<std::int64_t>(damage.y) + damage.height) * source_height / logical_height));
      s->pending_buffer_damage.push_back({left, top, right - left, bottom - top});
    }
    if (new_buffer_attached && s->pending_buffer_damage.empty())
      s->pending_buffer_damage.push_back({0, 0, buffer->width, buffer->height});
  }
  s->pending_surface_damage.clear();
  if (x != nullptr) {
    ++x->commit_count;
    x->ever_committed = true;
    const bool initial_configured = x->has_acked_configure && x->commit_count > x->ack_commit_count;
    // Configure dimensions do not invalidate a differently sized current buffer.
    if (s->current_buffer == nullptr) x->content_ready = false;
    else if (x->popup != nullptr) {
      if (!x->content_ready) x->content_ready = initial_configured;
    } else if (x->toplevel != nullptr) {
      if (!x->content_ready) x->content_ready = initial_configured;
    }
    if (x->toplevel != nullptr) {
      x->mapped = s->current_buffer != nullptr && initial_configured;
    } else {
      x->mapped = x->content_ready && (x->popup == nullptr ||
                   (x->popup->parent != nullptr && x->popup->parent->mapped) ||
                   (x->popup->layer_parent != nullptr && x->popup->layer_parent->layer_surface != nullptr &&
                     x->popup->layer_parent->layer_surface->mapped));
    }
    if (!was_mapped && x->mapped && x->popup != nullptr && x->popup->grabbed) activate_popup_grab(x->popup);
    if (x->toplevel != nullptr && x->mapped) apply_window_rules(x);
    if (x->content_ready) {
      const Rect geometry = effective_window_geometry(s);
      x->accepted_width = geometry.width;
      x->accepted_height = geometry.height;
    }
    const bool toplevel_unmapped = x->toplevel != nullptr && s->current_buffer == nullptr;
    if (toplevel_unmapped && s->observer != nullptr && s->observer->seat != nullptr &&
        root(s->observer->seat->interactive) == root(s)) end_interactive(s->observer->seat);
    if ((was_mapped && !x->mapped) || toplevel_unmapped) {
      for (auto* child : std::vector<SurfaceState*>(s->children)) {
        if (child->xdg_surface != nullptr && child->xdg_surface->popup != nullptr) popup_done(child->xdg_surface->popup);
      }
    }
  }
  if (s->lock_surface != nullptr) {
    auto* lock = s->lock_surface;
    if (s->current_buffer == nullptr) { lock->resource->post_error(protocol::EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER, "lock surface requires a buffer"); return; }
    const auto width = s->current_viewport.has_destination ? s->current_viewport.destination_width : s->current_buffer->width / s->current_buffer_scale;
    const auto height = s->current_viewport.has_destination ? s->current_viewport.destination_height : s->current_buffer->height / s->current_buffer_scale;
    if (width != static_cast<std::int32_t>(lock->width) || height != static_cast<std::int32_t>(lock->height)) {
      lock->resource->post_error(protocol::EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH, "lock surface dimensions mismatch"); return;
    }
    lock->mapped = true; lock->presented = false;
    notify_surface(s);
    if (s->observer->seat->keyboard_focus == nullptr) set_keyboard_focus(s->observer->seat, s);
  }
  if (x != nullptr && x->toplevel != nullptr && s->observer != nullptr && s->observer->seat != nullptr) {
    auto* seat = s->observer->seat;
    if (!was_mapped && x->mapped && x->accepted_width > 1 && x->accepted_height > 1) {
      auto* parent = x->transient_parent == nullptr ? nullptr : x->transient_parent->surface;
      const bool focus_transient = parent != nullptr && root(seat->keyboard_focus) == root(parent);
      if (seat->keyboard_focus == nullptr || portal_surface(s) || focus_transient) {
        auto* old_focus = seat->toplevel_focus;
        set_keyboard_focus(seat, s);
        if (old_focus != nullptr && old_focus != s) notify_surface(old_focus);
      }
    } else if (was_mapped && !x->mapped) {
      if (root(seat->interactive) == s) end_interactive(seat);
      if (root(seat->keyboard_focus) == s) {
        auto* fallback = focus_fallback(s->observer, s);
        set_keyboard_focus(seat, fallback);
        if (fallback != nullptr) notify_surface(fallback);
      }
      if (root(seat->pointer_focus) == s) seat->pointer_focus = nullptr;
      if (root(seat->hit_target) == s) seat->hit_target = nullptr;
      if (root(seat->pointer_grab) == s) seat->pointer_grab = nullptr;
      if (seat->toplevel_focus == s) seat->toplevel_focus = nullptr;
      if (seat->regular_focus == s) seat->regular_focus = nullptr;
    }
  }
  if ((x != nullptr && x->toplevel != nullptr && (was_mapped != x->mapped || constraints_changed)) ||
      s->layer_surface != nullptr) configure_layout(s->observer);
  if (s->observer != nullptr && s->observer->seat != nullptr) {
    auto* seat = s->observer->seat;
    for (auto* constraint : seat->constraints) if (constraint->surface == s) {
      if (constraint->region_pending) {
        constraint->region = std::move(constraint->pending_region);
        constraint->has_region = constraint->pending_has_region;
        constraint->region_pending = false;
      }
      if (constraint->hint_pending) {
        constraint->hint_x = constraint->pending_hint_x;
        constraint->hint_y = constraint->pending_hint_y;
        constraint->has_hint = true;
        constraint->hint_pending = false;
      }
      if (constraint->active && constraint_rects(constraint).empty()) deactivate_constraint(constraint, true);
    }
    if (seat->pointer_focus == s) {
      std::int32_t sx = 0, sy = 0;
      absolute_position(s, &sx, &sy);
      const auto old_constraint = std::find_if(seat->constraints.begin(), seat->constraints.end(), [s](const PointerConstraint* constraint) { return constraint->surface == s && constraint->active && !constraint->locked; });
      const std::int32_t old_x = old_constraint == seat->constraints.end() ? 0 : (*old_constraint)->x;
      const std::int32_t old_y = old_constraint == seat->constraints.end() ? 0 : (*old_constraint)->y;
      std::int32_t local_x = seat->pointer_x - sx, local_y = seat->pointer_y - sy;
      if (root(s) != nullptr && root(s)->xdg_surface != nullptr) {
        surface_local_from_global(s, seat->pointer_x, seat->pointer_y, &local_x, &local_y);
      }
      update_pointer_constraints(seat, local_x, local_y);
      if (old_constraint != seat->constraints.end() && (*old_constraint)->active &&
          (old_x != (*old_constraint)->x || old_y != (*old_constraint)->y)) {
        if (root(s) != nullptr && root(s)->xdg_surface != nullptr)
          surface_global_from_local(s, (*old_constraint)->x, (*old_constraint)->y, &seat->pointer_x, &seat->pointer_y);
        else {
          seat->pointer_x = sx + (*old_constraint)->x;
          seat->pointer_y = sy + (*old_constraint)->y;
        }
        auto* client = s->resource->client;
        for (auto* pointer : seat->pointers) if (pointer->client == client) {
          protocol::wl_pointer_send_motion(*pointer, timestamp_ms(), (*old_constraint)->x, (*old_constraint)->y);
        }
        send_pointer_frame(seat, client);
        sync_pointer_position(seat);
      }
    }
  }
  notify_surface(s);
  if (previous_buffer != nullptr) {
    if (!previous_buffer_released) send_buffer_release(previous_buffer);
    unref_buffer(previous_buffer);
  }
  if (!s->current_buffer_released && s->current_buffer != nullptr && s->current_buffer->pool != nullptr) {
    send_buffer_release(s->current_buffer);
    s->current_buffer_released = true;
  }
}
void surface_set_opaque_region(zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* region_resource) {
  auto* surface = resource->data<SurfaceState>();
  surface->pending_opaque_region = region_resource == nullptr ? std::vector<Rect>{}
                                                               : region_resource->data<RegionState>()->rects;
  surface->opaque_region_changed = true;
}
void surface_set_input_region(zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* region_resource) {
  auto* surface = resource->data<SurfaceState>();
  surface->pending_input_infinite = region_resource == nullptr;
  surface->pending_input_region = region_resource == nullptr ? std::vector<Rect>{}
                                                              : region_resource->data<RegionState>()->rects;
  surface->input_region_changed = true;
}
struct WlSurfaceKSurfaceHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    (surface_destroy)(&client, &resource);
  }
  void attach(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* buffer, std::int32_t x, std::int32_t y) {
    (surface_attach)(&client, &resource, buffer, x, y);
  }
  void damage(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    (surface_damage)(&client, &resource, x, y, width, height);
  }
  void frame(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t callback) {
    (surface_frame)(&client, &resource, callback);
  }
  void set_opaque_region(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* region) {
    (surface_set_opaque_region)(&client, &resource, region);
  }
  void set_input_region(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* region) {
    (surface_set_input_region)(&client, &resource, region);
  }
  void commit(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    (surface_commit)(&client, &resource);
  }
  void set_buffer_transform(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t transform) {
    ([](zwayland::server::Client*, zwayland::server::Resource*, std::int32_t) {})(&client, &resource, transform);
  }
  void set_buffer_scale(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t scale) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t scale) { if (scale <= 0) { resource->post_error(protocol::WL_SURFACE_ERROR_INVALID_SCALE, "buffer scale must be positive"); return; } auto* surface = resource->data<SurfaceState>(); surface->pending_buffer_scale = scale; surface->buffer_scale_changed = true; })(&client, &resource, scale);
  }
  void damage_buffer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    (surface_damage_buffer)(&client, &resource, x, y, width, height);
  }
  void offset(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y) {
    ([](zwayland::server::Client*, zwayland::server::Resource*, std::int32_t, std::int32_t) {})(&client, &resource, x, y);
  }
  void get_release(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t callback) {
    (void)client; (void)callback; resource.post_error(1, "request is not implemented");
  }
};
SurfaceState* compositor_surface_from_resource(zwayland::server::Client* client, zwayland::server::Resource* resource) {
  return resource != nullptr && resource->client == client &&
                 resource->interface == &protocol::wl_surface_interface && resource->data<SurfaceState>() != nullptr
             ? resource->data<SurfaceState>()
             : nullptr;
}
void create_surface(zwayland::server::Client* c, zwayland::server::Resource* comp, std::uint32_t id) { auto* r = c->create_resource(&protocol::wl_surface_interface, id, kCompositorVersion); auto* s = r == nullptr ? nullptr : new (std::nothrow) SurfaceState; if (r == nullptr || s == nullptr) { if (r != nullptr) r->destroy(); c->post_no_memory(); return; } static std::uint64_t next = 1; s->resource = r; s->id = next++; s->root_order = next_root_order(); s->stack_order = s->id; s->observer = comp->data<Observer>(); s->observer->surfaces->push_back(s); r->set_data(s); r->set_handler(protocol::wl_surface_handler(WlSurfaceKSurfaceHandler{})); r->set_destroy_handler([](zwayland::server::Resource& destroyed) { (surface_destroyed)(&destroyed); }); }
void associate_layer_popup(LayerSurfaceState* layer, zwayland::server::Client* client, zwayland::server::Resource* layer_resource,
                           zwayland::server::Resource* popup_resource) {
  auto* popup = popup_resource == nullptr ? nullptr : popup_resource->data<PopupState>();
  if (layer == nullptr || layer->surface == nullptr || popup == nullptr || popup->xdg == nullptr || popup->xdg->surface == nullptr || popup->parent != nullptr || popup->layer_parent != nullptr || popup->dismissed || popup_resource->client != client || popup->xdg->resource->client != client) { layer_resource->post_error(protocol::ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE, "invalid layer popup"); return; }
  auto* child = popup->xdg->surface;
  if (child->parent != nullptr) { layer_resource->post_error(protocol::ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE, "popup already has a parent"); return; }
  popup->layer_parent = layer->surface; child->parent = layer->surface; child->above_parent = true; layer->surface->children.push_back(child); refresh_tree_stacking(layer->surface); send_popup_configure(popup);
}
void region_subtract(RegionState* region, Rect cut) {
  if (cut.width <= 0 || cut.height <= 0) return;
  std::vector<Rect> result;
  for (const Rect rect : region->rects) {
    const std::int64_t left = std::max<std::int64_t>(rect.x, cut.x), top = std::max<std::int64_t>(rect.y, cut.y);
    const std::int64_t right = std::min<std::int64_t>(static_cast<std::int64_t>(rect.x) + rect.width, static_cast<std::int64_t>(cut.x) + cut.width);
    const std::int64_t bottom = std::min<std::int64_t>(static_cast<std::int64_t>(rect.y) + rect.height, static_cast<std::int64_t>(cut.y) + cut.height);
    if (left >= right || top >= bottom) { result.push_back(rect); continue; }
    if (rect.y < top) result.push_back({rect.x, rect.y, rect.width, static_cast<std::int32_t>(top - rect.y)});
    if (bottom < static_cast<std::int64_t>(rect.y) + rect.height) result.push_back({rect.x, static_cast<std::int32_t>(bottom), rect.width, static_cast<std::int32_t>(rect.y + static_cast<std::int64_t>(rect.height) - bottom)});
    if (rect.x < left) result.push_back({rect.x, static_cast<std::int32_t>(top), static_cast<std::int32_t>(left - rect.x), static_cast<std::int32_t>(bottom - top)});
    if (right < static_cast<std::int64_t>(rect.x) + rect.width) result.push_back({static_cast<std::int32_t>(right), static_cast<std::int32_t>(top), static_cast<std::int32_t>(rect.x + static_cast<std::int64_t>(rect.width) - right), static_cast<std::int32_t>(bottom - top)});
  }
  region->rects = std::move(result);
}
struct WlRegionKRegionHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void add(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) { if (width > 0 && height > 0) resource->data<RegionState>()->rects.push_back({x, y, width, height}); })(&client, &resource, x, y, width, height);
  }
  void subtract(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) { region_subtract(resource->data<RegionState>(), {x, y, width, height}); })(&client, &resource, x, y, width, height);
  }
};
void create_region(zwayland::server::Client* c, zwayland::server::Resource*, std::uint32_t id) { auto* r = c->create_resource(&protocol::wl_region_interface, id, 1); auto* region = r == nullptr ? nullptr : new (std::nothrow) RegionState; if (r == nullptr || region == nullptr) { if (r != nullptr) r->destroy(); c->post_no_memory(); return; } r->set_data(region); r->set_handler(protocol::wl_region_handler(WlRegionKRegionHandler{})); r->set_destroy_handler([](zwayland::server::Resource& destroyed) { ([](zwayland::server::Resource* resource) { delete resource->data<RegionState>(); })(&destroyed); }); }
struct WlCompositorKCompositorHandler {
  void create_surface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    (::zwwm::detail::create_surface)(&client, &resource, id);
  }
  void create_region(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    (::zwwm::detail::create_region)(&client, &resource, id);
  }
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
};
void sub_destroyed(zwayland::server::Resource* r) {
  auto* s = r->data<SurfaceState>();
  if (s == nullptr) return;
  s->subsurface = nullptr;
  if (s->parent != nullptr) {
    auto* parent = s->parent;
    std::erase(parent->children, s);
    s->parent = nullptr;
    refresh_tree_stacking(parent);
  }
}
void refresh_tree_stacking(SurfaceState* surface, std::uint64_t* index) {
  if (surface == nullptr || index == nullptr) return;
  for (auto* child : surface->children) if (!child->above_parent) refresh_tree_stacking(child, index);
  surface->stack_order = (*index)++;
  if (surface->current_buffer != nullptr) notify_surface(surface);
  for (auto* child : surface->children) if (child->above_parent) refresh_tree_stacking(child, index);
}
void refresh_tree_stacking(SurfaceState* surface) {
  auto* top = root(surface);
  std::uint64_t index = 0;
  refresh_tree_stacking(top, &index);
}
void place_subsurface(SurfaceState* surface, SurfaceState* sibling, bool above) {
  if (surface == nullptr || surface->parent == nullptr || sibling == nullptr ||
      (sibling != surface->parent && sibling->parent != surface->parent) || sibling == surface) return;
  auto& children = surface->parent->children;
  std::erase(children, surface);
  if (sibling == surface->parent) {
    surface->above_parent = above;
    const auto boundary = std::find_if(children.begin(), children.end(), [](const SurfaceState* child) { return child->above_parent; });
    children.insert(above ? children.end() : boundary, surface);
  } else {
    surface->above_parent = sibling->above_parent;
    const auto position = std::find(children.begin(), children.end(), sibling);
    children.insert(above ? std::next(position) : position, surface);
  }
  refresh_tree_stacking(surface);
}
void get_subsurface(zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t id, zwayland::server::Resource* child_r, zwayland::server::Resource* parent_r) { auto* child = child_r->data<SurfaceState>(); auto* parent = parent_r->data<SurfaceState>(); if (child == nullptr || parent == nullptr || child == parent || child->subsurface != nullptr || child->parent != nullptr || child->xdg_surface != nullptr || child->layer_role_assigned || child->drag_icon_role) { r->post_error(protocol::WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "surface already has a role"); return; } auto* sub = c->create_resource(&protocol::wl_subsurface_interface, id, 1); if (sub == nullptr) { c->post_no_memory(); return; } child->parent = parent; parent->children.push_back(child); refresh_tree_stacking(parent); struct WlSubsurfaceImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* q) { q->destroy(); })(&client, &resource);
  }
  void set_position(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y) {
    ([](zwayland::server::Client*, zwayland::server::Resource* q, std::int32_t x, std::int32_t y) { auto* s = q->data<SurfaceState>(); if (s == nullptr) return; s->x = x; s->y = y; notify_surface(s); })(&client, &resource, x, y);
  }
  void place_above(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* sibling) {
    ([](zwayland::server::Client*, zwayland::server::Resource* q, zwayland::server::Resource* sibling_r) { place_subsurface(q->data<SurfaceState>(), sibling_r->data<SurfaceState>(), true); })(&client, &resource, sibling);
  }
  void place_below(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* sibling) {
    ([](zwayland::server::Client*, zwayland::server::Resource* q, zwayland::server::Resource* sibling_r) { place_subsurface(q->data<SurfaceState>(), sibling_r->data<SurfaceState>(), false); })(&client, &resource, sibling);
  }
  void set_sync(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource*) {})(&client, &resource);
  }
  void set_desync(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource*) {})(&client, &resource);
  }
}; child->subsurface = sub; sub->set_data(child); sub->set_handler(protocol::wl_subsurface_handler(WlSubsurfaceImplHandler{})); sub->set_destroy_handler([](zwayland::server::Resource& destroyed) { (sub_destroyed)(&destroyed); }); }
struct WlSubcompositorKSubcompositorHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
  void get_subsurface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface, zwayland::server::Resource* parent) {
    (::zwwm::detail::get_subsurface)(&client, &resource, id, surface, parent);
  }
};
void top_destroyed(zwayland::server::Resource* r) {
  auto* x = r->data<XdgSurfaceState>();
  if (x == nullptr) return;
  auto* observer = x->surface == nullptr ? nullptr : x->surface->observer;
  auto* seat = observer == nullptr ? nullptr : observer->seat;
  const bool was_focused = seat != nullptr && root(seat->keyboard_focus) == x->surface;
  if (x->surface != nullptr) notify_toplevel_closed(x->surface->observer, r);
  if (x->decoration != nullptr) {
    x->decoration->post_error(protocol::ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
                           "xdg_toplevel destroyed before its decoration object");
  }
  const bool was_mapped = x->mapped;
  if (x->surface != nullptr && x->surface->observer != nullptr && x->surface->observer->seat != nullptr &&
      root(x->surface->observer->seat->interactive) == x->surface) end_interactive(x->surface->observer->seat);
  if (x->surface != nullptr) if (auto* output = output_state(x->surface->observer, x->output); output != nullptr && output->fullscreen == x->surface) output->fullscreen = nullptr;
  if (x->surface != nullptr) for (auto* child : std::vector<SurfaceState*>(x->surface->children)) if (child->xdg_surface != nullptr && child->xdg_surface->popup != nullptr) popup_done(child->xdg_surface->popup);
  x->mapped = false;
  if (x->dialog != nullptr) x->dialog->xdg = nullptr;
  if (x->surface != nullptr && x->surface->observer != nullptr && x->surface->observer->surfaces != nullptr)
    for (auto* surface : *x->surface->observer->surfaces)
      if (surface->xdg_surface != nullptr && surface->xdg_surface->transient_parent == x)
        surface->xdg_surface->transient_parent = nullptr;
  x->toplevel = nullptr;
  if (was_focused) set_keyboard_focus(seat, focus_fallback(observer, x->surface));
  if (seat != nullptr && seat->regular_focus == x->surface) seat->regular_focus = nullptr;
  if (x->surface != nullptr) notify_surface(x->surface);
  if (was_mapped && x->surface != nullptr) configure_layout(x->surface->observer);
}
void set_floating(XdgSurfaceState* x, bool floating) {
  if (x == nullptr || x->fullscreen || x->floating == floating) return;
  x->floating = floating;
  if (floating && x->last_floating_bounds.width > 0 && x->last_floating_bounds.height > 0) x->floating_bounds = x->last_floating_bounds;
  if (!floating) x->last_floating_bounds = x->floating_bounds;
  configure_layout(x->surface->observer, x);
}
void set_toplevel_parent(zwayland::server::Client* client, zwayland::server::Resource* resource, zwayland::server::Resource* parent_resource) {
  auto* x = resource->data<XdgSurfaceState>();
  auto* parent = parent_resource == nullptr ? nullptr :
      parent_resource->data<XdgSurfaceState>();
  if (x == nullptr || (parent_resource != nullptr &&
      (parent == nullptr || parent == x || parent->toplevel != parent_resource ||
       parent_resource->client != client))) return;
  x->transient_parent = parent;
  if (parent == nullptr) return;
  x->output = parent->output;
  x->tag = parent->tag;
}
void set_fullscreen(XdgSurfaceState* x, OutputId requested, bool enabled) {
  if (x == nullptr || x->surface == nullptr) return;
  auto* observer = x->surface->observer;
  if (enabled) {
    const OutputId target_id = requested && output_state(observer, requested) != nullptr ? requested : x->output;
    auto* target = output_state(observer, target_id);
    if (target == nullptr || (target->fullscreen != nullptr && target->fullscreen != x->surface)) return;
    if (x->fullscreen) {
      if (x->output == target_id) return;
      if (auto* previous = output_state(observer, x->output); previous != nullptr && previous->fullscreen == x->surface)
        previous->fullscreen = nullptr;
    } else {
      x->restore_floating = x->floating;
      if (x->floating) x->last_floating_bounds = x->floating_bounds;
    }
    x->output = target_id;
    x->tag = target->active_tag;
    x->fullscreen = true; x->floating = false; target->fullscreen = x->surface;
  } else {
    if (!x->fullscreen) return;
    auto* output = output_state(observer, x->output);
    if (output != nullptr && output->fullscreen == x->surface) output->fullscreen = nullptr;
    x->fullscreen = false; x->floating = x->restore_floating;
  }
  configure_layout(observer, x);
}
bool valid_grab_serial(XdgSurfaceState* x, zwayland::server::Client* client, std::uint32_t serial) {
  auto* seat = x == nullptr || x->surface == nullptr ? nullptr : x->surface->observer->seat;
  return seat != nullptr && seat->button_client == client && seat->button_serial == serial &&
         seat->pointer_grab != nullptr && root(seat->pointer_grab) == x->surface;
}
void begin_interactive(XdgSurfaceState* x, zwayland::server::Client* client, std::uint32_t serial, std::uint32_t edge) {
  if (!valid_grab_serial(x, client, serial) || x->fullscreen) return;
  if (edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE && edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP &&
      edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM && edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT &&
      edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT && edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT &&
      edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_RIGHT && edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT &&
      edge != protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT) return;
  if (endless_canvas(x->surface->observer)) configure_layout(x->surface->observer, x);
  else set_floating(x, true);
  auto* seat = x->surface->observer->seat;
  seat->interactive = x->surface; seat->resize_edge = edge;
  seat->interactive_start = endless_canvas(x->surface->observer) ? x->tile_bounds : x->floating_bounds;
  seat->interactive_canvas_x = x->canvas_bounds.x; seat->interactive_canvas_y = x->canvas_bounds.y;
  seat->interactive_canvas_width = x->canvas_bounds.width;
  seat->interactive_canvas_height = x->canvas_bounds.height;
  seat->interactive_pointer_x = seat->pointer_x; seat->interactive_pointer_y = seat->pointer_y;
}
void end_interactive(SeatState* seat) {
  if (seat == nullptr) return;
  const bool restore_cursor = seat->canvas_panning;
  seat->interactive = nullptr;
  seat->resize_edge = protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE;
  seat->compositor_interactive = false;
  seat->tiled_resize = false;
  seat->canvas_panning = false;
  seat->interactive_button = 0;
  seat->interactive_output = {};
  seat->weight_before = seat->weight_after = 0;
  configure_layout(seat->display.observer);
  if (restore_cursor) {
    seat->cursor_override_shape.clear();
    apply_cursor_shape(seat);
  }
}
void toplevel_fullscreen(zwayland::server::Client*, zwayland::server::Resource* resource, zwayland::server::Resource* output_resource) {
  auto* x = resource->data<XdgSurfaceState>();
  auto* output = output_resource == nullptr || std::strcmp(output_resource->interface->name, "wl_output") != 0 ? nullptr :
      output_resource->data<OutputState>();
  set_fullscreen(x, output == nullptr || output->retired ? OutputId{} : output->info.id, true);
}
struct XdgToplevelKTopHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
  void set_parent(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* parent) {
    (set_toplevel_parent)(&client, &resource, parent);
  }
  void set_title(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string title) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, const char* value) { auto* x = r->data<XdgSurfaceState>(); x->title = value == nullptr ? "" : value; notify_surface(x->surface); })(&client, &resource, title.c_str());
  }
  void set_app_id(zwayland::server::Client& client, zwayland::server::Resource& resource, std::string app_id) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, const char* value) { auto* x = r->data<XdgSurfaceState>(); x->app_id = value == nullptr ? "" : value; notify_surface(x->surface); })(&client, &resource, app_id.c_str());
  }
  void show_window_menu(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* seat, std::uint32_t serial, std::int32_t x, std::int32_t y) {
    ([](zwayland::server::Client*, zwayland::server::Resource*, zwayland::server::Resource*, std::uint32_t, std::int32_t, std::int32_t) {})(&client, &resource, seat, serial, x, y);
  }
  void move(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* seat, std::uint32_t serial) {
    ([](zwayland::server::Client* c, zwayland::server::Resource* r, zwayland::server::Resource*, std::uint32_t serial) { begin_interactive(r->data<XdgSurfaceState>(), c, serial, protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE); })(&client, &resource, seat, serial);
  }
  void resize(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* seat, std::uint32_t serial, std::uint32_t edges) {
    ([](zwayland::server::Client* c, zwayland::server::Resource* r, zwayland::server::Resource*, std::uint32_t serial, std::uint32_t edge) { begin_interactive(r->data<XdgSurfaceState>(), c, serial, edge); })(&client, &resource, seat, serial, edges);
  }
  void set_max_size(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t width, std::int32_t height) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t w, std::int32_t h) { auto* x = r->data<XdgSurfaceState>(); if (w >= 0 && h >= 0) { x->pending_max_width = w; x->pending_max_height = h; } })(&client, &resource, width, height);
  }
  void set_min_size(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t width, std::int32_t height) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t w, std::int32_t h) { auto* x = r->data<XdgSurfaceState>(); if (w >= 0 && h >= 0) { x->pending_min_width = w; x->pending_min_height = h; } })(&client, &resource, width, height);
  }
  void set_maximized(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource*) {})(&client, &resource);
  }
  void unset_maximized(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource*) {})(&client, &resource);
  }
  void set_fullscreen(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* output) {
    (toplevel_fullscreen)(&client, &resource, output);
  }
  void unset_fullscreen(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { ::zwwm::detail::set_fullscreen(r->data<XdgSurfaceState>(), {}, false); })(&client, &resource);
  }
  void set_minimized(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource*) {})(&client, &resource);
  }
};
void dialog_destroyed(zwayland::server::Resource* resource) {
  auto* dialog = resource->data<DialogState>();
  if (dialog == nullptr) return;
  if (dialog->xdg != nullptr && dialog->xdg->dialog == dialog) dialog->xdg->dialog = nullptr;
  delete dialog;
}
struct XdgDialogV1KDialogHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_modal(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) {
      auto* dialog = resource->data<DialogState>();
      if (dialog == nullptr || dialog->xdg == nullptr) return;
      dialog->modal = true;
      if (dialog->xdg->mapped) set_floating(dialog->xdg, true);
      else dialog->xdg->floating = true;
    })(&client, &resource);
  }
  void unset_modal(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) {
      auto* dialog = resource->data<DialogState>();
      if (dialog != nullptr) dialog->modal = false;
    })(&client, &resource);
  }
};
void bind_dialog_manager(zwayland::server::Client* client, void*, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::xdg_wm_dialog_v1_interface, id, std::min(version, 1U));
  if (resource == nullptr) { client->post_no_memory(); return; }
  struct XdgWmDialogV1ImplHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* manager) { manager->destroy(); })(&client, &resource);
  }
  void get_xdg_dialog(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* toplevel) {
    ([](zwayland::server::Client* dialog_client, zwayland::server::Resource* manager, std::uint32_t dialog_id, zwayland::server::Resource* toplevel) {
        auto* x = toplevel->data<XdgSurfaceState>();
        if (x == nullptr || x->toplevel != toplevel || x->dialog != nullptr) {
          manager->post_error(protocol::XDG_WM_DIALOG_V1_ERROR_ALREADY_USED,
                                 "xdg_toplevel already has a dialog role");
          return;
        }
        auto* dialog_resource = dialog_client->create_resource(&protocol::xdg_dialog_v1_interface, dialog_id, 1);
        auto* dialog = dialog_resource == nullptr ? nullptr : new (std::nothrow) DialogState;
        if (dialog_resource == nullptr || dialog == nullptr) {
          if (dialog_resource != nullptr) dialog_resource->destroy();
          dialog_client->post_no_memory();
          return;
        }
        dialog->resource = dialog_resource;
        dialog->xdg = x;
        x->dialog = dialog;
        dialog_resource->set_data(dialog); dialog_resource->set_handler(protocol::xdg_dialog_v1_handler(XdgDialogV1KDialogHandler{})); dialog_resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (dialog_destroyed)(&destroyed); });
      })(&client, &resource, id, toplevel);
  }
};
  resource->set_handler(protocol::xdg_wm_dialog_v1_handler(XdgWmDialogV1ImplHandler{}));
}
bool valid_positioner(const PositionerState& p) {
  return p.width > 0 && p.height > 0 && p.anchor_rect.width > 0 && p.anchor_rect.height > 0 &&
         static_cast<std::int64_t>(p.anchor_rect.x) + p.anchor_rect.width <= std::numeric_limits<std::int32_t>::max() &&
         static_cast<std::int64_t>(p.anchor_rect.y) + p.anchor_rect.height <= std::numeric_limits<std::int32_t>::max();
}
void send_pointer_frame(SeatState* seat, zwayland::server::Client* client) { for (auto* resource : seat->pointers) if (resource->client == client && resource->version >= 5) protocol::wl_pointer_send_frame(*resource); }
void set_keyboard_focus(SeatState* seat, SurfaceState* next) {
  if (seat != nullptr && seat->compositor_interactive && root(seat->interactive) != root(next)) end_interactive(seat);
  if (next != nullptr && root(next)->xdg_surface != nullptr && root(next)->xdg_surface->toplevel != nullptr && seat != nullptr && seat->keyboard_focus != nullptr) {
    const auto* current = root(seat->keyboard_focus);
    if (current != nullptr && *current->alive && current->layer_surface != nullptr && current->layer_surface->mapped &&
        current->layer_surface->current.keyboard == protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) return;
  }
  if (seat == nullptr || seat->keyboard_focus == next) return;
  if (auto* constraint = active_constraint(seat);
      constraint != nullptr && root(constraint->surface) != root(next))
    deactivate_constraint(constraint, true);
  const auto serial = seat->display.display->next_serial();
  auto* old_client = seat->keyboard_focus == nullptr ? nullptr : seat->keyboard_focus->resource->client;
  auto* new_client = next == nullptr ? nullptr : next->resource->client;
  if (seat->keyboard_focus != nullptr) for (auto* resource : seat->keyboards) if (resource->client == old_client) protocol::wl_keyboard_send_leave(*resource, serial, seat->keyboard_focus->resource);
  selection_focus_changed(seat, old_client, seat->session_lock == nullptr ? new_client : nullptr);
  seat->keyboard_focus = next;
  seat->toplevel_focus = root(next);
#ifdef ZWWM_XWAYLAND
  focus_xwayland_surface(next);
#endif
  if (next != nullptr && !portal_surface(root(next)) &&
      ((root(next)->xdg_surface != nullptr && root(next)->xdg_surface->toplevel != nullptr)
#ifdef ZWWM_XWAYLAND
      || root(next)->xwayland_surface != nullptr
#endif
      )) seat->regular_focus = root(next);
  if (next == nullptr) return;
  const auto keys = std::span(seat->pressed_keys);
  bool entered = false;
  for (auto* resource : seat->keyboards) if (resource->client == new_client) { protocol::wl_keyboard_send_enter(*resource, serial, next->resource, std::as_bytes(keys)); entered = true; }
  if (entered) remember_serial(*seat, new_client, serial);
  send_modifiers(*seat);
}
void refresh_layer_keyboard_focus(Observer* observer) {
  if (observer == nullptr || observer->seat == nullptr) return;
  auto* output = output_state(observer, observer->active_output);
  if (output == nullptr) return;
  SurfaceState* exclusive = nullptr;
  for (const std::uint32_t bucket : {protocol::ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, protocol::ZWLR_LAYER_SHELL_V1_LAYER_TOP}) {
    const auto& roots = output->layer_roots[bucket];
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
      auto* surface = *it;
      if (surface->layer_surface != nullptr && surface->layer_surface->mapped &&
          surface->layer_surface->current.keyboard == protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
        exclusive = surface;
        break;
      }
    }
    if (exclusive != nullptr) break;
  }
  if (exclusive != nullptr) {
    set_keyboard_focus(observer->seat, exclusive);
  } else if (observer->seat->keyboard_focus != nullptr) {
    auto* focused = root(observer->seat->keyboard_focus);
    auto* layer = focused == nullptr ? nullptr : focused->layer_surface;
    if (layer != nullptr && (!layer->mapped || layer->current.keyboard == protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)) {
      set_keyboard_focus(observer->seat, observer->seat->regular_focus);
    }
  }
}
void set_pointer_focus(SeatState* seat, SurfaceState* next, std::int32_t x, std::int32_t y) {
  if (seat == nullptr || seat->pointer_focus == next) return;
  const auto serial = seat->display.display->next_serial();
  auto* old_client = seat->pointer_focus == nullptr ? nullptr : seat->pointer_focus->resource->client;
  auto* new_client = next == nullptr ? nullptr : next->resource->client;
  if (seat->pointer_focus != nullptr) for (auto* resource : seat->pointers) if (resource->client == old_client) {
    protocol::wl_pointer_send_leave(*resource, serial, seat->pointer_focus->resource);
    seat->pointer_enter_serials.erase(resource);
  }
  if (next != nullptr) for (auto* resource : seat->pointers) if (resource->client == new_client) {
    protocol::wl_pointer_send_enter(*resource, serial, next->resource, (x), (y));
    seat->pointer_enter_serials[resource] = serial;
  }
  seat->pointer_focus = next;
  if (new_client != old_client) {
    seat->cursor_shape = "left_ptr";
    seat->cursor_hidden_by_client = false;
    apply_cursor_shape(seat);
  }
  if (new_client != nullptr) remember_serial(*seat, new_client, serial);
  if (old_client != nullptr) send_pointer_frame(seat, old_client);
  if (new_client != nullptr && new_client != old_client) send_pointer_frame(seat, new_client);
  update_pointer_constraints(seat, x, y);
}
[[maybe_unused]] bool descendant_of(const SurfaceState* surface, const SurfaceState* ancestor) { for (auto* item = surface; item != nullptr; item = item->parent) if (item == ancestor) return true; return false; }
bool popup_descendant(const PopupState* child, const PopupState* ancestor) { for (auto* item = child; item != nullptr; item = item->parent == nullptr ? nullptr : item->parent->popup) if (item == ancestor) return true; return false; }
PopupState* popup_chain_root(PopupState* p) { while (p != nullptr && p->parent != nullptr && p->parent->popup != nullptr) p = p->parent->popup; return p; }
void popup_done(PopupState* p) {
  if (p == nullptr || p->dismissed) return;
  p->dismissed = true;
  SurfaceState* surface = p->xdg == nullptr ? nullptr : p->xdg->surface;
  SurfaceState* fallback = p->parent == nullptr ? nullptr : root(p->parent->surface);
  SeatState* seat = surface == nullptr || surface->observer == nullptr ? nullptr : surface->observer->seat;
  if (surface != nullptr) {
    for (auto* child : std::vector<SurfaceState*>(surface->children)) {
      if (child->xdg_surface != nullptr && child->xdg_surface->popup != nullptr) popup_done(child->xdg_surface->popup);
    }
  }
  if (seat != nullptr && seat->popup_grab != nullptr && popup_descendant(seat->popup_grab, p)) {
    seat->popup_grab = nullptr;
    if (seat->pointer_focus != nullptr && descendant_of(seat->pointer_focus, surface)) set_pointer_focus(seat, fallback, 0, 0);
    if (seat->keyboard_focus != nullptr && descendant_of(seat->keyboard_focus, surface)) set_keyboard_focus(seat, fallback);
  }
  if (surface != nullptr) {
    if (surface->parent != nullptr) { auto* parent = surface->parent; std::erase(parent->children, surface); surface->parent = nullptr; refresh_tree_stacking(parent); }
    p->xdg->mapped = false;
    p->xdg->content_ready = false;
    notify_surface(surface);
  }
  p->parent = nullptr;
  if (p->resource != nullptr) protocol::xdg_popup_send_popup_done(*p->resource);
}
void popup_destroyed(zwayland::server::Resource* r) { auto* p = r->data<PopupState>(); if (p == nullptr) return; popup_done(p); if (p->xdg != nullptr) p->xdg->popup = nullptr; delete p; }
void positioner_destroy(zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); }
void positioner_size(zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t w, std::int32_t h) { if (w <= 0 || h <= 0) { r->post_error(protocol::XDG_POSITIONER_ERROR_INVALID_INPUT, "positioner size must be positive"); return; } auto* p = r->data<PositionerState>(); p->width = w; p->height = h; }
void positioner_rect(zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) { if (w <= 0 || h <= 0 || static_cast<std::int64_t>(x) + w > std::numeric_limits<std::int32_t>::max() || static_cast<std::int64_t>(y) + h > std::numeric_limits<std::int32_t>::max()) { r->post_error(protocol::XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor rectangle"); return; } r->data<PositionerState>()->anchor_rect = {x, y, w, h}; }
struct XdgPositionerKPositionerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    (positioner_destroy)(&client, &resource);
  }
  void set_size(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t width, std::int32_t height) {
    (positioner_size)(&client, &resource, width, height);
  }
  void set_anchor_rect(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    (positioner_rect)(&client, &resource, x, y, width, height);
  }
  void set_anchor(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t anchor) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::uint32_t a) { if (a > protocol::XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) { r->post_error(protocol::XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor"); return; } r->data<PositionerState>()->anchor = a; })(&client, &resource, anchor);
  }
  void set_gravity(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t gravity) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::uint32_t g) { if (g > protocol::XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT) { r->post_error(protocol::XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid gravity"); return; } r->data<PositionerState>()->gravity = g; })(&client, &resource, gravity);
  }
  void set_constraint_adjustment(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t constraint_adjustment) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::uint32_t a) { constexpr std::uint32_t valid = protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y | protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y | protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X | protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y; if ((a & ~valid) != 0) { r->post_error(protocol::XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid constraint adjustment"); return; } r->data<PositionerState>()->adjustment = a; })(&client, &resource, constraint_adjustment);
  }
  void set_offset(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t x, std::int32_t y) { auto* p = r->data<PositionerState>(); p->offset_x = x; p->offset_y = y; })(&client, &resource, x, y);
  }
  void set_reactive(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->data<PositionerState>()->reactive = true; })(&client, &resource);
  }
  void set_parent_size(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t parent_width, std::int32_t parent_height) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t w, std::int32_t h) { if (w <= 0 || h <= 0) { r->post_error(protocol::XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid parent size"); return; } auto* p = r->data<PositionerState>(); p->parent_width = w; p->parent_height = h; })(&client, &resource, parent_width, parent_height);
  }
  void set_parent_configure(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::uint32_t s) { r->data<PositionerState>()->parent_configure = s; })(&client, &resource, serial);
  }
};
void popup_destroy(zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); }
void activate_popup_grab(PopupState* p) {
  if (p == nullptr || p->dismissed || !p->grabbed || p->xdg == nullptr || p->xdg->surface == nullptr || !p->xdg->mapped) return;
  auto* seat = p->xdg->surface->observer == nullptr ? nullptr : p->xdg->surface->observer->seat;
  if (seat == nullptr || seat->popup_grab != p) return;
  set_keyboard_focus(seat, p->xdg->surface);
}
void popup_grab(zwayland::server::Client*, zwayland::server::Resource* r, zwayland::server::Resource* seat_r, std::uint32_t) {
  auto* p = r->data<PopupState>();
  auto* seat = seat_r->data<SeatState>();
  const bool nested_ok = p != nullptr && (p->parent == nullptr || p->parent->popup == nullptr || p->parent->popup->grabbed);
  const bool chain_ok = seat != nullptr && (seat->popup_grab == nullptr || (p != nullptr && popup_descendant(p, popup_chain_root(seat->popup_grab))));
  if (p == nullptr || p->dismissed || p->xdg == nullptr || !nested_ok || !chain_ok) return;
  p->grabbed = true;
  seat->popup_grab = p;
  activate_popup_grab(p);
}
void popup_reposition(zwayland::server::Client*, zwayland::server::Resource* r, zwayland::server::Resource* positioner, std::uint32_t token) { auto* p = r->data<PopupState>(); auto* q = positioner->data<PositionerState>(); if (p == nullptr || p->dismissed || q == nullptr || !valid_positioner(*q)) { r->post_error(protocol::XDG_WM_BASE_ERROR_INVALID_POSITIONER, "invalid positioner"); return; } p->positioner = *q; send_popup_configure(p, true, token); }
struct XdgPopupKPopupHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    (popup_destroy)(&client, &resource);
  }
  void grab(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* seat, std::uint32_t serial) {
    (popup_grab)(&client, &resource, seat, serial);
  }
  void reposition(zwayland::server::Client& client, zwayland::server::Resource& resource, zwayland::server::Resource* positioner, std::uint32_t token) {
    (popup_reposition)(&client, &resource, positioner, token);
  }
};
void xdg_destroyed(zwayland::server::Resource* r) { auto* x = r->data<XdgSurfaceState>(); if (x != nullptr) { if (x->popup != nullptr) { popup_done(x->popup); x->popup->xdg = nullptr; } if (x->surface != nullptr) { for (auto* child : std::vector<SurfaceState*>(x->surface->children)) if (child->xdg_surface != nullptr && child->xdg_surface->popup != nullptr) popup_done(child->xdg_surface->popup); x->surface->xdg_surface = nullptr; } if (x->toplevel != nullptr) x->toplevel->userdata.reset(); if (x->decoration != nullptr) x->decoration->userdata.reset(); delete x; } }
void get_top(zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t id) { auto* x = r->data<XdgSurfaceState>(); if (x->toplevel != nullptr) { r->post_error(protocol::XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "surface already has a role"); return; } x->output = x->surface->observer->active_output; if (auto* output = output_state(x->surface->observer, x->output)) x->tag = output->active_tag; x->toplevel = c->create_resource(&protocol::xdg_toplevel_interface, id, r->version); if (x->toplevel == nullptr) { c->post_no_memory(); return; } x->toplevel->set_data(x); x->toplevel->set_handler(protocol::xdg_toplevel_handler(XdgToplevelKTopHandler{})); x->toplevel->set_destroy_handler([](zwayland::server::Resource& destroyed) { (top_destroyed)(&destroyed); }); if (endless_canvas(x->surface->observer)) send_configure(x, 0, 0, 0, true); else configure_layout(x->surface->observer, x); }
void get_popup(zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t id, zwayland::server::Resource* parent_r, zwayland::server::Resource* positioner_r) { auto* x = r->data<XdgSurfaceState>(); auto* parent = parent_r == nullptr ? nullptr : parent_r->data<XdgSurfaceState>(); auto* q = positioner_r->data<PositionerState>(); if (x->toplevel != nullptr || x->popup != nullptr || (parent != nullptr && parent->surface == nullptr) || q == nullptr || !valid_positioner(*q)) { r->post_error(protocol::XDG_WM_BASE_ERROR_INVALID_POSITIONER, "invalid popup construction"); return; } auto* pr = c->create_resource(&protocol::xdg_popup_interface, id, r->version); auto* p = pr == nullptr ? nullptr : new (std::nothrow) PopupState{.resource = pr, .xdg = x, .parent = parent, .positioner = *q}; if (pr == nullptr || p == nullptr) { if (pr != nullptr) pr->destroy(); c->post_no_memory(); return; } x->popup = p; if (parent != nullptr) { x->surface->parent = parent->surface; parent->surface->children.push_back(x->surface); refresh_tree_stacking(parent->surface); } pr->set_data(p); pr->set_handler(protocol::xdg_popup_handler(XdgPopupKPopupHandler{})); pr->set_destroy_handler([](zwayland::server::Resource& destroyed) { (popup_destroyed)(&destroyed); }); if (parent != nullptr) send_popup_configure(p); }
struct XdgSurfaceKXdgSurfaceHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { auto* x = r->data<XdgSurfaceState>(); if (x->toplevel != nullptr || x->popup != nullptr) { r->post_error(protocol::XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT, "xdg role object must be destroyed first"); return; } r->destroy(); })(&client, &resource);
  }
  void get_toplevel(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    (get_top)(&client, &resource, id);
  }
  void get_popup(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* parent, zwayland::server::Resource* positioner) {
    (::zwwm::detail::get_popup)(&client, &resource, id, parent, positioner);
  }
  void set_window_geometry(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) { auto* surface = r->data<XdgSurfaceState>(); if (width <= 0 || height <= 0) { r->post_error(protocol::XDG_SURFACE_ERROR_INVALID_SIZE, "window geometry must have a positive size"); return; } surface->window_geometry = {x, y, width, height}; surface->window_geometry_set = true; })(&client, &resource, x, y, width, height);
  }
  void ack_configure(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r, std::uint32_t serial) { auto* x = r->data<XdgSurfaceState>(); const auto i = std::find(x->serials.begin(), x->serials.end(), serial); if (i == x->serials.end()) return; const auto size = std::find(x->serials.begin(), x->serials.end(), x->size_configure_serial); if (!x->size_configure_acked && size != x->serials.end() && size <= i) { x->size_configure_acked = true; x->size_ack_commit_count = x->commit_count; } x->serials.erase(x->serials.begin(), std::next(i)); x->has_acked_configure = true; x->last_acked_serial = serial; x->ack_commit_count = x->commit_count; })(&client, &resource, serial);
  }
};
void get_xdg(zwayland::server::Client* c, zwayland::server::Resource* wm, std::uint32_t id, zwayland::server::Resource* sr) { auto* s = sr->data<SurfaceState>(); if (s == nullptr || s->xdg_surface != nullptr || s->layer_role_assigned || s->parent != nullptr || s->drag_icon_role) { wm->post_error(protocol::XDG_WM_BASE_ERROR_ROLE, "surface already has a role"); return; } auto* r = c->create_resource(&protocol::xdg_surface_interface, id, wm->version); auto* x = r == nullptr ? nullptr : new (std::nothrow) XdgSurfaceState; if (x != nullptr) { x->resource = r; x->surface = s; } if (r == nullptr || x == nullptr) { if (r != nullptr) r->destroy(); c->post_no_memory(); return; } s->xdg_surface = x; r->set_data(x); r->set_handler(protocol::xdg_surface_handler(XdgSurfaceKXdgSurfaceHandler{})); r->set_destroy_handler([](zwayland::server::Resource& destroyed) { (xdg_destroyed)(&destroyed); }); }
struct XdgWmBaseKWmHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* r) { r->destroy(); })(&client, &resource);
  }
  void create_positioner(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    ([](zwayland::server::Client* c, zwayland::server::Resource* r, std::uint32_t id) { auto* q = c->create_resource(&protocol::xdg_positioner_interface, id, r->version); auto* p = q == nullptr ? nullptr : new (std::nothrow) PositionerState; if (q == nullptr || p == nullptr) { if (q != nullptr) q->destroy(); c->post_no_memory(); return; } q->set_data(p); q->set_handler(protocol::xdg_positioner_handler(XdgPositionerKPositionerHandler{})); q->set_destroy_handler([](zwayland::server::Resource& destroyed) { ([](zwayland::server::Resource* resource) { delete resource->data<PositionerState>(); })(&destroyed); }); })(&client, &resource, id);
  }
  void get_xdg_surface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface) {
    (get_xdg)(&client, &resource, id, surface);
  }
  void pong(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial) {
    ([](zwayland::server::Client*, zwayland::server::Resource*, std::uint32_t) {})(&client, &resource, serial);
  }
};
std::int32_t wayland_transform(OutputTransform transform) {
  switch (transform) {
    case OutputTransform::rotate_90: return protocol::WL_OUTPUT_TRANSFORM_90;
    case OutputTransform::rotate_180: return protocol::WL_OUTPUT_TRANSFORM_180;
    case OutputTransform::rotate_270: return protocol::WL_OUTPUT_TRANSFORM_270;
    default: return protocol::WL_OUTPUT_TRANSFORM_NORMAL;
  }
}
void send_output(const OutputState* output, zwayland::server::Resource* resource, bool initial) {
  const auto& info = output->info;
  const auto scale = std::max(1U, (info.scale_per_mille + 999U) / 1000U);
  if (initial && resource->version >= 4) {
    protocol::wl_output_send_name(*resource, info.connector.c_str());
    const std::string description = "zwwm " + info.connector;
    protocol::wl_output_send_description(*resource, description.c_str());
  }
  protocol::wl_output_send_geometry(*resource, info.logical_x, info.logical_y, 600, 340,
                          protocol::WL_OUTPUT_SUBPIXEL_UNKNOWN, "zwwm", info.connector.c_str(),
                          wayland_transform(info.transform));
  protocol::wl_output_send_mode(*resource, protocol::WL_OUTPUT_MODE_CURRENT | protocol::WL_OUTPUT_MODE_PREFERRED,
                      static_cast<std::int32_t>(info.physical_width),
                      static_cast<std::int32_t>(info.physical_height),
                      static_cast<std::int32_t>(info.refresh_millihz));
  if (resource->version >= 2) {
    protocol::wl_output_send_scale(*resource, static_cast<std::int32_t>(scale));
    protocol::wl_output_send_done(*resource);
  }
}
void bind_output_state(zwayland::server::Client* c, void* data, std::uint32_t v, std::uint32_t id) {
  auto* output = static_cast<OutputState*>(data);
  if (output == nullptr || output->retired) return;
  auto* resource = c->create_resource(&protocol::wl_output_interface, id, std::min(v, kOutputVersion));
  if (resource == nullptr) { c->post_no_memory(); return; }
  struct WlOutputOutputImplHandler {
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* item) { item->destroy(); })(&client, &resource);
  }
};
  output->resources.push_back(resource);
  resource->set_data(output); resource->set_handler(protocol::wl_output_handler(WlOutputOutputImplHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { ([](zwayland::server::Resource* item) {
    auto* state = item->data<OutputState>();
    if (state != nullptr) std::erase(state->resources, item);
  })(&destroyed); });
  send_output(output, resource, true);
}
void bind_comp(zwayland::server::Client* c, void* d, std::uint32_t v, std::uint32_t id) { auto* r = c->create_resource(&protocol::wl_compositor_interface, id, std::min(v, kCompositorVersion)); if (r == nullptr) { c->post_no_memory(); return; } r->set_data(static_cast<Observer*>(d)); r->set_handler(protocol::wl_compositor_handler(WlCompositorKCompositorHandler{})); }
void bind_wm(zwayland::server::Client* c, void*, std::uint32_t v, std::uint32_t id) { auto* r = c->create_resource(&protocol::xdg_wm_base_interface, id, std::min(v, kXdgVersion)); if (r == nullptr) { c->post_no_memory(); return; } r->set_handler(protocol::xdg_wm_base_handler(XdgWmBaseKWmHandler{})); }
void bind_sub(zwayland::server::Client* c, void*, std::uint32_t v, std::uint32_t id) { auto* r = c->create_resource(&protocol::wl_subcompositor_interface, id, std::min(v, kSubcompositorVersion)); if (r == nullptr) { c->post_no_memory(); return; } r->set_handler(protocol::wl_subcompositor_handler(WlSubcompositorKSubcompositorHandler{})); }
void decoration_destroyed(zwayland::server::Resource* resource) {
  auto* x = resource->data<XdgSurfaceState>();
  if (x != nullptr && x->decoration == resource) x->decoration = nullptr;
}
void decoration_preference(zwayland::server::Resource* resource, std::uint32_t preference) {
  auto* x = resource->data<XdgSurfaceState>();
  if (x == nullptr || x->decoration_preference == preference) return;
  x->decoration_preference = preference;
  protocol::zxdg_toplevel_decoration_v1_send_configure(*resource, protocol::ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  if (x->surface != nullptr) configure_layout(x->surface->observer, x);
}
struct ZxdgToplevelDecorationV1KToplevelDecorationHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void set_mode(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t mode) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource, std::uint32_t mode) {
      if (mode != protocol::ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
          mode != protocol::ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        resource->post_error(protocol::ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
                             "invalid xdg decoration mode " + std::to_string(mode));
        return;
      }
      decoration_preference(resource, mode);
    })(&client, &resource, mode);
  }
  void unset_mode(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { decoration_preference(resource, 0); })(&client, &resource);
  }
};
void get_toplevel_decoration(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id,
                             zwayland::server::Resource* toplevel) {
  auto* x = toplevel->data<XdgSurfaceState>();
  if (x == nullptr || x->toplevel != toplevel) {
    client->post_error(0, "decoration requested for an unknown xdg_toplevel");
    return;
  }
  if (x->decoration != nullptr) {
    manager->post_error(protocol::ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                           "xdg_toplevel already has a decoration object");
    return;
  }
  if (manager->version == 1 && x->ever_committed) {
    manager->post_error(protocol::ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
                           "version 1 decoration created after the surface was committed");
    return;
  }
  auto* decoration = client->create_resource(&protocol::zxdg_toplevel_decoration_v1_interface, id, manager->version);
  if (decoration == nullptr) { client->post_no_memory(); return; }
  x->decoration = decoration;
  x->decoration_preference = 0;
  decoration->set_data(x); decoration->set_handler(protocol::zxdg_toplevel_decoration_v1_handler(ZxdgToplevelDecorationV1KToplevelDecorationHandler{})); decoration->set_destroy_handler([](zwayland::server::Resource& destroyed) { (decoration_destroyed)(&destroyed); });
  protocol::zxdg_toplevel_decoration_v1_send_configure(*decoration, protocol::ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  if (x->surface != nullptr) configure_layout(x->surface->observer, x);
}
struct ZxdgDecorationManagerV1KDecorationManagerHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void get_toplevel_decoration(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* toplevel) {
    (::zwwm::detail::get_toplevel_decoration)(&client, &resource, id, toplevel);
  }
};
void bind_decoration_manager(zwayland::server::Client* client, void*, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zxdg_decoration_manager_v1_interface, id, std::min(version, kDecorationVersion));
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_handler(protocol::zxdg_decoration_manager_v1_handler(ZxdgDecorationManagerV1KDecorationManagerHandler{}));
}
}  // namespace detail
using namespace detail;
struct CompositorServer::Impl { std::uint32_t compositor = 0, shm = 0, wm = 0, dialog_manager = 0, decoration_manager = 0, cursor_shape_manager = 0, pointer_constraints = 0, relative_pointer_manager = 0, seat = 0, sub = 0, dmabuf = 0, data_device_manager = 0, data_control_manager = 0, wlr_data_control_manager = 0, session_lock_manager = 0, tags_manager = 0; std::shared_ptr<const RuntimeConfig> config; Observer observer; TagsState tags; std::vector<SurfaceState*> surfaces; SeatState seat_state; DmabufState dmabuf_state; std::vector<std::unique_ptr<OutputState>> outputs; std::vector<std::unique_ptr<OutputState>> retired_outputs; OutputId active_output; ~Impl() { const auto release = [this](auto& list) { for (auto& state : list) { if (state->global != 0) seat_state.display.display->destroy_global(state->global); for (auto* resource : state->resources) resource->userdata.reset(); } }; release(outputs); release(retired_outputs); } };
 CompositorServer::CompositorServer(zwayland::server::Display* d, std::shared_ptr<const RuntimeConfig> config) : impl_(new Impl) { impl_->config = std::move(config); if (impl_->config == nullptr) { delete impl_; impl_ = nullptr; throw std::invalid_argument("runtime configuration is required"); } impl_->observer.surfaces = &impl_->surfaces; impl_->observer.seat = &impl_->seat_state; impl_->observer.config = impl_->config.get(); impl_->observer.tags = &impl_->tags; impl_->seat_state.display.observer = &impl_->observer; impl_->seat_state.display = d; if (!initialize_xkb(&impl_->seat_state)) { release_xkb(&impl_->seat_state); delete impl_; impl_ = nullptr; throw std::runtime_error("could not initialize XKB for seat0"); } impl_->compositor = d->add_global(&protocol::wl_compositor_interface, kCompositorVersion, [data = &impl_->observer](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_comp)(&client, data, bound_version, id); }); impl_->shm = d->add_global(&protocol::wl_shm_interface, kShmVersion, [data = this](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_shm)(&client, data, bound_version, id); }); impl_->wm = d->add_global(&protocol::xdg_wm_base_interface, kXdgVersion, [data = this](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_wm)(&client, data, bound_version, id); }); impl_->tags_manager = d->add_global(&protocol::zwwm_tags_v1_interface, 1, [data = &impl_->observer](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_tags)(&client, data, bound_version, id); }); impl_->decoration_manager = d->add_global(&protocol::zxdg_decoration_manager_v1_interface, kDecorationVersion, [data = this](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_decoration_manager)(&client, data, bound_version, id); }); impl_->cursor_shape_manager = d->add_global(&protocol::wp_cursor_shape_manager_v1_interface, 1, [data = &impl_->observer](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_cursor_shape_manager)(&client, data, bound_version, id); }); impl_->pointer_constraints = d->add_global(&protocol::zwp_pointer_constraints_v1_interface, 1, [data = &impl_->seat_state](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_pointer_constraints)(&client, data, bound_version, id); }); impl_->seat = d->add_global(&protocol::wl_seat_interface, kSeatVersion, [data = &impl_->seat_state](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_seat)(&client, data, bound_version, id); }); impl_->sub = d->add_global(&protocol::wl_subcompositor_interface, kSubcompositorVersion, [data = this](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_sub)(&client, data, bound_version, id); }); impl_->data_device_manager = d->add_global(&protocol::wl_data_device_manager_interface, kDataDeviceManagerVersion, [data = &impl_->seat_state](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_data_device_manager)(&client, data, bound_version, id); }); impl_->data_control_manager = d->add_global(&protocol::zwwm_data_control_manager_v1_interface, 1, [data = &impl_->seat_state](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_data_control_manager)(&client, data, bound_version, id); }); impl_->wlr_data_control_manager = d->add_global(&protocol::zwlr_data_control_manager_v1_interface, 2, [data = &impl_->seat_state](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_wlr_data_control_manager)(&client, data, bound_version, id); }); }
 CompositorServer::~CompositorServer() { while (!impl_->seat_state.constraints.empty()) impl_->seat_state.constraints.back()->resource->destroy(); while (!impl_->tags.clients.empty()) impl_->tags.clients.back()->resource->destroy(); while (!impl_->seat_state.data_offers.empty()) impl_->seat_state.data_offers.back()->resource->destroy(); while (!impl_->seat_state.data_control_devices.empty()) impl_->seat_state.data_control_devices.back()->resource->destroy(); while (!impl_->seat_state.data_devices.empty()) impl_->seat_state.data_devices.back()->resource->destroy(); while (!impl_->seat_state.data_sources.empty()) impl_->seat_state.data_sources.back()->resource->destroy(); for (std::uint32_t global : {impl_->dmabuf, impl_->wlr_data_control_manager, impl_->data_control_manager, impl_->data_device_manager, impl_->sub, impl_->seat, impl_->pointer_constraints, impl_->cursor_shape_manager, impl_->decoration_manager, impl_->tags_manager, impl_->dialog_manager, impl_->wm, impl_->shm, impl_->compositor}) if (global != 0) impl_->seat_state.display.display->destroy_global(global); release_xkb(&impl_->seat_state); delete impl_; }
 CompositorServer::CompositorServer(zwayland::server::Display* d, std::shared_ptr<const RuntimeConfig> config,
                                    zwayland::server::EventLoop* event_loop)
     : CompositorServer(d, std::move(config)) {
#ifdef ZWWM_XWAYLAND
  if (event_loop != nullptr) (void)new XwaylandRuntime(d, event_loop, &impl_->observer);
#else
  (void)event_loop;
#endif
 }
 void CompositorServer::set_surface_commit_observer(SurfaceCommitObserver o, void* d) { impl_->observer.callback = o; impl_->observer.data = d; }
void CompositorServer::set_cursor_shape_observer(CursorShapeObserver o, void* d) { impl_->observer.cursor_callback = o; impl_->observer.cursor_data = d; }
void CompositorServer::set_pointer_position_observer(PointerPositionObserver o, void* d) { impl_->observer.pointer_position_callback = o; impl_->observer.pointer_position_data = d; }
void CompositorServer::set_toplevel_observer(ToplevelObserver o, void* d) { impl_->observer.toplevel_callback = o; impl_->observer.toplevel_data = d; }
void CompositorServer::set_presentation_observer(PresentationObserver o, void* d) { impl_->observer.presentation_callback = o; impl_->observer.presentation_data = d; }
void CompositorServer::set_event_observer(EventObserver o, void* d) { impl_->observer.event_callback = o; impl_->observer.event_data = d; }
void CompositorServer::set_portal_client(zwayland::server::Client* client) {
  impl_->observer.portal_pid = -1;
  if (client != nullptr) impl_->observer.portal_pid = client->pid();
}
void CompositorServer::set_config(std::shared_ptr<const RuntimeConfig> config) {
  if (config == nullptr) return;
  if (!update_xkb(&impl_->seat_state, config->keyboard)) return;
  impl_->config = std::move(config);
  impl_->observer.config = impl_->config.get();
  publish_keymap(&impl_->seat_state);
  auto outputs = this->outputs();
  for (auto& output : outputs) {
    const auto& configured = impl_->config->output_for(output.connector);
    const auto logical = configured.logical_size({output.physical_width, output.physical_height});
    output.logical_width = logical.width;
    output.logical_height = logical.height;
    output.scale_per_mille = configured.scale_per_mille;
    output.transform = configured.transform;
  }
  if (!outputs.empty()) set_outputs(std::move(outputs));
  for (auto* surface : impl_->surfaces) {
    if (surface->xdg_surface != nullptr && surface->xdg_surface->toplevel != nullptr) {
      surface->xdg_surface->rule_applied = false;
      surface->xdg_surface->background_blur_radius = 0;
      surface->xdg_surface->rule_opacity = 1.0F;
      apply_window_rules(surface->xdg_surface);
      notify_surface_tree(surface);
    }
    if (surface->fractional_scale != nullptr)
      protocol::wp_fractional_scale_v1_send_preferred_scale(
          *surface->fractional_scale->resource,
          output_config(&impl_->observer, surface_output(surface)).fractional_scale_120());
  }
  configure_layout(&impl_->observer);
  const auto reconfigure_popups = [&](const auto& self, SurfaceState* parent) -> void {
    for (auto* child : parent->children) if (child->xdg_surface != nullptr && child->xdg_surface->popup != nullptr) {
      auto* popup = child->xdg_surface->popup;
      if (popup->positioner.reactive) send_popup_configure(popup);
      self(self, child);
    }
  };
  for (auto* surface : impl_->surfaces) if (surface->xdg_surface != nullptr && surface->xdg_surface->toplevel != nullptr) reconfigure_popups(reconfigure_popups, surface);
  if (impl_->observer.event_callback != nullptr) impl_->observer.event_callback(impl_->observer.event_data, "config");
}
void CompositorServer::set_output_size(std::uint32_t width, std::uint32_t height, std::uint32_t refresh_millihz) {
  if (width == 0 || height == 0 || width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) || height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) return;
  const auto& configured = impl_->config->output_for("nested-1");
  const auto logical = configured.logical_size({width, height});
  set_outputs({OutputInfo{.id = OutputId{1}, .connector = "nested-1", .logical_width = logical.width,
                          .logical_height = logical.height, .physical_width = width,
                          .physical_height = height, .refresh_millihz = refresh_millihz,
                           .scale_per_mille = configured.scale_per_mille,
                           .transform = configured.transform, .modes = {}}});
}

void CompositorServer::set_outputs(std::vector<OutputInfo> outputs) {
  if (impl_->dialog_manager == 0)
    impl_->dialog_manager = impl_->seat_state.display.display->add_global(&protocol::xdg_wm_dialog_v1_interface, 1, [data = this](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_dialog_manager)(&client, data, bound_version, id); });
  if (impl_->session_lock_manager == 0)
    impl_->session_lock_manager = impl_->seat_state.display.display->add_global(&protocol::ext_session_lock_manager_v1_interface, 1, [data = &impl_->observer](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_session_lock_manager)(&client, data, bound_version, id); });
  impl_->observer.outputs = &impl_->outputs;
  outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [](const OutputInfo& info) {
    return !info.id || !info.enabled || info.physical_width == 0 || info.physical_height == 0;
  }), outputs.end());
  std::stable_sort(outputs.begin(), outputs.end(), [](const OutputInfo& left, const OutputInfo& right) {
    return left.connector < right.connector;
  });
  std::int32_t logical_x = 0;
  for (auto& info : outputs) {
    info.logical_x = logical_x;
    info.logical_y = 0;
    logical_x += static_cast<std::int32_t>(info.logical_width);
  }
  for (auto it = impl_->outputs.begin(); it != impl_->outputs.end();) {
    const bool remains = std::any_of(outputs.begin(), outputs.end(), [&](const OutputInfo& info) {
      return info.id == (*it)->info.id;
    });
    if (remains) { ++it; continue; }
    // Remove output-pinned layers instead of moving them to another output.
    auto* removed = it->get();
    for (auto& roots : removed->layer_roots) for (auto* surface : roots) {
      auto* layer = surface == nullptr ? nullptr : surface->layer_surface;
      if (layer == nullptr || layer->output != removed->info.id) continue;
      protocol::zwlr_layer_surface_v1_send_closed(*layer->resource);
      layer->closed = true;
      layer->mapped = false;
      layer->configured = false;
      layer->configure_requested = false;
      layer->serials.clear();
      const auto remove_tree = [&](const auto& self, SurfaceState* node) -> void {
        if (node == nullptr) return;
        if (node->entered_output == removed->info.id) {
          const auto client = node->resource->client;
          for (auto* resource : removed->resources) if (resource->client == client)
            protocol::wl_surface_send_leave(*node->resource, resource);
          node->entered_output = {};
        }
        if (impl_->observer.callback != nullptr) impl_->observer.callback(impl_->observer.data,
            ShmBufferView{.surface_id = node->id, .root_surface_id = surface->id, .output = removed->info.id,
                          .window_shader = {}, .border_shader = {}});
        for (auto* child : node->children) self(self, child);
      };
      remove_tree(remove_tree, surface);
      auto& seat = impl_->seat_state;
      if (root(seat.pointer_focus) == surface) set_pointer_focus(&seat, nullptr, 0, 0);
      if (root(seat.keyboard_focus) == surface) set_keyboard_focus(&seat, seat.regular_focus);
      if (root(seat.hit_target) == surface) seat.hit_target = nullptr;
      if (root(seat.pointer_grab) == surface) seat.pointer_grab = nullptr;
    }
    for (auto& roots : removed->layer_roots) roots.clear();
    removed->retired = true;
    if ((*it)->global != 0) impl_->seat_state.display.display->destroy_global((*it)->global);
    (*it)->global = 0;
    impl_->retired_outputs.push_back(std::move(*it));
    it = impl_->outputs.erase(it);
  }
  for (const auto& info : outputs) {
    const auto found = std::find_if(impl_->outputs.begin(), impl_->outputs.end(), [&](const auto& item) {
      return item->info.id == info.id;
    });
    if (found != impl_->outputs.end()) {
      (*found)->info = info;
      for (auto* resource : (*found)->resources) send_output(found->get(), resource, false);
      continue;
    }
    auto output = std::make_unique<OutputState>();
    output->info = info;
    output->global = impl_->seat_state.display->add_global(&protocol::wl_output_interface, kOutputVersion, [data = output.get()](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_output_state)(&client, data, bound_version, id); });
    impl_->outputs.push_back(std::move(output));
  }
  std::stable_sort(impl_->outputs.begin(), impl_->outputs.end(), [](const auto& left, const auto& right) {
    return left->info.connector < right->info.connector;
  });
  impl_->active_output = impl_->outputs.empty() ? OutputId{} : impl_->outputs.front()->info.id;
  impl_->observer.active_output = impl_->active_output;
  const auto output_exists = [&](OutputId id) {
    return std::any_of(impl_->outputs.begin(), impl_->outputs.end(), [&](const auto& output) {
      return output->info.id == id;
    });
  };
  if (impl_->seat_state.interactive != nullptr) {
    auto* interactive_xdg = root(impl_->seat_state.interactive)->xdg_surface;
    if (interactive_xdg == nullptr || !output_exists(interactive_xdg->output)) end_interactive(&impl_->seat_state);
  }
  for (auto* surface : impl_->surfaces) {
    if (surface->parent != nullptr) continue;
    if (surface->xdg_surface != nullptr && !output_exists(surface->xdg_surface->output)) {
      if (impl_->observer.callback != nullptr)
        impl_->observer.callback(impl_->observer.data, ShmBufferView{.surface_id = surface->id,
            .root_surface_id = surface->id, .output = surface->xdg_surface->output,
            .window_shader = {}, .border_shader = {}});
      surface->xdg_surface->output = impl_->active_output;
      if (auto* output = output_state(&impl_->observer, impl_->active_output)) surface->xdg_surface->tag = output->active_tag;
      if (surface->xdg_surface->fullscreen) {
        surface->xdg_surface->fullscreen = false;
        surface->xdg_surface->floating = surface->xdg_surface->restore_floating;
      }
      if (surface->xdg_surface->floating)
        surface->xdg_surface->floating_bounds = clamp_window(surface->xdg_surface->floating_bounds,
                                                             output_work(&impl_->observer, impl_->active_output));
      notify_surface_tree(surface);
    }
#ifdef ZWWM_XWAYLAND
    if (surface->xwayland_surface != nullptr && !output_exists(surface->xwayland_surface->output)) {
      surface->xwayland_surface->output = impl_->active_output;
      if (auto* assigned_output = output_state(&impl_->observer, impl_->active_output))
        surface->xwayland_surface->tag = assigned_output->active_tag;
      surface->xwayland_surface->tile_bounds = {};
      surface->xwayland_surface->content_bounds = {};
      notify_surface_tree(surface);
    }
#endif
    if (surface->layer_surface != nullptr && !output_exists(surface->layer_surface->output)) {
      auto* layer = surface->layer_surface;
      if (layer->output) {
        if (!layer->closed) protocol::zwlr_layer_surface_v1_send_closed(*layer->resource);
        layer->closed = true;
        layer->mapped = false;
      } else if (impl_->active_output) {
        // Created before any output existed; pin it to the first output now.
        layer->output = impl_->active_output;
        if (auto* owner = output_state(&impl_->observer, layer->output); owner != nullptr) {
          owner->layer_roots[layer->current.layer].push_back(surface);
          reorder_layer_roots(&impl_->observer, layer->output, layer->current.layer);
        }
      }
    }
  }
  configure_session_lock_outputs(impl_->seat_state.session_lock);
  if (!impl_->outputs.empty()) {
    const auto& first = impl_->outputs.front()->info;
    impl_->observer.physical_width = static_cast<std::int32_t>(first.physical_width);
    impl_->observer.physical_height = static_cast<std::int32_t>(first.physical_height);
    impl_->observer.refresh_millihz = first.refresh_millihz;
    impl_->observer.output_width = logical_x;
    impl_->observer.output_height = static_cast<std::int32_t>(first.logical_height);
  }
  configure_layout(&impl_->observer);
  if (impl_->observer.event_callback != nullptr) impl_->observer.event_callback(impl_->observer.event_data, "output");
}

std::vector<OutputInfo> CompositorServer::outputs() const {
  std::vector<OutputInfo> result;
  result.reserve(impl_->outputs.size());
  for (const auto& output : impl_->outputs) result.push_back(output->info);
  return result;
}
std::vector<CameraInfo> CompositorServer::cameras() const {
  std::vector<CameraInfo> result;
  result.reserve(impl_->outputs.size());
  for (const auto& output : impl_->outputs) {
    const auto tag = output->active_tag;
    const auto& viewport = output->canvas_viewports[tag - 1];
    result.push_back({output->info.id, output->info.connector, tag, viewport.x, viewport.y,
                      viewport.scale, output->info.id == impl_->active_output});
  }
  return result;
}

zwayland::server::Resource* CompositorServer::output_resource(zwayland::server::Client* client, OutputId id) const {
  const auto output = std::find_if(impl_->outputs.begin(), impl_->outputs.end(), [&](const auto& item) {
    return !id || item->info.id == id;
  });
  if (output == impl_->outputs.end()) return nullptr;
  const auto resource = std::find_if((*output)->resources.begin(), (*output)->resources.end(),
      [client](zwayland::server::Resource* item) { return item->client == client; });
  return resource == (*output)->resources.end() ? nullptr : *resource;
}

OutputId CompositorServer::output_id(const zwayland::server::Resource* resource) const {
  if (resource == nullptr) return {};
  for (const auto& output : impl_->outputs)
    if (std::find(output->resources.begin(), output->resources.end(), resource) != output->resources.end())
      return output->info.id;
  return {};
}

std::optional<OutputInfo> CompositorServer::output_info(OutputId id) const {
  const auto found = std::find_if(impl_->outputs.begin(), impl_->outputs.end(), [&](const auto& output) {
    return output->info.id == id;
  });
  return found == impl_->outputs.end() ? std::nullopt : std::optional<OutputInfo>((*found)->info);
}

KeyboardLayoutInfo CompositorServer::keyboard_layout() const {
  const auto* keymap = impl_->seat_state.xkb_keymap_handle;
  const auto* state = impl_->seat_state.xkb_state_handle;
  if (keymap == nullptr || state == nullptr) return {};
  const auto group = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE);
  const char* name = xkb_keymap_layout_get_name(keymap, group);
  return {name == nullptr ? "" : name, group};
}

bool CompositorServer::copy_capture_buffer(zwayland::server::Resource* resource, const OutputCapture& capture,
                                            std::uint32_t source_x, std::uint32_t source_y,
                                            std::uint32_t width, std::uint32_t height) {
  return copy_shm_capture_buffer(resource, capture, source_x, source_y, width, height);
}

bool CompositorServer::map_capture_region(const OutputCapture& capture, std::int32_t x, std::int32_t y,
                                          std::int32_t width, std::int32_t height,
                                          std::uint32_t* capture_x, std::uint32_t* capture_y,
                                          std::uint32_t* capture_width, std::uint32_t* capture_height) const {
  if (width <= 0 || height <= 0 || capture.width == 0 || capture.height == 0 || capture_x == nullptr ||
      capture_y == nullptr || capture_width == nullptr || capture_height == nullptr) return false;
  const auto info = output_info(capture.output);
  const auto logical_width = static_cast<std::int32_t>(info ? info->logical_width : impl_->observer.output_width);
  const auto logical_height = static_cast<std::int32_t>(info ? info->logical_height : impl_->observer.output_height);
  const auto left = std::clamp(x, 0, logical_width);
  const auto top = std::clamp(y, 0, logical_height);
  const auto right = static_cast<std::int32_t>(std::clamp<std::int64_t>(
      static_cast<std::int64_t>(x) + width, left, logical_width));
  const auto bottom = static_cast<std::int32_t>(std::clamp<std::int64_t>(
      static_cast<std::int64_t>(y) + height, top, logical_height));
  if (right <= left || bottom <= top) return false;
  const auto& configured = info ? impl_->config->output_for(info->connector) : impl_->config->output;
  const auto bounds = configured.physical_bounds(
      {{left, top}, {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}},
      {capture.width, capture.height});
  const auto px = std::clamp(bounds.origin.x, 0, static_cast<std::int32_t>(capture.width));
  const auto py = std::clamp(bounds.origin.y, 0, static_cast<std::int32_t>(capture.height));
  *capture_x = static_cast<std::uint32_t>(px);
  *capture_y = static_cast<std::uint32_t>(py);
  *capture_width = std::min(bounds.size.width, capture.width - *capture_x);
  *capture_height = std::min(bounds.size.height, capture.height - *capture_y);
  return *capture_width != 0 && *capture_height != 0;
}

std::vector<ToplevelInfo> CompositorServer::toplevels() const {
  std::vector<ToplevelInfo> result;
  for (const auto* surface : impl_->surfaces) {
    const auto* xdg = surface == nullptr ? nullptr : surface->xdg_surface;
    Rect bounds;
    OutputId output;
    std::string title;
    std::string app_id;
    bool mapped = xdg != nullptr && xdg->toplevel != nullptr && xdg->mapped;
    if (mapped) {
      bounds = xdg->tile_bounds;
      output = xdg->output;
      title = xdg->title;
      app_id = xdg->app_id;
    }
#ifdef ZWWM_XWAYLAND
    else if (surface != nullptr && surface->xwayland_surface != nullptr &&
             surface->xwayland_surface->window != nullptr && surface->xwayland_surface->window->mapped) {
      const auto* role = surface->xwayland_surface;
      mapped = true;
      bounds = role->tile_bounds;
      output = role->output;
      title = role->window->title;
      app_id = role->window->app_id;
    }
#endif
    if (surface == nullptr || surface->parent != nullptr || !mapped) continue;
    const auto assigned_output = output_info(output);
    const auto& configured = assigned_output ? impl_->config->output_for(assigned_output->connector) :
                                               impl_->config->output;
    const auto physical = configured.physical_bounds(
        {{bounds.x - (assigned_output ? assigned_output->logical_x : 0),
          bounds.y - (assigned_output ? assigned_output->logical_y : 0)},
         {static_cast<std::uint32_t>(std::max(0, bounds.width)),
          static_cast<std::uint32_t>(std::max(0, bounds.height))}},
        {assigned_output ? assigned_output->physical_width : static_cast<std::uint32_t>(impl_->observer.physical_width),
         assigned_output ? assigned_output->physical_height : static_cast<std::uint32_t>(impl_->observer.physical_height)});
    result.push_back({surface->id, std::move(title), std::move(app_id), bounds.x, bounds.y,
                        bounds.width, bounds.height,
                        1U | (impl_->seat_state.toplevel_focus == surface ? 4U : 0U),
                       static_cast<std::uint32_t>(std::max(0, physical.origin.x)),
                       static_cast<std::uint32_t>(std::max(0, physical.origin.y)),
                        physical.size.width, physical.size.height, output});
  }
  return result;
}

std::vector<TagInfo> CompositorServer::tags() const {
  std::vector<TagInfo> result;
  result.reserve(impl_->outputs.size());
  for (const auto& output : impl_->outputs)
    result.push_back({output->info.id, output->info.connector, output->active_tag});
  return result;
}

std::vector<LayerInfo> CompositorServer::layers() const {
  std::vector<LayerInfo> result;
  for (const auto* surface : impl_->surfaces) {
    if (surface == nullptr || surface->parent != nullptr || surface->layer_surface == nullptr) continue;
    const auto* layer = surface->layer_surface;
    const auto effect = impl_->config->layer_effect(layer->name_space);
    result.push_back({surface->id, layer->output, layer->name_space, layer->current.layer,
                      layer->xwlr == nullptr ? 0 : layer->xwlr->current_priority,
                      layer->current.zone, effect.blur_radius, effect.opacity, layer->mapped});
  }
  return result;
}

bool CompositorServer::dispatch_action(const std::string& action, const std::string& argument,
                                       std::string* error) {
  auto fail = [&](const char* message) { if (error != nullptr) *error = message; return false; };
  auto& seat = impl_->seat_state;
  auto* focused = seat.toplevel_focus;
  auto* xdg = focused == nullptr ? nullptr : focused->xdg_surface;
#ifdef ZWWM_XWAYLAND
  auto* xwayland = focused == nullptr ? nullptr : focused->xwayland_surface;
#endif
  if (action == "exec") {
    if (argument.empty()) return fail("exec requires an argument");
    if (!execute_binding(Keybinding{.modifiers = {}, .key = {}, .action = KeyAction::exec,
                                    .argument = argument}))
      return fail("could not launch command");
  } else if (action == "killactive") {
    if (xdg != nullptr && xdg->toplevel != nullptr) protocol::xdg_toplevel_send_close(*xdg->toplevel);
#ifdef ZWWM_XWAYLAND
    else if (xwayland != nullptr && xwayland->runtime != nullptr)
      xwayland->runtime->close_window(xwayland->window);
#endif
    else return fail("no active window");
  } else if (action == "killsession" || action == "exit") {
    seat.display->stop();
  } else if (action == "togglefloating") {
    if (xdg != nullptr) set_floating(xdg, !xdg->floating);
#ifdef ZWWM_XWAYLAND
    else if (xwayland != nullptr) {
      set_xwayland_floating(xwayland, !xwayland->floating);
      if (xwayland->runtime != nullptr) xwayland->runtime->publish_state(xwayland);
      configure_layout(&impl_->observer);
    }
#endif
    else return fail("no active window");
  } else if (action == "toggle-fullscreen" || action == "fullscreen") {
    if (xdg != nullptr) set_fullscreen(xdg, {}, !xdg->fullscreen);
#ifdef ZWWM_XWAYLAND
    else if (xwayland != nullptr) {
      xwayland->fullscreen = !xwayland->fullscreen;
      if (xwayland->runtime != nullptr) xwayland->runtime->publish_state(xwayland);
      configure_layout(&impl_->observer);
    }
#endif
    else return fail("no active window");
  } else if (action == "tag" || action == "movetotag") {
    if (argument.size() != 1 || argument[0] < '1' || argument[0] > '9') return fail("tag must be 1 through 9");
    const auto tag = static_cast<std::uint8_t>(argument[0] - '0');
    auto* output = output_state(&impl_->observer, impl_->active_output);
    if (action == "tag") {
      if (output == nullptr) return fail("no active output");
      switch_active_tag(&impl_->observer, impl_->active_output, tag);
    } else {
      if (xdg != nullptr) xdg->tag = tag;
#ifdef ZWWM_XWAYLAND
      else if (xwayland != nullptr) xwayland->tag = tag;
#endif
      else return fail("no active window");
      SurfaceState* fallback = nullptr;
      for (auto it = impl_->surfaces.rbegin(); it != impl_->surfaces.rend(); ++it) {
        auto* candidate = (*it)->xdg_surface;
        if ((*it)->parent == nullptr && candidate != nullptr && candidate->toplevel != nullptr &&
            candidate->output == impl_->active_output && visible_xdg(&impl_->observer, candidate)) {
          fallback = *it;
          break;
        }
      }
      if (output != nullptr && ((xdg != nullptr && xdg->tag == output->active_tag)
#ifdef ZWWM_XWAYLAND
          || (xwayland != nullptr && xwayland->tag == output->active_tag)
#endif
          )) fallback = focused;
      set_keyboard_focus(&seat, fallback);
      configure_layout(&impl_->observer);
      for (auto* surface : impl_->surfaces)
        if (surface->parent == nullptr && surface->xdg_surface != nullptr) notify_surface_tree(surface);
      if (xdg != nullptr) notify_toplevel_tags(&impl_->observer, xdg);
      if (impl_->observer.event_callback != nullptr) impl_->observer.event_callback(impl_->observer.event_data, "tag");
    }
  } else if (action == "focus") {
    std::vector<SurfaceState*> visible;
    for (auto* surface : impl_->surfaces) {
      if (surface->parent != nullptr) continue;
      bool is_visible = surface->xdg_surface != nullptr && surface->xdg_surface->toplevel != nullptr &&
                        visible_xdg(&impl_->observer, surface->xdg_surface);
#ifdef ZWWM_XWAYLAND
      if (!is_visible && surface->xwayland_surface != nullptr && surface->xwayland_surface->window != nullptr &&
          surface->xwayland_surface->window->mapped) {
        const auto* assigned_output = output_state(&impl_->observer, surface->xwayland_surface->output);
        is_visible = assigned_output != nullptr && surface->xwayland_surface->tag == assigned_output->active_tag;
      }
#endif
      if (is_visible) visible.push_back(surface);
    }
    if (visible.empty()) return fail("no visible window");
    if (!argument.empty()) {
      if (argument != "left" && argument != "right" && argument != "up" && argument != "down")
        return fail("focus direction must be left, right, up, or down");
      const auto output_of = [](const SurfaceState* surface) {
        if (surface->xdg_surface != nullptr) return surface->xdg_surface->output;
#ifdef ZWWM_XWAYLAND
        if (surface->xwayland_surface != nullptr) return surface->xwayland_surface->output;
#endif
        return OutputId{};
      };
      const auto center_of = [&](const SurfaceState* surface) -> std::pair<double, double> {
        if (const auto* candidate = surface->xdg_surface; candidate != nullptr) {
          if (endless_canvas(&impl_->observer) && candidate->canvas_bounds.initialized)
            return {candidate->canvas_bounds.x + candidate->canvas_bounds.width * 0.5,
                    candidate->canvas_bounds.y + candidate->canvas_bounds.height * 0.5};
          return {candidate->tile_bounds.x + candidate->tile_bounds.width * 0.5,
                  candidate->tile_bounds.y + candidate->tile_bounds.height * 0.5};
        }
#ifdef ZWWM_XWAYLAND
        const auto* candidate = surface->xwayland_surface;
        if (endless_canvas(&impl_->observer) && candidate->canvas_bounds.initialized)
          return {candidate->canvas_bounds.x + candidate->canvas_bounds.width * 0.5,
                  candidate->canvas_bounds.y + candidate->canvas_bounds.height * 0.5};
        return {candidate->tile_bounds.x + candidate->tile_bounds.width * 0.5,
                candidate->tile_bounds.y + candidate->tile_bounds.height * 0.5};
#else
        return {};
#endif
      };
      if (focused == nullptr || std::find(visible.begin(), visible.end(), focused) == visible.end()) {
        set_keyboard_focus(&seat, visible.front());
        configure_layout(&impl_->observer);
        return true;
      }
      const auto origin = center_of(focused);
      const auto output = output_of(focused);
      std::vector<SurfaceState*> directional;
      std::vector<layout::FocusPoint> centers;
      for (auto* candidate : visible) {
        if (candidate == focused || output_of(candidate) != output) continue;
        const auto center = center_of(candidate);
        directional.push_back(candidate);
        centers.push_back({center.first, center.second});
      }
      const auto direction = argument == "left" ? layout::FocusDirection::left :
          argument == "right" ? layout::FocusDirection::right :
          argument == "up" ? layout::FocusDirection::up : layout::FocusDirection::down;
      const auto nearest = layout::nearest_in_direction(
          {origin.first, origin.second}, centers, direction);
      if (!nearest) return fail("no visible window in that direction");
      set_keyboard_focus(&seat, directional[*nearest]);
      configure_layout(&impl_->observer);
      return true;
    }
    auto current = std::find(visible.begin(), visible.end(), focused);
    if (current == visible.end() || ++current == visible.end()) current = visible.begin();
    set_keyboard_focus(&seat, *current);
    configure_layout(&impl_->observer);
  } else {
    return fail("unknown action");
  }
  return true;
}
void CompositorServer::set_dmabuf_feedback(std::vector<std::pair<std::uint32_t, std::uint64_t>> formats, std::optional<dev_t> main_device) { std::sort(formats.begin(), formats.end()); formats.erase(std::unique(formats.begin(), formats.end()), formats.end()); if (formats.size() > std::numeric_limits<std::uint16_t>::max()) formats.resize(std::numeric_limits<std::uint16_t>::max()); impl_->dmabuf_state = {std::move(formats), main_device}; if (impl_->dmabuf == 0 && !impl_->dmabuf_state.formats.empty()) { const auto version = impl_->dmabuf_state.main_device.has_value() ? 4U : 3U; impl_->dmabuf = impl_->seat_state.display->add_global(&protocol::zwp_linux_dmabuf_v1_interface, version, [data = &impl_->dmabuf_state](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_dmabuf)(&client, data, bound_version, id); }); } }
void CompositorServer::notify_frame_presented(OutputId output) {
  const auto now = timestamp_ms();
  auto* lock = impl_->seat_state.session_lock;
  for (auto* surface : impl_->surfaces) {
    if (!*surface->alive) continue;
    auto* scene_root = root(surface);
    const OutputId assigned = scene_root != nullptr && scene_root->layer_surface != nullptr ? scene_root->layer_surface->output :
                               scene_root != nullptr && scene_root->lock_surface != nullptr ? scene_root->lock_surface->output :
                               scene_root != nullptr && scene_root->xdg_surface != nullptr ? scene_root->xdg_surface->output :
#ifdef ZWWM_XWAYLAND
                               scene_root != nullptr && scene_root->xwayland_surface != nullptr ? scene_root->xwayland_surface->output :
#endif
                               OutputId{};
    if ((output && assigned && assigned != output) || (lock != nullptr && scene_root->lock_surface == nullptr)) continue;
    while (!surface->frame_callbacks.empty()) {
      auto* callback = surface->frame_callbacks.front();
      protocol::wl_callback_send_done(*callback, now);
      callback->destroy();
    }
  }
  notify_session_lock_frame_presented(lock, output);
  if (impl_->observer.presentation_callback != nullptr) impl_->observer.presentation_callback(impl_->observer.presentation_data);
}
namespace detail {
Rect surface_local_bounds(const SurfaceState* surface) {
  if (surface == nullptr || surface->current_buffer == nullptr) return {0, 0, 0, 0};
  const auto& viewport = surface->current_viewport;
  return {0, 0,
          viewport.has_destination ? viewport.destination_width : viewport.has_source ? static_cast<int>(viewport.width) : surface->current_buffer->width / surface->current_buffer_scale,
          viewport.has_destination ? viewport.destination_height : viewport.has_source ? static_cast<int>(viewport.height) : surface->current_buffer->height / surface->current_buffer_scale};
}
Rect effective_window_geometry(const SurfaceState* surface) {
  const auto* buffer = surface == nullptr ? nullptr : surface->current_buffer;
  if (surface == nullptr || surface->xdg_surface == nullptr || buffer == nullptr) return {0, 0, 0, 0};
  const Rect local = surface_local_bounds(surface);
  const Rect requested = surface->xdg_surface->window_geometry;
  const int left = std::clamp(requested.x, 0, local.width);
  const int top = std::clamp(requested.y, 0, local.height);
  const int right = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(requested.x) + requested.width, left, local.width));
  const int bottom = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(requested.y) + requested.height, top, local.height));
  return right == left || bottom == top ? local
                               : Rect{left, top, right - left, bottom - top};
}

Rect global_surface_bounds(const SurfaceState* surface) {
  const auto* top = root(const_cast<SurfaceState*>(surface));
  if (top == nullptr || top->current_buffer == nullptr) return {0, 0, 0, 0};
  if (top->layer_surface != nullptr || top->lock_surface != nullptr) {
    std::int32_t x = 0, y = 0;
    absolute_position(surface, &x, &y);
    const Rect bounds = surface_local_bounds(surface);
    return {x, y, bounds.width, bounds.height};
  }
  Rect content;
  Rect geometry;
  if (top->xdg_surface != nullptr) {
    content = top->xdg_surface->content_bounds;
    geometry = effective_window_geometry(top);
  }
#ifdef ZWWM_XWAYLAND
  else if (top->xwayland_surface != nullptr) {
    content = top->xwayland_surface->content_bounds;
    geometry = surface_local_bounds(top);
  }
#endif
  else return {0, 0, 0, 0};
  if (surface == top) return content;
  std::int32_t x = 0, y = 0;
  absolute_position(surface, &x, &y);
  std::int32_t root_x = 0, root_y = 0;
  absolute_position(top, &root_x, &root_y);
  return {content.x + (x - root_x - geometry.x) * content.width / geometry.width,
          content.y + (y - root_y - geometry.y) * content.height / geometry.height,
          surface_local_bounds(surface).width * content.width / geometry.width,
          surface_local_bounds(surface).height * content.height / geometry.height};
}

void surface_local_from_global(const SurfaceState* surface, std::int32_t global_x, std::int32_t global_y, std::int32_t* x, std::int32_t* y) {
  const auto* top = root(const_cast<SurfaceState*>(surface));
  if (top != nullptr && (top->layer_surface != nullptr || top->lock_surface != nullptr)) {
    std::int32_t origin_x = 0, origin_y = 0;
    absolute_position(surface, &origin_x, &origin_y);
    *x = global_x - origin_x;
    *y = global_y - origin_y;
    return;
  }
  Rect content;
  Rect geometry;
  if (top->xdg_surface != nullptr) {
    content = top->xdg_surface->content_bounds;
    geometry = effective_window_geometry(top);
  }
#ifdef ZWWM_XWAYLAND
  else if (top->xwayland_surface != nullptr) {
    content = top->xwayland_surface->content_bounds;
    geometry = surface_local_bounds(top);
  }
#endif
  else { *x = 0; *y = 0; return; }
  if (surface == top) {
    *x = geometry.x + (global_x - content.x) * geometry.width / content.width;
    *y = geometry.y + (global_y - content.y) * geometry.height / content.height;
    return;
  }
  const Rect bounds = global_surface_bounds(surface);
  *x = (global_x - bounds.x) * geometry.width / content.width;
  *y = (global_y - bounds.y) * geometry.height / content.height;
}
void surface_global_from_local(const SurfaceState* surface, std::int32_t local_x, std::int32_t local_y, std::int32_t* x, std::int32_t* y) {
  const auto* top = root(const_cast<SurfaceState*>(surface));
  if (top != nullptr && (top->layer_surface != nullptr || top->lock_surface != nullptr)) {
    absolute_position(surface, x, y);
    *x += local_x;
    *y += local_y;
    return;
  }
  Rect content;
  Rect geometry;
  if (top->xdg_surface != nullptr) {
    content = top->xdg_surface->content_bounds;
    geometry = effective_window_geometry(top);
  }
#ifdef ZWWM_XWAYLAND
  else if (top->xwayland_surface != nullptr) {
    content = top->xwayland_surface->content_bounds;
    geometry = surface_local_bounds(top);
  }
#endif
  else { *x = 0; *y = 0; return; }
  if (surface == top) {
    *x = content.x + (local_x - geometry.x) * content.width / geometry.width;
    *y = content.y + (local_y - geometry.y) * content.height / geometry.height;
    return;
  }
  const Rect bounds = global_surface_bounds(surface);
  *x = bounds.x + local_x * content.width / geometry.width;
  *y = bounds.y + local_y * content.height / geometry.height;
}
bool accepts_input(const SurfaceState* surface, std::int32_t global_x, std::int32_t global_y) {
  std::int32_t local_x = 0, local_y = 0;
  surface_local_from_global(surface, global_x, global_y, &local_x, &local_y);
  if (surface->current_input_infinite) return true;
  return std::any_of(surface->current_input_region.begin(), surface->current_input_region.end(),
                     [local_x, local_y](const Rect& rect) {
    return local_x >= rect.x && local_y >= rect.y && local_x < rect.x + rect.width && local_y < rect.y + rect.height;
  });
}
SurfaceState* surface_tree_at(SurfaceState* surface, std::int32_t x, std::int32_t y) {
  if (surface == nullptr) return nullptr;
  for (auto it = surface->children.rbegin(); it != surface->children.rend(); ++it) {
    if ((*it)->above_parent) if (auto* hit = surface_tree_at(*it, x, y); hit != nullptr) return hit;
  }
  const bool mapped = surface->xdg_surface != nullptr ? surface->xdg_surface->mapped :
#ifdef ZWWM_XWAYLAND
      surface->xwayland_surface != nullptr ? surface->xwayland_surface->window != nullptr &&
          surface->xwayland_surface->window->mapped :
#endif
      true;
  if (mapped && surface->current_buffer != nullptr) {
    const Rect bounds = global_surface_bounds(surface);
    if (x >= bounds.x && y >= bounds.y && x < bounds.x + bounds.width && y < bounds.y + bounds.height &&
        accepts_input(surface, x, y)) return surface;
  }
  for (auto it = surface->children.rbegin(); it != surface->children.rend(); ++it) {
    if (!(*it)->above_parent) if (auto* hit = surface_tree_at(*it, x, y); hit != nullptr) return hit;
  }
  return nullptr;
}
SurfaceState* surface_at_global(const Observer* observer, std::int32_t x, std::int32_t y) {
  if (observer == nullptr || observer->surfaces == nullptr) return nullptr;
  const auto& surfaces = *observer->surfaces;
  // Reverse layer and creation order to mirror rendering order.
  OutputId pointer_output;
  if (observer->outputs != nullptr) for (const auto& output : *observer->outputs) {
    const auto& info = output->info;
    if (x >= info.logical_x && y >= info.logical_y && x < info.logical_x + static_cast<std::int32_t>(info.logical_width) && y < info.logical_y + static_cast<std::int32_t>(info.logical_height)) { pointer_output = info.id; break; }
  }
  auto* owner = output_state(const_cast<Observer*>(observer), pointer_output);
  if (observer->seat != nullptr && observer->seat->session_lock != nullptr) {
    auto* lock = observer->seat->session_lock;
    const auto surface = std::find_if(lock->surfaces.begin(), lock->surfaces.end(), [&](const auto* item) {
      return item->output == pointer_output && item->mapped && item->surface != nullptr;
    });
    return surface == lock->surfaces.end() ? nullptr : surface_tree_at((*surface)->surface, x, y);
  }
  const auto hit_layer = [owner, x, y](std::uint32_t layer) -> SurfaceState* {
    if (owner == nullptr) return nullptr;
    const auto& roots = owner->layer_roots[layer];
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
      auto* surface = *it;
      if (surface->parent != nullptr || surface->layer_surface == nullptr ||
           !surface->layer_surface->mapped || surface->layer_surface->output != owner->info.id || surface->layer_surface->current.layer != layer) continue;
      if (auto* hit = surface_tree_at(surface, x, y); hit != nullptr) return hit;
    }
    return nullptr;
  };
  if (auto* hit = hit_layer(protocol::ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY); hit != nullptr) return hit;
  if (auto* hit = hit_layer(protocol::ZWLR_LAYER_SHELL_V1_LAYER_TOP); hit != nullptr) return hit;
  const auto hit_window = [&](int category) -> SurfaceState* {
    SurfaceState* result = nullptr;
    std::uint64_t highest_order = 0;
    for (auto* surface : surfaces) {
      auto* xdg = surface->xdg_surface;
      bool mapped = xdg != nullptr && xdg->toplevel != nullptr && visible_xdg(observer, xdg);
      bool fullscreen = mapped && xdg->fullscreen;
      bool floating = mapped && xdg->floating && !fullscreen;
#ifdef ZWWM_XWAYLAND
      auto* xwayland = surface->xwayland_surface;
      if (!mapped && xwayland != nullptr && xwayland->window != nullptr && xwayland->window->mapped) {
        const auto* assigned_output = output_state(const_cast<Observer*>(observer), xwayland->output);
        mapped = assigned_output != nullptr && xwayland->tag == assigned_output->active_tag;
        fullscreen = mapped && xwayland->fullscreen;
        floating = mapped && xwayland->floating && !fullscreen;
      }
#endif
       if (surface->parent != nullptr || !mapped || (category == 2) != fullscreen ||
           (category == 1) != floating || (category == 0) != (!floating && !fullscreen)) continue;
      if (surface->root_order < highest_order) continue;
      if (auto* hit = surface_tree_at(surface, x, y); hit != nullptr) {
        result = hit;
        highest_order = surface->root_order;
      }
    }
    return result;
  };
  if (auto* hit = hit_window(2); hit != nullptr) return hit;
  if (auto* hit = hit_window(1); hit != nullptr) return hit;
  if (auto* hit = hit_window(0); hit != nullptr) return hit;
  if (auto* hit = hit_layer(protocol::ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM); hit != nullptr) return hit;
  if (auto* hit = hit_layer(protocol::ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND); hit != nullptr) return hit;
  return nullptr;
}
bool managed_surface(const SurfaceState* surface) {
  const auto* top = root(const_cast<SurfaceState*>(surface));
  return top != nullptr && ((top->xdg_surface != nullptr && top->xdg_surface->toplevel != nullptr)
#ifdef ZWWM_XWAYLAND
      || top->xwayland_surface != nullptr
#endif
      );
}
void route_pointer_motion(SeatState* seat, std::uint32_t time, SurfaceState* hit, std::int32_t x, std::int32_t y,
                          const std::optional<std::pair<std::int32_t, std::int32_t>>& global) {
  seat->hit_target = hit;
  if (auto* constraint = active_constraint(seat); constraint != nullptr) {
    if (constraint->locked) { seat->hit_target = constraint->surface; sync_pointer_position(seat); return; }
    if (seat->hit_target != constraint->surface) { x = constraint->x; y = constraint->y; }
    if (!confine_to_rects(constraint_rects(constraint), constraint->x, constraint->y, &x, &y)) deactivate_constraint(constraint, true);
    else { seat->hit_target = constraint->surface; constraint->x = x; constraint->y = y; }
  }
  if (global.has_value() && seat->hit_target == hit) {
    seat->pointer_x = global->first; seat->pointer_y = global->second;
  } else if (seat->hit_target != nullptr && managed_surface(seat->hit_target)) {
    surface_global_from_local(seat->hit_target, x, y, &seat->pointer_x, &seat->pointer_y);
  } else {
    std::int32_t offset_x = 0, offset_y = 0;
    if (seat->hit_target != nullptr) absolute_position(seat->hit_target, &offset_x, &offset_y);
    seat->pointer_x = offset_x + x; seat->pointer_y = offset_y + y;
  }
  if (seat->drag_origin != nullptr) {
    drag_motion(seat, time, seat->hit_target, x, y);
    sync_pointer_position(seat);
    return;
  }
  SurfaceState* next = seat->pointer_grab != nullptr ? seat->pointer_grab : seat->hit_target;
  if (seat->popup_grab != nullptr && !seat->popup_grab->dismissed && seat->popup_grab->xdg != nullptr) {
    auto* root_popup = popup_chain_root(seat->popup_grab);
    const bool in_chain = next != nullptr && next->xdg_surface != nullptr && next->xdg_surface->popup != nullptr && popup_descendant(next->xdg_surface->popup, root_popup);
    next = in_chain ? next : seat->popup_grab->xdg->surface;
  }
  std::int32_t next_x = 0, next_y = 0;
  if (next != nullptr && managed_surface(next)) surface_local_from_global(next, seat->pointer_x, seat->pointer_y, &next_x, &next_y);
  else if (next != nullptr) { absolute_position(next, &next_x, &next_y); next_x = seat->pointer_x - next_x; next_y = seat->pointer_y - next_y; }
  set_pointer_focus(seat, next, next_x, next_y);
  auto* hover_root = root(next);
  auto* hover_observer = hover_root == nullptr ? nullptr : hover_root->observer;
  if (hover_root != nullptr && hover_observer != nullptr && hover_observer->config != nullptr &&
      hover_observer->config->input.focus_mode == FocusMode::hover && seat->keyboard_focus != hover_root) {
    auto* current_root = root(seat->keyboard_focus);
    const bool exclusive_active = current_root != nullptr && current_root->layer_surface != nullptr &&
        current_root->layer_surface->mapped &&
        current_root->layer_surface->current.keyboard == protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
        current_root->layer_surface->current.layer >= protocol::ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    const bool target_accepts_keyboard = hover_root->layer_surface == nullptr ||
        hover_root->layer_surface->current.keyboard != protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    if (!exclusive_active && target_accepts_keyboard) {
      auto* old_focus = seat->toplevel_focus;
      set_keyboard_focus(seat, hover_root);
      configure_layout(hover_observer);
      if (old_focus != nullptr && old_focus != hover_root) notify_surface(old_focus);
      notify_surface(hover_root);
    }
  }
  update_pointer_constraints(seat, next_x, next_y);
  auto* constraint = active_constraint(seat);
  if (next != nullptr && (constraint == nullptr || !constraint->locked)) {
    if (constraint != nullptr && !constraint->locked) {
      next_x = constraint->x; next_y = constraint->y;
      if (managed_surface(next)) surface_global_from_local(next, next_x, next_y, &seat->pointer_x, &seat->pointer_y);
    }
    auto* client = next->resource->client;
    for (auto* resource : seat->pointers) if (resource->client == client) protocol::wl_pointer_send_motion(*resource, time, (next_x), (next_y));
    send_pointer_frame(seat, client);
  }
  sync_pointer_position(seat);
}
}  // namespace detail
void CompositorServer::pointer_motion(std::uint32_t time, std::uint64_t id, std::int32_t x, std::int32_t y) {
  idle_activity(&impl_->seat_state);
  const auto hit = std::find_if(impl_->surfaces.begin(), impl_->surfaces.end(), [id](const SurfaceState* surface) { return surface->id == id; });
  route_pointer_motion(&impl_->seat_state, time, hit == impl_->surfaces.end() ? nullptr : *hit, x, y, std::nullopt);
}
bool CompositorServer::pointer_pan_motion(double dx, double dy) {
  auto& seat = impl_->seat_state;
  idle_activity(&seat);
  if (!seat.canvas_panning) return false;
  auto* output = output_state(&impl_->observer, seat.interactive_output);
  if (output == nullptr) {
    end_interactive(&seat);
    return false;
  }
  auto& viewport = output->canvas_viewports[output->active_tag - 1];
  viewport.x -= dx / viewport.scale;
  viewport.y -= dy / viewport.scale;
  configure_layout(&impl_->observer);
  sync_pointer_position(&seat);
  return true;
}
void CompositorServer::pointer_motion_global(std::uint32_t time, std::int32_t x, std::int32_t y) {
  idle_activity(&impl_->seat_state);
  if (!impl_->outputs.empty()) {
    auto contains = [x, y](const auto& output) { const auto& info = output->info; return x >= info.logical_x && y >= info.logical_y && x < info.logical_x + static_cast<std::int32_t>(info.logical_width) && y < info.logical_y + static_cast<std::int32_t>(info.logical_height); };
    auto output = std::find_if(impl_->outputs.begin(), impl_->outputs.end(), contains);
    if (output == impl_->outputs.end()) {
      output = std::min_element(impl_->outputs.begin(), impl_->outputs.end(), [x, y](const auto& left, const auto& right) { const auto distance = [x, y](const auto& item) { const auto& info = item->info; const auto px = std::clamp(x, info.logical_x, info.logical_x + static_cast<std::int32_t>(info.logical_width) - 1); const auto py = std::clamp(y, info.logical_y, info.logical_y + static_cast<std::int32_t>(info.logical_height) - 1); return std::abs(static_cast<std::int64_t>(x) - px) + std::abs(static_cast<std::int64_t>(y) - py); }; return distance(left) < distance(right); });
      const auto& info = (*output)->info; x = std::clamp(x, info.logical_x, info.logical_x + static_cast<std::int32_t>(info.logical_width) - 1); y = std::clamp(y, info.logical_y, info.logical_y + static_cast<std::int32_t>(info.logical_height) - 1);
    }
    const OutputId previous_output = impl_->active_output;
    impl_->active_output = (*output)->info.id;
    impl_->observer.active_output = impl_->active_output;
    if (impl_->active_output != previous_output) notify_active_tags(&impl_->observer);
  }
  if (impl_->seat_state.canvas_panning) {
    auto* output = output_state(&impl_->observer, impl_->seat_state.interactive_output);
    if (output == nullptr) end_interactive(&impl_->seat_state);
    else {
      auto& viewport = output->canvas_viewports[output->active_tag - 1];
      const auto dx = layout::canvas_world_delta(x - impl_->seat_state.interactive_pointer_x, viewport);
      const auto dy = layout::canvas_world_delta(y - impl_->seat_state.interactive_pointer_y, viewport);
      viewport.x = impl_->seat_state.canvas_pan_start.x - dx;
      viewport.y = impl_->seat_state.canvas_pan_start.y - dy;
      configure_layout(&impl_->observer);
      impl_->seat_state.pointer_x = x;
      impl_->seat_state.pointer_y = y;
      sync_pointer_position(&impl_->seat_state);
      return;
    }
  } else if (impl_->seat_state.interactive != nullptr) {
    auto* xdg = impl_->seat_state.interactive->xdg_surface;
    if (xdg != nullptr) {
      const auto dx = x - impl_->seat_state.interactive_pointer_x;
      const auto dy = y - impl_->seat_state.interactive_pointer_y;
      if (endless_canvas(&impl_->observer) && xdg->canvas_bounds.initialized) {
        const auto* viewport = canvas_viewport(&impl_->observer, xdg->output, xdg->tag);
        if (viewport == nullptr) return;
        const auto world_dx = layout::canvas_world_delta(dx, *viewport);
        const auto world_dy = layout::canvas_world_delta(dy, *viewport);
        const auto edge = impl_->seat_state.resize_edge;
        if (edge == protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
          const auto snapped = snap_canvas_window(
              &impl_->observer, impl_->seat_state.interactive, xdg->output, xdg->tag, *viewport,
              impl_->seat_state.interactive_canvas_x + world_dx,
              impl_->seat_state.interactive_canvas_y + world_dy,
              xdg->canvas_bounds.width, xdg->canvas_bounds.height);
          xdg->canvas_bounds.x = snapped.first;
          xdg->canvas_bounds.y = snapped.second;
        } else {
          const auto original_right = impl_->seat_state.interactive_canvas_x + impl_->seat_state.interactive_canvas_width;
          const auto original_bottom = impl_->seat_state.interactive_canvas_y + impl_->seat_state.interactive_canvas_height;
          Rect bounds{0, 0, impl_->seat_state.interactive_canvas_width,
                      impl_->seat_state.interactive_canvas_height};
          if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT) != 0) bounds.width -= world_dx;
          if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_RIGHT) != 0) bounds.width += world_dx;
          if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP) != 0) bounds.height -= world_dy;
          if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM) != 0) bounds.height += world_dy;
          bounds = constrain_xdg_window(*impl_->config, *xdg, bounds);
          xdg->canvas_bounds.x = (edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT) != 0
                                    ? original_right - bounds.width : impl_->seat_state.interactive_canvas_x;
          xdg->canvas_bounds.y = (edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP) != 0
                                    ? original_bottom - bounds.height : impl_->seat_state.interactive_canvas_y;
          xdg->canvas_bounds.width = bounds.width;
          xdg->canvas_bounds.height = bounds.height;
        }
        configure_layout(&impl_->observer, xdg);
      } else if (impl_->seat_state.tiled_resize) {
        auto* output = output_state(&impl_->observer, impl_->seat_state.interactive_output);
        const auto work = output_work(&impl_->observer, impl_->seat_state.interactive_output);
        if (output == nullptr || work.width <= 0 || work.height <= 0) end_interactive(&impl_->seat_state);
        else if (impl_->seat_state.weight_before == 0) {
          const float start = impl_->seat_state.interactive_value_before;
          output->master_ratio = std::clamp(start + static_cast<float>(dx) / work.width,
                                             layout::MasterStack::kMinimumMasterRatio,
                                             layout::MasterStack::kMaximumMasterRatio);
          configure_layout(&impl_->observer);
        } else {
          const float total = impl_->seat_state.interactive_value_before + impl_->seat_state.interactive_value_after;
          const float delta = static_cast<float>(dy) / work.height * total;
          const float adjusted = std::clamp(delta, 0.1F - impl_->seat_state.interactive_value_before,
                                             impl_->seat_state.interactive_value_after - 0.1F);
          output->tile_weights[impl_->seat_state.weight_before] = impl_->seat_state.interactive_value_before + adjusted;
          output->tile_weights[impl_->seat_state.weight_after] = impl_->seat_state.interactive_value_after - adjusted;
          configure_layout(&impl_->observer);
        }
      } else {
      Rect bounds = impl_->seat_state.interactive_start;
      const auto edge = impl_->seat_state.resize_edge;
      if (edge == protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE) { bounds.x += dx; bounds.y += dy; }
      else {
        const auto original_right = bounds.x + bounds.width;
        const auto original_bottom = bounds.y + bounds.height;
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT) != 0) { bounds.x += dx; bounds.width -= dx; }
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_RIGHT) != 0) bounds.width += dx;
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP) != 0) { bounds.y += dy; bounds.height -= dy; }
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM) != 0) bounds.height += dy;
        bounds = constrain_xdg_window(*impl_->config, *xdg, bounds);
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT) != 0) bounds.x = original_right - bounds.width;
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP) != 0) bounds.y = original_bottom - bounds.height;
      }
      const auto work = output_work(&impl_->observer, xdg->output);
      bounds = clamp_window(bounds, work);
      bounds = constrain_xdg_window(*impl_->config, *xdg, bounds);
      if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT) != 0)
        bounds.x = impl_->seat_state.interactive_start.x + impl_->seat_state.interactive_start.width - bounds.width;
      if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP) != 0)
        bounds.y = impl_->seat_state.interactive_start.y + impl_->seat_state.interactive_start.height - bounds.height;
      xdg->floating_bounds = position_window(bounds, work);
      xdg->last_floating_bounds = xdg->floating_bounds;
      configure_layout(&impl_->observer, xdg);
      }
    }
#ifdef ZWWM_XWAYLAND
    else if (auto* role = impl_->seat_state.interactive->xwayland_surface;
             role != nullptr && endless_canvas(&impl_->observer) && role->canvas_bounds.initialized) {
      const auto dx = x - impl_->seat_state.interactive_pointer_x;
      const auto dy = y - impl_->seat_state.interactive_pointer_y;
      const auto* viewport = canvas_viewport(&impl_->observer, role->output, role->tag);
      if (viewport == nullptr) return;
      const auto world_dx = layout::canvas_world_delta(dx, *viewport);
      const auto world_dy = layout::canvas_world_delta(dy, *viewport);
      const auto edge = impl_->seat_state.resize_edge;
      if (edge == protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
        const auto snapped = snap_canvas_window(
            &impl_->observer, impl_->seat_state.interactive, role->output, role->tag, *viewport,
            impl_->seat_state.interactive_canvas_x + world_dx,
            impl_->seat_state.interactive_canvas_y + world_dy,
            role->canvas_bounds.width, role->canvas_bounds.height);
        role->canvas_bounds.x = snapped.first;
        role->canvas_bounds.y = snapped.second;
      } else {
        const auto original_right = impl_->seat_state.interactive_canvas_x + impl_->seat_state.interactive_canvas_width;
        const auto original_bottom = impl_->seat_state.interactive_canvas_y + impl_->seat_state.interactive_canvas_height;
        Rect bounds{0, 0, impl_->seat_state.interactive_canvas_width,
                    impl_->seat_state.interactive_canvas_height};
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT) != 0) bounds.width -= world_dx;
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_RIGHT) != 0) bounds.width += world_dx;
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP) != 0) bounds.height -= world_dy;
        if ((edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM) != 0) bounds.height += world_dy;
        bounds.width = std::max(1, bounds.width);
        bounds.height = std::max(1, bounds.height);
        role->canvas_bounds.x = (edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT) != 0
                                   ? original_right - bounds.width : impl_->seat_state.interactive_canvas_x;
        role->canvas_bounds.y = (edge & protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP) != 0
                                   ? original_bottom - bounds.height : impl_->seat_state.interactive_canvas_y;
        role->canvas_bounds.width = bounds.width;
        role->canvas_bounds.height = bounds.height;
      }
      configure_layout(&impl_->observer);
    }
#endif
  }
  auto* hit = surface_at_global(&impl_->observer, x, y);
  if (hit == nullptr) {
    auto* constraint = active_constraint(&impl_->seat_state);
    if (constraint == nullptr || constraint->surface == nullptr) {
      impl_->seat_state.pointer_x = x;
      impl_->seat_state.pointer_y = y;
      pointer_leave();
      return;
    }
    route_pointer_motion(&impl_->seat_state, time, constraint->surface, constraint->x, constraint->y, std::nullopt);
    return;
  }
  std::int32_t local_x = 0, local_y = 0;
  surface_local_from_global(hit, x, y, &local_x, &local_y);
  route_pointer_motion(&impl_->seat_state, time, hit, local_x, local_y, std::pair{x, y});
}
void CompositorServer::pointer_relative_motion(std::uint64_t time_usec, double dx, double dy,
                                               double dx_unaccelerated, double dy_unaccelerated) {
  auto& seat = impl_->seat_state;
  if (dx != 0.0 || dy != 0.0 || dx_unaccelerated != 0.0 || dy_unaccelerated != 0.0)
    idle_activity(&seat);
  if (seat.pointer_focus == nullptr) return;
  auto* client = seat.pointer_focus->resource->client;
  bool sent = false;
  for (auto* relative : seat.relative_pointers) {
    if (relative->pointer == nullptr || relative->pointer->client != client) continue;
    protocol::zwp_relative_pointer_v1_send_relative_motion(*
        relative->resource, static_cast<std::uint32_t>(time_usec >> 32U),
        static_cast<std::uint32_t>(time_usec), (dx), (dy),
        (dx_unaccelerated), (dy_unaccelerated));
    sent = true;
  }
  if (sent) for (auto* pointer : seat.pointers) {
    if (pointer->client == client && pointer->version >= 5)
      protocol::wl_pointer_send_frame(*pointer);
  }
}
void CompositorServer::pointer_leave() { auto& seat = impl_->seat_state; seat.hit_target = nullptr; if (seat.drag_origin != nullptr) drag_motion(&seat, 0, nullptr, 0, 0); if (auto* constraint = active_constraint(&seat); constraint != nullptr) deactivate_constraint(constraint, true); if (seat.popup_grab == nullptr && seat.pointer_grab == nullptr) set_pointer_focus(&seat, nullptr, 0, 0); }
void CompositorServer::pointer_button(std::uint32_t time, std::uint32_t button, std::uint32_t state) {
  auto& seat = impl_->seat_state;
  if (state != protocol::WL_POINTER_BUTTON_STATE_PRESSED && state != protocol::WL_POINTER_BUTTON_STATE_RELEASED) return;
  idle_activity(&seat);
  if (state == protocol::WL_POINTER_BUTTON_STATE_RELEASED && seat.compositor_interactive &&
      button == seat.interactive_button) {
    std::erase(seat.pressed_buttons, button);
    seat.pointer_grab = nullptr;
    end_interactive(&seat);
    auto* next = seat.hit_target;
    std::int32_t local_x = 0, local_y = 0;
    if (next != nullptr && managed_surface(next)) surface_local_from_global(next, seat.pointer_x, seat.pointer_y, &local_x, &local_y);
    set_pointer_focus(&seat, next, local_x, local_y);
    return;
  }
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED && seat.popup_grab != nullptr) {
    const bool inside = seat.hit_target != nullptr && seat.hit_target->xdg_surface != nullptr && seat.hit_target->xdg_surface->popup != nullptr && popup_descendant(seat.hit_target->xdg_surface->popup, popup_chain_root(seat.popup_grab));
    if (!inside) { popup_done(popup_chain_root(seat.popup_grab)); return; }
  }
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED && std::find(seat.pressed_buttons.begin(), seat.pressed_buttons.end(), button) != seat.pressed_buttons.end()) return;
  if (state == protocol::WL_POINTER_BUTTON_STATE_RELEASED && std::find(seat.pressed_buttons.begin(), seat.pressed_buttons.end(), button) == seat.pressed_buttons.end()) return;
  if (seat.drag_origin != nullptr) {
    drag_button(&seat, button, state);
    if (seat.drag_origin == nullptr) {
      auto* next = seat.hit_target;
      std::int32_t local_x = 0, local_y = 0;
      if (next != nullptr) surface_local_from_global(next, seat.pointer_x, seat.pointer_y, &local_x, &local_y);
      set_pointer_focus(&seat, next, local_x, local_y);
    }
    return;
  }
  SurfaceState* target = seat.pointer_grab != nullptr ? seat.pointer_grab : seat.pointer_focus;
  auto* target_root = root(target);
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED && seat.session_lock == nullptr && target_root != nullptr) {
    const auto* xdg = target_root->xdg_surface;
#ifdef ZWWM_XWAYLAND
    const auto* xwayland = target_root->xwayland_surface;
#endif
    const bool managed = (xdg != nullptr && xdg->toplevel != nullptr)
#ifdef ZWWM_XWAYLAND
        || (xwayland != nullptr && xwayland->window != nullptr && xwayland->window->mapped)
#endif
        ;
    const bool floating = (xdg != nullptr && xdg->floating)
#ifdef ZWWM_XWAYLAND
        || (xwayland != nullptr && xwayland->floating)
#endif
        ;
    if (managed && (floating || endless_canvas(&impl_->observer))) raise_root(target_root);
  }
  const bool background = target == nullptr || (target_root != nullptr && target_root->layer_surface != nullptr &&
      target_root->layer_surface->current.layer == protocol::ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_LEFT && background &&
      seat.session_lock == nullptr && endless_canvas(&impl_->observer)) {
    auto* output = output_state(&impl_->observer, impl_->active_output);
    if (output != nullptr) {
      seat.compositor_interactive = true;
      seat.canvas_panning = true;
      seat.cursor_override_shape = "grabbing";
      apply_cursor_shape(&seat);
      seat.interactive_button = button;
      seat.interactive_output = output->info.id;
      seat.interactive_pointer_x = seat.pointer_x;
      seat.interactive_pointer_y = seat.pointer_y;
      seat.canvas_pan_start = output->canvas_viewports[output->active_tag - 1];
      seat.pressed_buttons.push_back(button);
      seat.pointer_grab = nullptr;
      set_pointer_focus(&seat, nullptr, 0, 0);
      return;
    }
  }
  if (target == nullptr) {
    if (state == protocol::WL_POINTER_BUTTON_STATE_RELEASED) {
      std::erase(seat.pressed_buttons, button);
      if (seat.pressed_buttons.empty()) {
        seat.pointer_grab = nullptr;
        end_interactive(&seat);
      }
    }
    return;
  }
  const bool super = seat.xkb_state_handle != nullptr &&
      xkb_state_mod_name_is_active(seat.xkb_state_handle, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) != 0;
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED && seat.session_lock == nullptr && super &&
      (button == BTN_LEFT || button == BTN_RIGHT)) {
    auto* top = root(target);
    auto* xdg = top == nullptr ? nullptr : top->xdg_surface;
#ifdef ZWWM_XWAYLAND
    auto* xwayland = top == nullptr ? nullptr : top->xwayland_surface;
    if (xdg == nullptr && xwayland != nullptr && xwayland->window != nullptr && xwayland->window->mapped &&
        !xwayland->fullscreen && endless_canvas(&impl_->observer)) {
      if (seat.keyboard_focus != top) set_keyboard_focus(&seat, top);
      configure_layout(&impl_->observer);
      seat.compositor_interactive = true;
      seat.interactive_button = button;
      seat.pointer_grab = top;
      seat.pressed_buttons.push_back(button);
      seat.interactive_pointer_x = seat.pointer_x;
      seat.interactive_pointer_y = seat.pointer_y;
      seat.interactive_output = xwayland->output;
      seat.interactive = top;
      seat.interactive_start = xwayland->tile_bounds;
      seat.interactive_canvas_x = xwayland->canvas_bounds.x;
      seat.interactive_canvas_y = xwayland->canvas_bounds.y;
      seat.interactive_canvas_width = xwayland->canvas_bounds.width;
      seat.interactive_canvas_height = xwayland->canvas_bounds.height;
      if (button == BTN_RIGHT) {
        const auto horizontal = seat.pointer_x < xwayland->tile_bounds.x + xwayland->tile_bounds.width / 2
                                    ? protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT : protocol::XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        const auto vertical = seat.pointer_y < xwayland->tile_bounds.y + xwayland->tile_bounds.height / 2
                                  ? protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP : protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
        seat.resize_edge = horizontal | vertical;
      }
      return;
    }
#endif
    if (xdg != nullptr && xdg->toplevel != nullptr && xdg->mapped && !xdg->fullscreen) {
      if (seat.keyboard_focus != top) set_keyboard_focus(&seat, top);
      seat.compositor_interactive = true;
      seat.interactive_button = button;
      seat.pointer_grab = top;
      seat.pressed_buttons.push_back(button);
      seat.interactive_pointer_x = seat.pointer_x;
      seat.interactive_pointer_y = seat.pointer_y;
      seat.interactive_output = xdg->output;
      if (endless_canvas(&impl_->observer)) {
        configure_layout(&impl_->observer, xdg);
        seat.interactive = top;
        seat.interactive_start = xdg->tile_bounds;
        seat.interactive_canvas_x = xdg->canvas_bounds.x;
        seat.interactive_canvas_y = xdg->canvas_bounds.y;
        seat.interactive_canvas_width = xdg->canvas_bounds.width;
        seat.interactive_canvas_height = xdg->canvas_bounds.height;
        if (button == BTN_RIGHT) {
          const auto horizontal = seat.pointer_x < xdg->tile_bounds.x + xdg->tile_bounds.width / 2
                                      ? protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT : protocol::XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
          const auto vertical = seat.pointer_y < xdg->tile_bounds.y + xdg->tile_bounds.height / 2
                                    ? protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP : protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
          seat.resize_edge = horizontal | vertical;
        }
      } else if (button == BTN_LEFT && xdg->floating) {
        seat.interactive = top;
        seat.interactive_start = xdg->floating_bounds;
      } else if (button == BTN_RIGHT && xdg->floating) {
        seat.interactive = top;
        seat.interactive_start = xdg->floating_bounds;
        const auto horizontal = seat.pointer_x < xdg->floating_bounds.x + xdg->floating_bounds.width / 2
                                    ? protocol::XDG_TOPLEVEL_RESIZE_EDGE_LEFT : protocol::XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        const auto vertical = seat.pointer_y < xdg->floating_bounds.y + xdg->floating_bounds.height / 2
                                  ? protocol::XDG_TOPLEVEL_RESIZE_EDGE_TOP : protocol::XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
        seat.resize_edge = horizontal | vertical;
      } else if (button == BTN_RIGHT && !xdg->floating) {
        seat.interactive = top;
        seat.tiled_resize = true;
        auto* output = output_state(&impl_->observer, xdg->output);
        const auto work = output_work(&impl_->observer, xdg->output);
        std::vector<std::uint64_t> roots;
        for (auto* surface : impl_->surfaces) {
          auto* candidate = surface->xdg_surface;
          if (surface->parent == nullptr && candidate != nullptr && candidate->toplevel != nullptr &&
              !candidate->floating && !candidate->fullscreen && candidate->output == xdg->output &&
              visible_xdg(&impl_->observer, candidate)) roots.push_back(surface->id);
        }
        const auto current = std::find(roots.begin(), roots.end(), top->id);
        const std::size_t index = static_cast<std::size_t>(std::distance(roots.begin(), current));
        const std::size_t masters = std::min<std::size_t>(impl_->config->layout.master_count, roots.size());
        const auto placements = root_placements(&impl_->observer, nullptr, work, xdg->output);
        const auto placement = std::find_if(placements.begin(), placements.end(), [top](const auto& item) { return item.id == top->id; });
        const int split_x = placements.empty() || masters == 0 ? seat.pointer_x :
            placements.front().bounds.origin.x + static_cast<int>(placements.front().bounds.size.width) +
            static_cast<int>(impl_->config->layout.inner_gap) / 2;
        const std::size_t column_begin = index < masters ? 0 : masters;
        const std::size_t column_end = index < masters ? masters : roots.size();
        std::optional<std::size_t> neighbor;
        if (current != roots.end() && placement != placements.end()) {
          const int center = placement->bounds.origin.y + static_cast<int>(placement->bounds.size.height) / 2;
          if (seat.pointer_y < center && index > column_begin) neighbor = index - 1;
          else if (index + 1 < column_end) neighbor = index + 1;
          else if (index > column_begin) neighbor = index - 1;
        }
        int boundary_distance = std::numeric_limits<int>::max();
        if (neighbor) {
          const auto adjacent = std::find_if(placements.begin(), placements.end(), [&](const auto& item) { return item.id == roots[*neighbor]; });
          if (adjacent != placements.end()) {
            const int boundary = *neighbor < index ? placement->bounds.origin.y : adjacent->bounds.origin.y;
            boundary_distance = std::abs(seat.pointer_y - boundary);
          }
        }
        const bool split = roots.size() > masters && std::abs(seat.pointer_x - split_x) <= boundary_distance;
        if (split) {
          seat.interactive_value_before = output != nullptr && output->master_ratio ? *output->master_ratio :
              static_cast<float>(impl_->config->layout.master_ratio_percent) / 100.0F;
        } else if (neighbor && output != nullptr) {
          seat.weight_before = roots[std::min(index, *neighbor)];
          seat.weight_after = roots[std::max(index, *neighbor)];
          const auto before = output->tile_weights.find(seat.weight_before);
          const auto after = output->tile_weights.find(seat.weight_after);
          seat.interactive_value_before = before == output->tile_weights.end() ? 1.0F : before->second;
          seat.interactive_value_after = after == output->tile_weights.end() ? 1.0F : after->second;
        } else {
          end_interactive(&seat);
        }
      }
      return;
    }
  }
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED) {
    seat.pointer_grab = target;
    seat.pressed_buttons.push_back(button);
    SurfaceState* toplevel = root(target);
    const bool keyboard_allowed = toplevel == nullptr || toplevel->layer_surface == nullptr ||
                                   toplevel->layer_surface->current.keyboard != protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    auto* current_root = root(seat.keyboard_focus);
    const bool exclusive_active = current_root != nullptr && current_root->layer_surface != nullptr &&
        current_root->layer_surface->mapped &&
        current_root->layer_surface->current.keyboard == protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
        current_root->layer_surface->current.layer >= protocol::ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    if (keyboard_allowed && (!exclusive_active || current_root == toplevel) && seat.keyboard_focus != toplevel) {
      SurfaceState* old_focus = seat.toplevel_focus;
      set_keyboard_focus(&seat, toplevel);
      configure_layout(&impl_->observer);
      if (old_focus != nullptr) notify_surface(old_focus);
      notify_surface(toplevel);
    }
  } else {
    std::erase(seat.pressed_buttons, button);
  }
  const auto serial = seat.display->next_serial();
  auto* client = target->resource->client;
  for (auto* resource : seat.pointers) if (resource->client == client) protocol::wl_pointer_send_button(*resource, serial, time, button, state);
  remember_serial(seat, client, serial);
  if (state == protocol::WL_POINTER_BUTTON_STATE_PRESSED) { seat.button_client = client; seat.button_serial = serial; seat.button = button; }
  send_pointer_frame(&seat, client);
  if (state == protocol::WL_POINTER_BUTTON_STATE_RELEASED && seat.pressed_buttons.empty()) {
    seat.interactive = nullptr;
    seat.resize_edge = protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    seat.pointer_grab = nullptr;
    auto* next = seat.hit_target;
    std::int32_t local_x = 0, local_y = 0;
    if (next != nullptr && managed_surface(next)) surface_local_from_global(next, seat.pointer_x, seat.pointer_y, &local_x, &local_y);
    else if (next != nullptr) { absolute_position(next, &local_x, &local_y); local_x = seat.pointer_x - local_x; local_y = seat.pointer_y - local_y; }
    set_pointer_focus(&seat, next, local_x, local_y);
  }
}
void CompositorServer::pointer_axis(std::uint32_t time, std::uint32_t axis, double value, std::uint32_t source, std::int32_t discrete, std::int32_t value120, bool stop, std::uint32_t relative_direction) {
  auto& seat = impl_->seat_state;
  if (value != 0.0 || discrete != 0 || value120 != 0) idle_activity(&seat);
  if (seat.session_lock == nullptr && endless_canvas(&impl_->observer) &&
      axis == protocol::WL_POINTER_AXIS_VERTICAL_SCROLL &&
      source == protocol::WL_POINTER_AXIS_SOURCE_WHEEL && value != 0.0) {
    const std::string_view key = value < 0.0 ? "scrollback" : "scrollforward";
    for (const auto& binding : impl_->config->keybindings) {
      if (binding.key != key || !binding_modifiers_match(binding, seat)) continue;
      if (binding.action == KeyAction::zoomin || binding.action == KeyAction::zoomout) {
        zoom_canvas(&impl_->observer, &seat, binding.action == KeyAction::zoomin);
        return;
      }
    }
  }
  if (seat.pointer_focus == nullptr) return;
  if (impl_->config->input.reverse_mouse_scrolling &&
      (source == protocol::WL_POINTER_AXIS_SOURCE_WHEEL || source == protocol::WL_POINTER_AXIS_SOURCE_WHEEL_TILT)) {
    value = -value;
    discrete = -discrete;
    value120 = -value120;
  }
  auto* client = seat.pointer_focus->resource->client;
  for (auto* resource : seat.pointers) if (resource->client == client) {
    protocol::wl_pointer_send_axis(*resource, time, axis, (value));
    if (resource->version >= 5) protocol::wl_pointer_send_axis_source(*resource, source);
    if (resource->version >= 5 && stop) protocol::wl_pointer_send_axis_stop(*resource, time, axis);
    if (resource->version >= 5 && discrete != 0) protocol::wl_pointer_send_axis_discrete(*resource, axis, discrete);
    if (resource->version >= 8 && value120 != 0) protocol::wl_pointer_send_axis_value120(*resource, axis, value120);
    if (resource->version >= 9) protocol::wl_pointer_send_axis_relative_direction(*resource, axis, relative_direction);
  }
  send_pointer_frame(&seat, client);
}
void CompositorServer::keyboard_key(std::uint32_t time, std::uint32_t key, std::uint32_t state) {
  auto& seat = impl_->seat_state;
  if (state != protocol::WL_KEYBOARD_KEY_STATE_PRESSED && state != protocol::WL_KEYBOARD_KEY_STATE_RELEASED) return;
  idle_activity(&seat);
  const auto existing = std::find(seat.pressed_keys.begin(), seat.pressed_keys.end(), key);
  if ((state == protocol::WL_KEYBOARD_KEY_STATE_PRESSED && existing != seat.pressed_keys.end()) || (state == protocol::WL_KEYBOARD_KEY_STATE_RELEASED && existing == seat.pressed_keys.end())) return;
  if (state == protocol::WL_KEYBOARD_KEY_STATE_PRESSED) seat.pressed_keys.push_back(key); else seat.pressed_keys.erase(existing);
  if (seat.xkb_state_handle != nullptr && key <= std::numeric_limits<xkb_keycode_t>::max() - 8) xkb_state_update_key(seat.xkb_state_handle, static_cast<xkb_keycode_t>(key + 8), state == protocol::WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
  if (state == protocol::WL_KEYBOARD_KEY_STATE_PRESSED && seat.session_lock == nullptr) {
    for (const auto& binding : impl_->config->keybindings) {
      if (!binding_matches(binding, seat, key)) continue;
      auto* focused = seat.toplevel_focus;
      auto* xdg = focused == nullptr ? nullptr : focused->xdg_surface;
#ifdef ZWWM_XWAYLAND
      auto* xwayland = focused == nullptr ? nullptr : focused->xwayland_surface;
#endif
      if (binding.action == KeyAction::exec) execute_binding(binding);
      else if (binding.action == KeyAction::killactive && xdg != nullptr && xdg->toplevel != nullptr) protocol::xdg_toplevel_send_close(*xdg->toplevel);
#ifdef ZWWM_XWAYLAND
      else if (binding.action == KeyAction::killactive && xwayland != nullptr && xwayland->runtime != nullptr)
        xwayland->runtime->close_window(xwayland->window);
#endif
      else if (binding.action == KeyAction::killsession || binding.action == KeyAction::exit) seat.display->stop();
      else if (binding.action == KeyAction::togglefloating && xdg != nullptr) set_floating(xdg, !xdg->floating);
#ifdef ZWWM_XWAYLAND
      else if (binding.action == KeyAction::togglefloating && xwayland != nullptr) {
        set_xwayland_floating(xwayland, !xwayland->floating);
        if (xwayland->runtime != nullptr) xwayland->runtime->publish_state(xwayland);
        configure_layout(&impl_->observer);
      }
#endif
      else if (binding.action == KeyAction::fullscreen && xdg != nullptr) set_fullscreen(xdg, {}, !xdg->fullscreen);
#ifdef ZWWM_XWAYLAND
      else if (binding.action == KeyAction::fullscreen && xwayland != nullptr) {
        xwayland->fullscreen = !xwayland->fullscreen;
        if (xwayland->runtime != nullptr) xwayland->runtime->publish_state(xwayland);
        configure_layout(&impl_->observer);
      }
#endif
      else if ((binding.action == KeyAction::tag || binding.action == KeyAction::movetotag) && !binding.argument.empty()) {
        const auto tag = static_cast<std::uint8_t>(binding.argument.front() - '0');
        auto* output = output_state(&impl_->observer, impl_->active_output);
        if (binding.action == KeyAction::tag && output != nullptr) {
          switch_active_tag(&impl_->observer, impl_->active_output, tag);
          send_modifiers(seat);
          return;
        }
        else if (binding.action == KeyAction::movetotag && xdg != nullptr) xdg->tag = tag;
#ifdef ZWWM_XWAYLAND
        else if (binding.action == KeyAction::movetotag && xwayland != nullptr) xwayland->tag = tag;
#endif
        SurfaceState* fallback = nullptr;
        for (auto it = impl_->surfaces.rbegin(); it != impl_->surfaces.rend(); ++it) {
          auto* candidate = (*it)->xdg_surface;
          if ((*it)->parent == nullptr && candidate != nullptr && candidate->toplevel != nullptr &&
              candidate->output == impl_->active_output && visible_xdg(&impl_->observer, candidate)) { fallback = *it; break; }
        }
        if (binding.action == KeyAction::movetotag && output != nullptr &&
            ((xdg != nullptr && xdg->tag == output->active_tag)
#ifdef ZWWM_XWAYLAND
             || (xwayland != nullptr && xwayland->tag == output->active_tag)
#endif
            )) fallback = focused;
        set_keyboard_focus(&seat, fallback);
        configure_layout(&impl_->observer);
        for (auto* surface : impl_->surfaces) if (surface->parent == nullptr &&
            (surface->xdg_surface != nullptr
#ifdef ZWWM_XWAYLAND
             || surface->xwayland_surface != nullptr
#endif
            )) notify_surface_tree(surface);
        if (binding.action == KeyAction::movetotag && xdg != nullptr) notify_toplevel_tags(&impl_->observer, xdg);
      } else if (binding.action == KeyAction::focus) {
        std::vector<SurfaceState*> visible;
        for (auto* surface : impl_->surfaces) {
          if (surface->parent != nullptr) continue;
          bool is_visible = surface->xdg_surface != nullptr && surface->xdg_surface->toplevel != nullptr &&
                            visible_xdg(&impl_->observer, surface->xdg_surface);
#ifdef ZWWM_XWAYLAND
          if (!is_visible && surface->xwayland_surface != nullptr && surface->xwayland_surface->window != nullptr &&
              surface->xwayland_surface->window->mapped) {
            const auto* assigned_output = output_state(&impl_->observer, surface->xwayland_surface->output);
            is_visible = assigned_output != nullptr && surface->xwayland_surface->tag == assigned_output->active_tag;
          }
#endif
          if (is_visible) visible.push_back(surface);
        }
        if (!visible.empty()) {
          auto current = std::find(visible.begin(), visible.end(), focused);
          if (current == visible.end() || ++current == visible.end()) current = visible.begin();
          set_keyboard_focus(&seat, *current); configure_layout(&impl_->observer);
        }
      }
      send_modifiers(seat);
      return;
    }
  }
  if (seat.keyboard_focus == nullptr) return;
  const auto serial = seat.display->next_serial();
  auto* client = seat.keyboard_focus->resource->client;
  for (auto* resource : seat.keyboards) if (resource->client == client) protocol::wl_keyboard_send_key(*resource, serial, time, key, state);
  remember_serial(seat, client, serial);
  send_modifiers(seat);
}
void CompositorServer::keyboard_modifiers(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked, std::uint32_t group) { auto& seat = impl_->seat_state; if (seat.xkb_state_handle != nullptr) xkb_state_update_mask(seat.xkb_state_handle, depressed, latched, locked, 0, 0, group); send_modifiers(seat); }
void CompositorServer::keyboard_repeat_info(std::int32_t rate, std::int32_t delay) { if (rate < 0 || delay < 0) return; auto& seat = impl_->seat_state; seat.repeat_rate = rate; seat.repeat_delay = delay; for (auto* resource : seat.keyboards) if (resource->version >= 4) protocol::wl_keyboard_send_repeat_info(*resource, rate, delay); }
void CompositorServer::input_reset(bool preserve_keyboard_focus) { auto& seat = impl_->seat_state; cancel_drag(&seat); end_interactive(&seat); seat.pressed_buttons.clear(); seat.pressed_keys.clear(); seat.pointer_grab = nullptr; seat.popup_grab = nullptr; seat.button_client = nullptr; seat.button_serial = 0; seat.button = 0; set_pointer_focus(&seat, nullptr, 0, 0); if (!preserve_keyboard_focus) set_keyboard_focus(&seat, nullptr); if (seat.xkb_state_handle != nullptr) xkb_state_update_mask(seat.xkb_state_handle, 0, 0, 0, 0, 0, 0); if (preserve_keyboard_focus) send_modifiers(seat); }
}  // namespace zwwm
