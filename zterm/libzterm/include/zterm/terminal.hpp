#pragma once

#include <array>
#include <cstdint>
#include <compare>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <span>
#include <optional>
#include <utility>
#include <vector>

namespace zterm {

struct Color {
  std::uint8_t red = 220, green = 225, blue = 232;
  bool operator==(const Color&) const = default;
};
struct Attributes {
  bool bold = false, italic = false, underline = false, strike = false,
       reverse = false, dim = false, hidden = false;
  bool operator==(const Attributes&) const = default;
};
struct Cell {
  std::array<char32_t, 8> chars{};
  Color fg{}, bg{16, 19, 25};
  std::int16_t fg_index = -1, bg_index = -1;
  Attributes attrs{};
  std::uint8_t width = 1, natural_width = 1;
};
struct Position { int row = 0, col = 0; };
enum Modifier : unsigned { ModNone = 0, ModShift = 1, ModAlt = 2, ModCtrl = 4 };
enum class Key : unsigned {
  None, Enter, Backspace, Tab, Escape, Up, Down, Left, Right,
  Home, End, Insert, Delete, PageUp, PageDown, Function0 = 256
};
enum class SelectionMode { Character, Word, Line };
enum class CursorShape { Block, Beam, Underline };
struct CursorStyle { CursorShape shape = CursorShape::Beam; bool blink = true; };
struct MousePosition { int row = 0, col = 0, x = 0, y = 0; };

class Terminal {
 public:
  Terminal(int rows, int columns, std::size_t history_limit = 10000);
  void feed(std::string_view bytes);
  void resize(int rows, int columns);
  void key(Key key, unsigned modifiers = ModNone);
  void character(char32_t character, unsigned modifiers = ModNone);
  void focus(bool focused);
  void scroll(int lines);
  void begin_selection(int row, int column, SelectionMode mode = SelectionMode::Character);
  void extend_selection(int row, int column);
  void clear_selection();
  bool has_selection() const { return selection_; }
  std::pair<int, int> selection_range(int row) const;
  std::string selected_text() const;
  void paste(std::string_view text);
  void set_default_colors(Color foreground, Color background);
  static std::array<Color, 16> default_palette();
  void set_palette(std::span<const Color> colors);
  void set_history_limit(std::size_t lines);
  void set_cursor_style(CursorStyle style);
  CursorStyle cursor_style() const { return cursor_override_.value_or(default_cursor_style_); }
  Color foreground_color() const { return default_foreground_; }
  void set_input_options(bool mouse_reporting, bool alternate_scroll);
  void set_cell_size(unsigned width, unsigned height);
  bool reports_mouse() const;
  bool mouse_button(unsigned button, bool pressed, MousePosition position, unsigned modifiers = ModNone);
  bool mouse_motion(MousePosition position, unsigned modifiers = ModNone);
  bool mouse_wheel(bool horizontal, int steps, MousePosition position, unsigned modifiers = ModNone);
  bool alternate_scroll(int lines, bool horizontal = false, unsigned modifiers = ModNone);
  Cell cell(int row, int column) const;
  bool cursor_at(int row, int column) const;
  Position cursor_position() const { return screen().cursor; }
  int rows() const { return rows_; }
  int columns() const { return columns_; }
  std::uint64_t revision() const { return revision_; }
  std::string take_output();
  bool dirty = true;
  std::function<void(const std::string&)> title;

 private:
  struct Screen {
    std::vector<Cell> cells;
    std::vector<bool> wrapped;
    Position cursor{}, saved{};
    Cell pen{}, saved_pen{};
    bool wrap_pending = false, saved_wrap_pending = false;
  };
  struct HistoryLine { std::vector<Cell> cells; bool wrapped = false; };
  struct ResizeSnapshot { Screen screen; int rows, columns; };
  struct SelectionPoint {
    std::int64_t line = 0;
    int column = 0;
    auto operator<=>(const SelectionPoint&) const = default;
  };
  enum class Parser { Ground, Escape, Csi, Osc, OscEscape, Charset, Ignore, IgnoreEscape };
  enum class MouseTracking { None, Press, Clicks, Drag, All };
  Screen primary_, alternate_;
  int rows_, columns_, top_ = 0, bottom_, scroll_ = 0;
  std::size_t history_limit_;
  std::deque<HistoryLine> history_;
  std::vector<HistoryLine> primary_resize_tail_;
  std::optional<ResizeSnapshot> alternate_resize_source_;
  std::int64_t history_base_ = 0;
  bool selection_ = false, selection_anchor_valid_ = false;
  SelectionMode selection_mode_ = SelectionMode::Character;
  SelectionPoint anchor_first_{}, anchor_last_{}, selection_first_{}, selection_last_{};
  std::vector<bool> tabs_;
  bool alternate = false, application_cursor_ = false, origin_ = false,
       autowrap_ = true, insert_ = false, newline_ = false, cursor_visible_ = true,
       focus_reporting_ = false, bracketed_paste_ = false, dec_graphics_ = false;
  char charset_target_ = 0;
  Parser parser_ = Parser::Ground;
  std::string sequence_, output_;
  char32_t utf8_ = 0, utf8_min_ = 0;
  unsigned utf8_left_ = 0;
  std::uint64_t revision_ = 1;
  Color default_foreground_{220,225,232}, default_background_{16,19,25};
  std::array<Color, 16> palette_{};
  CursorStyle default_cursor_style_{};
  std::optional<CursorStyle> cursor_override_;
  MouseTracking mouse_tracking_ = MouseTracking::None;
  bool mouse_enabled_ = true, default_alternate_scroll_ = true,
       mouse_sgr_ = false, mouse_utf8_ = false, mouse_urxvt_ = false, mouse_pixels_ = false;
  std::optional<bool> alternate_scroll_override_;
  std::array<bool, 5> mouse_buttons_{};
  std::array<int, 5> last_mouse_motion_{-1,-1,-1,-1,-1};
  unsigned cell_pixel_width_ = 8, cell_pixel_height_ = 16;
  Screen& screen() { return alternate ? alternate_ : primary_; }
  const Screen& screen() const { return alternate ? alternate_ : primary_; }
  Cell blank() const;
  Cell default_cell() const;
  void reset();
  void put(char32_t character);
  void linefeed();
  void scroll_up(int first, int last, int count, bool history);
  void scroll_down(int first, int last, int count);
  void push_history(const Cell* cells, bool wrapped);
  std::int64_t visible_origin() const;
  std::pair<SelectionPoint, SelectionPoint> selection_extent(int row, int column) const;
  void erase(int first, int last);
  void repair_line(int row);
  int used_columns(std::span<const Cell> cells) const;
  void reflow_primary(int rows, int columns);
  void resize_alternate(int rows, int columns);
  void csi(char command);
  void osc();
  void sgr(const std::vector<int>& parameters);
  void mode(int parameter, bool enabled, bool private_mode);
  void output(std::string_view bytes);
  void mouse_report(unsigned button, bool release, MousePosition position, unsigned modifiers);
  void changed() { dirty = true; ++revision_; }
};

}  // namespace zterm
