#include "compositor_server_internal.hpp"

#include <ext-idle-notify-zwayland-server.h>

#include <algorithm>
#include <new>

namespace zwwm::detail {

namespace protocol = zwayland::generated;

void arm_idle_notification(IdleNotification* notification) {
  notification->seat->display->event_loop().update_timer(notification->timer,
                                                          std::max(1U, notification->timeout));
}

void idle_notification_destroyed(zwayland::server::Resource* resource) {
  auto* notification = resource->data<IdleNotification>();
  if (notification == nullptr) return;
  notification->seat->display->event_loop().remove(notification->timer);
  std::erase(notification->seat->idle_notifications, notification);
  delete notification;
}

struct ExtIdleNotificationV1Handler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};

void create_idle_notification(zwayland::server::Client* client, zwayland::server::Resource* manager,
                              std::uint32_t id, std::uint32_t timeout,
                              zwayland::server::Resource* seat_resource) {
  auto* seat = manager->data<SeatState>();
  if (seat_resource == nullptr || seat_resource->data<SeatState>() != seat) {
    client->post_error(0, "idle notification requested for an unknown seat");
    return;
  }
  auto* resource = client->create_resource(&protocol::ext_idle_notification_v1_interface, id,
                                            manager->version);
  auto* notification = resource == nullptr ? nullptr : new (std::nothrow) IdleNotification;
  if (resource == nullptr || notification == nullptr) {
    if (resource != nullptr) resource->destroy();
    client->post_no_memory();
    return;
  }
  notification->seat = seat;
  notification->resource = resource;
  notification->timeout = timeout;
  notification->timer = seat->display->event_loop().add_timer([notification] {
    if (notification->idle) return;
    notification->idle = true;
    protocol::ext_idle_notification_v1_send_idled(*notification->resource);
  });
  seat->idle_notifications.push_back(notification);
  resource->set_data(notification);
  resource->set_handler(protocol::ext_idle_notification_v1_handler(ExtIdleNotificationV1Handler{}));
  resource->set_destroy_handler([](zwayland::server::Resource& destroyed) {
    idle_notification_destroyed(&destroyed);
  });
  arm_idle_notification(notification);
}

struct ExtIdleNotifierV1Handler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void get_idle_notification(zwayland::server::Client& client, zwayland::server::Resource& resource,
                             std::uint32_t id, std::uint32_t timeout,
                             zwayland::server::Resource* seat) {
    create_idle_notification(&client, &resource, id, timeout, seat);
  }
  void get_input_idle_notification(zwayland::server::Client& client,
                                   zwayland::server::Resource& resource, std::uint32_t id,
                                   std::uint32_t timeout, zwayland::server::Resource* seat) {
    create_idle_notification(&client, &resource, id, timeout, seat);
  }
};

void bind_idle_notifier(zwayland::server::Client* client, void* data, std::uint32_t version,
                        std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::ext_idle_notifier_v1_interface, id,
                                            std::min(version, 2U));
  if (resource == nullptr) {
    client->post_no_memory();
    return;
  }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::ext_idle_notifier_v1_handler(ExtIdleNotifierV1Handler{}));
}

void idle_activity(SeatState* seat) {
  for (auto* notification : seat->idle_notifications) {
    if (notification->idle) {
      notification->idle = false;
      protocol::ext_idle_notification_v1_send_resumed(*notification->resource);
    }
    arm_idle_notification(notification);
  }
}

}  // namespace zwwm::detail
