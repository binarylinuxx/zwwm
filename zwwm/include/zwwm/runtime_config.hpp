#pragma once


#include <zwayland/server/display.hpp>
#include "zwwm/lang/config.hpp"
#include "zwwm/layout/master_stack.hpp"
#include "zwwm/animation.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "zwwm/unique_fd.hpp"
namespace zwwm {
namespace renderer { struct ShaderSources; }

struct Color {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
  std::uint8_t alpha = 255;

  [[nodiscard]] std::array<float, 4> normalized() const;
  [[nodiscard]] std::uint32_t argb8888() const;
  bool operator==(const Color&) const = default;
};

enum class CustomShaderRole : std::uint8_t { window, border, background };
using CustomShaderValue = std::variant<bool, std::int32_t, Color, std::vector<std::int32_t>,
                                       std::vector<Color>>;

struct CustomShaderConfig {
  std::string name;
  CustomShaderRole role = CustomShaderRole::window;
  std::string source;
  std::vector<std::pair<std::string, CustomShaderValue>> values;
};

struct DecorationConfig {
  bool enabled = true;
  std::uint32_t border_width = 4;
  std::uint32_t radius = 10;
  Color border_color{0x2e, 0x37, 0x44, 0xff};
  Color focused_border_color{0x3d, 0xa6, 0xf2, 0xff};
};

struct BlurConfig {
  bool enabled = true;
  std::uint32_t radius = 8;
  std::uint32_t passes = 3;
  std::uint32_t brightness_percent = 100;
  std::uint32_t contrast_percent = 100;
  std::uint32_t saturation_percent = 100;
  std::uint32_t noise_percent = 0;
};

enum class GlassQuality : std::uint8_t { low = 1, medium = 2, high = 3 };

struct GlassConfig {
  bool enabled = false;
  GlassQuality quality = GlassQuality::medium;
  std::uint32_t blur_radius = 18;
  std::uint32_t refraction = 8;
  std::uint32_t dispersion = 3;
  std::uint32_t ior_per_mille = 1500;
  std::uint32_t thickness = 12;
  std::uint32_t roughness_percent = 18;
  Color tint{0xb8, 0xd8, 0xff, 0x20};
  std::uint32_t brightness_percent = 110;
  std::uint32_t saturation_percent = 120;
  std::uint32_t frost_percent = 8;
  std::uint32_t animation_speed = 0;
};

struct LayoutConfig {
  enum class Kind : std::uint8_t { master_stack, focus_fibonacci, endless_canvas };

  Kind kind = Kind::master_stack;
  std::uint32_t master_count = 1;
  std::uint32_t master_ratio_percent = 60;
  std::uint32_t outer_gap = 16;
  std::uint32_t inner_gap = 12;
  std::uint32_t min_zoom_per_mille = 250;
  std::uint32_t max_zoom_per_mille = 1350;
  bool smart_gaps = false;
};

enum class FocusMode : std::uint8_t { by_click, hover };

struct InputConfig {
  FocusMode focus_mode = FocusMode::by_click;
  bool reverse_mouse_scrolling = false;
};

struct KeyboardConfig {
  std::string rules;
  std::string model;
  std::string layout;
  std::string variant;
  std::string options;
};

enum class OutputTransform : std::uint8_t { normal, rotate_90, rotate_180, rotate_270 };

struct OutputMode {
  bool preferred = true;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t refresh_millihz = 0;
};

struct OutputConfig {
  OutputMode mode;
  std::uint32_t scale_per_mille = 1000;
  std::uint32_t bit_depth = 8;
  OutputTransform transform = OutputTransform::normal;

