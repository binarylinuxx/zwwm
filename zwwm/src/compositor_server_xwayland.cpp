#include "compositor_server_xwayland_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace protocol = zwayland::generated;

namespace zwwm::detail {

XwaylandRuntime* active_xwayland = nullptr;

xcb_atom_t XwaylandRuntime::intern(xcb_connection_t* connection, const char* name) {
    const auto cookie = xcb_intern_atom(connection, 0, std::strlen(name), name);
    auto* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    const xcb_atom_t atom = reply == nullptr ? static_cast<xcb_atom_t>(XCB_ATOM_NONE) : reply->atom;
    std::free(reply);
    return atom;
  }

XwaylandWindow* XwaylandRuntime::window(xcb_window_t id) const {
    const auto found = std::find_if(windows.begin(), windows.end(),
                                    [id](const auto* candidate) { return candidate->id == id; });
    return found == windows.end() ? nullptr : *found;
  }

void XwaylandRuntime::publish_clients() const {
    if (connection == nullptr || screen == nullptr || net_client_list == XCB_ATOM_NONE) return;
    std::vector<xcb_window_t> mapped;
    for (const auto* candidate : windows) if (candidate->mapped) mapped.push_back(candidate->id);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root, net_client_list,
                        XCB_ATOM_WINDOW, 32, mapped.size(), mapped.data());
  }

void XwaylandRuntime::read_window_properties(XwaylandWindow* item) {
    if (item == nullptr || connection == nullptr) return;
    const auto read_string = [&](xcb_atom_t atom, xcb_atom_t type) {
      const auto cookie = xcb_get_property(connection, 0, item->id, atom, type, 0, 1024);
      auto* reply = xcb_get_property_reply(connection, cookie, nullptr);
      std::string value;
      if (reply != nullptr && xcb_get_property_value_length(reply) > 0)
        value.assign(static_cast<const char*>(xcb_get_property_value(reply)),
                     static_cast<std::size_t>(xcb_get_property_value_length(reply)));
      std::free(reply);
      return value;
    };
    auto title = read_string(net_wm_name, utf8_string);
    if (title.empty()) title = read_string(wm_name, XCB_GET_PROPERTY_TYPE_ANY);
    if (!title.empty()) item->title = std::move(title);
    auto window_class = read_string(wm_class, XCB_GET_PROPERTY_TYPE_ANY);
    if (!window_class.empty()) {
      const auto separator = window_class.find('\0');
      if (separator != std::string::npos && separator + 1 < window_class.size()) {
        const auto end = window_class.find('\0', separator + 1);
        item->app_id = window_class.substr(separator + 1, end - separator - 1);
      }
      else item->app_id = std::move(window_class);
    }
    const auto protocols_cookie = xcb_get_property(connection, 0, item->id, wm_protocols,
                                                   XCB_ATOM_ATOM, 0, 64);
    auto* protocols = xcb_get_property_reply(connection, protocols_cookie, nullptr);
    item->supports_delete = false;
    if (protocols != nullptr && protocols->type == XCB_ATOM_ATOM) {
      const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(protocols));
      for (std::uint32_t index = 0; index < protocols->value_len; ++index)
        if (atoms[index] == wm_delete_window) item->supports_delete = true;
    }
    std::free(protocols);
    const auto transient_cookie = xcb_get_property(connection, 0, item->id, wm_transient_for,
                                                   XCB_ATOM_WINDOW, 0, 1);
    auto* transient = xcb_get_property_reply(connection, transient_cookie, nullptr);
    item->transient_for = transient != nullptr && transient->type == XCB_ATOM_WINDOW && transient->value_len == 1 ?
        *static_cast<const xcb_window_t*>(xcb_get_property_value(transient)) : static_cast<xcb_window_t>(XCB_WINDOW_NONE);
    std::free(transient);
    const auto types_cookie = xcb_get_property(connection, 0, item->id, net_wm_window_type,
                                               XCB_ATOM_ATOM, 0, 64);
    auto* types = xcb_get_property_reply(connection, types_cookie, nullptr);
    item->window_types.clear();
    if (types != nullptr && types->type == XCB_ATOM_ATOM) {
      const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(types));
      item->window_types.assign(atoms, atoms + types->value_len);
    }
    std::free(types);
    item->window_role = read_string(wm_window_role, XCB_GET_PROPERTY_TYPE_ANY);

    const auto state_cookie = xcb_get_property(connection, 0, item->id, net_wm_state,
                                               XCB_ATOM_ATOM, 0, 64);
    auto* state = xcb_get_property_reply(connection, state_cookie, nullptr);
    item->modal = false;
    bool fullscreen = false;
    if (state != nullptr && state->type == XCB_ATOM_ATOM) {
      const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(state));
      for (std::uint32_t index = 0; index < state->value_len; ++index) {
        item->modal |= atoms[index] == net_wm_state_modal;
        fullscreen |= atoms[index] == net_wm_state_fullscreen;
      }
    }
    std::free(state);
    if (item->surface != nullptr) item->surface->fullscreen = fullscreen;

    item->min_width = item->min_height = item->max_width = item->max_height = 0;
    item->base_width = item->base_height = 0;
    const auto hints_cookie = xcb_get_property(connection, 0, item->id, wm_normal_hints,
                                               wm_size_hints, 0, 18);
    auto* hints = xcb_get_property_reply(connection, hints_cookie, nullptr);
    if (hints != nullptr && hints->format == 32 && hints->value_len >= 9) {
      const auto* values = static_cast<const std::uint32_t*>(xcb_get_property_value(hints));
      constexpr std::uint32_t user_position = 1U << 0U;
      constexpr std::uint32_t program_position = 1U << 2U;
      constexpr std::uint32_t min_size = 1U << 4U;
      constexpr std::uint32_t max_size = 1U << 5U;
      constexpr std::uint32_t base_size = 1U << 8U;
      item->position_specified |= (values[0] & (user_position | program_position)) != 0;
      if ((values[0] & min_size) != 0) {
        item->min_width = static_cast<std::int32_t>(values[5]);
        item->min_height = static_cast<std::int32_t>(values[6]);
      }
      if ((values[0] & max_size) != 0) {
        item->max_width = static_cast<std::int32_t>(values[7]);
        item->max_height = static_cast<std::int32_t>(values[8]);
      }
      if ((values[0] & base_size) != 0 && hints->value_len >= 17) {
        item->base_width = static_cast<std::int32_t>(values[15]);
        item->base_height = static_cast<std::int32_t>(values[16]);
      }
    }
    std::free(hints);
  }

