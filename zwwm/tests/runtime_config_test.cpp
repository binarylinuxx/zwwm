#include "zwwm/runtime_config.hpp"

#include <wayland-server-core.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

struct ReloadState {
  std::shared_ptr<const zwwm::RuntimeConfig> config;
  int applied = 0;
};

void apply_reload(void* data, std::shared_ptr<const zwwm::RuntimeConfig> config) {
  auto* state = static_cast<ReloadState*>(data);
  state->config = std::move(config);
  ++state->applied;
}

zwwm::RuntimeConfigResult compile(const char* text) {
  const auto parsed = zwwm::lang::parse_config(text);
  assert(parsed.ok());
  return zwwm::compile_runtime_config(parsed.config);
}

}  // namespace

int main() {
  const auto defaults = compile("");
  assert(defaults.ok());
  assert(defaults.config->decoration.border_width == 4);
  assert(defaults.config->decoration.radius == 10);
  assert(defaults.config->layout.master_count == 1);
  assert(defaults.config->layout.master_ratio_percent == 60);
  assert(defaults.config->layout.outer_gap == 16);
  assert(defaults.config->layout.inner_gap == 12);
  assert(!defaults.config->animations.enabled);
  assert(defaults.config->animations.duration_ms == 280);
  assert(defaults.config->output.mode.preferred);
  assert(defaults.config->output.scale_per_mille == 1000);

  const auto valid = compile(R"(
    decoration = {
      enabled = true border-width = 7 radius = 13
      border-color = "#10203040" focused-border-color = "#abcdef"
    }
    layout = {
      default = "master-stack" master-count = 2 master-ratio = 70
      outer-gap = 5 inner-gap = 9 smart-gaps = true
    }
    animations = {
      enabled = true duration-ms = 310 open-window = false resize = true close = false
      spring = { stiffness = 200 damping = 30 mass-per-mille = 1200 }
      cubic-bezier = { x1 = 100 y1 = 200 x2 = 700 y2 = 900 }
    }
    output = { mode = "2560x1440@143.95" scale-per-mille = 1250 transform = "90" }
  )");
  assert(valid.ok());
  assert(valid.config->decoration.border_width == 7);
  assert(valid.config->decoration.border_color == zwwm::Color(0x10, 0x20, 0x30, 0x40));
  assert(valid.config->decoration.focused_border_color == zwwm::Color(0xab, 0xcd, 0xef, 0xff));
  assert(valid.config->layout.master_count == 2);
  assert(valid.config->layout.master_ratio_percent == 70);
  assert(valid.config->animations.duration_ms == 310);
  assert(valid.config->animations.spring_stiffness == 200);
  assert(valid.config->animations.spring_mass_per_mille == 1200);
  assert(valid.config->animations.bezier_x2_per_mille == 700);
  assert(!valid.config->animations.open_window && !valid.config->animations.close);
  assert(!valid.config->output.mode.preferred && valid.config->output.mode.width == 2560);
  assert(valid.config->output.mode.refresh_millihz == 143950);
  assert(valid.config->output.transform == zwwm::OutputTransform::rotate_90);
  assert(valid.config->output.fractional_scale_120() == 150);
  assert(valid.config->output.integer_scale() == 2);
  const auto logical_output = valid.config->output.logical_size({2560, 1440});
  assert(logical_output.width == 1152 && logical_output.height == 2048);
  const auto physical_point = valid.config->output.logical_to_physical({100, 200}, {2560, 1440});
  const auto logical_point = valid.config->output.physical_to_logical(physical_point, {2560, 1440});
  assert(logical_point.x == 100 && logical_point.y == 200);

  assert(!compile("decoration = { border-width = -1 }").ok());
  assert(!compile("decoration = { border-color = \"#12345g\" }").ok());
  assert(!compile("layout = { master-count = 0 }").ok());
  assert(!compile("layout = { master-ratio = 9 }").ok());
  assert(!compile("layout = { master-ratio = 91 }").ok());
  assert(!compile("layout = { default = \"columns\" }").ok());
  assert(!compile("animations = { duration-ms = -1 }").ok());
  assert(!compile("animations = { spring = { stiffness = 0 } }").ok());
  assert(!compile("animations = { cubic-bezier = { y2 = 1001 } }").ok());
  assert(!compile("output = { mode = \"1920x1080\" }").ok());
  assert(!compile("output = { scale-per-mille = 200 }").ok());
  assert(!compile("output = { transform = \"flip\" }").ok());

  const auto shorthand = compile("layout = { gaps = 20 outer-gap = 3 }");
  assert(shorthand.ok());
  assert(shorthand.config->layout.outer_gap == 3);
  assert(shorthand.config->layout.inner_gap == 20);

  const auto smart = compile("layout = { outer-gap = 25 smart-gaps = true }");
  assert(smart.ok());
  const auto one = smart.config->placements({1}, {{0, 0}, {100, 100}});
  assert(one.size() == 1);
  assert(one[0].bounds.origin.x == 0 && one[0].bounds.origin.y == 0);
  assert(one[0].bounds.size.width == 100 && one[0].bounds.size.height == 100);
  const auto two = smart.config->placements({1, 2}, {{0, 0}, {100, 100}});
  assert(two.size() == 2);
  assert(two[0].bounds.origin.x == 25 && two[0].bounds.origin.y == 25);

  const auto borderless = compile("decoration = { enabled = false border-width = 99 radius = 50 }");
  assert(borderless.ok());
  assert(borderless.config->border_width() == 0 && borderless.config->radius() == 0);
  const auto content = borderless.config->content_bounds({{4, 5}, {80, 70}});
  assert(content.origin.x == 4 && content.origin.y == 5);
  assert(content.size.width == 80 && content.size.height == 70);

  const auto path = (std::filesystem::temp_directory_path() / "zwwm-runtime-config-test.zw").string();
  {
    FILE* file = std::fopen(path.c_str(), "w");
    assert(file != nullptr);
    std::fputs("layout = { outer-gap = 31 } decoration = { border-width = 6 radius = 18 }", file);
    std::fclose(file);
  }
  const auto reloaded = zwwm::load_runtime_config_file(path);
  assert(reloaded.ok());
  assert(reloaded.config->layout.outer_gap == 31);
  assert(reloaded.config->border_width() == 6 && reloaded.config->radius() == 18);
  const auto before_invalid = reloaded.config;
  {
    FILE* file = std::fopen(path.c_str(), "w");
    assert(file != nullptr);
    std::fputs("layout = { outer-gap = -1 }", file);
    std::fclose(file);
  }
  const auto invalid_reload = zwwm::load_runtime_config_file(path);
  assert(!invalid_reload.ok());
  assert(before_invalid->layout.outer_gap == 31);  // Failed reload leaves the active config unchanged.
  assert(std::remove(path.c_str()) == 0);

  const auto reload_path = (std::filesystem::temp_directory_path() /
                            ("zwwm-reload-" + std::to_string(getpid()) + ".zw")).string();
  const auto replacement_path = reload_path + ".new";
  {
    std::ofstream file(reload_path);
    file << "layout = { outer-gap = 4 }";
  }
  const auto initial = zwwm::load_runtime_config_file(reload_path);
  assert(initial.ok());
  wl_event_loop* loop = wl_event_loop_create();
  assert(loop != nullptr);
  ReloadState state{initial.config};
  {
    zwwm::ConfigReloader watcher(loop, reload_path, initial.config, apply_reload, &state);
    assert(watcher.start());
    {
      std::ofstream file(replacement_path);
      file << "layout = { outer-gap = 27 } decoration = { border-width = 8 radius = 19 }";
    }
    std::filesystem::rename(replacement_path, reload_path);  // Atomic-save pattern.
    assert(wl_event_loop_dispatch(loop, 50) >= 0);
    assert(wl_event_loop_dispatch(loop, 0) >= 0);
    assert(state.applied == 1);
    assert(state.config->layout.outer_gap == 27);
    assert(state.config->border_width() == 8 && state.config->radius() == 19);
    const auto last_good = state.config;
    {
      std::ofstream file(replacement_path);
      file << "decoration = { radius = -4 }";
    }
    std::filesystem::rename(replacement_path, reload_path);
    assert(wl_event_loop_dispatch(loop, 50) >= 0);
    assert(wl_event_loop_dispatch(loop, 0) >= 0);
    assert(state.applied == 1);
    assert(state.config == last_good);
  }
  wl_event_loop_destroy(loop);
  assert(std::remove(reload_path.c_str()) == 0);
}
