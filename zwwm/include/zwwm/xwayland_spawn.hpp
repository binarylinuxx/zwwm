#pragma once

#include <array>
#include <string>

#include <sys/types.h>

namespace zwwm {

struct XwaylandDisplay {
  int number = -1;
  int lock_fd = -1;
  std::array<int, 2> listen_fds{-1, -1};
};

std::string find_xwayland_binary();
bool create_xwayland_display(XwaylandDisplay* display);
void destroy_xwayland_display(XwaylandDisplay* display);
pid_t spawn_xwayland(const std::string& binary, const XwaylandDisplay& display,
                     int wayland_fd, int wm_fd, int display_fd);

}  // namespace zwwm