bool XwaylandRuntime::automatic_floating(const XwaylandWindow* item) const {
    if (item == nullptr) return false;
    const std::array floating_types{
        net_wm_window_type_dialog, net_wm_window_type_splash, net_wm_window_type_toolbar,
        net_wm_window_type_utility, net_wm_window_type_tooltip, net_wm_window_type_popup_menu,
        net_wm_window_type_dock, net_wm_window_type_dropdown_menu, net_wm_window_type_menu,
        kde_net_wm_window_type_override};
    const bool floating_type = std::any_of(item->window_types.begin(), item->window_types.end(),
        [&](xcb_atom_t type) { return std::find(floating_types.begin(), floating_types.end(), type) != floating_types.end(); });
    const bool floating_role = item->window_role.find("task_dialog") != std::string::npos ||
                               item->window_role.find("pop-up") != std::string::npos;
    const bool fixed_size = item->min_width > 0 && item->min_height > 0 &&
                            item->min_width == item->max_width && item->min_height == item->max_height;
    return item->override_redirect || item->transient_for != XCB_WINDOW_NONE || item->modal ||
           floating_type || floating_role || fixed_size;
  }

bool XwaylandRuntime::reclassify(XwaylandWindow* item) {
    if (item == nullptr || item->surface == nullptr) return false;
    auto* role = item->surface;
    const bool floating = role->floating_override.value_or(automatic_floating(item));
    if (floating == role->floating) return false;
    if (floating && (item->override_redirect || item->position_specified ||
                      (item->requested_bounds.width > 1 && item->requested_bounds.height > 1)))
      role->floating_bounds = item->position_specified || item->override_redirect ? item->requested_bounds :
          Rect{0, 0, 0, 0};
    role->floating = floating;
    return true;
  }

void XwaylandRuntime::publish_state(XwaylandSurfaceState* role) const {
    if (role == nullptr || role->window == nullptr || connection == nullptr) return;
    std::vector<xcb_atom_t> states;
    if (role->window->modal) states.push_back(net_wm_state_modal);
    if (role->fullscreen) states.push_back(net_wm_state_fullscreen);
    if (!role->floating && !role->fullscreen) {
      states.push_back(net_wm_state_maximized_vert);
      states.push_back(net_wm_state_maximized_horz);
    }
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, role->window->id, net_wm_state,
                        XCB_ATOM_ATOM, 32, states.size(), states.data());
  }

void XwaylandRuntime::close_window(XwaylandWindow* item) const {
    if (item == nullptr || connection == nullptr) return;
    if (!item->supports_delete) {
      xcb_destroy_window(connection, item->id);
      xcb_flush(connection);
      return;
    }
    xcb_client_message_event_t message{};
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = item->id;
    message.type = wm_protocols;
    message.data.data32[0] = wm_delete_window;
    message.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(connection, 0, item->id, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char*>(&message));
    xcb_flush(connection);
  }

