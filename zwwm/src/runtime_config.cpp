#include "zwwm/runtime_config.hpp"
#include "zwwm/renderer/opengl.hpp"

#include <zwayland/server/display.hpp>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fnmatch.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <cmath>

namespace zwwm {
namespace {

constexpr std::int64_t kMaximumDimension = std::numeric_limits<std::int32_t>::max();

bool install_default_file(const std::filesystem::path& source,
                          const std::filesystem::path& destination) {
  if (std::filesystem::exists(destination) || !std::filesystem::is_regular_file(source)) return false;
  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) return false;
  const auto temporary = destination.string() + ".tmp-" + std::to_string(getpid());
  std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, error);
  if (error) { std::filesystem::remove(temporary); return false; }
  std::filesystem::rename(temporary, destination, error);
  if (error) { std::filesystem::remove(temporary); return false; }
  return true;
}

bool bootstrap_user_config(const std::filesystem::path& config) {
  if (config.empty()) return false;
  const std::filesystem::path shader_dir = config.parent_path() / "shaders";
  for (const char* name : {"window.frag", "border.frag", "background.frag"}) {
    if (!install_default_file(std::filesystem::path(ZWWM_DATA_DIR) / "shaders" / name,
                               shader_dir / name) && !std::filesystem::exists(shader_dir / name))
      return false;
  }
  if (!install_default_file(std::filesystem::path(ZWWM_DATA_DIR) / "background.png",
                            config.parent_path() / "background.png") &&
      !std::filesystem::exists(config.parent_path() / "background.png"))
    return false;
  return install_default_file(std::filesystem::path(ZWWM_DATA_DIR) / "config.zw", config) ||
      install_default_file("/etc/xdg/zwwm/config.zw", config) || std::filesystem::exists(config);
}

const lang::Assignment* assignment(const lang::Config& config, std::string_view key) {
  const auto item = std::find_if(config.assignments.rbegin(), config.assignments.rend(),
                                 [key](const auto& value) { return value.key == key; });
  return item == config.assignments.rend() ? nullptr : &*item;
}

const std::string* resolved_string(const lang::Value& value,
                                   const std::unordered_map<std::string, std::string>& variables) {
  if (const auto* text = lang::as_string(value); text != nullptr) return text;
  if (const auto* reference = std::get_if<lang::VariableReference>(&value.data); reference != nullptr) {
    const auto item = variables.find(reference->name);
    return item == variables.end() ? nullptr : &item->second;
  }
  return nullptr;
}

void error(std::vector<lang::Diagnostic>& diagnostics, lang::SourceLocation location,
           std::string message) {
  diagnostics.push_back({lang::DiagnosticLevel::error, location, std::move(message)});
}

bool dimension(const lang::Value::Object& object, std::string_view name, std::uint32_t& target,
               lang::SourceLocation location, std::vector<lang::Diagnostic>& diagnostics,
               bool positive = false) {
  const auto* value = lang::find_field(object, name);
  if (value == nullptr) return false;
  const auto* integer = lang::as_integer(*value);
  if (integer == nullptr || *integer < (positive ? 1 : 0) || *integer > kMaximumDimension) {
    error(diagnostics, location, std::string(name) + (positive ? " must be a positive integer"
                                                     : " must be a nonnegative integer"));
    return true;
  }
  target = static_cast<std::uint32_t>(*integer);
  return true;
}

bool decode_color(std::string_view text, Color& color) {
  if (text.size() != 7 && text.size() != 9) return false;
  if (text.front() != '#') return false;
  std::array<std::uint8_t, 4> channels{0, 0, 0, 255};
  for (std::size_t index = 0; index < (text.size() - 1) / 2; ++index) {
    unsigned value = 0;
    const auto begin = text.data() + 1 + index * 2;
    const auto result = std::from_chars(begin, begin + 2, value, 16);
    if (result.ec != std::errc{} || result.ptr != begin + 2) return false;
    channels[index] = static_cast<std::uint8_t>(value);
  }
  color = {channels[0], channels[1], channels[2], channels[3]};
  return true;
}

std::optional<CustomShaderValue> shader_value(
    const lang::Value& value, const std::unordered_map<std::string, std::string>& variables) {
  if (const auto* boolean = lang::as_boolean(value); boolean != nullptr) return *boolean;
  if (const auto* integer = lang::as_integer(value); integer != nullptr &&
      *integer >= std::numeric_limits<std::int32_t>::min() &&
      *integer <= std::numeric_limits<std::int32_t>::max())
    return static_cast<std::int32_t>(*integer);
  if (const auto* text = resolved_string(value, variables); text != nullptr) {
    Color color;
    if (decode_color(*text, color)) return color;
    std::int32_t integer = 0;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), integer);
    if (parsed.ec == std::errc{} && parsed.ptr == text->data() + text->size()) return integer;
    return std::nullopt;
  }
  const auto* array = lang::as_array(value);
  if (array == nullptr) return std::nullopt;
  if (!array->values.empty() && array->values.size() <= 20) {
    std::vector<Color> colors;
    colors.reserve(array->values.size());
    for (const auto& element : array->values) {
      const auto* text = resolved_string(element, variables);
      Color color;
      if (text == nullptr || !decode_color(*text, color)) {
        colors.clear();
        break;
      }
      colors.push_back(color);
    }
    if (!colors.empty()) return colors;
  }
  if (array->values.size() < 2 || array->values.size() > 4) return std::nullopt;
  std::vector<std::int32_t> components;
  components.reserve(array->values.size());
  for (const auto& component : array->values) {
    const auto* integer = lang::as_integer(component);
    if (integer == nullptr || *integer < std::numeric_limits<std::int32_t>::min() ||
        *integer > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
    components.push_back(static_cast<std::int32_t>(*integer));
  }
  return components;
}

std::optional<CustomShaderRole> shader_role(std::string_view value) {
  if (value == "window") return CustomShaderRole::window;
  if (value == "border") return CustomShaderRole::border;
  if (value == "background") return CustomShaderRole::background;
  return std::nullopt;
}

void extract_color(const lang::Value::Object& object, std::string_view name, Color& target,
                   lang::SourceLocation location, std::vector<lang::Diagnostic>& diagnostics) {
  const auto* value = lang::find_field(object, name);
  if (value == nullptr) return;
  const auto* text = lang::as_string(*value);
  if (text == nullptr || !decode_color(*text, target)) {
    error(diagnostics, location, std::string(name) + " must be #RRGGBB or #RRGGBBAA");
  }
}

void print_diagnostics(const std::string& path, const std::vector<lang::Diagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    std::fprintf(stderr, "%s:%zu:%zu: %s: %s\n", path.c_str(), diagnostic.location.line,
                 diagnostic.location.column,
                 diagnostic.level == lang::DiagnosticLevel::error ? "error" : "warning",
                 diagnostic.message.c_str());
  }
}

