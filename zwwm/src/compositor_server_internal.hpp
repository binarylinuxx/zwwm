#pragma once

#include "zwwm/compositor_server.hpp"

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <xdg-shell-zwayland-server.h>
#include <pointer-constraints-zwayland-server.h>
#include <ext-idle-notify-zwayland-server.h>
#include <wlr-layer-shell-unstable-v1-zwayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include "zwwm/runtime_config.hpp"
#include "zwwm/unique_fd.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace protocol = zwayland::generated;

namespace zwwm::detail {

inline constexpr std::uint32_t kSeatVersion = 5;
inline constexpr std::uint32_t kShmVersion = 1;

struct SurfaceState;
struct TagsState;
struct SeatState;
struct DataSourceState;
struct DataDeviceState;
struct DataOfferState;
struct PointerConstraint;
struct RelativePointer;
struct IdleNotification;
struct XdgSurfaceState;
struct DialogState;
struct PopupState;
struct ViewportState;
struct FractionalScaleState;
struct LayerSurfaceState;
struct LockSurfaceState;
struct SessionLockState;
#ifdef ZWWM_XWAYLAND
struct XwaylandSelection;
struct XwaylandSurfaceState;
#endif

struct CanvasBounds {
  std::int64_t x = 0, y = 0;
  std::int32_t width = 0, height = 0;
  bool initialized = false;
};
struct CanvasViewport {
  double x = 0.0, y = 0.0;
  double scale = 1.0;
  double zoom_log_velocity = 0.0;
  std::uint64_t last_zoom_tick_ms = 0;
};

struct OutputState {
  struct FibonacciLeaf { std::uint64_t id = 0; std::vector<bool> path; };

