#include "compositor_server_xwayland_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <new>
#include <sys/epoll.h>
#include <unistd.h>

namespace zwwm::detail {

XwaylandSelection* xwayland_selection_for(XwaylandRuntime* runtime, xcb_atom_t atom) {
  if (runtime == nullptr) return nullptr;
  if (atom == runtime->clipboard_atom) return &runtime->clipboard;
  if (atom == runtime->primary_atom) return &runtime->primary;
  return nullptr;
}

xcb_atom_t xwayland_mime_atom(XwaylandRuntime* runtime, const std::string& mime) {
  if (mime == "text/plain;charset=utf-8") return runtime->utf8_string;
  if (mime == "text/plain") return runtime->text_atom;
  return XwaylandRuntime::intern(runtime->connection, mime.c_str());
}

std::string xwayland_atom_mime(XwaylandRuntime* runtime, xcb_atom_t atom) {
  if (atom == runtime->utf8_string) return "text/plain;charset=utf-8";
  if (atom == runtime->text_atom) return "text/plain";
  const auto cookie = xcb_get_atom_name(runtime->connection, atom);
  auto* reply = xcb_get_atom_name_reply(runtime->connection, cookie, nullptr);
  std::string result;
  if (reply != nullptr)
    result.assign(xcb_get_atom_name_name(reply), static_cast<std::size_t>(xcb_get_atom_name_name_length(reply)));
  std::free(reply);
  return result;
}

void xwayland_send_notify(XwaylandRuntime* runtime, const xcb_selection_request_event_t& request, bool success) {
  xcb_selection_notify_event_t notify{};
  notify.response_type = XCB_SELECTION_NOTIFY;
  notify.time = request.time;
  notify.requestor = request.requestor;
  notify.selection = request.selection;
  notify.target = request.target;
  notify.property = success ? (request.property == XCB_ATOM_NONE ? request.target : request.property) :
                              static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  xcb_send_event(runtime->connection, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                 reinterpret_cast<const char*>(&notify));
  xcb_flush(runtime->connection);
}

void remove_receive(XwaylandRuntime* runtime, XwaylandReceiveTransfer* transfer) {
  if (transfer->event_source >= 0) runtime->loop->remove(transfer->event_source);
  if (transfer->fd >= 0) ::close(transfer->fd);
  if (transfer->window != XCB_WINDOW_NONE) xcb_destroy_window(runtime->connection, transfer->window);
  std::erase(runtime->receives, transfer);
  delete transfer;
}

int flush_receive(int, std::uint32_t, void* data) {
  auto* transfer = static_cast<XwaylandReceiveTransfer*>(data);
  auto* runtime = transfer->selection->runtime;
  while (transfer->offset < transfer->data.size()) {
    const auto count = ::write(transfer->fd, transfer->data.data() + transfer->offset,
                               transfer->data.size() - transfer->offset);
    if (count > 0) { transfer->offset += static_cast<std::size_t>(count); continue; }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 1;
    remove_receive(runtime, transfer);
    return 0;
  }
  transfer->data.clear();
  transfer->offset = 0;
  if (!transfer->incremental) remove_receive(runtime, transfer);
  else {
    if (transfer->event_source >= 0) { runtime->loop->remove(transfer->event_source); transfer->event_source = -1; }
    xcb_delete_property(runtime->connection, transfer->window, runtime->wl_selection_atom);
    xcb_flush(runtime->connection);
  }
  return 0;
}

void write_receive_property(XwaylandReceiveTransfer* transfer) {
  auto* runtime = transfer->selection->runtime;
  const auto cookie = xcb_get_property(runtime->connection, 1, transfer->window,
                                       runtime->wl_selection_atom, XCB_GET_PROPERTY_TYPE_ANY,
                                       0, 0x1fffffff);
  auto* reply = xcb_get_property_reply(runtime->connection, cookie, nullptr);
  if (reply == nullptr) { remove_receive(runtime, transfer); return; }
  if (!transfer->incremental && reply->type == runtime->incr_atom) {
    transfer->incremental = true;
    std::free(reply);
    xcb_delete_property(runtime->connection, transfer->window, runtime->wl_selection_atom);
    xcb_flush(runtime->connection);
    return;
  }
  const auto length = static_cast<std::size_t>(xcb_get_property_value_length(reply));
  if (transfer->incremental && length == 0) {
    std::free(reply);
    remove_receive(runtime, transfer);
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(xcb_get_property_value(reply));
  transfer->data.assign(bytes, bytes + length);
  std::free(reply);
  const int flags = ::fcntl(transfer->fd, F_GETFL);
  if (flags >= 0) (void)::fcntl(transfer->fd, F_SETFL, flags | O_NONBLOCK);
  flush_receive(transfer->fd, EPOLLOUT, transfer);
  if (std::find(runtime->receives.begin(), runtime->receives.end(), transfer) != runtime->receives.end() &&
      !transfer->data.empty() && transfer->event_source < 0)
    transfer->event_source = runtime->loop->add_fd(transfer->fd, EPOLLOUT,
        [transfer](int fd, int mask) {
          return flush_receive(fd, static_cast<std::uint32_t>(mask), transfer) != 0;
        });
}

void xwayland_receive_selection(DataSourceState* source, const char* mime, int fd) {
  auto* selection = source == nullptr ? nullptr : source->xwayland_selection;
  auto* runtime = selection == nullptr ? nullptr : selection->runtime;
  if (runtime == nullptr || runtime->connection == nullptr || fd < 0) { if (fd >= 0) ::close(fd); return; }
  auto* transfer = new (std::nothrow) XwaylandReceiveTransfer;
  if (transfer == nullptr) { ::close(fd); return; }
  transfer->selection = selection;
  transfer->fd = fd;
  transfer->window = xcb_generate_id(runtime->connection);
  const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_create_window(runtime->connection, XCB_COPY_FROM_PARENT, transfer->window, runtime->screen->root,
                    0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, runtime->screen->root_visual,
                    XCB_CW_EVENT_MASK, &mask);
  runtime->receives.push_back(transfer);
  xcb_convert_selection(runtime->connection, transfer->window, selection->atom,
                        xwayland_mime_atom(runtime, mime == nullptr ? "" : mime),
                        runtime->wl_selection_atom, XCB_CURRENT_TIME);
  xcb_flush(runtime->connection);
}

void install_xwayland_source(XwaylandSelection* selection, const std::vector<std::string>& mimes) {
  auto* runtime = selection->runtime;
  auto* seat = runtime == nullptr || runtime->observer == nullptr ? nullptr : runtime->observer->seat;
  if (seat == nullptr || mimes.empty()) return;
  auto* source = new (std::nothrow) DataSourceState;
  if (source == nullptr) return;
  source->seat = seat;
  source->protocol = DataProtocol::zwlr;
  source->role = DataSourceRole::selection;
  source->mime_types = mimes;
  source->xwayland_selection = selection;
  auto*& current = selection->primary ? seat->primary_selection : seat->selection;
  auto* previous = selection->source;
  if (current != nullptr) cancel_source(current);
  current = source;
  selection->source = source;
  if (previous != nullptr) delete previous;
  if (selection->primary) send_control_selection(seat, true);
  else {
    auto* focused = seat->keyboard_focus == nullptr ? nullptr : seat->keyboard_focus->resource->client;
    send_selection(seat, focused);
    send_control_selection(seat, false);
  }
}

bool handle_xwayland_selection_event(XwaylandRuntime* runtime, xcb_generic_event_t* generic) {
  if (runtime == nullptr || runtime->connection == nullptr) return false;
  const auto type = generic->response_type & 0x7fU;
  if (runtime->xfixes != nullptr && runtime->xfixes->present != 0 &&
      type == runtime->xfixes->first_event + XCB_XFIXES_SELECTION_NOTIFY) {
    const auto* event = reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(generic);
    auto* selection = xwayland_selection_for(runtime, event->selection);
    if (selection == nullptr) return true;
    selection->owner = event->owner;
    if (event->owner == XCB_WINDOW_NONE) {
      auto* seat = runtime->observer == nullptr ? nullptr : runtime->observer->seat;
      if (seat == nullptr) return true;
      auto*& current = selection->primary ? seat->primary_selection : seat->selection;
      if (current == selection->source) {
        current = nullptr;
        cancel_source(selection->source);
        if (selection->primary) send_control_selection(seat, true);
        else { send_selection(seat, seat->keyboard_focus == nullptr ? nullptr : seat->keyboard_focus->resource->client); send_control_selection(seat, false); }
      }
      delete selection->source;
      selection->source = nullptr;
      return true;
    }
    if (event->owner == selection->window) { selection->timestamp = event->timestamp; return true; }
    xcb_convert_selection(runtime->connection, selection->window, selection->atom,
                          runtime->targets_atom, runtime->wl_selection_atom, event->timestamp);
    xcb_flush(runtime->connection);
    return true;
  }
  if (type == XCB_SELECTION_NOTIFY) {
    const auto* event = reinterpret_cast<xcb_selection_notify_event_t*>(generic);
    if (event->target == runtime->targets_atom) {
      auto* selection = xwayland_selection_for(runtime, event->selection);
      if (selection == nullptr || event->property == XCB_ATOM_NONE) return true;
      const auto cookie = xcb_get_property(runtime->connection, 1, selection->window,
                                           runtime->wl_selection_atom, XCB_ATOM_ATOM, 0, 4096);
      auto* reply = xcb_get_property_reply(runtime->connection, cookie, nullptr);
      std::vector<std::string> mimes;
      if (reply != nullptr && reply->type == XCB_ATOM_ATOM) {
        const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
        for (std::uint32_t index = 0; index < reply->value_len; ++index) {
          if (atoms[index] == runtime->targets_atom || atoms[index] == runtime->timestamp_atom) continue;
          auto mime = xwayland_atom_mime(runtime, atoms[index]);
          if (!mime.empty() && std::find(mimes.begin(), mimes.end(), mime) == mimes.end()) mimes.push_back(std::move(mime));
        }
      }
      std::free(reply);
      install_xwayland_source(selection, mimes);
      return true;
    }
    const auto transfer = std::find_if(runtime->receives.begin(), runtime->receives.end(),
        [event](const auto* candidate) { return candidate->window == event->requestor; });
    if (transfer != runtime->receives.end()) {
      if (event->property == XCB_ATOM_NONE) remove_receive(runtime, *transfer);
      else write_receive_property(*transfer);
      return true;
    }
  }
  if (type == XCB_PROPERTY_NOTIFY) {
    const auto* event = reinterpret_cast<xcb_property_notify_event_t*>(generic);
    const auto transfer = std::find_if(runtime->receives.begin(), runtime->receives.end(),
        [event](const auto* candidate) { return candidate->window == event->window; });
    if (transfer != runtime->receives.end() && (*transfer)->incremental &&
        event->atom == runtime->wl_selection_atom && event->state == XCB_PROPERTY_NEW_VALUE) {
      write_receive_property(*transfer);
      return true;
    }
  }
  if (type != XCB_SELECTION_REQUEST) return false;
  const auto* event = reinterpret_cast<xcb_selection_request_event_t*>(generic);
  auto* selection = xwayland_selection_for(runtime, event->selection);
  auto* seat = runtime->observer == nullptr ? nullptr : runtime->observer->seat;
  auto* source = selection == nullptr || seat == nullptr ? nullptr :
      (selection->primary ? seat->primary_selection : seat->selection);
  const auto property = event->property == XCB_ATOM_NONE ? event->target : event->property;
  if (selection == nullptr || source == nullptr || source->xwayland_selection != nullptr ||
      (event->time != XCB_CURRENT_TIME && event->time < selection->timestamp) ||
      seat->keyboard_focus == nullptr || seat->keyboard_focus->resource->client != runtime->client) {
    xwayland_send_notify(runtime, *event, false);
    return true;
  }
  if (event->target == runtime->targets_atom) {
    std::vector<xcb_atom_t> atoms{runtime->timestamp_atom, runtime->targets_atom};
    for (const auto& mime : source->mime_types) atoms.push_back(xwayland_mime_atom(runtime, mime));
    xcb_change_property(runtime->connection, XCB_PROP_MODE_REPLACE, event->requestor, property,
                        XCB_ATOM_ATOM, 32, atoms.size(), atoms.data());
    xwayland_send_notify(runtime, *event, true);
    return true;
  }
  if (event->target == runtime->timestamp_atom) {
    xcb_change_property(runtime->connection, XCB_PROP_MODE_REPLACE, event->requestor, property,
                        XCB_ATOM_INTEGER, 32, 1, &selection->timestamp);
    xwayland_send_notify(runtime, *event, true);
    return true;
  }
  auto mime = xwayland_atom_mime(runtime, event->target);
  if (std::find(source->mime_types.begin(), source->mime_types.end(), mime) == source->mime_types.end()) {
    const auto prefix = mime.find('/');
    const auto needle = prefix == std::string::npos ? mime : mime.substr(0, prefix);
    const auto found = std::find_if(source->mime_types.begin(), source->mime_types.end(),
                                    [&needle](const auto& candidate) { return candidate.find(needle) != std::string::npos; });
    if (found == source->mime_types.end()) { xwayland_send_notify(runtime, *event, false); return true; }
    mime = *found;
  }
  int pipe_fds[2];
  if (::pipe2(pipe_fds, O_CLOEXEC) != 0) { xwayland_send_notify(runtime, *event, false); return true; }
  const int read_flags = ::fcntl(pipe_fds[0], F_GETFL);
  if (read_flags >= 0) (void)::fcntl(pipe_fds[0], F_SETFL, read_flags | O_NONBLOCK);
  auto* transfer = new (std::nothrow) XwaylandSendTransfer;
  if (transfer == nullptr) { ::close(pipe_fds[0]); ::close(pipe_fds[1]); xwayland_send_notify(runtime, *event, false); return true; }
  transfer->selection = selection;
  transfer->request = *event;
  transfer->request.property = property;
  transfer->fd = pipe_fds[0];
  runtime->sends.push_back(transfer);
  send_source_data(source, mime.c_str(), pipe_fds[1]);
  ::close(pipe_fds[1]);
  transfer->event_source = runtime->loop->add_fd(transfer->fd, EPOLLIN,
      [transfer](int fd, int) {
        auto* item = transfer;
        auto* runtime = item->selection->runtime;
        std::array<std::uint8_t, 8192> buffer;
        while (true) {
          const auto count = ::read(fd, buffer.data(), buffer.size());
          if (count > 0) { item->data.insert(item->data.end(), buffer.begin(), buffer.begin() + count); continue; }
          if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
          if (count < 0 || item->data.empty()) xwayland_send_notify(runtime, item->request, false);
          else {
            xcb_change_property(runtime->connection, XCB_PROP_MODE_REPLACE, item->request.requestor,
                                item->request.property, item->request.target, 8,
                                item->data.size(), item->data.data());
            xwayland_send_notify(runtime, item->request, true);
          }
          item->event_source = -1;
          ::close(item->fd); item->fd = -1;
          std::erase(runtime->sends, item);
          delete item;
          return false;
        }
      });
  return true;
}

void xwayland_wayland_selection_changed(SeatState* seat, bool is_primary) {
  auto* runtime = active_xwayland;
  if (runtime == nullptr || runtime->connection == nullptr || runtime->observer == nullptr ||
      runtime->observer->seat != seat) return;
  auto* selection = is_primary ? &runtime->primary : &runtime->clipboard;
  auto* source = is_primary ? seat->primary_selection : seat->selection;
  if (source != nullptr && source->xwayland_selection == selection) return;
  xcb_set_selection_owner(runtime->connection,
                          source == nullptr ? static_cast<xcb_window_t>(XCB_WINDOW_NONE) : selection->window,
                          selection->atom, XCB_CURRENT_TIME);
  selection->notify_on_focus = source != nullptr;
  xcb_flush(runtime->connection);
}

void xwayland_keyboard_focus_changed(XwaylandRuntime* runtime) {
  if (runtime == nullptr || runtime->connection == nullptr || runtime->observer == nullptr ||
      runtime->observer->seat == nullptr) return;
  auto* seat = runtime->observer->seat;
  for (auto* selection : {&runtime->clipboard, &runtime->primary}) {
    auto* source = selection->primary ? seat->primary_selection : seat->selection;
    if (!selection->notify_on_focus || source == nullptr || source->xwayland_selection != nullptr) continue;
    xcb_set_selection_owner(runtime->connection, selection->window, selection->atom, XCB_CURRENT_TIME);
    selection->notify_on_focus = false;
  }
}

void xwayland_reset_selections(XwaylandRuntime* runtime) {
  if (runtime == nullptr) return;
  auto* seat = runtime->observer == nullptr ? nullptr : runtime->observer->seat;
  for (auto* selection : {&runtime->clipboard, &runtime->primary}) {
    auto* source = selection->source;
    if (seat != nullptr && source != nullptr) {
      auto*& current = selection->primary ? seat->primary_selection : seat->selection;
      if (current == source) current = nullptr;
      cancel_source(source);
      if (selection->primary) send_control_selection(seat, true);
      else {
        send_selection(seat, seat->keyboard_focus == nullptr ? nullptr :
                       seat->keyboard_focus->resource->client);
        send_control_selection(seat, false);
      }
      delete source;
    }
    selection->source = nullptr;
    selection->runtime = runtime;
    selection->atom = XCB_ATOM_NONE;
    selection->window = XCB_WINDOW_NONE;
    selection->owner = XCB_WINDOW_NONE;
    selection->timestamp = XCB_CURRENT_TIME;
    selection->notify_on_focus = false;
  }
}

}  // namespace zwwm::detail