void XwaylandRuntime::focus(XwaylandWindow* item) {
    if (connection == nullptr || screen == nullptr) return;
    const xcb_window_t id = item == nullptr ? static_cast<xcb_window_t>(XCB_WINDOW_NONE) : item->id;
    if (item != nullptr) {
      xcb_client_message_event_t message{};
      message.response_type = XCB_CLIENT_MESSAGE;
      message.format = 32;
      message.window = item->id;
      message.type = wm_protocols;
      message.data.data32[0] = wm_take_focus;
      message.data.data32[1] = XCB_CURRENT_TIME;
      xcb_send_event(connection, 0, item->id, XCB_EVENT_MASK_NO_EVENT,
                     reinterpret_cast<const char*>(&message));
    }
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, id, XCB_CURRENT_TIME);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root, net_active_window,
                        XCB_ATOM_WINDOW, 32, 1, &id);
    if (item != nullptr) xwayland_keyboard_focus_changed(this);
    xcb_flush(connection);
  }

void XwaylandRuntime::apply_geometry(XwaylandWindow* item) const {
    if (item == nullptr || item->surface == nullptr || item->surface->surface == nullptr) return;
    auto* surface = item->surface->surface;
    surface->x = item->x;
    surface->y = item->y;
    item->surface->output = surface->observer == nullptr ? OutputId{} : surface->observer->active_output;
    notify_surface(surface);
  }

void XwaylandRuntime::try_associate(XwaylandSurfaceState* role) {
    if (role == nullptr || role->associated || role->serial == 0) return;
    const auto found = std::find_if(windows.begin(), windows.end(),
                                    [role](const auto* item) { return item->serial == role->serial; });
    if (found == windows.end()) return;
    auto* item = *found;
    if (item->surface != nullptr) return;
    item->surface = role;
    role->window = item;
    role->associated = true;
    read_window_properties(item);
    role->floating = role->floating_override.value_or(automatic_floating(item));
    if (role->floating && (item->override_redirect || item->position_specified ||
                            (item->requested_bounds.width > 1 && item->requested_bounds.height > 1)))
      role->floating_bounds = item->position_specified || item->override_redirect ? item->requested_bounds :
          Rect{0, 0, 0, 0};
    if (role->surface != nullptr && role->surface->observer != nullptr)
      role->output = role->surface->observer->active_output;
    if (role->surface != nullptr && role->surface->observer != nullptr &&
        role->surface->observer->outputs != nullptr) {
      const auto output = std::find_if(role->surface->observer->outputs->begin(),
          role->surface->observer->outputs->end(), [role](const auto& candidate) {
            return candidate->info.id == role->output;
          });
      if (output != role->surface->observer->outputs->end()) role->tag = (*output)->active_tag;
    }
    apply_geometry(item);
    if (item->mapped && role->surface != nullptr)
      configure_layout(role->surface->observer, nullptr);
  }

void XwaylandRuntime::try_associate(XwaylandWindow* item) {
    if (item == nullptr || item->serial == 0 || item->surface != nullptr) return;
    for (auto* role : surfaces) {
      if (role->serial == item->serial && !role->associated) {
        try_associate(role);
        return;
      }
    }
  }

void XwaylandRuntime::remove_role(XwaylandSurfaceState* role) {
    if (role == nullptr) return;
    if (role->window != nullptr) role->window->surface = nullptr;
    std::erase(surfaces, role);
    delete role;
  }

