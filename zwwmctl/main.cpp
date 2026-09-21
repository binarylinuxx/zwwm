#include <ext-zwwm-manager-v1-zwayland-client.h>
#include <zwayland/client/core.hpp>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace protocol = zwayland::generated;

volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }

std::string escape(std::string_view value) {
  std::string result;
  for (const char character : value) {
    if (character == '\\') result += "\\\\";
    else if (character == '\t') result += "\\t";
    else if (character == '\n') result += "\\n";
    else if (character == '\r') result += "\\r";
    else result += character;
  }
  return result;
}

bool split(std::string_view line, std::vector<std::string>* fields) {
  fields->clear(); fields->emplace_back(); bool escaped = false;
  for (const char character : line) {
    if (escaped) {
      if (character == 't') fields->back() += '\t';
      else if (character == 'n') fields->back() += '\n';
      else if (character == 'r') fields->back() += '\r';
      else if (character == '\\') fields->back() += '\\';
      else return false;
      escaped = false;
    } else if (character == '\\') escaped = true;
    else if (character == '\t') fields->emplace_back();
    else fields->back() += character;
  }
  return !escaped;
}

std::string json_string(std::string_view value) {
  std::string result = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char character : value) {
    if (character == '"' || character == '\\') { result += '\\'; result += static_cast<char>(character); }
    else if (character == '\b') result += "\\b";
    else if (character == '\f') result += "\\f";
    else if (character == '\n') result += "\\n";
    else if (character == '\r') result += "\\r";
    else if (character == '\t') result += "\\t";
    else if (character < 0x20) { result += "\\u00"; result += hex[character >> 4U]; result += hex[character & 15U]; }
    else result += static_cast<char>(character);
  }
  result += '"';
  return result;
}

