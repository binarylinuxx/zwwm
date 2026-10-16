#include <zwayland/server/display.hpp>
#include <tags-zwayland-server.h>

int check_server() {
  zwayland::server::EventLoop loop;
  zwayland::wire::MessageBuilder message(1);
  message.set_opcode(0);
  message.append_uint(42);
  return message.finish().size() == 12 ? 0 : 1;
}