void XwaylandRuntime::process_event(xcb_generic_event_t* generic) {
    if (handle_xwayland_selection_event(this, generic)) return;
    const auto type = generic->response_type & 0x7fU;
    switch (type) {
      case XCB_CREATE_NOTIFY: {
        const auto* event = reinterpret_cast<xcb_create_notify_event_t*>(generic);
        if (screen == nullptr || event->window == screen->root || window(event->window) != nullptr) break;
        auto* item = new (std::nothrow) XwaylandWindow;
        if (item == nullptr) break;
        item->id = event->window; item->x = event->x; item->y = event->y;
        item->width = event->width; item->height = event->height;
        item->requested_bounds = {event->x, event->y, event->width, event->height};
        item->override_redirect = event->override_redirect != 0;
        windows.push_back(item);
        const std::uint32_t window_mask = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(connection, item->id, XCB_CW_EVENT_MASK, &window_mask);
        read_window_properties(item);
        break;
      }
      case XCB_DESTROY_NOTIFY: {
        const auto* event = reinterpret_cast<xcb_destroy_notify_event_t*>(generic);
        auto* item = window(event->window);
        if (item == nullptr) break;
        if (item->surface != nullptr) {
          item->surface->window = nullptr;
          item->surface->associated = false;
          notify_surface(item->surface->surface);
        }
        std::erase(windows, item);
        delete item;
        publish_clients();
        break;
      }
      case XCB_MAP_REQUEST: {
        const auto* event = reinterpret_cast<xcb_map_request_event_t*>(generic);
        if (auto* item = window(event->window); item != nullptr) {
          read_window_properties(item);
          (void)reclassify(item);
        }
        xcb_map_window(connection, event->window);
        break;
      }
      case XCB_MAP_NOTIFY: {
        const auto* event = reinterpret_cast<xcb_map_notify_event_t*>(generic);
        if (auto* item = window(event->window); item != nullptr) {
          item->override_redirect = event->override_redirect != 0;
          read_window_properties(item);
          (void)reclassify(item);
          const bool first_map = !item->ever_mapped;
          item->mapped = true;
          item->ever_mapped = true;
          apply_geometry(item);
          if (item->surface != nullptr && item->surface->surface != nullptr) {
            configure_layout(item->surface->surface->observer, nullptr);
            publish_state(item->surface);
            if (first_map && !item->override_redirect && item->transient_for == XCB_WINDOW_NONE && !item->modal)
              recenter_canvas_on_surface(item->surface->surface->observer, item->surface->surface);
            if (!item->override_redirect) focus_xwayland_if_needed(item->surface->surface);
          }
          publish_clients();
        }
        break;
      }
      case XCB_UNMAP_NOTIFY: {
        const auto* event = reinterpret_cast<xcb_unmap_notify_event_t*>(generic);
        if (auto* item = window(event->window); item != nullptr) {
          item->mapped = false;
          if (item->surface != nullptr) notify_surface(item->surface->surface);
          if (item->surface != nullptr && item->surface->surface != nullptr)
            configure_layout(item->surface->surface->observer, nullptr);
          publish_clients();
        }
        break;
      }
      case XCB_CONFIGURE_REQUEST: {
        const auto* event = reinterpret_cast<xcb_configure_request_event_t*>(generic);
        auto* item = window(event->window);
        auto* role = item == nullptr ? nullptr : item->surface;
        if (item != nullptr) {
          if ((event->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)) != 0)
            item->position_specified = true;
          if ((event->value_mask & XCB_CONFIG_WINDOW_X) != 0) item->requested_bounds.x = event->x;
          if ((event->value_mask & XCB_CONFIG_WINDOW_Y) != 0) item->requested_bounds.y = event->y;
          if ((event->value_mask & XCB_CONFIG_WINDOW_WIDTH) != 0) item->requested_bounds.width = event->width;
          if ((event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) != 0) item->requested_bounds.height = event->height;
          const bool was_fullscreen = role != nullptr && role->fullscreen;
          read_window_properties(item);
          const bool classification_changed = reclassify(item);
          if (item->mapped && role != nullptr && role->surface != nullptr &&
              (classification_changed || was_fullscreen != role->fullscreen)) {
            publish_state(role);
            configure_layout(role->surface->observer, nullptr);
          }
        }
        if (item != nullptr && item->mapped && !item->override_redirect && role != nullptr &&
            (!role->floating || role->fullscreen)) {
          const std::array<std::uint32_t, 4> values{
              static_cast<std::uint32_t>(role->content_bounds.x),
              static_cast<std::uint32_t>(role->content_bounds.y),
              static_cast<std::uint32_t>(role->content_bounds.width),
              static_cast<std::uint32_t>(role->content_bounds.height)};
          xcb_configure_window(connection, event->window,
              XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
              values.data());
          break;
        }
        std::uint32_t values[7];
        std::size_t count = 0;
        if ((event->value_mask & XCB_CONFIG_WINDOW_X) != 0) values[count++] = event->x;
        if ((event->value_mask & XCB_CONFIG_WINDOW_Y) != 0) values[count++] = event->y;
        if ((event->value_mask & XCB_CONFIG_WINDOW_WIDTH) != 0) values[count++] = event->width;
        if ((event->value_mask & XCB_CONFIG_WINDOW_HEIGHT) != 0) values[count++] = event->height;
        if ((event->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) != 0) values[count++] = event->border_width;
        if ((event->value_mask & XCB_CONFIG_WINDOW_SIBLING) != 0) values[count++] = event->sibling;
        if ((event->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) != 0) values[count++] = event->stack_mode;
        xcb_configure_window(connection, event->window, event->value_mask, values);
        break;
      }
      case XCB_CONFIGURE_NOTIFY: {
        const auto* event = reinterpret_cast<xcb_configure_notify_event_t*>(generic);
        if (auto* item = window(event->window); item != nullptr) {
          item->x = event->x; item->y = event->y; item->width = event->width; item->height = event->height;
          item->override_redirect = event->override_redirect != 0;
          const bool was_fullscreen = item->surface != nullptr && item->surface->fullscreen;
          read_window_properties(item);
          const bool classification_changed = reclassify(item);
          bool floating_geometry_changed = false;
          if (item->mapped && item->surface != nullptr && item->surface->floating && !item->surface->fullscreen) {
            const Rect requested{item->x, item->y, item->width, item->height};
            if (requested.width > 0 && requested.height > 0 &&
                (requested.x != item->surface->content_bounds.x || requested.y != item->surface->content_bounds.y ||
                 requested.width != item->surface->content_bounds.width ||
                 requested.height != item->surface->content_bounds.height)) {
              item->surface->floating_bounds = requested;
              floating_geometry_changed = true;
            }
          }
          apply_geometry(item);
          if (item->mapped && item->surface != nullptr && item->surface->surface != nullptr &&
              (classification_changed || floating_geometry_changed || was_fullscreen != item->surface->fullscreen)) {
            publish_state(item->surface);
            configure_layout(item->surface->surface->observer, nullptr);
          }
        }
        break;
      }
      case XCB_PROPERTY_NOTIFY: {
        const auto* event = reinterpret_cast<xcb_property_notify_event_t*>(generic);
        if (auto* item = window(event->window); item != nullptr) {
          const bool was_fullscreen = item->surface != nullptr && item->surface->fullscreen;
          read_window_properties(item);
          const bool classification_changed = reclassify(item);
          if (item->mapped && item->surface != nullptr && item->surface->surface != nullptr &&
              (classification_changed || was_fullscreen != item->surface->fullscreen)) {
            publish_state(item->surface);
            configure_layout(item->surface->surface->observer, nullptr);
          }
          if (item->surface != nullptr && item->surface->surface != nullptr)
            notify_surface(item->surface->surface);
        }
        break;
      }
      case XCB_CLIENT_MESSAGE: {
        const auto* event = reinterpret_cast<xcb_client_message_event_t*>(generic);
        auto* item = window(event->window);
        if (item == nullptr) break;
        if (event->type == wl_surface_serial) {
          item->serial = static_cast<std::uint64_t>(event->data.data32[0]) |
                         (static_cast<std::uint64_t>(event->data.data32[1]) << 32U);
          try_associate(item);
        } else if (event->type == net_active_window && item->surface != nullptr &&
                    item->surface->surface != nullptr) {
          auto* seat = item->surface->surface->observer->seat;
          if (seat->keyboard_focus == nullptr)
            set_keyboard_focus(seat, item->surface->surface);
        } else if (event->type == net_wm_state && item->surface != nullptr && event->format == 32) {
          auto* role = item->surface;
          const auto update = [event](xcb_atom_t atom, bool current) {
            if (event->data.data32[1] != atom && event->data.data32[2] != atom) return current;
            if (event->data.data32[0] == 0) return false;
            if (event->data.data32[0] == 1) return true;
            return !current;
          };
          role->fullscreen = update(net_wm_state_fullscreen, role->fullscreen);
          item->modal = update(net_wm_state_modal, item->modal);
          (void)reclassify(item);
          if (event->data.data32[1] == net_wm_state_hidden || event->data.data32[2] == net_wm_state_hidden)
            item->mapped = event->data.data32[0] == 0;
          publish_state(role);
          configure_layout(role->surface->observer, nullptr);
        }
        break;
      }
      default: break;
    }
  }