bool parse_output_mode(std::string_view text, OutputMode& mode) {
  if (text == "preferred") { mode = {}; return true; }
  const auto x = text.find('x');
  const auto at = text.find('@', x == std::string_view::npos ? 0 : x + 1);
  if (x == std::string_view::npos || at == std::string_view::npos) return false;
  std::uint32_t width = 0, height = 0;
  double refresh = 0.0;
  const auto width_result = std::from_chars(text.data(), text.data() + x, width);
  const auto height_result = std::from_chars(text.data() + x + 1, text.data() + at, height);
  const auto refresh_result = std::from_chars(text.data() + at + 1, text.data() + text.size(), refresh);
  if (width_result.ec != std::errc{} || width_result.ptr != text.data() + x || width == 0 ||
      height_result.ec != std::errc{} || height_result.ptr != text.data() + at || height == 0 ||
      refresh_result.ec != std::errc{} || refresh_result.ptr != text.data() + text.size() || refresh <= 0.0 ||
      refresh > std::numeric_limits<std::uint32_t>::max() / 1000.0) return false;
  mode = {false, width, height, static_cast<std::uint32_t>(std::lround(refresh * 1000.0))};
  return true;
}

void parse_output_config(const lang::Value::Object& object, OutputConfig& output,
                         lang::SourceLocation location,
                         std::vector<lang::Diagnostic>& diagnostics,
                         std::string_view prefix) {
  const std::string field_prefix(prefix);
  if (const auto* value = lang::find_field(object, "mode"); value != nullptr) {
    const auto* text = lang::as_string(*value);
    if (text == nullptr || !parse_output_mode(*text, output.mode))
      error(diagnostics, location,
            field_prefix + "mode must be \"preferred\" or WIDTHxHEIGHT@HZ");
  }
  if (const auto* value = lang::find_field(object, "scale-per-mille"); value != nullptr) {
    const auto* scale = lang::as_integer(*value);
    if (scale == nullptr || *scale < 250 || *scale > 8000)
      error(diagnostics, location,
            field_prefix + "scale-per-mille must be an integer from 250 to 8000");
    else output.scale_per_mille = static_cast<std::uint32_t>(*scale);
  }
  if (const auto* value = lang::find_field(object, "bit-depth"); value != nullptr) {
    const auto* depth = lang::as_integer(*value);
    if (depth == nullptr || (*depth != 8 && *depth != 10))
      error(diagnostics, location, field_prefix + "bit-depth must be 8 or 10");
    else output.bit_depth = static_cast<std::uint32_t>(*depth);
  }
  if (const auto* value = lang::find_field(object, "transform"); value != nullptr) {
    const auto* text = lang::as_string(*value);
    if (text == nullptr)
      error(diagnostics, location, field_prefix + "transform must be a string");
    else if (*text == "normal") output.transform = OutputTransform::normal;
    else if (*text == "90") output.transform = OutputTransform::rotate_90;
    else if (*text == "180") output.transform = OutputTransform::rotate_180;
    else if (*text == "270") output.transform = OutputTransform::rotate_270;
    else
      error(diagnostics, location,
            field_prefix + "transform must be \"normal\", \"90\", \"180\", or \"270\"");
  }
}

std::optional<KeyAction> key_action(std::string_view text) {
  if (text == "exec") return KeyAction::exec;
  if (text == "reload") return KeyAction::reload;
  if (text == "exit") return KeyAction::exit;
  if (text == "focus") return KeyAction::focus;
  if (text == "killactive") return KeyAction::killactive;
  if (text == "killsession") return KeyAction::killsession;
  if (text == "togglefloating") return KeyAction::togglefloating;
  if (text == "toggle-fullscreen" || text == "fullscreen") return KeyAction::fullscreen;
  if (text == "tag") return KeyAction::tag;
  if (text == "movetotag") return KeyAction::movetotag;
  if (text == "zoomin") return KeyAction::zoomin;
  if (text == "zoomout") return KeyAction::zoomout;
  return std::nullopt;
}

bool parse_rule_size(std::string_view text, WindowRule& rule) {
  const auto x = text.find('x');
  if (x == std::string_view::npos) return false;
  const auto parse = [](std::string_view value, std::uint32_t& result, bool& percent) {
    percent = value.ends_with('%');
    if (percent) value.remove_suffix(1);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result > 0 &&
           (!percent || result <= 100);
  };
  return parse(text.substr(0, x), rule.width, rule.width_percent) &&
         parse(text.substr(x + 1), rule.height, rule.height_percent);
}

}  // namespace

std::array<float, 4> Color::normalized() const {
  constexpr float scale = 1.0F / 255.0F;
  return {red * scale, green * scale, blue * scale, alpha * scale};
}

std::uint32_t Color::argb8888() const {
  return static_cast<std::uint32_t>(alpha) << 24U | static_cast<std::uint32_t>(red) << 16U |
         static_cast<std::uint32_t>(green) << 8U | blue;
}

std::uint32_t RuntimeConfig::border_width() const {
  return static_cast<std::uint32_t>(std::max(0, shader_integer(border_shader, "width")));
}

std::uint32_t RuntimeConfig::radius() const {
  return static_cast<std::uint32_t>(std::max(0, shader_integer(border_shader, "radius")));
}

std::int32_t RuntimeConfig::shader_integer(std::string_view shader_name, std::string_view value_name,
                                           std::int32_t fallback) const {
  const auto shader = std::find_if(shaders.begin(), shaders.end(), [&](const auto& candidate) {
    return candidate.name == shader_name;
  });
  if (shader == shaders.end()) return fallback;
  const auto value = std::find_if(shader->values.begin(), shader->values.end(), [&](const auto& candidate) {
    return candidate.first == value_name;
  });
  if (value == shader->values.end()) return fallback;
  const auto* integer = std::get_if<std::int32_t>(&value->second);
  return integer == nullptr ? fallback : *integer;
}

renderer::Size OutputConfig::logical_size(renderer::Size physical) const {
  if (transform == OutputTransform::rotate_90 || transform == OutputTransform::rotate_270)
    std::swap(physical.width, physical.height);
  return {std::max(1U, static_cast<std::uint32_t>((static_cast<std::uint64_t>(physical.width) * 1000U + scale_per_mille / 2U) / scale_per_mille)),
          std::max(1U, static_cast<std::uint32_t>((static_cast<std::uint64_t>(physical.height) * 1000U + scale_per_mille / 2U) / scale_per_mille))};
}

renderer::Point OutputConfig::logical_to_physical(renderer::Point point, renderer::Size physical) const {
  const auto scaled_x = static_cast<std::int32_t>(std::lround(point.x * scale_per_mille / 1000.0));
  const auto scaled_y = static_cast<std::int32_t>(std::lround(point.y * scale_per_mille / 1000.0));
  switch (transform) {
    case OutputTransform::rotate_90: return {static_cast<std::int32_t>(physical.width) - 1 - scaled_y, scaled_x};
    case OutputTransform::rotate_180: return {static_cast<std::int32_t>(physical.width) - 1 - scaled_x, static_cast<std::int32_t>(physical.height) - 1 - scaled_y};
    case OutputTransform::rotate_270: return {scaled_y, static_cast<std::int32_t>(physical.height) - 1 - scaled_x};
    default: return {scaled_x, scaled_y};
  }
}