std::string json_line(std::string_view line) {
  std::vector<std::string> fields;
  if (!split(line, &fields) || fields.empty()) return "{\"ok\":false,\"error\":\"invalid protocol response\"}";
  if (fields[0] == "error") return "{\"ok\":false,\"error\":" + json_string(fields.size() > 1 ? fields[1] : "unknown error") + "}";
  if (fields[0] == "ok") return "{\"ok\":true,\"message\":" + json_string(fields.size() > 1 ? fields[1] : "") + "}";
  if (fields[0] == "event") return "{\"event\":" + json_string(fields.size() > 1 ? fields[1] : "") + "}";
  if (fields.size() < 2 || fields[0] != "data") return "{\"ok\":false,\"error\":\"unknown protocol response\"}";
  if (fields[1] == "version" && fields.size() >= 3) return "{\"ok\":true,\"version\":" + fields[2] + "}";
  if (fields[1] == "status" && fields.size() >= 6)
    return "{\"ok\":true,\"protocol\":" + fields[2] + ",\"outputs\":" + fields[3] + ",\"clients\":" + fields[4] + ",\"layers\":" + fields[5] + "}";
  if (fields[1] == "outputs" && fields.size() >= 3) {
    std::string result = "{\"ok\":true,\"outputs\":["; std::size_t at = 3;
    const std::size_t count = std::strtoull(fields[2].c_str(), nullptr, 10);
    for (std::size_t output = 0; output < count; ++output) {
      if (at + 14 > fields.size()) return "{\"ok\":false,\"error\":\"truncated output response\"}";
      if (output != 0) result += ',';
      const std::string transform = fields[at + 10] == "1" ? "90" : fields[at + 10] == "2" ? "180" : fields[at + 10] == "3" ? "270" : "normal";
      result += "{\"connector\":" + json_string(fields[at]) + ",\"id\":" + fields[at + 1] +
                ",\"logical\":{\"x\":" + fields[at + 2] + ",\"y\":" + fields[at + 3] + ",\"width\":" + fields[at + 4] + ",\"height\":" + fields[at + 5] +
                "},\"current_mode\":{\"width\":" + fields[at + 6] + ",\"height\":" + fields[at + 7] + ",\"refresh_millihz\":" + fields[at + 8] +
                "},\"scale_per_mille\":" + fields[at + 9] + ",\"transform\":" + json_string(transform) + ",\"bit_depth\":" + fields[at + 11] +
                ",\"enabled\":" + (fields[at + 12] == "1" ? "true" : "false") + ",\"modes\":[";
      const std::size_t modes = std::strtoull(fields[at + 13].c_str(), nullptr, 10); at += 14;
      for (std::size_t mode = 0; mode < modes; ++mode) {
        if (at + 4 > fields.size()) return "{\"ok\":false,\"error\":\"truncated mode response\"}";
        if (mode != 0) result += ',';
        result += "{\"width\":" + fields[at] + ",\"height\":" + fields[at + 1] + ",\"refresh_millihz\":" + fields[at + 2] +
                  ",\"preferred\":" + (fields[at + 3] == "1" ? "true" : "false") + "}"; at += 4;
      }
      result += "]}";
    }
    return result + "]}";
  }
  if (fields[1] == "clients" && fields.size() >= 3) {
    std::string result = "{\"ok\":true,\"clients\":["; std::size_t at = 3;
    const std::size_t count = std::strtoull(fields[2].c_str(), nullptr, 10);
    for (std::size_t index = 0; index < count; ++index, at += 9) {
      if (at + 9 > fields.size()) return "{\"ok\":false,\"error\":\"truncated clients response\"}";
      if (index != 0) result += ',';
      result += "{\"id\":" + fields[at] + ",\"app_id\":" + json_string(fields[at + 1]) + ",\"title\":" + json_string(fields[at + 2]) +
                ",\"output\":" + fields[at + 3] + ",\"x\":" + fields[at + 4] + ",\"y\":" + fields[at + 5] +
                ",\"width\":" + fields[at + 6] + ",\"height\":" + fields[at + 7] + ",\"state\":" + fields[at + 8] + "}";
    }
    return result + "]}";
  }
  if (fields[1] == "tags" && fields.size() >= 3) {
    std::string result = "{\"ok\":true,\"tags\":["; std::size_t at = 3;
    const std::size_t count = std::strtoull(fields[2].c_str(), nullptr, 10);
    for (std::size_t index = 0; index < count; ++index, at += 3) {
      if (at + 3 > fields.size()) return "{\"ok\":false,\"error\":\"truncated tags response\"}";
      if (index != 0) result += ',';
      result += "{\"connector\":" + json_string(fields[at]) + ",\"output\":" + fields[at + 1] + ",\"active\":" + fields[at + 2] + "}";
    }
    return result + "]}";
  }
  if (fields[1] == "layers" && fields.size() >= 3) {
    std::string result = "{\"ok\":true,\"layers\":["; std::size_t at = 3;
    const std::size_t count = std::strtoull(fields[2].c_str(), nullptr, 10);
    for (std::size_t index = 0; index < count; ++index, at += 9) {
      if (at + 9 > fields.size()) return "{\"ok\":false,\"error\":\"truncated layers response\"}";
      if (index != 0) result += ',';
      result += "{\"id\":" + fields[at] + ",\"output\":" + fields[at + 1] + ",\"namespace\":" + json_string(fields[at + 2]) +
                ",\"layer\":" + fields[at + 3] + ",\"priority\":" + fields[at + 4] + ",\"exclusive_zone\":" + fields[at + 5] +
                ",\"blur_radius\":" + fields[at + 6] + ",\"opacity\":" + fields[at + 7] + ",\"mapped\":" + (fields[at + 8] == "1" ? "true" : "false") + "}";
    }
    return result + "]}";
  }
  return "{\"ok\":false,\"error\":\"unsupported protocol response\"}";
}

std::uint64_t combine(std::uint32_t high, std::uint32_t low) {
  return (static_cast<std::uint64_t>(high) << 32U) | low;
}

struct Client {
  std::unique_ptr<zwayland::client::Display> display;
  zwayland::client::Proxy* registry = nullptr;
  zwayland::client::Proxy* manager = nullptr;
  zwayland::client::Proxy* snapshot = nullptr;
  zwayland::client::Proxy* result = nullptr;
  zwayland::client::Proxy* subscription = nullptr;
  std::string kind;
  std::vector<std::string> fields;
  std::size_t count = 0;
  bool done = false;
  bool failed = false;
  bool json = false;