  OutputInfo info;
  std::uint32_t global = 0;
  std::vector<zwayland::server::Resource*> resources;
  std::array<std::vector<SurfaceState*>, 4> layer_roots;
  std::uint8_t active_tag = 1;
  SurfaceState* fullscreen = nullptr;
  std::optional<float> master_ratio;
  std::unordered_map<std::uint64_t, float> tile_weights;
  std::array<std::vector<FibonacciLeaf>, 9> fibonacci_leaves;
  std::array<CanvasViewport, 9> canvas_viewports;
  bool retired = false;
};

struct Observer {
  CompositorServer::SurfaceCommitObserver callback = nullptr;
  void* data = nullptr;
  CompositorServer::CursorShapeObserver cursor_callback = nullptr;
  void* cursor_data = nullptr;
  CompositorServer::PointerPositionObserver pointer_position_callback = nullptr;
  void* pointer_position_data = nullptr;
  CompositorServer::ToplevelObserver toplevel_callback = nullptr;
  void* toplevel_data = nullptr;
  CompositorServer::PresentationObserver presentation_callback = nullptr;
  void* presentation_data = nullptr;
  CompositorServer::EventObserver event_callback = nullptr;
  void* event_data = nullptr;
  std::vector<SurfaceState*>* surfaces = nullptr;
  std::vector<std::unique_ptr<OutputState>>* outputs = nullptr;
  SeatState* seat = nullptr;
  const RuntimeConfig* config = nullptr;
  TagsState* tags = nullptr;
  pid_t portal_pid = -1;
  OutputId active_output;
  std::int32_t output_width = 960, output_height = 540;
  std::int32_t physical_width = 960, physical_height = 540;
  std::uint32_t refresh_millihz = 60000;
};

struct Pool { void* mapping = nullptr; std::size_t size = 0; UniqueFd fd; std::size_t buffers = 0; bool destroyed = false; };
struct Buffer { Pool* pool = nullptr; zwayland::server::Resource* resource = nullptr; std::size_t references = 1; std::int32_t offset = 0, width = 0, height = 0, stride = 0; std::uint32_t format = 0; std::optional<renderer::DmabufAttributes> dmabuf; };
struct DmabufState { std::vector<std::pair<std::uint32_t, std::uint64_t>> formats; std::optional<dev_t> main_device; };
struct Rect { std::int32_t x, y, width, height; };
struct RegionState { std::vector<Rect> rects; };

struct ViewportConfig {
  double x = 0, y = 0, width = 0, height = 0;
  std::int32_t destination_width = 0, destination_height = 0;
  bool has_source = false, has_destination = false;
};
struct PositionerState { std::int32_t width = 0, height = 0, offset_x = 0, offset_y = 0, parent_width = 0, parent_height = 0; Rect anchor_rect{0, 0, 0, 0}; std::uint32_t anchor = protocol::XDG_POSITIONER_ANCHOR_NONE, gravity = protocol::XDG_POSITIONER_GRAVITY_NONE, adjustment = protocol::XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE, parent_configure = 0; bool reactive = false; };
struct SurfaceState {
  ~SurfaceState();
  zwayland::server::Resource* resource = nullptr; Buffer* pending_buffer = nullptr; Buffer* current_buffer = nullptr;
  bool current_buffer_released = true; bool buffer_changed = false;
  std::uint64_t buffer_generation = 0;
  bool ever_committed = false;
  std::vector<Rect> pending_surface_damage, pending_buffer_damage;
  std::vector<zwayland::server::Resource*> frame_callbacks; XdgSurfaceState* xdg_surface = nullptr; SurfaceState* parent = nullptr;
  std::vector<zwayland::server::Resource*> pending_frame_callbacks;
  zwayland::server::Resource* subsurface = nullptr;
  std::vector<SurfaceState*> children; std::int32_t x = 0, y = 0; Observer* observer = nullptr; std::uint64_t id = 0; std::uint64_t root_order = 0; std::uint64_t stack_order = 0;
  ViewportState* viewport = nullptr; FractionalScaleState* fractional_scale = nullptr;
  OutputId entered_output;
  ViewportConfig pending_viewport, current_viewport; bool viewport_changed = false;
  std::int32_t pending_buffer_scale = 1, current_buffer_scale = 1; bool buffer_scale_changed = false;
  std::vector<Rect> pending_input_region, current_input_region;
  bool pending_input_infinite = true, current_input_infinite = true, input_region_changed = false;
  std::vector<Rect> pending_opaque_region, current_opaque_region;
  bool opaque_region_changed = false;
  bool above_parent = true;
  LayerSurfaceState* layer_surface = nullptr;
  LockSurfaceState* lock_surface = nullptr;
  bool layer_role_assigned = false;
  bool drag_icon_role = false;
#ifdef ZWWM_XWAYLAND
  XwaylandSurfaceState* xwayland_surface = nullptr;
#endif
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
struct LayerState { std::uint32_t layer = protocol::ZWLR_LAYER_SHELL_V1_LAYER_TOP, anchor = 0, keyboard = protocol::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE; std::int32_t width = 0, height = 0, zone = 0, top = 0, right = 0, bottom = 0, left = 0; };
struct XwlrLayerState { zwayland::server::Resource* resource = nullptr; LayerSurfaceState* layer = nullptr; std::uint32_t pending_edge = 0, current_edge = 0, reported_anchor = 0; std::int32_t pending_priority = 0, current_priority = 0, reported_zone = 0; double pending_opacity = 256, current_opacity = 256; bool ignored_reported = false; };
struct LayerSurfaceState { zwayland::server::Resource* resource = nullptr; SurfaceState* surface = nullptr; OutputId output; std::string name_space; LayerState pending, current; XwlrLayerState* xwlr = nullptr; std::uint64_t creation = 0; std::vector<std::uint32_t> serials; std::uint32_t last_sent = 0, last_acked = 0; std::int32_t configured_width = -1, configured_height = -1; bool configured = false, mapped = false, configure_requested = false, closed = false; };
struct LockSurfaceState { SessionLockState* lock = nullptr; zwayland::server::Resource* resource = nullptr; SurfaceState* surface = nullptr; OutputId output; std::vector<std::uint32_t> serials; std::uint32_t acked = 0; std::uint32_t width = 0, height = 0; bool mapped = false, presented = false; };
struct SessionLockState { Observer* observer = nullptr; zwayland::server::Client* client = nullptr; zwayland::server::Resource* resource = nullptr; std::vector<LockSurfaceState*> surfaces; bool accepted = false, locked_sent = false, unlocked = false; };
struct ViewportState { zwayland::server::Resource* resource = nullptr; SurfaceState* surface = nullptr; std::shared_ptr<bool> surface_alive; };
struct FractionalScaleState { zwayland::server::Resource* resource = nullptr; SurfaceState* surface = nullptr; std::shared_ptr<bool> surface_alive; };

struct ProtocolGlobals {
  zwayland::server::Display* display = nullptr;
  std::uint32_t viewporter = 0;
  std::uint32_t fractional_scale_manager = 0;
  std::uint32_t layer_shell = 0;
  std::uint32_t xwlr_layer_shell = 0;
  std::uint32_t relative_pointer_manager = 0;
  std::uint32_t idle_notifier = 0;
  Observer* observer = nullptr;
  ProtocolGlobals& operator=(zwayland::server::Display* next);
  operator zwayland::server::Display*() const { return display; }
  zwayland::server::Display* operator->() const { return display; }
  ~ProtocolGlobals();
};

struct SeatState {
  ~SeatState();
  ProtocolGlobals display;
  xkb_context* xkb_context_handle = nullptr;
  xkb_keymap* xkb_keymap_handle = nullptr;
  xkb_state* xkb_state_handle = nullptr;
  std::string keymap;
  std::vector<zwayland::server::Resource*> pointers;
  std::vector<zwayland::server::Resource*> keyboards;
  SurfaceState* pointer_focus = nullptr;
  SurfaceState* hit_target = nullptr;
  SurfaceState* toplevel_focus = nullptr;
  SurfaceState* keyboard_focus = nullptr;
  SurfaceState* regular_focus = nullptr;
  std::vector<DataSourceState*> data_sources;
  std::vector<DataDeviceState*> data_devices;
  std::vector<DataDeviceState*> data_control_devices;
  std::vector<DataOfferState*> data_offers;
  DataSourceState* selection = nullptr;
  DataSourceState* primary_selection = nullptr;
  std::vector<std::pair<zwayland::server::Client*, std::uint32_t>> input_serials;
  std::unordered_map<zwayland::server::Resource*, std::uint32_t> pointer_enter_serials;
  zwayland::server::Client* button_client = nullptr;
  std::uint32_t button_serial = 0;
  std::uint32_t button = 0;
  DataSourceState* drag_source = nullptr;
  SurfaceState* drag_origin = nullptr;
  SurfaceState* drag_icon = nullptr;
  SurfaceState* drag_target = nullptr;
  std::uint32_t drag_button = 0;
  std::uint32_t drag_action = 0;
  PopupState* popup_grab = nullptr;
  std::int32_t pointer_x = 0, pointer_y = 0;
  SurfaceState* pointer_grab = nullptr;
  SurfaceState* interactive = nullptr;
  std::uint32_t resize_edge = protocol::XDG_TOPLEVEL_RESIZE_EDGE_NONE;
  Rect interactive_start{0, 0, 0, 0};
  std::int32_t interactive_pointer_x = 0, interactive_pointer_y = 0;
  std::int64_t interactive_canvas_x = 0, interactive_canvas_y = 0;
  std::int32_t interactive_canvas_width = 0, interactive_canvas_height = 0;
  CanvasViewport canvas_pan_start;
  bool compositor_interactive = false;
  bool tiled_resize = false;
  bool canvas_panning = false;
  bool canvas_zooming = false;
  int canvas_zoom_timer = -1;
  std::uint32_t interactive_button = 0;
  OutputId interactive_output;
  std::uint64_t weight_before = 0, weight_after = 0;
  float interactive_value_before = 1.0F, interactive_value_after = 1.0F;
  std::vector<std::uint32_t> pressed_buttons;
  std::vector<std::uint32_t> pressed_keys;
  std::vector<PointerConstraint*> constraints;
  std::vector<RelativePointer*> relative_pointers;
  std::vector<IdleNotification*> idle_notifications;
  std::string cursor_shape = "left_ptr";
  std::string cursor_override_shape;
  bool cursor_hidden_for_lock = false;
  bool cursor_hidden_by_client = false;
  std::int32_t repeat_rate = 25, repeat_delay = 600;
  SessionLockState* session_lock = nullptr;
};
struct PointerConstraint { SeatState* seat = nullptr; SurfaceState* surface = nullptr; zwayland::server::Resource* pointer = nullptr; zwayland::server::Resource* resource = nullptr; std::vector<Rect> region; std::vector<Rect> pending_region; std::uint32_t lifetime = protocol::ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT; std::int32_t x = 0, y = 0; double pending_hint_x = 0, pending_hint_y = 0; double hint_x = 0, hint_y = 0; bool locked = false, active = false, defunct = false; bool has_region = false, pending_has_region = false, region_pending = false; bool hint_pending = false, has_hint = false; };
struct RelativePointer { SeatState* seat = nullptr; zwayland::server::Resource* pointer = nullptr; zwayland::server::Resource* resource = nullptr; };
struct IdleNotification { SeatState* seat = nullptr; zwayland::server::Resource* resource = nullptr; std::uint32_t timeout = 0; int timer = -1; bool idle = false; };

enum class DataSourceRole { unused, selection, drag };
enum class DataProtocol { standard, zwwm, zwlr };
struct DataSourceState { SeatState* seat = nullptr; zwayland::server::Resource* resource = nullptr; std::vector<std::string> mime_types; std::uint32_t actions = 0; DataSourceRole role = DataSourceRole::unused; bool cancelled = false; DataProtocol protocol = DataProtocol::standard;
#ifdef ZWWM_XWAYLAND
  XwaylandSelection* xwayland_selection = nullptr;
#endif
};
struct DataDeviceState { SeatState* seat = nullptr; zwayland::server::Resource* resource = nullptr; DataProtocol protocol = DataProtocol::standard; };
struct DataOfferState { SeatState* seat = nullptr; DataSourceState* source = nullptr; zwayland::server::Resource* resource = nullptr; DataProtocol protocol = DataProtocol::standard; std::uint32_t actions = 0; std::uint32_t preferred_action = 0; std::uint32_t action = 0; bool drag = false; bool active = false; bool dropped = false; bool accepted = false; };

SurfaceState* root(SurfaceState* surface);
void absolute_position(const SurfaceState* surface, std::int32_t* x, std::int32_t* y);
void surface_local_from_global(const SurfaceState* surface, std::int32_t global_x, std::int32_t global_y,
                               std::int32_t* local_x, std::int32_t* local_y);
void surface_global_from_local(const SurfaceState* surface, std::int32_t local_x, std::int32_t local_y,
                               std::int32_t* global_x, std::int32_t* global_y);
SurfaceState* compositor_surface_from_resource(zwayland::server::Client* client, zwayland::server::Resource* resource);
void bind_viewporter(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_fractional_scale_manager(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void send_buffer_release(Buffer* buffer);
void unref_buffer(Buffer* buffer);
void bind_shm(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_dmabuf(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
bool copy_shm_capture_buffer(zwayland::server::Resource* resource, const OutputCapture& capture,
                             std::uint32_t source_x, std::uint32_t source_y,
                             std::uint32_t width, std::uint32_t height);
bool initialize_xkb(SeatState* seat);
bool update_xkb(SeatState* seat, const KeyboardConfig& config);
void release_xkb(SeatState* seat);
void publish_keymap(SeatState* seat);
void remember_serial(SeatState& seat, zwayland::server::Client* client, std::uint32_t serial);
bool valid_selection_serial(const SeatState& seat, zwayland::server::Client* client, std::uint32_t serial);
void send_modifiers(SeatState& seat);
bool binding_matches(const Keybinding& binding, const SeatState& seat, std::uint32_t key);
bool binding_modifiers_match(const Keybinding& binding, const SeatState& seat);
bool execute_binding(const Keybinding& binding);
void apply_cursor_shape(SeatState* seat);
void sync_pointer_position(SeatState* seat);
std::vector<Rect> constraint_rects(const PointerConstraint* constraint);
bool confine_to_rects(const std::vector<Rect>& rects, std::int32_t old_x, std::int32_t old_y,
                      std::int32_t* x, std::int32_t* y);
void deactivate_constraint(PointerConstraint* constraint, bool send_event);
void update_pointer_constraints(SeatState* seat, std::int32_t x, std::int32_t y);
PointerConstraint* active_constraint(SeatState* seat);
void bind_cursor_shape_manager(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_relative_pointer_manager(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_idle_notifier(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void idle_activity(SeatState* seat);
void bind_pointer_constraints(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_seat(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void notify_surface(SurfaceState* surface);
void notify_surface_tree(SurfaceState* surface);
void configure_layout(Observer* observer, XdgSurfaceState* candidate = nullptr);
void end_interactive(SeatState* seat);
void set_pointer_focus(SeatState* seat, SurfaceState* next, std::int32_t x, std::int32_t y);
void set_keyboard_focus(SeatState* seat, SurfaceState* next);
OutputId surface_output(const SurfaceState* surface);
OutputState* output_state(Observer* observer, OutputId id);
void reorder_layer_roots(Observer* observer, OutputId output, std::uint32_t bucket);
void refresh_layer_keyboard_focus(Observer* observer);
void configure_layer_surface(LayerSurfaceState* layer, Rect area);
void associate_layer_popup(LayerSurfaceState* layer, zwayland::server::Client* client, zwayland::server::Resource* layer_resource,
                           zwayland::server::Resource* popup_resource);
void bind_layer_shell(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_xwlr_layer_shell(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_session_lock_manager(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void configure_session_lock_outputs(SessionLockState* lock);
void notify_session_lock_frame_presented(SessionLockState* lock, OutputId output);
void selection_focus_changed(SeatState* seat, zwayland::server::Client* old_client, zwayland::server::Client* new_client);
void bind_data_device_manager(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_data_control_manager(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void bind_wlr_data_control_manager(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
void send_source_data(DataSourceState* source, const char* mime, std::int32_t fd);
void send_selection(SeatState* seat, zwayland::server::Client* client);
void send_control_selection(SeatState* seat, bool primary);
void cancel_source(DataSourceState* source);
void drag_motion(SeatState* seat, std::uint32_t time, SurfaceState* target,
                 std::int32_t x, std::int32_t y);
void drag_button(SeatState* seat, std::uint32_t button, std::uint32_t state);
void cancel_drag(SeatState* seat);
void drag_surface_destroyed(SeatState* seat, SurfaceState* surface);

}  // namespace zwwm::detail
