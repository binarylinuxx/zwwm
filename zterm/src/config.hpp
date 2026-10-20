#pragma once

#include <zterm/terminal.hpp>
#include <array>
#include <string>
#include <vector>

namespace zterm {

struct Options {
  enum class Action { Run, Help, Daemon };
  std::string font = "monospace";
  unsigned size = 16, weight = 400, history_lines = 10000;
  float text_rounding = 0.5f, icon_scale = 1.3f;
  bool standalone = false;
  bool mouse_reporting = true, alternate_scroll = true;
  unsigned scroll_lines = 3, touchpad_scroll_per_mille = 1000;
  CursorStyle cursor{};
  unsigned width = 960, height = 600;
  Color foreground{220,225,232}, background{16,19,25};
  std::array<float, 4> selection{0.22f,0.48f,0.9f,0.45f};
  std::vector<std::string> command, shell;
  std::vector<Color> palette;
  std::string config_path;
  Action action = Action::Run;
};

Options load_options(int argc, char** argv);
void print_help();

}  // namespace zterm
