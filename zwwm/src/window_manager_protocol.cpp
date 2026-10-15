#include "zwwm/window_manager_protocol.hpp"
#include "zwwm/runtime_config.hpp"
#include "zwwm/compositor_server.hpp"

#include <zwwm-window-manager-v1-zwayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <cstring>
#include <span>
#include <new>
#include <vector>

namespace protocol = zwayland::generated;
namespace zwwm {
namespace {
std::uint32_t high(std::uint64_t id) { return static_cast<std::uint32_t>(id >> 32U); }
std::uint32_t low(std::uint64_t id) { return static_cast<std::uint32_t>(id); }
std::uint64_t join(std::uint32_t hi, std::uint32_t lo) { return (static_cast<std::uint64_t>(hi) << 32U) | lo; }
bool identifier(const std::string& name) {
  return !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
      std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

struct IntegerField {
  const char* name;
  std::uint32_t AnimationConfig::* member;
  int minimum, maximum;
};
constexpr IntegerField integer_fields[] = {
    {"animations.duration-ms", &AnimationConfig::duration_ms, 0, 10000},
    {"animations.open-duration-ms", &AnimationConfig::open_duration_ms, 0, 10000},
    {"animations.close-duration-ms", &AnimationConfig::close_duration_ms, 0, 10000},
    {"animations.move-duration-ms", &AnimationConfig::move_duration_ms, 0, 10000},
    {"animations.fade-duration-ms", &AnimationConfig::fade_duration_ms, 0, 10000},
    {"animations.tag-duration-ms", &AnimationConfig::tag_duration_ms, 0, 10000},
    {"animations.overview-duration-ms", &AnimationConfig::overview_duration_ms, 0, 10000},
    {"animations.open-scale-per-mille", &AnimationConfig::open_scale_per_mille, 500, 1000},
    {"animations.close-scale-per-mille", &AnimationConfig::close_scale_per_mille, 500, 1000},
    {"animations.open-offset-px", &AnimationConfig::open_offset_px, 0, 4096},
    {"animations.close-offset-px", &AnimationConfig::close_offset_px, 0, 4096},
    {"animations.tag-scale-per-mille", &AnimationConfig::tag_scale_per_mille, 0, 1000},
    {"animations.tag-parallax-per-mille", &AnimationConfig::tag_parallax_per_mille, 0, 1000},
    {"animations.tag-fade-per-mille", &AnimationConfig::tag_fade_per_mille, 0, 1000},
    {"animations.cubic-bezier.x1", &AnimationConfig::bezier_x1_per_mille, 0, 1000},
    {"animations.cubic-bezier.y1", &AnimationConfig::bezier_y1_per_mille, 0, 1000},
    {"animations.cubic-bezier.x2", &AnimationConfig::bezier_x2_per_mille, 0, 1000},
    {"animations.cubic-bezier.y2", &AnimationConfig::bezier_y2_per_mille, 0, 1000},
};

std::string validate(const RuntimeConfig& config) {
  auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (context == nullptr) return "could not validate keyboard configuration";
  const auto text = [](const std::string& value) { return value.empty() ? nullptr : value.c_str(); };
  const xkb_rule_names names{text(config.keyboard.rules), text(config.keyboard.model),
      text(config.keyboard.layout), text(config.keyboard.variant), text(config.keyboard.options)};
  auto* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_context_unref(context);
  if (keymap == nullptr) return "invalid keyboard configuration";
  xkb_keymap_unref(keymap);
  const auto shader = [&config](const std::string& name, CustomShaderRole role) {
    return std::any_of(config.shaders.begin(), config.shaders.end(), [&](const auto& value) {
      return value.name == name && value.role == role;
    });
  };
  if (!shader(config.window_shader, CustomShaderRole::window) ||
      !shader(config.border_shader, CustomShaderRole::border) ||
      (!config.background_shader.empty() && !shader(config.background_shader, CustomShaderRole::background)))
    return "selected shaders must exist with matching roles";
  for (const auto& rule : config.window_rules)
    if ((!rule.window_shader.empty() && !shader(rule.window_shader, CustomShaderRole::window)) ||
        (!rule.border_shader.empty() && !shader(rule.border_shader, CustomShaderRole::border)))
      return "window rule references an unknown shader or mismatched role";
  return {};
}
}  // namespace

struct WindowManagerProtocol::Impl {
  struct Transaction {
    Impl* owner;
    std::shared_ptr<const RuntimeConfig> base;
    RuntimeConfig draft;
    std::uint32_t revision;
    std::string error;
    bool finished = false;
    bool editable() {
      if (finished) fail("transaction has already been committed");
      return error.empty();
    }
    void fail(std::string message) { if (error.empty()) error = std::move(message); }
  };
  zwayland::server::Display* display;
  CompositorServer* compositor = nullptr;
  Current current;
  Apply apply;
  std::uint32_t global = 0, revision = 1;
  std::uint32_t serial = 1, changes = 0;
  int pending = -1;
  zwayland::server::Resource* policy_owner = nullptr;
  std::vector<zwayland::server::Resource*> resources, managers;