int XwaylandRuntime::dispatch(std::uint32_t mask) {
    if ((mask & (EPOLLHUP | EPOLLERR)) != 0) {
      if (restart_source < 0) {
        restart_source = loop->add_idle([this] {
          restart_source = -1;
          reset_server();
          (void)start();
        });
      }
      return 0;
    }
    while (auto* event = xcb_poll_for_event(connection)) {
      process_event(event);
      std::free(event);
    }
    xcb_flush(connection);
    return 0;
  }

void XwaylandRuntime::reset_server() {
    if (xwm_source >= 0) { loop->remove(xwm_source); xwm_source = -1; }
    if (ready_source >= 0) { loop->remove(ready_source); ready_source = -1; }
    xwayland_reset_selections(this);
    for (auto* transfer : receives) {
      if (transfer->event_source >= 0) loop->remove(transfer->event_source);
      if (transfer->fd >= 0) ::close(transfer->fd);
      delete transfer;
    }
    receives.clear();
    for (auto* transfer : sends) {
      if (transfer->event_source >= 0) loop->remove(transfer->event_source);
      if (transfer->fd >= 0) ::close(transfer->fd);
      delete transfer;
    }
    sends.clear();
    for (auto* item : windows) {
      if (item->surface != nullptr) {
        item->surface->window = nullptr;
        item->surface->associated = false;
      }
      delete item;
    }
    windows.clear();
    if (connection != nullptr) { xcb_disconnect(connection); connection = nullptr; }
    else if (wm_fd >= 0) ::close(wm_fd);
    wm_fd = -1;
    screen = nullptr;
    xfixes = nullptr;
    if (ready_fd >= 0) ::close(ready_fd);
    ready_fd = -1;
    if (client != nullptr) {
      auto* old_client = client;
      client = nullptr;
      old_client->destroy();
    }
    surfaces.clear();
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, WNOHANG);
    }
    pid = -1;
    destroy_xwayland_display(&xdisplay);
  }

