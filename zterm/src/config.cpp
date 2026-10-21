#include "config.hpp"
#include "text.hpp"

#include <zwwm/lang/config.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace zterm {
namespace lang = zwwm::lang;
namespace {

constexpr std::pair<std::string_view, std::string FontFamilies::*> font_fields[] = {
  {"family",&FontFamilies::mono},{"mono",&FontFamilies::mono},{"bold",&FontFamilies::bold},
  {"italic",&FontFamilies::italic},{"bold-italic",&FontFamilies::bold_italic},
  {"symbols",&FontFamilies::symbols},{"emoji",&FontFamilies::emoji}};

CursorShape cursor_shape(const std::string& name) {
  if (name == "beam" || name == "bar") return CursorShape::Beam;
  if (name == "line" || name == "underline") return CursorShape::Underline;
  if (name == "block") return CursorShape::Block;
  throw std::runtime_error("cursor shape must be beam, line, or block");
}

class Decoder {
 public:
  Decoder(Options& options, const lang::Config& config) : options_(options) {
    for (const auto& assignment : config.assignments)
      if (!assignment.key.empty() && assignment.key.front() >= 'A' && assignment.key.front() <= 'Z') variables_[assignment.key] = assignment.value;
  }

  void apply(const lang::Config& config) {
    const bool shared = std::any_of(config.assignments.begin(), config.assignments.end(), [](const auto& item) { return item.key == "zterm"; });
    for (const auto& assignment : config.assignments) {
      if (variables_.contains(assignment.key)) continue;
      try {
        if (shared) {
          if (assignment.key == "zterm") object(assignment.value, "zterm", [this](const auto& key, const auto& value) { setting(key, value); });
        } else setting(assignment.key, assignment.value);
      } catch (const std::exception& error) {
        throw std::runtime_error(std::to_string(assignment.location.line) + ":" + std::to_string(assignment.location.column) + ": " + error.what());
      }
    }
  }

 private:
  Options& options_;
  std::unordered_map<std::string, lang::Value> variables_;

  lang::Value resolve(const lang::Value& value, unsigned depth = 0) const {
    if (depth >= 32) throw std::runtime_error("configuration variable cycle");
    if (const auto* reference = std::get_if<lang::VariableReference>(&value.data)) {
      if (const auto found = variables_.find(reference->name); found != variables_.end()) return resolve(found->second, depth + 1);
      if (const auto* environment = std::getenv(reference->name.c_str())) return lang::Value{std::string(environment)};
      throw std::runtime_error("undefined variable $" + reference->name);
    }
    return value;
  }

  std::string string(const lang::Value& value, const std::string& name, bool empty = false) const {
    const auto resolved = resolve(value); const auto* text = lang::as_string(resolved);
    if (!text || (!empty && text->empty()) || text->find('\0') != std::string::npos) throw std::runtime_error(name + " must be a nonempty string");
    return *text;
  }

  unsigned integer(const lang::Value& value, const std::string& name, unsigned minimum, unsigned maximum) const {
    const auto resolved = resolve(value); const auto* number = lang::as_integer(resolved);
    if (!number || *number < minimum || *number > maximum) throw std::runtime_error(name + " must be an integer in " + std::to_string(minimum) + ".." + std::to_string(maximum));
    return *number;
  }

  bool boolean(const lang::Value& value, const std::string& name) const {
    const auto resolved = resolve(value); const auto* result = lang::as_boolean(resolved);
    if (!result) throw std::runtime_error(name + " must be true or false");
    return *result;
  }

  template <typename Handler>
  void object(const lang::Value& value, const std::string& name, Handler handler) const {
    const auto resolved = resolve(value); const auto* table = lang::as_object(resolved);
    if (!table) throw std::runtime_error(name + " must be an object");
    for (size_t i = 0; i < table->names.size(); ++i) handler(table->names[i], table->values[i]);
  }

  std::array<float, 4> color(const lang::Value& value, const std::string& name, bool alpha = false) const {
    const auto text = string(value, name);
    if ((text.size() != 7 && !(alpha && text.size() == 9)) || text[0] != '#') throw std::runtime_error(name + " must be #RRGGBB" + (alpha ? " or #RRGGBBAA" : ""));
    std::array<float, 4> result{0,0,0,1};
    for (size_t i = 0; i < (text.size() - 1) / 2; ++i) {
      unsigned number = 0; const char* begin = text.data() + 1 + i * 2;
      const auto [end, error] = std::from_chars(begin, begin + 2, number, 16);
      if (error != std::errc{} || end != begin + 2) throw std::runtime_error(name + " contains an invalid color");
      result[i] = number / 255.f;
    }
    return result;
  }