  struct Destroy {
    void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  };

  zwayland::server::Resource* make(zwayland::server::Client& client,
                                  const zwayland::server::Interface* interface, std::uint32_t id,
                                  std::uint32_t version = 1) {
    auto* resource = client.create_resource(interface, id, version);
    if (resource == nullptr) { client.post_no_memory(); return nullptr; }
    resources.push_back(resource);
    resource->set_data(this);
    resource->set_destroy_handler([this](zwayland::server::Resource& resource) {
      if (policy_owner == &resource) release_policy();
      std::erase(resources, &resource);
      std::erase(managers, &resource);
    });
    return resource;
  }
  void release_policy() {
    policy_owner = nullptr;
    compositor->set_external_policy(false);
  }
  bool permitted(zwayland::server::Client& client) const {
    return policy_owner == nullptr || policy_owner->client == &client;
  }
  void finish(zwayland::server::Client& client, std::uint32_t id, const std::string& error) {
    auto* result = make(client, &protocol::zwwm_window_result_v1_interface, id);
    if (result == nullptr) return;
    result->set_handler(protocol::zwwm_window_result_v1_handler(Destroy{}));
    if (error.empty()) protocol::zwwm_window_result_v1_send_done(*result);
    else protocol::zwwm_window_result_v1_send_failed(*result, error.c_str());
  }
  void input(const PolicyInput& input) {
    if (policy_owner == nullptr || compositor->session_locked()) return;
    switch (input.kind) {
      case PolicyInput::Kind::motion:
        protocol::zwwm_window_manager_v1_send_pointer_motion(*policy_owner, input.time,
            high(input.window), low(input.window), input.x, input.y); break;
      case PolicyInput::Kind::button:
        protocol::zwwm_window_manager_v1_send_pointer_button(*policy_owner, input.time,
            input.code, input.state, high(input.window), low(input.window)); break;
      case PolicyInput::Kind::binding:
        protocol::zwwm_window_manager_v1_send_binding(*policy_owner, input.name.c_str(), input.time, input.state, input.code); break;
      case PolicyInput::Kind::touch:
        protocol::zwwm_window_manager_v1_send_touch(*policy_owner, input.time, static_cast<std::int32_t>(input.code),
            input.state, input.x, input.y); break;
      case PolicyInput::Kind::request:
        protocol::zwwm_window_manager_v1_send_window_request(*policy_owner, high(input.window), low(input.window),
            input.code, input.state, input.time, high(input.output), low(input.output)); break;
    }
  }

  static Transaction* draft(zwayland::server::Resource& resource) { return resource.data<Transaction>(); }
  static CustomShaderConfig* shader(Transaction* transaction, const std::string& name,
                                    const std::string& field) {
    if (!identifier(field)) { transaction->fail("invalid shader value name"); return nullptr; }
    const auto found = std::find_if(transaction->draft.shaders.begin(), transaction->draft.shaders.end(),
                                    [&](const auto& item) { return item.name == name; });
    if (found == transaction->draft.shaders.end()) { transaction->fail("unknown shader " + name); return nullptr; }
    return &*found;
  }
  static void value(CustomShaderConfig* shader, const std::string& name, CustomShaderValue value) {
    const auto found = std::find_if(shader->values.begin(), shader->values.end(),
                                    [&](const auto& item) { return item.first == name; });
    if (found == shader->values.end()) shader->values.emplace_back(name, std::move(value));
    else found->second = std::move(value);
  }