bool XwaylandRuntime::initialize_xwm() {
    connection = xcb_connect_to_fd(wm_fd, nullptr);
    wm_fd = -1;
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) return false;
    const auto setup = xcb_get_setup(connection);
    auto iterator = xcb_setup_roots_iterator(setup);
    if (iterator.rem == 0) return false;
    screen = iterator.data;
    const std::uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                               XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                               XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection, screen->root, XCB_CW_EVENT_MASK, &mask);
    xcb_composite_redirect_subwindows(connection, screen->root, XCB_COMPOSITE_REDIRECT_MANUAL);
    wl_surface_serial = intern(connection, "WL_SURFACE_SERIAL");
    net_active_window = intern(connection, "_NET_ACTIVE_WINDOW");
    net_client_list = intern(connection, "_NET_CLIENT_LIST");
    net_supported = intern(connection, "_NET_SUPPORTED");
    net_supporting_wm_check = intern(connection, "_NET_SUPPORTING_WM_CHECK");
    net_wm_name = intern(connection, "_NET_WM_NAME");
    utf8_string = intern(connection, "UTF8_STRING");
    wm_name = intern(connection, "WM_NAME");
    wm_class = intern(connection, "WM_CLASS");
    wm_protocols = intern(connection, "WM_PROTOCOLS");
    wm_take_focus = intern(connection, "WM_TAKE_FOCUS");
    wm_delete_window = intern(connection, "WM_DELETE_WINDOW");
    wm_transient_for = intern(connection, "WM_TRANSIENT_FOR");
    net_wm_state = intern(connection, "_NET_WM_STATE");
    net_wm_state_modal = intern(connection, "_NET_WM_STATE_MODAL");
    net_wm_state_fullscreen = intern(connection, "_NET_WM_STATE_FULLSCREEN");
    net_wm_state_maximized_vert = intern(connection, "_NET_WM_STATE_MAXIMIZED_VERT");
    net_wm_state_maximized_horz = intern(connection, "_NET_WM_STATE_MAXIMIZED_HORZ");
    net_wm_state_hidden = intern(connection, "_NET_WM_STATE_HIDDEN");
    net_wm_window_type = intern(connection, "_NET_WM_WINDOW_TYPE");
    net_wm_window_type_normal = intern(connection, "_NET_WM_WINDOW_TYPE_NORMAL");
    net_wm_window_type_dialog = intern(connection, "_NET_WM_WINDOW_TYPE_DIALOG");
    net_wm_window_type_splash = intern(connection, "_NET_WM_WINDOW_TYPE_SPLASH");
    net_wm_window_type_toolbar = intern(connection, "_NET_WM_WINDOW_TYPE_TOOLBAR");
    net_wm_window_type_utility = intern(connection, "_NET_WM_WINDOW_TYPE_UTILITY");
    net_wm_window_type_tooltip = intern(connection, "_NET_WM_WINDOW_TYPE_TOOLTIP");
    net_wm_window_type_popup_menu = intern(connection, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
    net_wm_window_type_dock = intern(connection, "_NET_WM_WINDOW_TYPE_DOCK");
    net_wm_window_type_dropdown_menu = intern(connection, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
    net_wm_window_type_menu = intern(connection, "_NET_WM_WINDOW_TYPE_MENU");
    kde_net_wm_window_type_override = intern(connection, "_KDE_NET_WM_WINDOW_TYPE_OVERRIDE");
    wm_window_role = intern(connection, "WM_WINDOW_ROLE");
    wm_normal_hints = intern(connection, "WM_NORMAL_HINTS");
    wm_size_hints = intern(connection, "WM_SIZE_HINTS");
    wm_s0 = intern(connection, "WM_S0");
    net_wm_cm_s0 = intern(connection, "_NET_WM_CM_S0");
    clipboard_atom = intern(connection, "CLIPBOARD");
    primary_atom = XCB_ATOM_PRIMARY;
    targets_atom = intern(connection, "TARGETS");
    timestamp_atom = intern(connection, "TIMESTAMP");
    wl_selection_atom = intern(connection, "_WL_SELECTION");
    text_atom = intern(connection, "TEXT");
    incr_atom = intern(connection, "INCR");
    wm_window = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, wm_window, screen->root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    const std::array supported{
        net_active_window, net_client_list, net_supporting_wm_check, net_wm_state,
        net_wm_state_modal, net_wm_state_fullscreen, net_wm_state_maximized_vert,
        net_wm_state_maximized_horz, net_wm_state_hidden, net_wm_window_type,
        net_wm_window_type_normal, net_wm_window_type_dialog, net_wm_window_type_splash,
        net_wm_window_type_toolbar, net_wm_window_type_utility, net_wm_window_type_tooltip,
        net_wm_window_type_popup_menu, net_wm_window_type_dock, net_wm_window_type_dropdown_menu,
        net_wm_window_type_menu, kde_net_wm_window_type_override};
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root, net_supported,
                        XCB_ATOM_ATOM, 32, supported.size(), supported.data());
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root, net_supporting_wm_check,
                        XCB_ATOM_WINDOW, 32, 1, &wm_window);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, wm_window, net_supporting_wm_check,
                        XCB_ATOM_WINDOW, 32, 1, &wm_window);
    constexpr char name[] = "zwwm";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, wm_window, net_wm_name,
                        utf8_string, 8, sizeof(name) - 1, name);
    xcb_set_selection_owner(connection, wm_window, wm_s0, XCB_CURRENT_TIME);
    xcb_set_selection_owner(connection, wm_window, net_wm_cm_s0, XCB_CURRENT_TIME);
    const std::uint32_t selection_mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    const std::uint32_t xfixes_mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
    xfixes = xcb_get_extension_data(connection, &xcb_xfixes_id);
    if (xfixes != nullptr && xfixes->present != 0) {
      const auto version_cookie = xcb_xfixes_query_version(connection, XCB_XFIXES_MAJOR_VERSION,
                                                            XCB_XFIXES_MINOR_VERSION);
      std::free(xcb_xfixes_query_version_reply(connection, version_cookie, nullptr));
    }
    const auto create_selection = [&](XwaylandSelection* selection, xcb_atom_t atom, bool is_primary) {
      selection->runtime = this;
      selection->atom = atom;
      selection->primary = is_primary;
      selection->window = xcb_generate_id(connection);
      xcb_create_window(connection, XCB_COPY_FROM_PARENT, selection->window, screen->root,
                        0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                        XCB_CW_EVENT_MASK, &selection_mask);
      xcb_xfixes_select_selection_input(connection, selection->window, atom, xfixes_mask);
    };
    create_selection(&clipboard, clipboard_atom, false);
    create_selection(&primary, primary_atom, true);
    const int fd = xcb_get_file_descriptor(connection);
    try {
      xwm_source = loop->add_fd(fd, EPOLLIN | EPOLLHUP | EPOLLERR,
          [this](int, int event_mask) {
            dispatch(static_cast<std::uint32_t>(event_mask));
            return xwm_source >= 0;
          });
    } catch (const std::exception&) {
      xwm_source = -1;
    }
    xcb_flush(connection);
    return xwm_source >= 0;
  }