  void output(std::string line) const {
    const std::string rendered = json ? json_line(line) : line;
    std::puts(rendered.c_str());
    std::fflush(stdout);
  }
};

void add(std::vector<std::string>& fields, std::string value) { fields.push_back(std::move(value)); }
template <typename T> void add_number(std::vector<std::string>& fields, T value) { fields.push_back(std::to_string(value)); }

struct SnapshotHandler {
  Client* client;

  void status(zwayland::client::Proxy&, std::uint32_t version, std::uint32_t outputs,
              std::uint32_t clients, std::uint32_t layers) {
      auto& self = *client;
      self.fields = {"data", "status", std::to_string(version), std::to_string(outputs),
                     std::to_string(clients), std::to_string(layers)};
  }
  void output(zwayland::client::Proxy&, const std::string& connector, std::uint32_t id_hi,
              std::uint32_t id_lo, std::int32_t x, std::int32_t y, std::uint32_t logical_width,
              std::uint32_t logical_height, std::uint32_t physical_width, std::uint32_t physical_height,
              std::uint32_t refresh, std::uint32_t scale, std::uint32_t transform, std::uint32_t bit_depth,
              std::uint32_t enabled, std::uint32_t modes) {
      auto& self = *client; ++self.count;
      add(self.fields, connector); add_number(self.fields, combine(id_hi, id_lo)); add_number(self.fields, x); add_number(self.fields, y);
      add_number(self.fields, logical_width); add_number(self.fields, logical_height); add_number(self.fields, physical_width);
      add_number(self.fields, physical_height); add_number(self.fields, refresh); add_number(self.fields, scale);
      add_number(self.fields, transform); add_number(self.fields, bit_depth); add_number(self.fields, enabled); add_number(self.fields, modes);
  }
  void mode(zwayland::client::Proxy&, std::uint32_t width, std::uint32_t height,
            std::uint32_t refresh, std::uint32_t preferred) {
      auto& fields = client->fields;
      add_number(fields, width); add_number(fields, height); add_number(fields, refresh); add_number(fields, preferred);
  }
  void toplevel(zwayland::client::Proxy&, std::uint32_t id_hi, std::uint32_t id_lo,
                const std::string& app_id, const std::string& title, std::uint32_t output_hi,
                std::uint32_t output_lo, std::int32_t x, std::int32_t y, std::int32_t width,
                std::int32_t height, std::uint32_t state) {
      auto& self = *client; ++self.count;
      add_number(self.fields, combine(id_hi, id_lo)); add(self.fields, app_id); add(self.fields, title);
      add_number(self.fields, combine(output_hi, output_lo)); add_number(self.fields, x); add_number(self.fields, y);
      add_number(self.fields, width); add_number(self.fields, height); add_number(self.fields, state);
  }
  void tag(zwayland::client::Proxy&, const std::string& connector, std::uint32_t output_hi,
           std::uint32_t output_lo, std::uint32_t active) {
      auto& self = *client; ++self.count;
      add(self.fields, connector); add_number(self.fields, combine(output_hi, output_lo)); add_number(self.fields, active);
  }
  void layer(zwayland::client::Proxy&, std::uint32_t id_hi, std::uint32_t id_lo,
             std::uint32_t output_hi, std::uint32_t output_lo, const std::string& name_space,
             std::uint32_t layer_value, std::int32_t priority, std::int32_t zone,
             std::uint32_t blur, std::uint32_t opacity, std::uint32_t mapped) {
      auto& self = *client; ++self.count;
      add_number(self.fields, combine(id_hi, id_lo)); add_number(self.fields, combine(output_hi, output_lo));
      add(self.fields, name_space); add_number(self.fields, layer_value); add_number(self.fields, priority); add_number(self.fields, zone);
      add_number(self.fields, blur); add(self.fields, std::to_string(opacity / 1000000.0)); add_number(self.fields, mapped);
  }
  void done(zwayland::client::Proxy&) { client->done = true; }
  void failed(zwayland::client::Proxy&, const std::string& message) {
    client->failed = true; client->done = true;
    client->fields = {"error", message.empty() ? "snapshot failed" : message};
  }
};

