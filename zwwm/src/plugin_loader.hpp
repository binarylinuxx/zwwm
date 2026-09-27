#pragma once

#include "zwwm/plugin.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace zwwm {
class CompositorServer;

class PluginLoader {
 public:
  explicit PluginLoader(CompositorServer* compositor);
  ~PluginLoader();
  PluginLoader(const PluginLoader&) = delete;
  PluginLoader& operator=(const PluginLoader&) = delete;

  void load(const std::vector<std::string>& paths, const std::string& config_path);
  bool configure(const std::vector<std::pair<std::string, std::string>>& settings,
                 std::string* error);
  void emit(const char* event);
  void gesture(const ZwwmPluginGesture& event);

 private:
  struct Module { void* handle; ZwwmPlugin plugin; std::string name; };
  struct Action { ZwwmPluginAction callback; void* userdata; std::size_t owner; };

  static bool register_action(void* context, const char* name, ZwwmPluginAction action, void* userdata);
  static bool dispatch_action(void* context, const char* name, const char* argument);
  static bool handle_action(void* context, const std::string& name, const std::string& argument,
                            std::string* error);

  CompositorServer* compositor_;
  ZwwmPluginHost host_;
  std::vector<Module> modules_;
  std::unordered_map<std::string, Action> actions_;
};
}  // namespace zwwm