  static Color rgb(const std::array<float, 4>& color) {
    return {static_cast<std::uint8_t>(std::lround(color[0] * 255)), static_cast<std::uint8_t>(std::lround(color[1] * 255)), static_cast<std::uint8_t>(std::lround(color[2] * 255))};
  }

  void setting(const std::string& name, const lang::Value& value) {
    if (name == "font") object(value, name, [this](const auto& key, const auto& item) {
      const auto field = std::find_if(std::begin(font_fields), std::end(font_fields), [&](const auto& field) { return field.first == key; });
      if (field != std::end(font_fields)) options_.fonts.*(field->second) = string(item, "font." + key, field->second != &FontFamilies::mono);
      else if (key == "size") options_.size = integer(item, "font.size", 6, 96);
      else if (key == "weight") {
        const auto resolved = resolve(item);
        options_.weight = lang::as_integer(resolved) ? integer(resolved, "font.weight", 100, 1000) : Text::parse_weight(string(resolved, "font.weight"));
      } else throw std::runtime_error("unknown font setting: " + key);
    });
    else if (name == "rendering") object(value, name, [this](const auto& key, const auto& item) {
      if (key == "text-rounding-per-mille") options_.text_rounding = integer(item, "rendering." + key, 0, 1000) / 1000.f;
      else if (key == "icon-scale-per-mille") options_.icon_scale = integer(item, "rendering." + key, 500, 3000) / 1000.f;
      else if (key == "background-opacity-per-mille") options_.background_opacity = integer(item, "rendering." + key, 0, 1000) / 1000.f;
      else throw std::runtime_error("unknown rendering setting: " + key);
    });
    else if (name == "colors") object(value, name, [this](const auto& key, const auto& item) {
      if (key == "foreground") options_.foreground = rgb(color(item, "colors." + key));
      else if (key == "background") options_.background = rgb(color(item, "colors." + key));
      else if (key == "selection") options_.selection = color(item, "colors." + key, true);
      else if (key == "palette") {
        const auto resolved = resolve(item); const auto* list = lang::as_array(resolved);
        if (!list || list->values.size() != 16) throw std::runtime_error("colors.palette must contain exactly 16 RGB colors");
        options_.palette.clear();
        for (const auto& entry : list->values) options_.palette.push_back(rgb(color(entry, "colors.palette")));
      } else {
        constexpr std::array<std::string_view, 16> names{
          "black","red","green","yellow","blue","magenta","cyan","white",
          "bright-black","bright-red","bright-green","bright-yellow","bright-blue","bright-magenta","bright-cyan","bright-white"};
        const auto found = std::find(names.begin(), names.end(), key);
        if (found == names.end()) throw std::runtime_error("unknown colors setting: " + key);
        if (options_.palette.empty()) {
          const auto defaults = Terminal::default_palette();
          options_.palette.assign(defaults.begin(), defaults.end());
        }
        options_.palette[found - names.begin()] = rgb(color(item, "colors." + key));
      }
    });
    else if (name == "window") object(value, name, [this](const auto& key, const auto& item) {
      if (key == "width") options_.width = integer(item, "window.width", 1, 8192);
      else if (key == "height") options_.height = integer(item, "window.height", 1, 8192);
      else if (key == "padding") options_.padding_x = options_.padding_y = integer(item, "window.padding", 0, 512);
      else if (key == "padding-x") options_.padding_x = integer(item, "window.padding-x", 0, 512);
      else if (key == "padding-y") options_.padding_y = integer(item, "window.padding-y", 0, 512);
      else throw std::runtime_error("unknown window setting: " + key);
    });
    else if (name == "cursor") object(value, name, [this](const auto& key, const auto& item) {
      if (key == "shape") options_.cursor.shape = cursor_shape(string(item, "cursor.shape"));
      else if (key == "blink") options_.cursor.blink = boolean(item, "cursor.blink");
      else throw std::runtime_error("unknown cursor setting: " + key);
    });
    else if (name == "input") object(value, name, [this](const auto& key, const auto& item) {
      if (key == "mouse-reporting") options_.mouse_reporting = boolean(item, "input." + key);
      else if (key == "alternate-scroll") options_.alternate_scroll = boolean(item, "input." + key);
      else if (key == "scroll-lines") options_.scroll_lines = integer(item, "input." + key, 1, 32);
      else if (key == "touchpad-scroll-per-mille") options_.touchpad_scroll_per_mille = integer(item, "input." + key, 100, 5000);
      else throw std::runtime_error("unknown input setting: " + key);
    });
    else if (name == "scrollback-lines") options_.history_lines = integer(value, name, 0, 100000);
    else if (name == "standalone") {
      const auto resolved = resolve(value); const auto* boolean = lang::as_boolean(resolved);
      if (!boolean) throw std::runtime_error("standalone must be true or false");
      options_.standalone = *boolean;
    } else if (name == "shell") {
      const auto resolved = resolve(value); options_.shell.clear();
      if (const auto* list = lang::as_array(resolved)) for (const auto& entry : list->values) options_.shell.push_back(string(entry, name));
      else options_.shell.push_back(string(resolved, name));
    } else throw std::runtime_error("unknown zterm setting: " + name);
  }
};

std::string default_config() {
  if (const auto* path = std::getenv("ZTERM_CONFIG"); path && *path) return path;
  std::filesystem::path base;
  if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) base = xdg;
  else if (const auto* home = std::getenv("HOME")) base = std::filesystem::path(home) / ".config";
  const auto user = base / "zterm/config.zw";
  if (std::filesystem::exists(user)) return user.string();
  const auto* environment = std::getenv("XDG_CONFIG_DIRS");
  const std::string directories = environment ? environment : "/etc/xdg";
  for (size_t start = 0; start <= directories.size();) {
    const auto end = directories.find(':', start);
    const auto directory = directories.substr(start, end == std::string::npos ? end : end - start);
    if (!directory.empty()) {
      const auto path = std::filesystem::path(directory) / "zterm/config.zw";
      if (std::filesystem::exists(path)) return path.string();
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return user.string();
}

}  // namespace

Options load_options(int argc, char** argv) {
  Options options;
  if (argc > 1 && std::string_view(argv[1]) == "daemon") {
    if (argc == 3 && std::string_view(argv[2]) == "--help") { options.action = Options::Action::Help; return options; }
    if (argc != 2) throw std::runtime_error("usage: zterm daemon");
    options.action = Options::Action::Daemon; return options;
  }
  std::vector<std::function<void(Options&)>> overrides;
  const auto set = [&overrides](auto member, auto value) {
    overrides.emplace_back([member, value = std::move(value)](Options& target) { target.*member = value; });
  };
  std::optional<std::string> config;
  bool no_config = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    std::string font_flag;
    std::string FontFamilies::* font_field = nullptr;
    for (const auto& [name, member] : font_fields) {
      const auto flag = name == "family" ? std::string("--font") : "--font-" + std::string(name);
      if (argument == flag || argument.starts_with(flag + "=")) { font_flag = flag; font_field = member; break; }
    }
    const auto value = [&](const std::string& name) -> std::string {
      if (argument != name) return argument.substr(name.size() + 1);
      if (i + 1 == argc) throw std::runtime_error(name + " requires a value");
      return argv[++i];
    };
    const auto number = [](const std::string& text, float low, float high, const std::string& name) {
      float result = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
      if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(result) || result < low || result > high)
        throw std::runtime_error(name + " must be in " + std::to_string(low) + ".." + std::to_string(high));
      return result;
    };
    if (argument == "--help") { options.action = Options::Action::Help; return options; }
    if (argument == "--config" || argument.starts_with("--config=")) { config = value("--config"); no_config = false; if (config->empty()) throw std::runtime_error("--config requires a path"); }
    else if (argument == "--no-config") { no_config = true; config.reset(); }
    else if (argument == "--standalone") set(&Options::standalone, true);
    else if (argument == "--padding" || argument.starts_with("--padding=") ||
             argument == "--padding-x" || argument.starts_with("--padding-x=") ||
             argument == "--padding-y" || argument.starts_with("--padding-y=")) {
      const auto name = argument.substr(0,argument.find('='));
      const auto text = value(name); unsigned padding = 0;
      const auto [end,error] = std::from_chars(text.data(),text.data()+text.size(),padding);
      if (error != std::errc{} || end != text.data()+text.size() || padding > 512)
        throw std::runtime_error("padding must be an integer in 0..512");
      if (name != "--padding-y") set(&Options::padding_x,padding);
      if (name != "--padding-x") set(&Options::padding_y,padding);
    }
    else if (argument == "--cursor" || argument.starts_with("--cursor=")) {
      const auto shape = cursor_shape(value("--cursor"));
      overrides.emplace_back([shape](Options& target) { target.cursor.shape = shape; });
    } else if (argument == "--cursor-blink" || argument == "--no-cursor-blink") {
      const bool blink = argument == "--cursor-blink";
      overrides.emplace_back([blink](Options& target) { target.cursor.blink = blink; });
    }
    else if (font_field) {
      std::string family = argument == font_flag ? "" : argument.substr(font_flag.size() + 1);
      while (i + 1 < argc && argv[i + 1][0] != '-') { if (!family.empty()) family += ' '; family += argv[++i]; }
      if (family.empty()) throw std::runtime_error("--font requires a font family");
      overrides.emplace_back([font_field, family = std::move(family)](Options& target) { target.fonts.*font_field = family; });
    } else if (argument == "--font-size" || argument.starts_with("--font-size=")) {
      const auto text = value("--font-size"); unsigned size = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), size);
      if (error != std::errc{} || end != text.data() + text.size() || size < 6 || size > 96) throw std::runtime_error("font size must be an integer in 6..96");
      set(&Options::size, size);
    } else if (argument == "--font-weight" || argument.starts_with("--font-weight=")) set(&Options::weight, Text::parse_weight(value("--font-weight")));
    else if (argument == "--text-rounding" || argument.starts_with("--text-rounding=")) set(&Options::text_rounding, number(value("--text-rounding"), 0, 1, "text rounding"));
    else if (argument == "--icon-scale" || argument.starts_with("--icon-scale=")) set(&Options::icon_scale, number(value("--icon-scale"), 0.5f, 3, "icon scale"));
    else if (argument == "--background-opacity" || argument.starts_with("--background-opacity=")) set(&Options::background_opacity, number(value("--background-opacity"), 0, 1, "background opacity"));
    else if (argument == "--shell" || argument.starts_with("--shell=")) {
      auto shell = value("--shell"); if (shell.empty()) throw std::runtime_error("--shell requires a program");
      set(&Options::shell, std::vector<std::string>{std::move(shell)});
    } else if (argument == "-e" || argument == "--") {
      if (i + 1 == argc) throw std::runtime_error(argument + " requires a command");
      std::vector<std::string> command;
      for (++i; i < argc; ++i) command.emplace_back(argv[i]);
      set(&Options::command, std::move(command)); break;
    } else throw std::runtime_error("unknown or incomplete option: " + argument);
  }
  if (!no_config) {
    options.config_path = config.value_or(default_config());
    const auto* environment = std::getenv("ZTERM_CONFIG");
    if (config || (environment && *environment) || std::filesystem::exists(options.config_path)) {
      const auto parsed = lang::parse_config_file(options.config_path);
      if (!parsed.ok()) {
        std::string errors;
        for (const auto& error : parsed.diagnostics) if (error.level == lang::DiagnosticLevel::error)
          errors += options.config_path + ":" + std::to_string(error.location.line) + ":" + std::to_string(error.location.column) + ": " + error.message + "\n";
        throw std::runtime_error(errors);
      }
      try { Decoder(options, parsed.config).apply(parsed.config); }
      catch (const std::exception& error) { throw std::runtime_error(options.config_path + ":" + error.what()); }
    }
  }
  for (const auto& override : overrides) override(options);
  return options;
}