renderer::Point OutputConfig::physical_to_logical(renderer::Point point, renderer::Size physical) const {
  renderer::Point untransformed = point;
  switch (transform) {
    case OutputTransform::rotate_90: untransformed = {point.y, static_cast<std::int32_t>(physical.width) - 1 - point.x}; break;
    case OutputTransform::rotate_180: untransformed = {static_cast<std::int32_t>(physical.width) - 1 - point.x, static_cast<std::int32_t>(physical.height) - 1 - point.y}; break;
    case OutputTransform::rotate_270: untransformed = {static_cast<std::int32_t>(physical.height) - 1 - point.y, point.x}; break;
    default: break;
  }
  return {static_cast<std::int32_t>(std::lround(untransformed.x * 1000.0 / scale_per_mille)),
          static_cast<std::int32_t>(std::lround(untransformed.y * 1000.0 / scale_per_mille))};
}

renderer::Rect OutputConfig::physical_bounds(renderer::Rect bounds, renderer::Size physical) const {
  const auto scale = [this](std::int64_t value) {
    return static_cast<std::int32_t>(std::lround(value * scale_per_mille / 1000.0));
  };
  const auto x = scale(bounds.origin.x), y = scale(bounds.origin.y);
  const auto width = static_cast<std::uint32_t>(std::max(1, scale(bounds.size.width)));
  const auto height = static_cast<std::uint32_t>(std::max(1, scale(bounds.size.height)));
  switch (transform) {
    case OutputTransform::rotate_90:
      return {{static_cast<std::int32_t>(physical.width) - y - static_cast<std::int32_t>(height), x}, {height, width}};
    case OutputTransform::rotate_180:
      return {{static_cast<std::int32_t>(physical.width) - x - static_cast<std::int32_t>(width),
               static_cast<std::int32_t>(physical.height) - y - static_cast<std::int32_t>(height)}, {width, height}};
    case OutputTransform::rotate_270:
      return {{y, static_cast<std::int32_t>(physical.height) - x - static_cast<std::int32_t>(width)}, {height, width}};
    default: return {{x, y}, {width, height}};
  }
}

std::uint32_t OutputConfig::integer_scale() const { return std::max(1U, (scale_per_mille + 999U) / 1000U); }
std::uint32_t OutputConfig::fractional_scale_120() const { return std::max(1U, (scale_per_mille * 120U + 500U) / 1000U); }

std::vector<layout::Placement> RuntimeConfig::placements(
    const std::vector<layout::WindowId>& windows, renderer::Rect work_area) const {
  layout::MasterStack master_stack;
  (void)master_stack.set_master_count(layout.master_count);
  (void)master_stack.set_master_ratio(static_cast<float>(layout.master_ratio_percent) / 100.0F);
  const std::uint32_t outer_gap = layout.smart_gaps && windows.size() == 1 ? 0 : layout.outer_gap;
  (void)master_stack.set_outer_gap(static_cast<std::int32_t>(outer_gap));
  (void)master_stack.set_inner_gap(static_cast<std::int32_t>(layout.inner_gap));
  return master_stack.arrange(windows, work_area);
}

renderer::Rect RuntimeConfig::content_bounds(renderer::Rect placement) const {
  const auto border = static_cast<std::int64_t>(border_width());
  const auto width = std::max<std::int64_t>(1, static_cast<std::int64_t>(placement.size.width) - 2 * border);
  const auto height = std::max<std::int64_t>(1, static_cast<std::int64_t>(placement.size.height) - 2 * border);
  return {{static_cast<std::int32_t>(static_cast<std::int64_t>(placement.origin.x) + border),
           static_cast<std::int32_t>(static_cast<std::int64_t>(placement.origin.y) + border)},
          {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}};
}

LayerEffect RuntimeConfig::layer_effect(std::string_view name_space) const {
  LayerEffect result;
  const std::string value(name_space);
  for (const auto& rule : layer_rules) {
    if (fnmatch(rule.name_space.c_str(), value.c_str(), 0) == 0) result = rule.effect;
  }
  return result;
}

const OutputConfig& RuntimeConfig::output_for(std::string_view connector) const {
  auto configured = std::find_if(outputs.rbegin(), outputs.rend(), [&](const auto& candidate) {
    return candidate.first == connector;
  });
  if (configured == outputs.rend()) {
    const auto separator = connector.find('-');
    if (separator != std::string_view::npos && connector.substr(0, separator).starts_with("card")) {
      const auto short_name = connector.substr(separator + 1);
      configured = std::find_if(outputs.rbegin(), outputs.rend(), [&](const auto& candidate) {
        return candidate.first == short_name;
      });
    }
  }
  return configured == outputs.rend() ? output : configured->second;
}

bool RuntimeConfigResult::ok() const {
  return config != nullptr && std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
           return diagnostic.level == lang::DiagnosticLevel::error;
         });
}

