#pragma once


#include <zwayland/server/display.hpp>
#include "compositor_server_internal.hpp"

#ifdef ZWWM_XWAYLAND

#include <xwayland-shell-zwayland-server.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <optional>

#include "zwwm/xwayland_spawn.hpp"

namespace zwwm::detail {

struct XwaylandRuntime;
struct XwaylandWindow {
  xcb_window_t id = XCB_WINDOW_NONE;
  std::uint64_t serial = 0;
  std::int16_t x = 0, y = 0;
  std::uint16_t width = 1, height = 1;
  Rect requested_bounds{0, 0, 1, 1};
  bool mapped = false;
  bool override_redirect = false;
  bool position_specified = false;
  bool supports_delete = false;
  bool modal = false;
  xcb_window_t transient_for = XCB_WINDOW_NONE;
  std::vector<xcb_atom_t> window_types;
  std::string window_role;
  std::int32_t min_width = 0, min_height = 0;
  std::int32_t max_width = 0, max_height = 0;
  std::int32_t base_width = 0, base_height = 0;
  std::string title;
  std::string app_id;
  XwaylandSurfaceState* surface = nullptr;
};
struct XwaylandSurfaceState {
  XwaylandRuntime* runtime = nullptr;
  SurfaceState* surface = nullptr;
  zwayland::server::Resource* resource = nullptr;
  std::uint64_t pending_serial = 0;
  std::uint64_t serial = 0;
  XwaylandWindow* window = nullptr;
  OutputId output;
  Rect tile_bounds{0, 0, 0, 0};
  Rect content_bounds{0, 0, 0, 0};
  Rect floating_bounds{0, 0, 0, 0};
  CanvasBounds canvas_bounds;
  std::uint8_t tag = 1;
  std::optional<bool> floating_override;
  bool floating = false;
  bool fullscreen = false;
  bool associated = false;
};
struct XwaylandSelection { XwaylandRuntime* runtime = nullptr; xcb_atom_t atom = XCB_ATOM_NONE; xcb_window_t window = XCB_WINDOW_NONE; xcb_window_t owner = XCB_WINDOW_NONE; xcb_timestamp_t timestamp = XCB_CURRENT_TIME; DataSourceState* source = nullptr; bool primary = false; bool notify_on_focus = false; };
struct XwaylandReceiveTransfer { XwaylandSelection* selection = nullptr; xcb_window_t window = XCB_WINDOW_NONE; int fd = -1; int event_source = -1; std::vector<std::uint8_t> data; std::size_t offset = 0; bool incremental = false; };
struct XwaylandSendTransfer { XwaylandSelection* selection = nullptr; xcb_selection_request_event_t request{}; int fd = -1; int event_source = -1; std::vector<std::uint8_t> data; };

struct XwaylandRuntime {
  zwayland::server::Display* display = nullptr; zwayland::server::EventLoop* loop = nullptr; std::uint32_t shell = 0; zwayland::server::Client* client = nullptr;
  int ready_source = -1; int xwm_source = -1; int restart_source = -1;
  zwayland::server::Listener display_destroy{}; zwayland::server::Listener client_destroy{}; XwaylandDisplay xdisplay; pid_t pid = -1;
  int ready_fd = -1; int wm_fd = -1; xcb_connection_t* connection = nullptr; xcb_screen_t* screen = nullptr;
  const xcb_query_extension_reply_t* xfixes = nullptr;
  xcb_atom_t wl_surface_serial = XCB_ATOM_NONE, net_active_window = XCB_ATOM_NONE, net_client_list = XCB_ATOM_NONE;
  xcb_atom_t net_supported = XCB_ATOM_NONE, net_supporting_wm_check = XCB_ATOM_NONE, net_wm_name = XCB_ATOM_NONE;
  xcb_atom_t utf8_string = XCB_ATOM_NONE, wm_name = XCB_ATOM_NONE, wm_class = XCB_ATOM_NONE;
  xcb_atom_t wm_protocols = XCB_ATOM_NONE, wm_take_focus = XCB_ATOM_NONE, wm_delete_window = XCB_ATOM_NONE;
  xcb_atom_t wm_transient_for = XCB_ATOM_NONE, net_wm_state = XCB_ATOM_NONE, net_wm_state_fullscreen = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_modal = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_maximized_vert = XCB_ATOM_NONE, net_wm_state_maximized_horz = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_hidden = XCB_ATOM_NONE, wm_s0 = XCB_ATOM_NONE, net_wm_cm_s0 = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type = XCB_ATOM_NONE, net_wm_window_type_normal = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_dialog = XCB_ATOM_NONE, net_wm_window_type_splash = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_toolbar = XCB_ATOM_NONE, net_wm_window_type_utility = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_tooltip = XCB_ATOM_NONE, net_wm_window_type_popup_menu = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_dock = XCB_ATOM_NONE, net_wm_window_type_dropdown_menu = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_menu = XCB_ATOM_NONE, kde_net_wm_window_type_override = XCB_ATOM_NONE;
  xcb_atom_t wm_window_role = XCB_ATOM_NONE, wm_normal_hints = XCB_ATOM_NONE, wm_size_hints = XCB_ATOM_NONE;
  xcb_atom_t clipboard_atom = XCB_ATOM_NONE, primary_atom = XCB_ATOM_NONE, targets_atom = XCB_ATOM_NONE;
  xcb_atom_t timestamp_atom = XCB_ATOM_NONE, wl_selection_atom = XCB_ATOM_NONE, text_atom = XCB_ATOM_NONE;
  xcb_atom_t incr_atom = XCB_ATOM_NONE; xcb_window_t wm_window = XCB_WINDOW_NONE; Observer* observer = nullptr;
  XwaylandSelection clipboard; XwaylandSelection primary;
  std::vector<XwaylandReceiveTransfer*> receives; std::vector<XwaylandSendTransfer*> sends;
  std::vector<XwaylandWindow*> windows; std::vector<XwaylandSurfaceState*> surfaces;

