# Native plugins

zwwm loads C++ shared libraries listed in `config.zw` at session startup:

```zw
plugins = ["/home/me/.local/lib/zwwm/example.so"]
plugin-settings = {
  example = { swipe-distance = 80 enabled = true }
}
```

Relative paths are resolved against the directory containing `config.zw`.
Plugins are loaded in list order and shut down in reverse order. Changing the
list requires restarting zwwm; live config reload does not unload or reload
native code. A plugin that fails to load is logged without stopping the session.
Each plugin supplies a unique `name` matching its `plugin-settings` entry. The
settings object may contain strings, integers, booleans, null, nested objects,
and arrays; zwwm sends it to the plugin as JSON. Settings are validated and
applied on startup and each successful config reload. An omitted entry is `{}`.

Include the installed `zwwm/plugin.hpp` header (or the one in
`zwwm/include/zwwm/`) and export `zwwm_plugin_init` with C linkage:

```cpp
#include <zwwm/plugin.hpp>

#include <cstdio>

namespace {
struct State { const ZwwmPluginHost* host; double swipe = 0; } state;

bool hello(void*, const char* argument, char*, std::size_t) {
  std::fprintf(stderr, "hello from zwwm: %s\n", argument);
  return true;
}

void event(void*, const char* name) {
  if (name != nullptr) std::fprintf(stderr, "zwwm event: %s\n", name);
}

void configure(void*, const char* json) {
  // Parse this plugin's settings using a JSON library of your choice.
  std::fprintf(stderr, "example settings: %s\n", json);
}

void gesture(void* userdata, const ZwwmPluginGesture* input) {
  auto* plugin = static_cast<State*>(userdata);
  if (input->kind != ZwwmGestureKind::swipe || input->fingers != 3) return;
  if (input->phase == ZwwmGesturePhase::begin) plugin->swipe = 0;
  if (input->phase == ZwwmGesturePhase::update) plugin->swipe += input->dx;
  if (input->phase == ZwwmGesturePhase::end && !input->cancelled && plugin->swipe > 80)
    plugin->host->dispatch_action(plugin->host->context, "zoomin", "");
}
}

extern "C" bool zwwm_plugin_init(const ZwwmPluginHost* host, ZwwmPlugin* plugin) {
  if (host == nullptr || plugin == nullptr ||
      host->abi_version != ZWWM_PLUGIN_ABI_VERSION) return false;
  state.host = host;
  if (!host->register_action(host->context, "plugin.hello", hello, nullptr)) return false;
  plugin->abi_version = ZWWM_PLUGIN_ABI_VERSION;
  plugin->name = "example";
  plugin->userdata = &state;
  plugin->on_event = event;
  plugin->on_gesture = gesture;
  plugin->on_configure = configure;
  return true;
}
```

Compile it with `c++ -std=c++23 -fPIC -shared -I/path/to/zwwm/include
example.cpp -o example.so`, then run `zwwmctl dispatch plugin.hello world`.
Plugin actions must use the `plugin.` prefix and have unique names. An action
returns `false` to report failure and may write a NUL-terminated message into
the supplied error buffer. Event names include `output`, `tag`, `camera`,
`layer`, `window`, `config`, and `keyboard`. Callback strings are only valid
during the call. The host's `dispatch_action` callback can invoke compositor
actions from a plugin. Callbacks run on the compositor thread; plugins should
not retain host pointers after their `shutdown` callback returns.

`on_gesture` receives direct-DRM libinput swipe, pinch, and hold begin/update/end
events. Swipe and pinch deltas are output-logical and follow the configured
output transform; pinch scale is relative to gesture start and angle changes
are in degrees. The `output` connector and `device` sysname strings are valid
during the callback.
The nested Wayland backend does not receive libinput gestures.

`validate_config` is optional; when provided it can reject an options JSON
object by returning `false` and writing an error into the supplied buffer.
zwwm validates every loaded plugin before calling any `on_configure` callback,
and rejects a live config reload if validation fails. `on_configure` applies
the validated settings and should not throw. Callback JSON strings are valid
only during their calls. The module list still requires a restart to change.