RuntimeConfigResult compile_runtime_config(const lang::Config& parsed) {
  RuntimeConfigResult result;
  result.config = std::make_shared<RuntimeConfig>();
  auto mutable_config = std::const_pointer_cast<RuntimeConfig>(result.config);
  std::unordered_map<std::string, std::string> variables;
  for (const auto& item : parsed.assignments) {
    if (item.key.empty() || !std::isupper(static_cast<unsigned char>(item.key.front()))) continue;
    if (const auto* value = lang::as_string(item.value); value != nullptr) variables.insert_or_assign(item.key, *value);
  }

  if (const auto* item = assignment(parsed, "environment"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "environment must be an object");
    } else {
      for (std::size_t index = 0; index < object->names.size(); ++index) {
        const auto& name = object->names[index];
        const auto& value = object->values[index];
        const auto* text = resolved_string(value, variables);
        const bool valid_name = !name.empty() && (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') &&
            std::all_of(std::next(name.begin()), name.end(), [](unsigned char character) {
              return std::isalnum(character) || character == '_';
            });
        if (!valid_name) error(result.diagnostics, item->location, "environment variable names must be shell identifiers");
        else if (text == nullptr) error(result.diagnostics, item->location, "environment values must resolve to strings");
        else mutable_config->environment.emplace_back(name, *text);
      }
    }
  }

  if (const auto* item = assignment(parsed, "exec-sh-on-startup"); item != nullptr) {
    const auto* commands = lang::as_array(item->value);
    if (commands == nullptr) {
      error(result.diagnostics, item->location, "exec-sh-on-startup must be an array");
    } else {
      for (const auto& value : commands->values) {
        const auto* command = resolved_string(value, variables);
        if (command == nullptr || command->empty())
          error(result.diagnostics, item->location,
                "exec-sh-on-startup entries must resolve to nonempty strings");
        else
          mutable_config->startup_commands.push_back(*command);
      }
    }
  }

  for (const auto& item : parsed.assignments) {
    if (item.key != "bind") continue;
    const auto* list = lang::as_list(item.value);
    if (list == nullptr || list->values.size() != 4) continue;
    const auto* modifiers = resolved_string(list->values[0], variables);
    const auto* key = resolved_string(list->values[1], variables);
    const auto* action = resolved_string(list->values[2], variables);
    const auto* argument = resolved_string(list->values[3], variables);
    if (modifiers == nullptr || key == nullptr || action == nullptr || argument == nullptr ||
        key->empty() || action->empty()) {
      error(result.diagnostics, item.location,
            "bind key and action must resolve to nonempty strings");
      continue;
    }
    const auto typed_action = key_action(*action);
    if (!typed_action) {
      error(result.diagnostics, item.location, "unknown bind action");
      continue;
    }
    if ((*typed_action == KeyAction::tag || *typed_action == KeyAction::movetotag) &&
        (*argument < "1" || *argument > "9" || argument->size() != 1)) {
      error(result.diagnostics, item.location, "tag action argument must be 1..9");
      continue;
    }
    mutable_config->keybindings.push_back({*modifiers, *key, *typed_action, *argument});
  }

  for (const auto& item : parsed.assignments) if (item.key == "rule") {
    const auto* object = lang::as_object(item.value);
    const auto* match_value = object == nullptr ? nullptr : lang::find_field(*object, "match");
    const auto* set_value = object == nullptr ? nullptr : lang::find_field(*object, "set");
    const auto* match = match_value == nullptr ? nullptr : lang::as_object(*match_value);
    const auto* set = set_value == nullptr ? nullptr : lang::as_object(*set_value);
    if (match == nullptr || set == nullptr) { error(result.diagnostics, item.location, "rule requires match and set objects"); continue; }
    WindowRule rule;
    if (const auto* value = lang::find_field(*match, "app-id")) { const auto* text = lang::as_string(*value); if (text == nullptr) error(result.diagnostics, item.location, "rule app-id must be a string"); else rule.app_id = *text; }
    if (const auto* value = lang::find_field(*match, "title")) { const auto* text = lang::as_string(*value); if (text == nullptr) error(result.diagnostics, item.location, "rule title must be a string"); else rule.title = *text; }
    if (rule.app_id.empty() && rule.title.empty()) { error(result.diagnostics, item.location, "rule must match app-id and/or title"); continue; }
    if (const auto* value = lang::find_field(*set, "floating")) { const auto* enabled = lang::as_boolean(*value); if (enabled == nullptr) error(result.diagnostics, item.location, "rule floating must be boolean"); else rule.floating = *enabled; }
    if (const auto* value = lang::find_field(*set, "size")) { const auto* text = lang::as_string(*value); if (text == nullptr || !parse_rule_size(*text, rule)) error(result.diagnostics, item.location, "rule size must be WIDTHxHEIGHT or WIDTH%xHEIGHT%"); }
    if (const auto* value = lang::find_field(*set, "blur")) { const auto* enabled = lang::as_boolean(*value); if (enabled == nullptr) error(result.diagnostics, item.location, "rule blur must be boolean"); else rule.blur = *enabled; }
    if (const auto* value = lang::find_field(*set, "blur-radius")) { const auto* radius = lang::as_integer(*value); if (radius == nullptr || *radius < 0 || *radius > 64) error(result.diagnostics, item.location, "rule blur-radius must be 0..64"); else rule.blur_radius = static_cast<std::uint32_t>(*radius); }
    if (const auto* value = lang::find_field(*set, "glass")) { const auto* enabled = lang::as_boolean(*value); if (enabled == nullptr) error(result.diagnostics, item.location, "rule glass must be boolean"); else rule.glass = *enabled; }
    if (const auto* value = lang::find_field(*set, "opacity")) { const auto* opacity = lang::as_integer(*value); if (opacity == nullptr || *opacity < 0 || *opacity > 100) error(result.diagnostics, item.location, "rule opacity must be 0..100"); else rule.opacity = static_cast<float>(*opacity) / 100.0F; }
    if (const auto* value = lang::find_field(*set, "window-shader")) { const auto* text = resolved_string(*value, variables); if (text == nullptr || text->empty()) error(result.diagnostics, item.location, "rule window-shader must resolve to a nonempty string"); else rule.window_shader = *text; }
    if (const auto* value = lang::find_field(*set, "border-shader")) { const auto* text = resolved_string(*value, variables); if (text == nullptr || text->empty()) error(result.diagnostics, item.location, "rule border-shader must resolve to a nonempty string"); else rule.border_shader = *text; }
    mutable_config->window_rules.push_back(std::move(rule));
  }

  for (const auto& item : parsed.assignments) if (item.key == "layer-rule") {
    const auto* object = lang::as_object(item.value);
    const auto* match_value = object == nullptr ? nullptr : lang::find_field(*object, "match");
    const auto* set_value = object == nullptr ? nullptr : lang::find_field(*object, "set");
    const auto* match = match_value == nullptr ? nullptr : lang::as_object(*match_value);
    const auto* set = set_value == nullptr ? nullptr : lang::as_object(*set_value);
    if (match == nullptr || set == nullptr) { error(result.diagnostics, item.location, "layer-rule requires match and set objects"); continue; }
    LayerRule rule;
    if (const auto* value = lang::find_field(*match, "namespace")) {
      const auto* text = lang::as_string(*value);
      if (text == nullptr || text->empty()) error(result.diagnostics, item.location, "layer-rule namespace must be a nonempty string");
      else rule.name_space = *text;
    }
    if (rule.name_space.empty()) continue;
    if (const auto* value = lang::find_field(*set, "blur")) {
      const auto* enabled = lang::as_boolean(*value);
      if (enabled == nullptr) error(result.diagnostics, item.location, "layer-rule blur must be boolean");
      else rule.effect.blur_radius = *enabled ? 8U : 0U;
    }
    if (const auto* value = lang::find_field(*set, "blur-radius")) {
      const auto* radius = lang::as_integer(*value);
      if (radius == nullptr || *radius < 0 || *radius > 64) error(result.diagnostics, item.location, "layer-rule blur-radius must be 0..64");
      else rule.effect.blur_radius = static_cast<std::uint32_t>(*radius);
    }
    if (const auto* value = lang::find_field(*set, "ignore-alpha")) {
      const auto* threshold = lang::as_integer(*value);
      if (threshold == nullptr || *threshold < 0 || *threshold > 100) error(result.diagnostics, item.location, "layer-rule ignore-alpha must be 0..100");
      else rule.effect.ignore_alpha = static_cast<float>(*threshold) / 100.0F;
    }
    if (const auto* value = lang::find_field(*set, "opacity")) {
      const auto* opacity = lang::as_integer(*value);
      if (opacity == nullptr || *opacity < 0 || *opacity > 100) error(result.diagnostics, item.location, "layer-rule opacity must be 0..100");
      else rule.effect.opacity = static_cast<float>(*opacity) / 100.0F;
    }
    mutable_config->layer_rules.push_back(std::move(rule));
  }

  if (const auto* item = assignment(parsed, "shaders"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "shaders must be an object");
    } else {
      mutable_config->shaders.clear();
      for (std::size_t index = 0; index < object->names.size(); ++index) {
            const std::string& name = object->names[index];
            const auto* definition = lang::as_object(object->values[index]);
            const auto* source_value = definition == nullptr ? nullptr : lang::find_field(*definition, "source");
            const auto* role_value = definition == nullptr ? nullptr : lang::find_field(*definition, "role");
            const auto* source = source_value == nullptr ? nullptr : resolved_string(*source_value, variables);
            const auto* role_text = role_value == nullptr ? nullptr : resolved_string(*role_value, variables);
            const auto role = role_text == nullptr ? std::nullopt : shader_role(*role_text);
            if (name.empty() || definition == nullptr || source == nullptr || source->empty() || !role) {
              error(result.diagnostics, item->location,
                    "each shader requires a name, source, and window, border, or background role");
              continue;
            }
            CustomShaderConfig shader{.name = name, .role = *role, .source = *source, .values = {}};
            if (const auto* values_value = lang::find_field(*definition, "values"); values_value != nullptr) {
              const auto* values = lang::as_object(*values_value);
              if (values == nullptr) {
                error(result.diagnostics, item->location, "shader values must be an object");
              } else {
                for (std::size_t value_index = 0; value_index < values->names.size(); ++value_index) {
                  const auto& parameter_name = values->names[value_index];
                  const bool valid_name = !parameter_name.empty() &&
                      (std::isalpha(static_cast<unsigned char>(parameter_name.front())) || parameter_name.front() == '_') &&
                      std::all_of(std::next(parameter_name.begin()), parameter_name.end(), [](unsigned char character) {
                        return std::isalnum(character) || character == '_';
                      });
                  auto parameter = shader_value(values->values[value_index], variables);
                  if (!valid_name || !parameter) {
                    error(result.diagnostics, item->location,
                          "shader values require identifier names and boolean, integer, color, 1..20 color array, or 2..4 integer array values");
                  } else {
                    shader.values.emplace_back(parameter_name, std::move(*parameter));
                  }
                }
              }
            }
            mutable_config->shaders.push_back(std::move(shader));
      }
      const auto select_default = [&](CustomShaderRole role, std::string& target) {
        const auto shader = std::find_if(mutable_config->shaders.begin(), mutable_config->shaders.end(),
                                         [&](const auto& candidate) { return candidate.role == role; });
        if (shader == mutable_config->shaders.end())
          error(result.diagnostics, item->location, "shaders must define each of the window, border, and background roles");
        else
          target = shader->name;
      };
      select_default(CustomShaderRole::window, mutable_config->window_shader);
      select_default(CustomShaderRole::border, mutable_config->border_shader);
      select_default(CustomShaderRole::background, mutable_config->background_shader);
      }
  }

  if (const auto* item = assignment(parsed, "blur"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "blur must be an object");
    } else {
      if (const auto* value = lang::find_field(*object, "enabled")) {
        const auto* enabled = lang::as_boolean(*value);
        if (enabled == nullptr) error(result.diagnostics, item->location, "blur.enabled must be a boolean");
        else mutable_config->blur.enabled = *enabled;
      }
      dimension(*object, "radius", mutable_config->blur.radius, item->location, result.diagnostics);
      if (mutable_config->blur.radius > 64) error(result.diagnostics, item->location, "blur.radius must be 0..64");
      dimension(*object, "passes", mutable_config->blur.passes, item->location, result.diagnostics);
      if (mutable_config->blur.passes < 1 || mutable_config->blur.passes > 8)
        error(result.diagnostics, item->location, "blur.passes must be 1..8");
      dimension(*object, "brightness", mutable_config->blur.brightness_percent, item->location, result.diagnostics);
      if (mutable_config->blur.brightness_percent > 200)
        error(result.diagnostics, item->location, "blur.brightness must be 0..200");
      dimension(*object, "contrast", mutable_config->blur.contrast_percent, item->location, result.diagnostics);
      if (mutable_config->blur.contrast_percent > 200)
        error(result.diagnostics, item->location, "blur.contrast must be 0..200");
      dimension(*object, "saturation", mutable_config->blur.saturation_percent, item->location, result.diagnostics);
      if (mutable_config->blur.saturation_percent > 200)
        error(result.diagnostics, item->location, "blur.saturation must be 0..200");
      dimension(*object, "noise", mutable_config->blur.noise_percent, item->location, result.diagnostics);
      if (mutable_config->blur.noise_percent > 100)
        error(result.diagnostics, item->location, "blur.noise must be 0..100");
    }
  }

  if (const auto* item = assignment(parsed, "glass"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "glass must be an object");
    } else {
      if (const auto* value = lang::find_field(*object, "enabled")) {
        const auto* enabled = lang::as_boolean(*value);
        if (enabled == nullptr) error(result.diagnostics, item->location, "glass.enabled must be a boolean");
        else mutable_config->glass.enabled = *enabled;
      }
      if (const auto* value = lang::find_field(*object, "quality")) {
        const auto* quality = lang::as_string(*value);
        if (quality == nullptr || (*quality != "low" && *quality != "medium" && *quality != "high"))
          error(result.diagnostics, item->location, "glass.quality must be low, medium, or high");
        else mutable_config->glass.quality = *quality == "low" ? GlassQuality::low :
            *quality == "medium" ? GlassQuality::medium : GlassQuality::high;
      }
      const auto ranged = [&](std::string_view name, std::uint32_t& target, std::int64_t maximum) {
        const auto* value = lang::find_field(*object, name);
        if (value == nullptr) return;
        const auto* integer = lang::as_integer(*value);
        if (integer == nullptr || *integer < 0 || *integer > maximum)
          error(result.diagnostics, item->location, "glass." + std::string(name) + " must be 0.." + std::to_string(maximum));
        else target = static_cast<std::uint32_t>(*integer);
      };
      ranged("blur-radius", mutable_config->glass.blur_radius, 64);
      ranged("refraction", mutable_config->glass.refraction, 64);
      ranged("dispersion", mutable_config->glass.dispersion, 32);
      ranged("ior-per-mille", mutable_config->glass.ior_per_mille, 2500);
      if (mutable_config->glass.ior_per_mille < 1001)
        error(result.diagnostics, item->location, "glass.ior-per-mille must be 1001..2500");
      ranged("thickness", mutable_config->glass.thickness, 128);
      ranged("roughness", mutable_config->glass.roughness_percent, 100);
      ranged("brightness", mutable_config->glass.brightness_percent, 200);
      ranged("saturation", mutable_config->glass.saturation_percent, 200);
      ranged("frost", mutable_config->glass.frost_percent, 100);
      ranged("animation-speed", mutable_config->glass.animation_speed, 1000);
      extract_color(*object, "tint", mutable_config->glass.tint, item->location, result.diagnostics);
    }
  }

  if (const auto* item = assignment(parsed, "layout"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "layout must be an object");
    } else {
      dimension(*object, "master-count", mutable_config->layout.master_count, item->location,
                result.diagnostics, true);
      if (const auto* value = lang::find_field(*object, "master-ratio"); value != nullptr) {
        const auto* ratio = lang::as_integer(*value);
        if (ratio == nullptr || *ratio < 10 || *ratio > 90) {
          error(result.diagnostics, item->location, "layout.master-ratio must be an integer percent from 10 to 90");
        } else {
          mutable_config->layout.master_ratio_percent = static_cast<std::uint32_t>(*ratio);
        }
      }
      std::uint32_t gaps = 0;
      const bool has_gaps = dimension(*object, "gaps", gaps, item->location, result.diagnostics);
      if (has_gaps) {
        if (lang::find_field(*object, "outer-gap") == nullptr) mutable_config->layout.outer_gap = gaps;
        if (lang::find_field(*object, "inner-gap") == nullptr) mutable_config->layout.inner_gap = gaps;
      }
      dimension(*object, "outer-gap", mutable_config->layout.outer_gap, item->location, result.diagnostics);
      dimension(*object, "inner-gap", mutable_config->layout.inner_gap, item->location, result.diagnostics);
      dimension(*object, "min-zoom-per-mille", mutable_config->layout.min_zoom_per_mille,
                item->location, result.diagnostics, true);
      dimension(*object, "max-zoom-per-mille", mutable_config->layout.max_zoom_per_mille,
                item->location, result.diagnostics, true);
      if (mutable_config->layout.min_zoom_per_mille > mutable_config->layout.max_zoom_per_mille)
        error(result.diagnostics, item->location,
              "layout.min-zoom-per-mille must not exceed layout.max-zoom-per-mille");
      if (const auto* value = lang::find_field(*object, "smart-gaps"); value != nullptr) {
        const auto* smart = lang::as_boolean(*value);
        if (smart == nullptr) error(result.diagnostics, item->location, "layout.smart-gaps must be a boolean");
        else mutable_config->layout.smart_gaps = *smart;
      }
      if (const auto* value = lang::find_field(*object, "default"); value != nullptr) {
        const auto* name = lang::as_string(*value);
        if (name == nullptr || (*name != "master-stack" && *name != "focus-fibonacci" &&
                                *name != "endless-canvas"))
          error(result.diagnostics, item->location,
                "layout.default must be \"master-stack\", \"focus-fibonacci\", or \"endless-canvas\"");
        else mutable_config->layout.kind = *name == "focus-fibonacci" ? LayoutConfig::Kind::focus_fibonacci :
                                      *name == "endless-canvas" ? LayoutConfig::Kind::endless_canvas :
                                                                    LayoutConfig::Kind::master_stack;
      }
    }
  }

  if (const auto* item = assignment(parsed, "input"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "input must be an object");
    } else {
      if (const auto* value = lang::find_field(*object, "focus-mode"); value != nullptr) {
        const auto* mode = lang::as_string(*value);
        if (mode == nullptr || (*mode != "hover" && *mode != "by-click" && *mode != "click"))
          error(result.diagnostics, item->location, "input.focus-mode must be \"hover\" or \"by-click\"");
        else mutable_config->input.focus_mode = *mode == "hover" ? FocusMode::hover : FocusMode::by_click;
      }
      if (const auto* value = lang::find_field(*object, "reverse-mouse-scrolling"); value != nullptr) {
        const auto* enabled = lang::as_boolean(*value);
        if (enabled == nullptr)
          error(result.diagnostics, item->location, "input.reverse-mouse-scrolling must be a boolean");
        else mutable_config->input.reverse_mouse_scrolling = *enabled;
      }
    }
  }

  if (const auto* item = assignment(parsed, "keyboard"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "keyboard must be an object");
    } else {
      const auto string_field = [&](std::string_view name, std::string& target) {
        const auto* value = lang::find_field(*object, name);
        if (value == nullptr) return;
        const auto* text = lang::as_string(*value);
        if (text == nullptr) error(result.diagnostics, item->location, "keyboard." + std::string(name) + " must be a string");
        else target = *text;
      };
      string_field("rules", mutable_config->keyboard.rules);
      string_field("model", mutable_config->keyboard.model);
      string_field("layout", mutable_config->keyboard.layout);
      string_field("variant", mutable_config->keyboard.variant);
      string_field("options", mutable_config->keyboard.options);
      const auto nullable = [](const std::string& value) { return value.empty() ? nullptr : value.c_str(); };
      const xkb_rule_names names{nullable(mutable_config->keyboard.rules), nullable(mutable_config->keyboard.model),
                                 nullable(mutable_config->keyboard.layout), nullable(mutable_config->keyboard.variant),
                                 nullable(mutable_config->keyboard.options)};
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      xkb_keymap* keymap = context == nullptr ? nullptr :
          xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (keymap == nullptr) error(result.diagnostics, item->location, "keyboard XKB configuration is invalid");
      if (keymap != nullptr) xkb_keymap_unref(keymap);
      if (context != nullptr) xkb_context_unref(context);
    }
  }

  if (const auto* item = assignment(parsed, "animations"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "animations must be an object");
    } else {
      const auto boolean = [&](std::string_view name, bool& target) {
        const auto* value = lang::find_field(*object, name);
        if (value == nullptr) return;
        const auto* enabled = lang::as_boolean(*value);
        if (enabled == nullptr) error(result.diagnostics, item->location, "animations." + std::string(name) + " must be a boolean");
        else target = *enabled;
      };
      boolean("enabled", mutable_config->animations.enabled);
      boolean("open-window", mutable_config->animations.open_window);
      boolean("resize", mutable_config->animations.resize);
      boolean("close", mutable_config->animations.close);
      dimension(*object, "duration-ms", mutable_config->animations.duration_ms, item->location,
                 result.diagnostics);
      const auto ranged = [&](std::string_view name, std::uint32_t& target,
                              std::int64_t minimum, std::int64_t maximum) {
        const auto* value = lang::find_field(*object, name);
        if (value == nullptr) return;
        const auto* integer = lang::as_integer(*value);
        if (integer == nullptr || *integer < minimum || *integer > maximum) {
          error(result.diagnostics, item->location, "animations." + std::string(name) +
                    " must be an integer from " + std::to_string(minimum) + " to " +
                    std::to_string(maximum));
        } else {
          target = static_cast<std::uint32_t>(*integer);
        }
      };
      ranged("tag-duration-ms", mutable_config->animations.tag_duration_ms, 0, 10000);
      ranged("open-scale-per-mille", mutable_config->animations.open_scale_per_mille, 500, 1000);
      ranged("close-scale-per-mille", mutable_config->animations.close_scale_per_mille, 500, 1000);
      ranged("open-offset-px", mutable_config->animations.open_offset_px, 0, 4096);
      ranged("close-offset-px", mutable_config->animations.close_offset_px, 0, 4096);
      ranged("tag-scale-per-mille", mutable_config->animations.tag_scale_per_mille, 500, 1000);
      ranged("tag-parallax-per-mille", mutable_config->animations.tag_parallax_per_mille, 0, 1000);
      ranged("tag-fade-per-mille", mutable_config->animations.tag_fade_per_mille, 0, 1000);
      if (const auto* spring_value = lang::find_field(*object, "spring"); spring_value != nullptr) {
        const auto* spring = lang::as_object(*spring_value);
        if (spring == nullptr) {
          error(result.diagnostics, item->location, "animations.spring must be an object");
        } else {
          dimension(*spring, "stiffness", mutable_config->animations.spring_stiffness, item->location, result.diagnostics, true);
          dimension(*spring, "damping", mutable_config->animations.spring_damping, item->location, result.diagnostics);
          dimension(*spring, "mass-per-mille", mutable_config->animations.spring_mass_per_mille, item->location, result.diagnostics, true);
        }
      }
      if (const auto* bezier_value = lang::find_field(*object, "cubic-bezier"); bezier_value != nullptr) {
        const auto* bezier = lang::as_object(*bezier_value);
        if (bezier == nullptr) {
          error(result.diagnostics, item->location, "animations.cubic-bezier must be an object");
        } else {
          const auto point = [&](std::string_view name, std::uint32_t& target) {
            const auto* value = lang::find_field(*bezier, name);
            if (value == nullptr) return;
            const auto* integer = lang::as_integer(*value);
            if (integer == nullptr || *integer < 0 || *integer > 1000) {
              error(result.diagnostics, item->location,
                    "animations.cubic-bezier." + std::string(name) + " must be an integer from 0 to 1000");
            } else target = static_cast<std::uint32_t>(*integer);
          };
          point("x1", mutable_config->animations.bezier_x1_per_mille);
          point("y1", mutable_config->animations.bezier_y1_per_mille);
          point("x2", mutable_config->animations.bezier_x2_per_mille);
          point("y2", mutable_config->animations.bezier_y2_per_mille);
        }
      }
    }
  }

  if (const auto* item = assignment(parsed, "output"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "output must be an object");
    } else {
      parse_output_config(*object, mutable_config->output, item->location,
                          result.diagnostics, "output.");
    }
  }

  if (const auto* item = assignment(parsed, "outputs"); item != nullptr) {
    const auto* object = lang::as_object(item->value);
    if (object == nullptr) {
      error(result.diagnostics, item->location, "outputs must be an object");
    } else {
      mutable_config->outputs.clear();
      for (std::size_t index = 0; index < object->names.size(); ++index) {
        const auto& connector = object->names[index];
        const auto* definition = lang::as_object(object->values[index]);
        if (definition == nullptr) {
          error(result.diagnostics, item->location,
                "outputs." + connector + " must be an object");
          continue;
        }
        OutputConfig configured = mutable_config->output;
        parse_output_config(*definition, configured, item->location, result.diagnostics,
                            "outputs." + connector + ".");
        mutable_config->outputs.emplace_back(connector, configured);
      }
    }
  }

  const auto validate_shader = [&](const std::string& name, CustomShaderRole role,
                                   lang::SourceLocation location, std::string_view field) {
    const auto shader = std::find_if(mutable_config->shaders.begin(),
                                     mutable_config->shaders.end(),
                                     [&](const auto& candidate) { return candidate.name == name; });
    if (shader == mutable_config->shaders.end() || shader->role != role)
      error(result.diagnostics, location, std::string(field) + " must name a shader with the matching role");
  };
  const auto shaders_location = assignment(parsed, "shaders") == nullptr ? lang::SourceLocation{} :
      assignment(parsed, "shaders")->location;
  validate_shader(mutable_config->window_shader, CustomShaderRole::window, shaders_location, "window shader");
  validate_shader(mutable_config->border_shader, CustomShaderRole::border, shaders_location, "border shader");
  validate_shader(mutable_config->background_shader, CustomShaderRole::background, shaders_location, "background shader");
  for (const auto& rule : mutable_config->window_rules) {
    if (!rule.window_shader.empty()) validate_shader(rule.window_shader, CustomShaderRole::window, {}, "rule window-shader");
    if (!rule.border_shader.empty()) validate_shader(rule.border_shader, CustomShaderRole::border, {}, "rule border-shader");
  }
  if (!result.ok()) result.config.reset();
  return result;
}