bool XwaylandRuntime::start() {
    const auto binary = find_xwayland_binary();
    if (binary.empty() || !create_xwayland_display(&xdisplay)) return false;
    int wayland_pair[2]{-1, -1}, wm_pair[2]{-1, -1}, ready_pair[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland_pair) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm_pair) != 0 ||
        ::pipe2(ready_pair, O_CLOEXEC) != 0) return false;
    client = display->add_client(wayland_pair[0]);
    if (client == nullptr) return false;
    client_destroy.notify = [](zwayland::server::Listener* listener, void*) {
      auto* runtime = reinterpret_cast<XwaylandRuntime*>(
          reinterpret_cast<char*>(listener) - offsetof(XwaylandRuntime, client_destroy));
      runtime->client = nullptr;
    };
    client->add_destroy_listener(client_destroy);
    wm_fd = wm_pair[0];
    ready_fd = ready_pair[0];
    pid = spawn_xwayland(binary, xdisplay, wayland_pair[1], wm_pair[1], ready_pair[1]);
    ::close(wayland_pair[1]); ::close(wm_pair[1]); ::close(ready_pair[1]);
    for (int& fd : xdisplay.listen_fds) { if (fd >= 0) ::close(fd); fd = -1; }
    if (pid < 0) return false;
    const std::string name = ":" + std::to_string(xdisplay.number);
    (void)::setenv("DISPLAY", name.c_str(), 1);
    try {
      ready_source = loop->add_fd(ready_fd, EPOLLIN | EPOLLHUP | EPOLLERR,
        [this](int fd, int mask) {
          if ((mask & EPOLLIN) == 0) return (mask & (EPOLLHUP | EPOLLERR)) == 0;
          char buffer[64];
          const auto count = ::read(fd, buffer, sizeof(buffer));
          if (count <= 0 || std::find(buffer, buffer + count, '\n') == buffer + count) return true;
          ready_source = -1;
          ::close(ready_fd);
          ready_fd = -1;
          (void)initialize_xwm();
          return false;
        });
    } catch (const std::exception&) {
      ready_source = -1;
    }
    return ready_source >= 0;
  }

void XwaylandRuntime::surface_resource_destroyed(zwayland::server::Resource* resource) {
    auto* role = resource->data<XwaylandSurfaceState>();
    if (role == nullptr) return;
    role->resource = nullptr;
    if (role->surface == nullptr) {
      if (role->runtime != nullptr) role->runtime->remove_role(role);
      else delete role;
    }
  }