  struct ConfigurationHandler {
    void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
    void set_integer(zwayland::server::Client&, zwayland::server::Resource& resource,
                      const std::string& name, std::int32_t number) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      if (name == "overview.master-ratio") {
        if (number < 10 || number > 90) t->fail("overview.master-ratio must be 10..90");
        else t->draft.overview_master_ratio = static_cast<unsigned>(number);
        return;
      }
      if (name == "overview.scene-radius") {
        if (number < 0 || number > 512) t->fail("overview.scene-radius must be 0..512");
        else t->draft.overview_scene_radius = static_cast<unsigned>(number);
        return;
      }
      if (name == "overview.backdrop-blur-radius" || name == "overview.backdrop-blur-passes") {
        const bool passes = name == "overview.backdrop-blur-passes";
        if (number < (passes ? 1 : 0) || number > (passes ? 8 : 128)) t->fail("out-of-range value for " + name);
        else if (passes) t->draft.overview_backdrop_blur_passes = static_cast<unsigned>(number);
        else t->draft.overview_backdrop_blur_radius = static_cast<unsigned>(number);
        return;
      }
      for (const auto& field : integer_fields) {
        if (name != field.name) continue;
        if (number < field.minimum || number > field.maximum) t->fail("out-of-range value for " + name);
        else t->draft.animations.*(field.member) = static_cast<std::uint32_t>(number);
        return;
      }
      t->fail("unknown integer property " + name);
    }
    void set_boolean(zwayland::server::Client&, zwayland::server::Resource& resource,
                      const std::string& name, std::uint32_t boolean) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      if (boolean > 1) { t->fail("boolean values must be 0 or 1"); return; }
      bool* target = name == "animations.enabled" ? &t->draft.animations.enabled :
          name == "animations.open-window" ? &t->draft.animations.open_window :
          name == "animations.resize" ? &t->draft.animations.resize :
          name == "animations.close" ? &t->draft.animations.close :
          name == "input.reverse-mouse-scrolling" ? &t->draft.input.reverse_mouse_scrolling : nullptr;
      if (target == nullptr) t->fail("unknown boolean property " + name);
      else *target = boolean != 0;
    }
    void set_string(zwayland::server::Client&, zwayland::server::Resource& resource,
                     const std::string& name, const std::string& string) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      if (name == "input.focus-mode") {
        if (string == "by-click") t->draft.input.focus_mode = FocusMode::by_click;
        else if (string == "hover") t->draft.input.focus_mode = FocusMode::hover;
        else t->fail("input.focus-mode must be by-click or hover");
        return;
      }
      if (name == "overview.layout") {
        if (string == "vertical-strip") t->draft.overview_stack_master = false;
        else if (string == "stack-master") t->draft.overview_stack_master = true;
        else t->fail("overview.layout must be vertical-strip or stack-master");
        return;
      }
      if (name == "overview.backdrop-image") {
        if (!string.empty() && !std::filesystem::path(string).is_absolute()) t->fail("overview.backdrop-image must be an absolute PNG path or empty");
        else t->draft.overview_backdrop_image = string;
        return;
      }
      if (name == "animations.tag-transition-axis") {
        if (string == "horizontal") t->draft.animations.tag_transition_axis = TagTransitionAxis::horizontal;
        else if (string == "vertical") t->draft.animations.tag_transition_axis = TagTransitionAxis::vertical;
        else t->fail("animations.tag-transition-axis must be horizontal or vertical");
        return;
      }
      std::string* target = name == "keyboard.rules" ? &t->draft.keyboard.rules :
          name == "keyboard.model" ? &t->draft.keyboard.model :
          name == "keyboard.layout" ? &t->draft.keyboard.layout :
          name == "keyboard.variant" ? &t->draft.keyboard.variant :
          name == "keyboard.options" ? &t->draft.keyboard.options :
          name == "shaders.window" ? &t->draft.window_shader :
          name == "shaders.border" ? &t->draft.border_shader :
          name == "shaders.background" ? &t->draft.background_shader : nullptr;
      if (target == nullptr) t->fail("unknown string property " + name);
      else *target = string;
    }
    void clear_bindings(zwayland::server::Client&, zwayland::server::Resource& resource) {
      auto* t = draft(resource); if (t->editable()) t->draft.keybindings.clear();
    }
    void add_binding(zwayland::server::Client&, zwayland::server::Resource& resource,
                      const std::string& modifiers, const std::string& key,
                      const std::string& action, const std::string& argument) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      lang::Value::List values;
      for (const auto* string : {&modifiers, &key, &action, &argument}) values.values.emplace_back(*string);
      lang::Config typed;
      typed.assignments.push_back({{}, "bind", lang::Value{std::move(values)}});
      const auto result = compile_runtime_config(typed);
      if (!result.ok() || result.config->keybindings.empty()) {
        t->fail(result.diagnostics.empty() ? "invalid binding" : result.diagnostics.front().message);
        return;
      }
      t->draft.keybindings.push_back(result.config->keybindings.front());
    }
    void clear_window_rules(zwayland::server::Client&, zwayland::server::Resource& resource) {
      auto* t = draft(resource); if (t->editable()) t->draft.window_rules.clear();
    }
    void add_window_rule(zwayland::server::Client&, zwayland::server::Resource& resource,
                          const std::string& app, const std::string& title, std::int32_t floating,
                          std::int32_t opacity, const std::string& window_shader, const std::string& border_shader) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      if ((app.empty() && title.empty()) || floating < -1 || floating > 1 || opacity < -1 || opacity > 100) {
        t->fail("invalid window rule"); return;
      }
      WindowRule rule;
      rule.app_id = app; rule.title = title;
      if (floating >= 0) rule.floating = floating != 0;
      if (opacity >= 0) rule.opacity = opacity / 100.0F;
      rule.window_shader = window_shader; rule.border_shader = border_shader;
      t->draft.window_rules.push_back(std::move(rule));
    }
    void clear_layer_rules(zwayland::server::Client&, zwayland::server::Resource& resource) {
      auto* t = draft(resource); if (t->editable()) t->draft.layer_rules.clear();
    }
    void add_layer_rule(zwayland::server::Client&, zwayland::server::Resource& resource,
                         const std::string& name, std::uint32_t radius, std::uint32_t opacity, std::uint32_t alpha) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      if (name.empty() || radius > 64 || opacity > 100 || alpha > 100) { t->fail("invalid layer rule"); return; }
      t->draft.layer_rules.push_back({name, {radius, alpha / 100.0F, opacity / 100.0F}});
    }
    void set_output(zwayland::server::Client&, zwayland::server::Resource& resource,
                     const std::string& connector, std::uint32_t scale, std::uint32_t transform) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      if (scale < 250 || scale > 8000 || transform > 3) { t->fail("invalid output settings"); return; }
      OutputConfig* output = &t->draft.output;
      if (!connector.empty()) {
        auto found = std::find_if(t->draft.outputs.begin(), t->draft.outputs.end(),
                                  [&](const auto& item) { return item.first == connector; });
        if (found == t->draft.outputs.end()) {
          t->draft.outputs.emplace_back(connector, t->draft.output);
          output = &t->draft.outputs.back().second;
        } else output = &found->second;
      }
      output->scale_per_mille = scale; output->transform = static_cast<OutputTransform>(transform);
    }
    void clear_outputs(zwayland::server::Client&, zwayland::server::Resource& resource) {
      auto* t = draft(resource); if (t->editable()) t->draft.outputs.clear();
    }
    void clear_shaders(zwayland::server::Client&, zwayland::server::Resource& resource) {
      auto* t = draft(resource); if (t->editable()) t->draft.shaders.clear();
    }
    void define_shader(zwayland::server::Client&, zwayland::server::Resource& resource,
                        const std::string& name, std::uint32_t role, const std::string& source) {
      auto* t = draft(resource);
      if (!t->editable()) return;
      if (name.empty() || role > 2 || !std::filesystem::path(source).is_absolute()) {
        t->fail("shader requires a name, valid role, and absolute source filename"); return;
      }
      CustomShaderConfig definition{name, static_cast<CustomShaderRole>(role), source, {}};
      const auto found = std::find_if(t->draft.shaders.begin(), t->draft.shaders.end(),
                                      [&](const auto& item) { return item.name == name; });
      if (found == t->draft.shaders.end()) t->draft.shaders.push_back(std::move(definition));
      else *found = std::move(definition);
    }
    void shader_integer(zwayland::server::Client&, zwayland::server::Resource& resource,
                         const std::string& name, const std::string& field, std::int32_t number) {
      auto* t = draft(resource); if (!t->editable()) return;
      if (auto* selected = shader(t, name, field)) value(selected, field, number);
    }
    void shader_boolean(zwayland::server::Client&, zwayland::server::Resource& resource,
                         const std::string& name, const std::string& field, std::uint32_t boolean) {
      auto* t = draft(resource); if (!t->editable()) return;
      if (boolean > 1) { t->fail("boolean values must be 0 or 1"); return; }
      if (auto* selected = shader(t, name, field)) value(selected, field, boolean != 0);
    }
    void shader_color(zwayland::server::Client&, zwayland::server::Resource& resource,
                       const std::string& name, const std::string& field, std::uint32_t rgba) {
      auto* t = draft(resource); if (!t->editable()) return;
      if (auto* selected = shader(t, name, field)) value(selected, field,
          Color{static_cast<std::uint8_t>(rgba >> 24U), static_cast<std::uint8_t>(rgba >> 16U),
                static_cast<std::uint8_t>(rgba >> 8U), static_cast<std::uint8_t>(rgba)});
    }
    void shader_colors(zwayland::server::Client&, zwayland::server::Resource& resource,
                        const std::string& name, const std::string& field, std::span<const std::byte> rgba) {
      auto* t = draft(resource); if (!t->editable()) return;
      if (rgba.empty() || rgba.size() > 80 || rgba.size() % 4 != 0) {
        t->fail("shader gradient requires one to twenty RGBA colors"); return;
      }
      std::vector<Color> colors;
      for (std::size_t index = 0; index < rgba.size(); index += 4)
        colors.push_back({std::to_integer<std::uint8_t>(rgba[index]), std::to_integer<std::uint8_t>(rgba[index + 1]),
                          std::to_integer<std::uint8_t>(rgba[index + 2]), std::to_integer<std::uint8_t>(rgba[index + 3])});
      if (auto* selected = shader(t, name, field)) value(selected, field, std::move(colors));
    }
    void shader_vector(zwayland::server::Client&, zwayland::server::Resource& resource,
                        const std::string& name, const std::string& field, std::span<const std::byte> components) {
      auto* t = draft(resource); if (!t->editable()) return;
      if (components.size() < 8 || components.size() > 16 || components.size() % 4 != 0) {
        t->fail("shader vector requires two to four 32-bit integers"); return;
      }
      std::vector<std::int32_t> integers(components.size() / 4);
      std::memcpy(integers.data(), components.data(), components.size());
      if (auto* selected = shader(t, name, field)) value(selected, field, std::move(integers));
    }
    void commit(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
      auto* t = draft(resource);
      auto* result = t->owner->make(client, &protocol::zwwm_configuration_result_v1_interface, id);
      if (result == nullptr) return;
      result->set_handler(protocol::zwwm_configuration_result_v1_handler(Destroy{}));
      if (t->finished) t->fail("transaction has already been committed");
      t->finished = true;
      if (t->error.empty() && (t->revision != t->owner->revision || t->base != t->owner->current()))
        t->fail("configuration changed; create a new transaction");
      if (t->error.empty()) t->error = validate(t->draft);
      if (t->error.empty()) t->error = t->owner->apply(std::make_shared<const RuntimeConfig>(t->draft));
      if (t->error.empty()) protocol::zwwm_configuration_result_v1_send_applied(*result, t->owner->revision);
      else protocol::zwwm_configuration_result_v1_send_failed(*result, t->error.c_str());
    }
  };

  struct ManagerHandler {
    void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
    void create_configuration(zwayland::server::Client& client, zwayland::server::Resource& manager,
                               std::uint32_t id) {
      auto* self = manager.data<Impl>();
      auto base = self->current();
      auto* resource = self->make(client, &protocol::zwwm_configuration_v1_interface, id);
      if (resource == nullptr) return;
      auto* transaction = new (std::nothrow) Transaction{self, base, *base, self->revision, {}, false};
      if (transaction == nullptr) { resource->destroy(); client.post_no_memory(); return; }
      resource->set_data(transaction);
      resource->set_handler(protocol::zwwm_configuration_v1_handler(ConfigurationHandler{}));
      resource->set_destroy_handler([self](zwayland::server::Resource& resource) {
        std::erase(self->resources, &resource);
        delete draft(resource);
      });
      protocol::zwwm_configuration_v1_send_base_revision(*resource, self->revision);
    }
    void get_state(zwayland::server::Client& client, zwayland::server::Resource& manager, std::uint32_t id) {
      auto* self = manager.data<Impl>();
      auto* state = self->make(client, &protocol::zwwm_window_state_v1_interface, id);
      if (state == nullptr) return;
      state->set_handler(protocol::zwwm_window_state_v1_handler(Destroy{}));
      const auto tags = self->compositor->tags();
      for (const auto& output : self->compositor->outputs()) {
        const auto tag = std::find_if(tags.begin(), tags.end(), [&](const auto& item) { return item.output == output.id; });
        protocol::zwwm_window_state_v1_send_output(*state, high(output.id.value), low(output.id.value), output.connector.c_str(),
            output.logical_x, output.logical_y, output.logical_width, output.logical_height, tag == tags.end() ? 1 : tag->active);
      }
      for (const auto& window : self->compositor->managed_windows())
        protocol::zwwm_window_state_v1_send_window(*state, high(window.id), low(window.id), high(window.output.value), low(window.output.value),
            window.tag, window.state, window.app_id.c_str(), window.title.c_str(), high(window.parent), low(window.parent),
            window.bounds.origin.x, window.bounds.origin.y,
            window.bounds.size.width, window.bounds.size.height, window.min_width, window.min_height, window.max_width, window.max_height);
      protocol::zwwm_window_state_v1_send_session_locked(*state, self->compositor->session_locked() ? 1 : 0);
      protocol::zwwm_window_state_v1_send_done(*state, self->serial);
    }
    void claim_policy(zwayland::server::Client& client, zwayland::server::Resource& manager, std::uint32_t id) {
      auto* self = manager.data<Impl>();
      if (self->policy_owner != nullptr && self->policy_owner != &manager) {
        self->finish(client, id, "another manager owns window policy"); return;
      }
      self->policy_owner = &manager;
      self->compositor->set_external_policy(true);
      self->finish(client, id, {});
    }
    void release_policy(zwayland::server::Client&, zwayland::server::Resource& manager) {
      auto* self = manager.data<Impl>();
      if (self->policy_owner == &manager) self->release_policy();
    }
    void window_action(zwayland::server::Client& client, zwayland::server::Resource& manager,
                        std::uint32_t id, std::uint32_t hi, std::uint32_t lo, std::uint32_t action, std::uint32_t enabled) {
      auto* self = manager.data<Impl>();
      std::string error;
      if (!self->permitted(client)) error = "another manager owns window policy";
      else if (action > 5 || enabled > 1) error = "invalid window action";
      else self->compositor->control_window(join(hi, lo), static_cast<WindowAction>(action), enabled != 0, &error);
      self->finish(client, id, error);
    }
    void assign_window(zwayland::server::Client& client, zwayland::server::Resource& manager,
                        std::uint32_t id, std::uint32_t hi, std::uint32_t lo,
                        std::uint32_t output_hi, std::uint32_t output_lo, std::uint32_t tag) {
      auto* self = manager.data<Impl>();
      std::string error;
      if (!self->permitted(client)) error = "another manager owns window policy";
      else if (tag < 1 || tag > 9) error = "invalid tag";
      else self->compositor->assign_window(join(hi, lo), OutputId{join(output_hi, output_lo)}, static_cast<std::uint8_t>(tag), &error);
      self->finish(client, id, error);
    }
    void select_tag(zwayland::server::Client& client, zwayland::server::Resource& manager,
                     std::uint32_t id, std::uint32_t hi, std::uint32_t lo, std::uint32_t tag) {
      auto* self = manager.data<Impl>();
      std::string error;
      if (!self->permitted(client)) error = "another manager owns window policy";
      else if (tag < 1 || tag > 9) error = "invalid tag";
      else self->compositor->select_tag(OutputId{join(hi, lo)}, static_cast<std::uint8_t>(tag), &error);
      self->finish(client, id, error);
    }
    void dispatch(zwayland::server::Client& client, zwayland::server::Resource& manager,
                   std::uint32_t id, const std::string& action, const std::string& argument) {
      auto* self = manager.data<Impl>();
      std::string error;
      if (!self->permitted(client)) error = "another manager owns window policy";
      else if (self->compositor->session_locked()) error = "session is locked";
      else if (!self->compositor->dispatch_action(action, argument, &error) && error.empty()) error = "invalid action";
      self->finish(client, id, error);
    }
  };
};

