#include "zwwm/lang/config.hpp"

#include <cassert>
#include <vector>

int main() {
  const auto parsed = zwwm::lang::parse_config(R"(
    -- Startup and a user variable
    exec-sh-on-startup = ["waybar", "swww-daemon"]
    SUPERKEY = "Mod4"
    bind = $SUPERKEY, Return, exec, "ghostty"
    decoration = {
      enabled = true
      border-width = 2
      radius = 10
      border-color = "#112233"
      focused-border-color = "#445566ff"
    }
    layout = { default = "master-stack" master-count = 1 master-ratio = 60 gaps = 8 smart-gaps = true }
    animations = { enabled = true duration-ms = 200 spring = { stiffness = 160 damping = 24 mass-per-mille = 1000 } cubic-bezier = { x1 = 200 y1 = 0 x2 = 800 y2 = 1000 } open-window = true resize = true close = true }
    output = { mode = "2560x1440@143.95" scale-per-mille = 1250 transform = "90" }
    rule = {
      match = { app-id = "firefox" }
      set = { floating = true decoration = "client" }
    }
  )");
  assert(parsed.ok());
  assert(parsed.config.assignments.size() == 8);

  const auto values = zwwm::lang::parse_config("number = -42\nempty = null\nitems = [1, 2,]\n");
  assert(values.ok());
  assert(*zwwm::lang::as_integer(*zwwm::lang::find_assignment(values.config, "number")) == -42);
  assert(std::holds_alternative<std::monostate>(zwwm::lang::find_assignment(values.config, "empty")->data));
  assert(zwwm::lang::as_array(*zwwm::lang::find_assignment(values.config, "items"))->values.size() == 2);

  const auto validation = zwwm::lang::validate_config(parsed.config);
  assert(validation.ok());

  const auto example = zwwm::lang::parse_config_file(ZW_LANG_EXAMPLE_PATH);
  assert(example.ok());
  assert(example.config.assignments.size() == 15);
  assert(zwwm::lang::validate_config(example.config).ok());

  const auto invalid = zwwm::lang::parse_config("bind = $MISSING, Return, exec\n");
  assert(invalid.ok());
  assert(!zwwm::lang::validate_config(invalid.config).ok());

  const auto invalid_radius = zwwm::lang::parse_config("decoration = { radius = -1 }\n");
  assert(invalid_radius.ok());
  assert(!zwwm::lang::validate_config(invalid_radius.config).ok());

  for (const char* text : {
           "decoration = { border-width = -1 }\n",
           "decoration = { border-color = \"red\" }\n",
           "layout = { master-count = 0 }\n",
           "layout = { master-ratio = 91 }\n",
           "layout = { outer-gap = -1 }\n",
           "layout = { default = \"tiling\" }\n",
           "animations = { duration-ms = -1 }\n",
           "animations = { spring = { mass-per-mille = 0 } }\n",
           "animations = { cubic-bezier = { x1 = 1001 } }\n",
           "output = { mode = \"2560@60\" }\n",
           "output = { scale-per-mille = 0 }\n",
           "output = { transform = \"flipped\" }\n",
       }) {
    const auto item = zwwm::lang::parse_config(text);
    assert(item.ok());
    assert(!zwwm::lang::validate_config(item.config).ok());
  }

  const std::vector<zwwm::lang::ValidationRule> rules = {
      [](const zwwm::lang::Assignment& assignment, zwwm::lang::ValidationContext& context) {
        if (assignment.key == "exec-sh-on-startup") {
          context.error(assignment.location, "startup commands are disabled by this caller");
        }
      },
  };
  assert(!zwwm::lang::validate_config(parsed.config, rules).ok());
}