  static xcb_atom_t intern(xcb_connection_t* connection, const char* name);
  XwaylandWindow* window(xcb_window_t id) const;
  void publish_clients() const;
  void read_window_properties(XwaylandWindow* item);
  bool automatic_floating(const XwaylandWindow* item) const;
  bool reclassify(XwaylandWindow* item);
  void publish_state(XwaylandSurfaceState* role) const;
  void close_window(XwaylandWindow* item) const;
  void focus(XwaylandWindow* item);
  void apply_geometry(XwaylandWindow* item) const;
  void try_associate(XwaylandSurfaceState* role);
  void try_associate(XwaylandWindow* item);
  void remove_role(XwaylandSurfaceState* role);
  void process_event(xcb_generic_event_t* generic);
  int dispatch(std::uint32_t mask);
  void reset_server();
  bool initialize_xwm();
  bool start();
  static void surface_resource_destroyed(zwayland::server::Resource* resource);
  static void get_xwayland_surface(zwayland::server::Client*, zwayland::server::Resource*, std::uint32_t, zwayland::server::Resource*);
  static void bind_shell(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
  XwaylandRuntime(zwayland::server::Display* display, zwayland::server::EventLoop* loop, Observer* observer);
  ~XwaylandRuntime();
};

extern XwaylandRuntime* active_xwayland;
bool handle_xwayland_selection_event(XwaylandRuntime* runtime, xcb_generic_event_t* event);
void xwayland_receive_selection(DataSourceState* source, const char* mime, int fd);
void xwayland_wayland_selection_changed(SeatState* seat, bool primary);
void xwayland_keyboard_focus_changed(XwaylandRuntime* runtime);
void xwayland_reset_selections(XwaylandRuntime* runtime);
void focus_xwayland_surface(SurfaceState* surface);
void focus_xwayland_if_needed(SurfaceState* surface);
void set_xwayland_floating(XwaylandSurfaceState* role, bool floating);

}  // namespace zwwm::detail

#endif