void XwaylandRuntime::get_xwayland_surface(zwayland::server::Client* binding_client, zwayland::server::Resource* shell_resource,
                                   std::uint32_t id, zwayland::server::Resource* surface_resource) {
    auto* runtime = shell_resource->data<XwaylandRuntime>();
    auto* surface = surface_resource == nullptr || surface_resource->client != binding_client ? nullptr :
        surface_resource->data<SurfaceState>();
    if (runtime == nullptr || binding_client != runtime->client || surface == nullptr ||
        surface->xdg_surface != nullptr || surface->layer_role_assigned || surface->parent != nullptr || surface->drag_icon_role) {
      shell_resource->post_error(protocol::XWAYLAND_SHELL_V1_ERROR_ROLE,
                             "wl_surface already has a role");
      return;
    }
    auto* resource = binding_client->create_resource(&protocol::xwayland_surface_v1_interface, id, 1);
    auto* role = resource == nullptr ? nullptr : new (std::nothrow) XwaylandSurfaceState;
    if (resource == nullptr || role == nullptr) {
      if (resource != nullptr) resource->destroy();
      binding_client->post_no_memory();
      return;
    }
    role->runtime = runtime; role->surface = surface; role->resource = resource;
    surface->xwayland_surface = role;
    surface->layer_role_assigned = true;
    runtime->surfaces.push_back(role);
    struct XwaylandSurfaceV1ImplementationHandler {
  void set_serial(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t serial_lo, std::uint32_t serial_hi) {
    ([](zwayland::server::Client*, zwayland::server::Resource* role_resource, std::uint32_t serial_lo, std::uint32_t serial_hi) {
          auto* state = role_resource->data<XwaylandSurfaceState>();
          const auto serial = static_cast<std::uint64_t>(serial_lo) |
                              (static_cast<std::uint64_t>(serial_hi) << 32U);
          if (serial == 0) {
            role_resource->post_error(protocol::XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL,
                                   "xwayland surface serial must be non-zero");
            return;
          }
          if (state->serial != 0) {
            role_resource->post_error(protocol::XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED,
                                   "wl_surface is already associated with an X11 window");
            return;
          }
          state->serial = serial;
          if (state->runtime != nullptr) state->runtime->try_associate(state);
        })(&client, &resource, serial_lo, serial_hi);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* role_resource) { role_resource->destroy(); })(&client, &resource);
  }
};
    resource->set_data(role); resource->set_handler(protocol::xwayland_surface_v1_handler(XwaylandSurfaceV1ImplementationHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (surface_resource_destroyed)(&destroyed); });
  }

void XwaylandRuntime::bind_shell(zwayland::server::Client* binding_client, void* data, std::uint32_t version, std::uint32_t id) {
    auto* runtime = static_cast<XwaylandRuntime*>(data);
    if (runtime == nullptr || binding_client != runtime->client) {
      binding_client->post_error(0, "xwayland_shell_v1 is restricted to the compositor-owned Xwayland client");
      return;
    }
    auto* resource = binding_client->create_resource(&protocol::xwayland_shell_v1_interface, id, std::min(version, 1U));
    if (resource == nullptr) { binding_client->post_no_memory(); return; }
    struct XwaylandShellV1ImplementationHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* shell_resource) { shell_resource->destroy(); })(&client, &resource);
  }
  void get_xwayland_surface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface) {
    (XwaylandRuntime::get_xwayland_surface)(&client, &resource, id, surface);
  }
};
    resource->set_data(runtime); resource->set_handler(protocol::xwayland_shell_v1_handler(XwaylandShellV1ImplementationHandler{}));
  }

XwaylandRuntime::XwaylandRuntime(zwayland::server::Display* display_value, zwayland::server::EventLoop* loop_value, Observer* observer_value)
      : display(display_value), loop(loop_value), observer(observer_value) {
    active_xwayland = this;
    display_destroy.notify = [](zwayland::server::Listener* listener, void*) {
      auto* runtime = reinterpret_cast<XwaylandRuntime*>(
          reinterpret_cast<char*>(listener) - offsetof(XwaylandRuntime, display_destroy));
      delete runtime;
    };
    display->destroy_signal.add(display_destroy);
    shell = display->add_global(&protocol::xwayland_shell_v1_interface, 1, [data = this](zwayland::server::Client& client, std::uint32_t bound_version, std::uint32_t id) { (bind_shell)(&client, data, bound_version, id); });
    if (shell == 0 || !start()) {
      if (shell != 0) { display->destroy_global(shell); shell = 0; }
    }
  }

XwaylandRuntime::~XwaylandRuntime() {
    if (active_xwayland == this) active_xwayland = nullptr;
    display->destroy_signal.remove(display_destroy);
    if (restart_source >= 0) { loop->remove(restart_source); restart_source = -1; }
    reset_server();
    if (shell != 0) display->destroy_global(shell);
}

void focus_xwayland_surface(SurfaceState* surface) {
  if (active_xwayland == nullptr) return;
  while (surface != nullptr && surface->parent != nullptr) surface = surface->parent;
  auto* role = surface == nullptr ? nullptr : surface->xwayland_surface;
  active_xwayland->focus(role == nullptr ? nullptr : role->window);
}

void set_xwayland_floating(XwaylandSurfaceState* role, bool floating) {
  if (role == nullptr || role->floating == floating) return;
  if (floating && role->tile_bounds.width > 0 && role->tile_bounds.height > 0)
    role->floating_bounds = role->tile_bounds;
  role->floating_override = floating;
  role->floating = floating;
}

}  // namespace zwwm::detail