struct ResultHandler {
  Client* client;
  void succeeded(zwayland::client::Proxy&, const std::string& message) {
    client->fields = {"ok", message}; client->done = true;
  }
  void failed(zwayland::client::Proxy&, const std::string& message) {
    client->fields = {"error", message.empty() ? "request failed" : message};
    client->failed = true; client->done = true;
  }
};

struct SubscriptionHandler {
  Client* client;
  void event(zwayland::client::Proxy&, const std::string& event) {
    client->output("event\t" + escape(event));
  }
};

struct RegistryHandler {
  Client* client;
  void global(zwayland::client::Proxy& registry, std::uint32_t name,
              std::string_view interface, std::uint32_t version) {
    if (interface == protocol::ext_zwwm_manager_v1_interface.name)
      client->manager = zwayland::client::core::bind(
          *client->display, registry, name, protocol::ext_zwwm_manager_v1_interface,
      std::min(version, 3U));
  }
  void global_remove(zwayland::client::Proxy&, std::uint32_t) {}
};

std::string line(const Client& client) {
  std::string result;
  if (!client.failed && !client.kind.empty() && client.kind != "status") {
    result = "data\t" + client.kind + "\t" + std::to_string(client.count);
    for (const auto& field : client.fields) result += '\t' + escape(field);
    return result;
  }
  for (std::size_t index = 0; index < client.fields.size(); ++index) {
    if (index != 0) result += '\t';
    result += escape(client.fields[index]);
  }
  return result;
}

void usage(const char* program) {
  std::fprintf(stderr, "usage: %s {version|ping|status|output|outputs|clients|tags|layers|reload|rebuild-switch-shaders|setcursor THEME SIZE|dispatch ACTION [ARG]|events [EVENT ...]} [-j|--json]\n", program);
}

