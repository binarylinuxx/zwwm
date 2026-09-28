#include "zwwm/plugin.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
struct DemoState {
  const ZwwmPluginHost* host = nullptr;
  int swipe_distance = 80;
  bool gestures = true;
  double swipe_x = 0.0;
  bool swiping = false;
} demo;

bool parse_settings(std::string_view json, int& distance, bool& gestures) {
  distance = 80;
  gestures = true;
  if (json.size() < 2 || json.front() != '{' || json.back() != '}') return false;
  std::size_t pos = 1;
  while (pos + 1 < json.size()) {
    if (json[pos] != '"') return false;
    const auto end = json.find('"', pos + 1);
    if (end == std::string_view::npos || end + 1 >= json.size() || json[end + 1] != ':') return false;
    const auto key = json.substr(pos + 1, end - pos - 1);
    pos = end + 2;
    if (key == "swipe-distance") {
      const char* begin = json.data() + pos;
      const auto result = std::from_chars(begin, json.data() + json.size(), distance);
      if (result.ec != std::errc{} || distance < 10 || distance > 2000) return false;
      pos = static_cast<std::size_t>(result.ptr - json.data());
    } else if (key == "gestures") {
      if (json.substr(pos).starts_with("true")) { gestures = true; pos += 4; }
      else if (json.substr(pos).starts_with("false")) { gestures = false; pos += 5; }
      else return false;
    } else return false;
    if (json[pos] == ',') ++pos;
    else if (json[pos] != '}' || pos != json.size() - 1) return false;
  }
  return pos == json.size() - 1;
}

bool action(void* data, const char* argument, char* error, std::size_t capacity) {
  auto* state = static_cast<DemoState*>(data);
  const std::string_view direction = argument == nullptr ? "" : argument;
  if (direction != "in" && direction != "out") {
    std::snprintf(error, capacity, "use plugin.demo in or plugin.demo out");
    return false;
  }
  const char* command = direction == "in" ? "zoomin" : "zoomout";
  if (!state->host->dispatch_action(state->host->context, command, "")) {
    std::snprintf(error, capacity, "could not %s the canvas", command);
    return false;
  }
  std::fprintf(stderr, "zwwm demo: %s\n", command);
  return true;
}

bool validate_config(void*, const char* json, char* error, std::size_t capacity) {
  int distance = 80;
  bool gestures = true;
  if (parse_settings(json, distance, gestures)) return true;
  std::snprintf(error, capacity, "expected gestures = true/false and swipe-distance = 10..2000");
  return false;
}

void configure(void* data, const char* json) {
  auto* state = static_cast<DemoState*>(data);
  parse_settings(json, state->swipe_distance, state->gestures);
  std::fprintf(stderr, "zwwm demo: gestures %s, swipe distance %d\n",
               state->gestures ? "enabled" : "disabled", state->swipe_distance);
}

void gesture(void* data, const ZwwmPluginGesture* input) {
  auto* state = static_cast<DemoState*>(data);
  if (!state->gestures || input->kind != ZwwmGestureKind::swipe) return;
  if (input->phase == ZwwmGesturePhase::begin) {
    state->swiping = input->fingers == 3;
    state->swipe_x = 0.0;
  } else if (input->phase == ZwwmGesturePhase::update && state->swiping) {
    state->swipe_x += input->dx;
  } else if (input->phase == ZwwmGesturePhase::end) {
    if (state->swiping && !input->cancelled &&
        std::abs(state->swipe_x) >= state->swipe_distance)
      state->host->dispatch_action(state->host->context,
                                   state->swipe_x > 0.0 ? "zoomin" : "zoomout", "");
    state->swiping = false;
  }
}

void on_event(void*, const char* event) {
  if (event != nullptr && std::strcmp(event, "output") == 0)
    std::fprintf(stderr, "zwwm demo: outputs changed\n");
}

void shutdown(void*) { std::fprintf(stderr, "zwwm demo: unloaded\n"); }
}  // namespace

extern "C" bool zwwm_plugin_init(const ZwwmPluginHost* host, ZwwmPlugin* plugin) {
  if (host == nullptr || plugin == nullptr || host->abi_version != ZWWM_PLUGIN_ABI_VERSION)
    return false;
  demo.host = host;
  if (!host->register_action(host->context, "plugin.demo", action, &demo)) return false;
  plugin->abi_version = ZWWM_PLUGIN_ABI_VERSION;
  plugin->userdata = &demo;
  plugin->name = "demo";
  plugin->on_event = on_event;
  plugin->on_gesture = gesture;
  plugin->validate_config = validate_config;
  plugin->on_configure = configure;
  plugin->shutdown = shutdown;
  std::fprintf(stderr, "zwwm demo: loaded (try zwwmctl dispatch plugin.demo in)\n");
  return true;
}