  [[nodiscard]] renderer::Size logical_size(renderer::Size physical) const;
  [[nodiscard]] renderer::Point logical_to_physical(renderer::Point point, renderer::Size physical) const;
  [[nodiscard]] renderer::Point physical_to_logical(renderer::Point point, renderer::Size physical) const;
  [[nodiscard]] renderer::Rect physical_bounds(renderer::Rect bounds, renderer::Size physical) const;
  [[nodiscard]] std::uint32_t integer_scale() const;
  [[nodiscard]] std::uint32_t fractional_scale_120() const;
};

enum class KeyAction : std::uint8_t {
  exec, reload, exit, focus, killactive, killsession, togglefloating, fullscreen, tag, movetotag,
  zoomin, zoomout
};

struct Keybinding {
  std::string modifiers;
  std::string key;
  KeyAction action = KeyAction::exec;
  std::string argument;
};

struct WindowRule {
  std::string app_id;
  std::string title;
  std::optional<bool> floating;
  std::uint32_t width = 0, height = 0;
  bool width_percent = false, height_percent = false;
  std::optional<bool> blur;
  std::optional<std::uint32_t> blur_radius;
  std::optional<bool> glass;
  std::optional<float> opacity;
  std::string window_shader;
  std::string border_shader;
};

struct LayerEffect {
  std::uint32_t blur_radius = 0;
  float ignore_alpha = 0.0F;
  float opacity = 1.0F;
};

struct LayerRule {
  std::string name_space;
  LayerEffect effect;
};

class RuntimeConfig {
 public:
  DecorationConfig decoration;
  BlurConfig blur;
  GlassConfig glass;
  std::vector<CustomShaderConfig> shaders{
      {"window", CustomShaderRole::window, "shaders/window.frag", {}},
      {"border", CustomShaderRole::border, "shaders/border.frag", {}},
      {"background", CustomShaderRole::background, "shaders/background.frag", {}}};
  std::string window_shader = "window";
  std::string border_shader = "border";
  std::string background_shader = "background";
  std::string background_image = "background.png";
  LayoutConfig layout;
  InputConfig input;
  KeyboardConfig keyboard;
  OutputConfig output;
  std::vector<std::pair<std::string, OutputConfig>> outputs;
  AnimationConfig animations;
  std::vector<std::pair<std::string, std::string>> environment;
  std::vector<std::string> startup_commands;
  std::vector<Keybinding> keybindings;
  std::vector<WindowRule> window_rules;
  std::vector<LayerRule> layer_rules;

  [[nodiscard]] std::uint32_t border_width() const;
  [[nodiscard]] std::uint32_t radius() const;
  [[nodiscard]] std::int32_t shader_integer(std::string_view shader, std::string_view value,
                                             std::int32_t fallback = 0) const;
  [[nodiscard]] std::vector<layout::Placement> placements(
      const std::vector<layout::WindowId>& windows, renderer::Rect work_area) const;
  [[nodiscard]] renderer::Rect content_bounds(renderer::Rect placement) const;
  [[nodiscard]] LayerEffect layer_effect(std::string_view name_space) const;
  [[nodiscard]] const OutputConfig& output_for(std::string_view connector) const;
};

struct RuntimeConfigResult {
  std::shared_ptr<const RuntimeConfig> config;
  std::vector<lang::Diagnostic> diagnostics;

  [[nodiscard]] bool ok() const;
};

[[nodiscard]] RuntimeConfigResult compile_runtime_config(const lang::Config& config);

struct LoadedRuntimeConfig {
  std::shared_ptr<const RuntimeConfig> config;
  std::string path;
  std::string error;
};

// Parses, validates, and compiles a file without changing any live state.
[[nodiscard]] RuntimeConfigResult load_runtime_config_file(const std::string& path);
[[nodiscard]] bool load_shader_sources(const RuntimeConfig& config,
                                       renderer::ShaderSources* sources,
                                       std::string* error);

class ConfigReloader {
 public:
  using Apply = void (*)(void*, std::shared_ptr<const RuntimeConfig>);
  using Error = void (*)(void*, const std::string&);

  ConfigReloader(::zwayland::server::EventLoop* event_loop, std::string path,
                 std::shared_ptr<const RuntimeConfig> initial, Apply apply, void* data,
                 Error error = nullptr);
  ~ConfigReloader();
  ConfigReloader(const ConfigReloader&) = delete;
  ConfigReloader& operator=(const ConfigReloader&) = delete;

  [[nodiscard]] bool start();
  [[nodiscard]] const std::string& last_error() const;

 private:
  static int on_inotify(int fd, std::uint32_t mask, void* data);
  static void reload_idle(void* data);
  void reload();

  ::zwayland::server::EventLoop* event_loop_ = nullptr;
  int source_ = -1;
  std::string path_;
  std::string directory_;
  std::string basename_;
  bool watching_config_directory_ = false;
  std::shared_ptr<const RuntimeConfig> config_;
  Apply apply_ = nullptr;
  Error error_ = nullptr;
  void* data_ = nullptr;
  UniqueFd fd_;
  bool pending_ = false;
  std::string last_error_;
};

// Selects and loads the startup configuration. Throws after printing diagnostics
// when an explicitly selected or discovered file is invalid.
[[nodiscard]] LoadedRuntimeConfig load_runtime_config();
[[nodiscard]] std::string format_runtime_config_error(
    const std::string& path, const std::vector<lang::Diagnostic>& diagnostics);

}  // namespace zwwm