std::uint32_t event_mask(const std::vector<std::string>& events) {
  if (events.empty()) return 0x7fU;
  std::uint32_t result = 0;
  for (const auto& event : events) {
    if (event == "ready") result |= protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_READY;
    else if (event == "config") result |= protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_CONFIG;
    else if (event == "tag") result |= protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_TAG;
    else if (event == "window") result |= protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_WINDOW;
    else if (event == "layer") result |= protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_LAYER;
    else if (event == "output") result |= protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_OUTPUT;
    else if (event == "shutdown") result |= protocol::EXT_ZWWM_MANAGER_V1_EVENT_MASK_SHUTDOWN;
    else return 0;
  }
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::printf("zwwmctl %s\n", ZWWM_VERSION);
    return EXIT_SUCCESS;
  }
  if (argc < 2) { usage(argv[0]); return EXIT_FAILURE; }
  const std::string command(argv[1]);
  std::vector<std::string> arguments;
  for (int index = 2; index < argc; ++index) arguments.emplace_back(argv[index]);
  Client client;
  if (!arguments.empty() && (arguments.back() == "-j" || arguments.back() == "--json")) { client.json = true; arguments.pop_back(); }
  if (command == "version") { client.output("data\tversion\t1"); return EXIT_SUCCESS; }

  client.display = zwayland::client::Display::connect();
  constexpr const char* connection_error =
      "zwwmctl: failed to connect: is zwwm running? does the compositor implement "
      "ext-zwwm-manager-unstable-v1 protocol?\n";
  if (client.display == nullptr) { std::fputs(connection_error, stderr); return EXIT_FAILURE; }
  zwayland::client::core::observe_registry(*client.display, RegistryHandler{&client});
  client.registry = zwayland::client::core::get_registry(*client.display);
  if (client.registry == nullptr || !client.display->roundtrip() || client.manager == nullptr) {
    std::fputs(connection_error, stderr);
    return EXIT_FAILURE;
  }
  if (command == "ping") {
    if (!arguments.empty()) { usage(argv[0]); return EXIT_FAILURE; }
    client.output("ok\tpong");
    return EXIT_SUCCESS;
  }

  bool stream = false;
  if (command == "status" || command == "output" || command == "outputs" || command == "clients" ||
      command == "tags" || command == "layers") {
    if (!arguments.empty()) { usage(argv[0]); return EXIT_FAILURE; }
    client.kind = command == "output" ? "outputs" : command;
    client.fields.clear();
    const std::uint32_t type = client.kind == "status" ? protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_STATUS :
        client.kind == "outputs" ? protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_OUTPUTS :
        client.kind == "clients" ? protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_TOPLEVELS :
        client.kind == "tags" ? protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_TAGS : protocol::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LAYERS;
    protocol::ext_zwwm_snapshot_v1_observe(*client.display, SnapshotHandler{&client});
    const std::uint32_t snapshot_id = protocol::ext_zwwm_manager_v1_get_snapshot(
        *client.display, client.manager->id, type);
    client.snapshot = client.display->find_proxy(snapshot_id);
  } else if (command == "reload") {
    if (!arguments.empty()) { usage(argv[0]); return EXIT_FAILURE; }
    protocol::ext_zwwm_result_v1_observe(*client.display, ResultHandler{&client});
    const std::uint32_t result_id = protocol::ext_zwwm_manager_v1_reload(
        *client.display, client.manager->id);
    client.result = client.display->find_proxy(result_id);
  } else if (command == "rebuild-switch-shaders") {
    if (!arguments.empty()) { usage(argv[0]); return EXIT_FAILURE; }
    protocol::ext_zwwm_result_v1_observe(*client.display, ResultHandler{&client});
    const std::uint32_t result_id = protocol::ext_zwwm_manager_v1_rebuild_switch_shaders(
        *client.display, client.manager->id);
    client.result = client.display->find_proxy(result_id);
  } else if (command == "setcursor") {
    if (arguments.size() != 2) { usage(argv[0]); return EXIT_FAILURE; }
    char* end = nullptr;
    const unsigned long parsed_size = std::strtoul(arguments[1].c_str(), &end, 10);
    if (end == arguments[1].c_str() || *end != '\0' || parsed_size == 0 || parsed_size > 1024) {
      std::fputs("zwwmctl: cursor size must be between 1 and 1024\n", stderr);
      return EXIT_FAILURE;
    }
    protocol::ext_zwwm_result_v1_observe(*client.display, ResultHandler{&client});
    const std::uint32_t result_id = protocol::ext_zwwm_manager_v1_set_cursor(
        *client.display, client.manager->id, arguments[0], static_cast<std::uint32_t>(parsed_size));
    client.result = client.display->find_proxy(result_id);
  } else if (command == "dispatch") {
    if (arguments.empty() || arguments.size() > 2) { usage(argv[0]); return EXIT_FAILURE; }
    protocol::ext_zwwm_result_v1_observe(*client.display, ResultHandler{&client});
    const std::uint32_t result_id = protocol::ext_zwwm_manager_v1_dispatch(
        *client.display, client.manager->id, arguments[0],
        arguments.size() == 2 ? arguments[1] : "");
    client.result = client.display->find_proxy(result_id);
  } else if (command == "events") {
    const std::uint32_t mask = event_mask(arguments);
    if (mask == 0) { usage(argv[0]); return EXIT_FAILURE; }
    protocol::ext_zwwm_subscription_v1_observe(*client.display, SubscriptionHandler{&client});
    const std::uint32_t subscription_id = protocol::ext_zwwm_manager_v1_subscribe(
        *client.display, client.manager->id, mask);
    client.subscription = client.display->find_proxy(subscription_id);
    stream = true;
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
  } else {
    usage(argv[0]); return EXIT_FAILURE;
  }

  while (!interrupted && (stream || !client.done)) {
    if (!client.display->dispatch()) {
      if (interrupted) break;
      std::fputs("zwwmctl: Wayland dispatch failed\n", stderr);
      client.failed = true;
      break;
    }
  }
  if (!stream && client.done) client.output(line(client));
  if (client.subscription != nullptr)
    protocol::ext_zwwm_subscription_v1_destroy(*client.display, client.subscription->id);
  if (client.result != nullptr)
    protocol::ext_zwwm_result_v1_destroy(*client.display, client.result->id);
  if (client.snapshot != nullptr)
    protocol::ext_zwwm_snapshot_v1_destroy(*client.display, client.snapshot->id);
  protocol::ext_zwwm_manager_v1_destroy(*client.display, client.manager->id);
  client.display->destroy_proxy(client.registry->id);
  client.display->flush();
  return client.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