WindowManagerProtocol::WindowManagerProtocol(zwayland::server::Display* display, CompositorServer* compositor, Current current, Apply apply)
    : impl_(std::make_unique<Impl>()) {
  impl_->display = display; impl_->current = std::move(current); impl_->apply = std::move(apply);
  impl_->compositor = compositor;
  compositor->set_policy_input_handler([self = impl_.get()](const PolicyInput& input) { self->input(input); });
  impl_->global = display->add_global(&protocol::zwwm_window_manager_v1_interface, 2,
      [self = impl_.get()](zwayland::server::Client& client, std::uint32_t version, std::uint32_t id) {
        auto* resource = self->make(client, &protocol::zwwm_window_manager_v1_interface, id, version);
        if (resource == nullptr) return;
        resource->set_handler(protocol::zwwm_window_manager_v1_handler(Impl::ManagerHandler{}));
        self->managers.push_back(resource);
        protocol::zwwm_window_manager_v1_send_revision(*resource, self->revision);
      });
}
WindowManagerProtocol::~WindowManagerProtocol() {
  if (impl_->pending >= 0) impl_->display->event_loop().remove(impl_->pending);
  impl_->compositor->set_policy_input_handler({});
  impl_->display->destroy_global(impl_->global);
  while (!impl_->resources.empty()) impl_->resources.back()->destroy();
}
void WindowManagerProtocol::config_changed() {
  ++impl_->revision;
  for (auto* manager : impl_->managers) protocol::zwwm_window_manager_v1_send_revision(*manager, impl_->revision);
}
void WindowManagerProtocol::emit(const std::string& event) {
  const std::uint32_t bit = event == "window" ? 1U : event == "output" ? 2U : event == "tag" ? 4U :
      event == "config" ? 8U : event == "camera" ? 16U : event == "keyboard" ? 32U : event == "session" ? 64U : 0U;
  if (bit == 0) return;
  ++impl_->serial;
  impl_->changes |= bit;
  if (impl_->pending >= 0) return;
  impl_->pending = impl_->display->add_idle([self = impl_.get()] {
    self->pending = -1;
    const auto changes = self->changes;
    self->changes = 0;
    for (auto* manager : self->managers)
      protocol::zwwm_window_manager_v1_send_changed(*manager, self->serial, changes);
  });
}
void WindowManagerProtocol::gesture(const ZwwmPluginGesture& gesture) {
  auto* owner = impl_->policy_owner;
  if (owner == nullptr || impl_->compositor->session_locked()) return;
  protocol::zwwm_window_manager_v1_send_gesture(*owner, static_cast<std::uint32_t>(gesture.kind),
      static_cast<std::uint32_t>(gesture.phase), gesture.fingers, high(gesture.time_usec), low(gesture.time_usec),
      gesture.dx, gesture.dy, gesture.scale, gesture.angle_delta, gesture.cancelled ? 1 : 0,
      gesture.device == nullptr ? "" : gesture.device, gesture.output == nullptr ? "" : gesture.output);
}
}  // namespace zwwm