void print_help() {
  std::puts("zterm daemon\n"
            "zterm [--config FILE] [--no-config] [--standalone]\n"
             "      [--font FAMILY] [--font-size PIXELS] [--font-weight WEIGHT]\n"
             "      [--font-mono FAMILY] [--font-bold FAMILY] [--font-italic FAMILY]\n"
             "      [--font-bold-italic FAMILY] [--font-symbols FAMILY] [--font-emoji FAMILY]\n"
            "      [--text-rounding 0..1] [--icon-scale 0.5..3] [--cursor beam|line|block]\n"
             "      [--background-opacity 0..1] [--cursor-blink|--no-cursor-blink]\n"
             "      [--padding PIXELS] [--padding-x PIXELS] [--padding-y PIXELS]\n"
             "      [--shell PROGRAM] [-e COMMAND [ARGS...]]\n"
            "Config: $ZTERM_CONFIG or $XDG_CONFIG_HOME/zterm/config.zw; command-line flags override it.\n"
            "Ctrl+Shift+R: reload; Ctrl+Shift+C/V: copy/paste; drag or double/triple-click: select.\n"
            "Shift-drag: select in mouse-aware applications. Wheel/trackpad: scrollback or application input.\n"
            "Shift+PageUp/PageDown: scrollback. --standalone bypasses native zwwm bootstrap.");
}

}  // namespace zterm