RuntimeConfigResult load_runtime_config_file(const std::string& path) {
  const auto parsed = lang::parse_config_file(path);
  RuntimeConfigResult result;
  result.diagnostics = parsed.diagnostics;
  if (!parsed.ok()) return result;
  const auto validation = lang::validate_config(parsed.config);
  result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
  if (!validation.ok()) return result;
  auto compiled = compile_runtime_config(parsed.config);
  result.diagnostics.insert(result.diagnostics.end(), compiled.diagnostics.begin(), compiled.diagnostics.end());
  result.config = std::move(compiled.config);
  if (result.config != nullptr) {
    auto mutable_config = std::const_pointer_cast<RuntimeConfig>(result.config);
    const auto base = std::filesystem::path(path).parent_path();
    for (auto& shader : mutable_config->shaders) {
      std::filesystem::path shader_path(shader.source);
      if (shader_path.is_relative()) shader.source = (base / shader_path).lexically_normal().string();
    }
    std::filesystem::path background_path(mutable_config->background_image);
    if (background_path.is_relative())
      mutable_config->background_image = (base / background_path).lexically_normal().string();
  }
  return result;
}

bool load_shader_sources(const RuntimeConfig& config, renderer::ShaderSources* sources,
                         std::string* message) {
  if (sources == nullptr) return false;
  renderer::ShaderSources loaded;
  loaded.window = config.window_shader;
  loaded.border = config.border_shader;
  loaded.background = config.background_shader;
  loaded.background_image = config.background_image;
  for (const auto& shader : config.shaders) {
    std::ifstream input(shader.source, std::ios::binary | std::ios::ate);
    if (!input) {
      if (message != nullptr) *message = "could not open shader " + shader.name + ": " + shader.source;
      return false;
    }
    const auto size = input.tellg();
    if (size < 0 || size > 1024 * 1024) {
      if (message != nullptr) *message = "shader " + shader.name + " exceeds 1 MiB";
      return false;
    }
    renderer::NamedShaderSource loaded_shader;
    loaded_shader.name = shader.name;
    loaded_shader.role = shader.role == CustomShaderRole::window ? renderer::ShaderRole::window :
                         shader.role == CustomShaderRole::border ? renderer::ShaderRole::border :
                                                                   renderer::ShaderRole::background;
    loaded_shader.source.resize(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!loaded_shader.source.empty() &&
        !input.read(loaded_shader.source.data(), static_cast<std::streamsize>(size))) {
      if (message != nullptr) *message = "could not read shader " + shader.name + ": " + shader.source;
      return false;
    }
    for (const auto& [name, value] : shader.values) {
      renderer::ShaderValue converted;
      if (const auto* boolean = std::get_if<bool>(&value)) converted = *boolean;
      else if (const auto* integer = std::get_if<std::int32_t>(&value)) converted = *integer;
      else if (const auto* color = std::get_if<Color>(&value)) converted = color->normalized();
       else if (const auto* components = std::get_if<std::vector<std::int32_t>>(&value)) converted = *components;
       else {
         std::vector<std::array<float, 4>> colors;
         for (const auto& color : std::get<std::vector<Color>>(value)) colors.push_back(color.normalized());
         converted = std::move(colors);
       }
      loaded_shader.values.emplace_back(name, std::move(converted));
    }
    loaded.programs.push_back(std::move(loaded_shader));
  }
  *sources = std::move(loaded);
  return true;
}

