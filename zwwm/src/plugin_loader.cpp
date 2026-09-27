#include "plugin_loader.hpp"

#include "zwwm/compositor_server.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>

namespace zwwm {

PluginLoader::PluginLoader(CompositorServer* compositor)
    : compositor_(compositor), host_{ZWWM_PLUGIN_ABI_VERSION, this, register_action, dispatch_action} {
  compositor_->set_external_action_handler(handle_action, this);
}

PluginLoader::~PluginLoader() {
  compositor_->set_event_observer(nullptr, nullptr);
  compositor_->set_external_action_handler(nullptr, nullptr);
  actions_.clear();
  for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
    if (it->plugin.shutdown != nullptr) {
      try { it->plugin.shutdown(it->plugin.userdata); }
      catch (...) { std::fprintf(stderr, "zwwm: plugin shutdown threw an exception\n"); }
    }
    dlclose(it->handle);
  }
}

bool PluginLoader::register_action(void* context, const char* name, ZwwmPluginAction action,
                                   void* userdata) {
  if (context == nullptr || name == nullptr || action == nullptr ||
      std::string_view(name).substr(0, 7) != "plugin." || name[7] == '\0') return false;
  auto* self = static_cast<PluginLoader*>(context);
  return self->actions_.emplace(name, Action{action, userdata, self->modules_.size()}).second;
}

bool PluginLoader::dispatch_action(void* context, const char* name, const char* argument) {
  if (context == nullptr || name == nullptr) return false;
  auto* self = static_cast<PluginLoader*>(context);
  std::string error;
  return self->compositor_->dispatch_action(name, argument == nullptr ? "" : argument, &error);
}

bool PluginLoader::handle_action(void* context, const std::string& name,
                                 const std::string& argument, std::string* error) {
  auto* self = static_cast<PluginLoader*>(context);
  const auto it = self->actions_.find(name);
  if (it == self->actions_.end()) {
    if (error != nullptr) *error = "unknown action";
    return false;
  }
  std::array<char, 256> message{};
  try {
    const bool success = it->second.callback(it->second.userdata, argument.c_str(),
                                             message.data(), message.size());
    message.back() = '\0';
    if (success)
      return true;
    if (error != nullptr) *error = message.front() == '\0' ? "plugin action failed" : message.data();
  } catch (...) {
    if (error != nullptr) *error = "plugin action threw an exception";
  }
  return false;
}

void PluginLoader::load(const std::vector<std::string>& paths, const std::string& config_path) {
  const auto directory = config_path.empty() ? std::filesystem::current_path() :
                         std::filesystem::absolute(config_path).parent_path();
  for (const auto& name : paths) {
    const auto path = std::filesystem::absolute(std::filesystem::path(name).is_absolute() ?
                                               std::filesystem::path(name) : directory / name);
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      std::fprintf(stderr, "zwwm: could not load plugin %s: %s\n", path.c_str(), dlerror());
      continue;
    }
    dlerror();
    auto* symbol = dlsym(handle, "zwwm_plugin_init");
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr) {
      std::fprintf(stderr, "zwwm: plugin %s: %s\n", path.c_str(), symbol_error);
      dlclose(handle);
      continue;
    }
    const auto entry = reinterpret_cast<ZwwmPluginInit>(symbol);
    ZwwmPlugin plugin{ZWWM_PLUGIN_ABI_VERSION, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr};
    bool initialized = false;
    try { initialized = entry(&host_, &plugin); }
    catch (...) { std::fprintf(stderr, "zwwm: plugin %s initialization threw an exception\n", path.c_str()); }
    const bool named = plugin.name != nullptr && *plugin.name != '\0' &&
        std::none_of(modules_.begin(), modules_.end(), [&](const Module& module) {
          return module.name == plugin.name;
        });
    if (!initialized || plugin.abi_version != ZWWM_PLUGIN_ABI_VERSION || !named) {
      if (initialized && plugin.shutdown != nullptr) {
        try { plugin.shutdown(plugin.userdata); } catch (...) {}
      }
      std::erase_if(actions_, [owner = modules_.size()](const auto& action) {
        return action.second.owner == owner;
      });
      std::fprintf(stderr, "zwwm: plugin %s initialization failed or ABI version mismatched\n", path.c_str());
      dlclose(handle);
      continue;
    }
    modules_.push_back({handle, plugin, plugin.name});
  }
}

bool PluginLoader::configure(const std::vector<std::pair<std::string, std::string>>& settings,
                             std::string* error) {
  const auto options_for = [&settings](const Module& module) -> const char* {
    const auto found = std::find_if(settings.rbegin(), settings.rend(), [&](const auto& item) {
      return item.first == module.name;
    });
    return found == settings.rend() ? "{}" : found->second.c_str();
  };
  for (const auto& module : modules_) {
    if (module.plugin.validate_config == nullptr) continue;
    std::array<char, 256> message{};
    try {
      const bool valid = module.plugin.validate_config(module.plugin.userdata, options_for(module),
                                                       message.data(), message.size());
      message.back() = '\0';
      if (valid) continue;
    } catch (...) { message[0] = '\0'; }
    if (error != nullptr)
      *error = "plugin " + module.name + ": " +
          (message[0] == '\0' ? "invalid configuration" : message.data());
    return false;
  }
  for (const auto& module : modules_) {
    if (module.plugin.on_configure == nullptr) continue;
    try { module.plugin.on_configure(module.plugin.userdata, options_for(module)); }
    catch (...) {
      if (error != nullptr) *error = "plugin " + module.name + ": configuration callback threw";
      return false;
    }
  }
  return true;
}

void PluginLoader::emit(const char* event) {
  if (event == nullptr) return;
  for (const auto& module : modules_) {
    if (module.plugin.on_event == nullptr) continue;
    try { module.plugin.on_event(module.plugin.userdata, event); }
    catch (...) { std::fprintf(stderr, "zwwm: plugin event handler threw an exception\n"); }
  }
}

void PluginLoader::gesture(const ZwwmPluginGesture& event) {
  for (const auto& module : modules_) {
    if (module.plugin.on_gesture == nullptr) continue;
    try { module.plugin.on_gesture(module.plugin.userdata, &event); }
    catch (...) { std::fprintf(stderr, "zwwm: plugin gesture handler threw an exception\n"); }
  }
}

}  // namespace zwwm