LoadedRuntimeConfig load_runtime_config() {
  std::string path;
  bool explicit_path = false;
  if (const char* configured = std::getenv("ZWWM_CONFIG"); configured != nullptr) {
    path = configured;
    explicit_path = true;
  } else {
    std::filesystem::path user_path;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      user_path = std::filesystem::path(xdg) / "zwwm/config.zw";
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      user_path = std::filesystem::path(home) / ".config/zwwm/config.zw";
    }
    if (!user_path.empty()) {
      const bool bootstrapped = bootstrap_user_config(user_path);
      if (bootstrapped || std::filesystem::exists(user_path)) path = user_path.string();
    }
    if (path.empty() && std::filesystem::exists("/etc/xdg/zwwm/config.zw"))
      path = "/etc/xdg/zwwm/config.zw";
  }

  if (path.empty() && !explicit_path) return {std::make_shared<const RuntimeConfig>(), {}, {}};
  if (!explicit_path && !std::filesystem::exists(path))
    return {std::make_shared<const RuntimeConfig>(), std::move(path), {}};

  auto compiled = load_runtime_config_file(path);
  print_diagnostics(path, compiled.diagnostics);
  if (!compiled.ok()) {
    auto message = format_runtime_config_error(path, compiled.diagnostics);
    return {std::make_shared<const RuntimeConfig>(), std::move(path), std::move(message)};
  }
  return {std::move(compiled.config), std::move(path), {}};
}

std::string format_runtime_config_error(const std::string& path,
                                        const std::vector<lang::Diagnostic>& diagnostics) {
  const auto item = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
    return diagnostic.level == lang::DiagnosticLevel::error;
  });
  if (item == diagnostics.end()) return "Configuration parsing, validation, or compilation failed";
  const auto filename = std::filesystem::path(path).filename().string();
  return (filename.empty() ? path : filename) + ":" + std::to_string(item->location.line) + ":" +
         std::to_string(item->location.column) + ": " + item->message;
}

ConfigReloader::ConfigReloader(::zwayland::server::EventLoop* event_loop, std::string path,
                               std::shared_ptr<const RuntimeConfig> initial, Apply apply, void* data,
                               Error error)
    : event_loop_(event_loop), path_(std::move(path)), config_(std::move(initial)), apply_(apply),
      error_(error), data_(data) {
  const std::filesystem::path file(path_);
  directory_ = file.parent_path().empty() ? "." : file.parent_path().string();
  basename_ = file.filename().string();
}

ConfigReloader::~ConfigReloader() {
  if (source_ >= 0) event_loop_->remove(source_);
}

bool ConfigReloader::start() {
  if (event_loop_ == nullptr || basename_.empty() || config_ == nullptr || apply_ == nullptr) {
    last_error_ = "configuration watcher is not initialized";
    return false;
  }
  fd_.reset(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
  const auto watch_directory = std::filesystem::exists(directory_)
                                   ? std::filesystem::path(directory_)
                                   : std::filesystem::path(directory_).parent_path();
  watching_config_directory_ = watch_directory == std::filesystem::path(directory_);
  if (!fd_ || watch_directory.empty() ||
      inotify_add_watch(fd_.get(), watch_directory.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE) < 0) {
    last_error_ = "could not watch configuration directory";
    fd_.reset();
    return false;
  }
  try {
    source_ = event_loop_->add_fd(fd_.get(), EPOLLIN | EPOLLERR | EPOLLHUP,
                                  [this](int fd, int mask) {
                                    return on_inotify(fd, static_cast<std::uint32_t>(mask), this) != 0;
                                  });
  } catch (const std::exception&) {
    last_error_ = "could not register configuration watcher";
    fd_.reset();
    return false;
  }
  return true;
}

const std::string& ConfigReloader::last_error() const { return last_error_; }

int ConfigReloader::on_inotify(int fd, std::uint32_t mask, void* data) {
  auto* reloader = static_cast<ConfigReloader*>(data);
  if ((mask & (EPOLLERR | EPOLLHUP)) != 0) { reloader->last_error_ = "configuration watcher stopped"; return 0; }
  alignas(inotify_event) char bytes[4096];
  bool relevant = false;
  for (ssize_t count; (count = read(fd, bytes, sizeof(bytes))) > 0;) {
    for (char* cursor = bytes; cursor < bytes + count;) {
      const auto* event = reinterpret_cast<const inotify_event*>(cursor);
      if (event->len != 0) {
        if (reloader->watching_config_directory_ && reloader->basename_ == event->name) {
          relevant = true;
        } else if (!reloader->watching_config_directory_ &&
                   std::filesystem::path(reloader->directory_).filename() == event->name) {
          const int watch = inotify_add_watch(fd, reloader->directory_.c_str(),
                                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
          if (watch >= 0) {
            reloader->watching_config_directory_ = true;
            relevant = std::filesystem::exists(reloader->path_);
          }
        }
      }
      cursor += sizeof(inotify_event) + event->len;
    }
  }
  if (relevant && !reloader->pending_) {
    reloader->pending_ = true;
    reloader->event_loop_->add_idle([reloader]() { reload_idle(reloader); });
  }
  return 1;
}

void ConfigReloader::reload_idle(void* data) {
  auto* reloader = static_cast<ConfigReloader*>(data);
  reloader->pending_ = false;
  reloader->reload();
}

void ConfigReloader::reload() {
  auto next = load_runtime_config_file(path_);
  if (!next.ok()) {
    print_diagnostics(path_, next.diagnostics);
    std::fprintf(stderr, "zwwm: configuration reload failed\n");
    if (error_ != nullptr) error_(data_, format_runtime_config_error(path_, next.diagnostics));
    return;
  }
  config_ = std::move(next.config);
  apply_(data_, config_);
  std::fprintf(stderr, "zwwm: configuration reloaded\n");
}

}  // namespace zwwm
