#include <zterm/terminal.hpp>

#include <algorithm>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace zterm {
namespace {

Color palette(int index, const std::array<Color, 16>* custom = nullptr) {
  constexpr std::array<Color, 16> base{{
      {0,0,0},{205,0,0},{0,205,0},{205,205,0},{0,0,238},{205,0,205},{0,205,205},{229,229,229},
      {127,127,127},{255,0,0},{0,255,0},{255,255,0},{92,92,255},{255,0,255},{0,255,255},{255,255,255}}};
  index = std::clamp(index, 0, 255);
  if (index < 16) return custom ? (*custom)[index] : base[index];
  if (index >= 232) {
    const auto value = static_cast<std::uint8_t>(8 + (index - 232) * 10);
    return {value, value, value};
  }
  index -= 16;
  constexpr std::array<std::uint8_t, 6> levels{0, 95, 135, 175, 215, 255};
  return {levels[index / 36], levels[(index / 6) % 6], levels[index % 6]};
}

void utf8(std::string& text, char32_t character) {
  if (character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff)) character = 0xfffd;
  if (character < 0x80) text.push_back(character);
  else if (character < 0x800) { text.push_back(0xc0 | (character >> 6)); text.push_back(0x80 | (character & 63)); }
  else if (character < 0x10000) {
    text.push_back(0xe0 | (character >> 12)); text.push_back(0x80 | ((character >> 6) & 63)); text.push_back(0x80 | (character & 63));
  } else {
    text.push_back(0xf0 | (character >> 18)); text.push_back(0x80 | ((character >> 12) & 63));
    text.push_back(0x80 | ((character >> 6) & 63)); text.push_back(0x80 | (character & 63));
  }
}

void validate_size(int rows, int columns) {
  if (rows < 1 || columns < 1 || rows > 4096 || columns > 4096 ||
      static_cast<std::uint64_t>(rows) * columns > 1024 * 1024)
    throw std::invalid_argument("terminal dimensions exceed limits");
}

std::vector<int> parameters(std::string_view sequence) {
  std::vector<int> result;
  int value = 0;
  for (const char character : sequence) {
    if (character >= '0' && character <= '9') value = std::min(1000000, value * 10 + character - '0');
    else if (character == ';' || character == ':') {
      if (result.size() >= 64) break;
      result.push_back(value); value = 0;
    }
  }
  result.push_back(value);
  return result;
}

}  // namespace

Terminal::Terminal(int rows, int columns, std::size_t history_limit)
    : rows_(rows), columns_(columns), bottom_(rows - 1), history_limit_(std::min(history_limit, std::size_t{100000})) {
  validate_size(rows, columns);
  palette_ = default_palette();
  reset();
}

Cell Terminal::default_cell() const {
  Cell result; result.fg = default_foreground_; result.bg = default_background_; return result;
}

Cell Terminal::blank() const {
  Cell cell; cell.fg = screen().pen.fg; cell.bg = screen().pen.bg;
  cell.fg_index = screen().pen.fg_index; cell.bg_index = screen().pen.bg_index;
  return cell;
}

void Terminal::reset() {
  primary_resize_tail_.clear(); alternate_resize_source_.reset();
  primary_ = {}; alternate_ = {}; alternate = false;
  primary_.cells.resize(static_cast<size_t>(rows_) * columns_, default_cell());
  alternate_.cells.resize(primary_.cells.size(), default_cell());
  primary_.pen = primary_.saved_pen = alternate_.pen = alternate_.saved_pen = default_cell();
  primary_.wrapped.assign(rows_, false); alternate_.wrapped.assign(rows_, false);
  tabs_.assign(columns_, false);
  for (int col = 8; col < columns_; col += 8) tabs_[col] = true;
  history_.clear(); history_base_ = 0; clear_selection(); top_ = scroll_ = 0; bottom_ = rows_ - 1;
  application_cursor_ = origin_ = insert_ = newline_ = focus_reporting_ = bracketed_paste_ = dec_graphics_ = false;
  cursor_visible_ = autowrap_ = true;
  cursor_override_.reset(); alternate_scroll_override_.reset(); mouse_tracking_ = MouseTracking::None;
  mouse_sgr_ = mouse_utf8_ = mouse_urxvt_ = mouse_pixels_ = false;
  mouse_buttons_.fill(false); last_mouse_motion_.fill(-1);
  utf8_left_ = 0; sequence_.clear(); parser_ = Parser::Ground;
  changed();
}

void Terminal::output(std::string_view bytes) {
  if (output_.size() + bytes.size() > 4 * 1024 * 1024) throw std::runtime_error("terminal output queue is full");
  output_.append(bytes);
}

void Terminal::push_history(const Cell* cells, bool wrapped) {
  if (!history_limit_) { ++history_base_; clear_selection(); return; }
  history_.push_back({std::vector<Cell>(cells, cells + used_columns({cells, static_cast<size_t>(columns_)})), wrapped});
  if (scroll_) ++scroll_;
  if (history_.size() > history_limit_) {
    history_.pop_front(); ++history_base_;
    if (selection_first_.line < history_base_) clear_selection();
  }
  scroll_ = std::min(scroll_, static_cast<int>(history_.size()));
}

void Terminal::scroll_up(int first, int last, int count, bool history) {
  count = std::clamp(count, 1, last - first + 1);
  auto& cells = screen().cells;
  auto& wrapped = screen().wrapped;
  if (history && !alternate && first == 0 && last == rows_ - 1)
    for (int row = 0; row < count; ++row) push_history(cells.data() + row * columns_, wrapped[row]);
  std::move(cells.begin() + (first + count) * columns_, cells.begin() + (last + 1) * columns_, cells.begin() + first * columns_);
  std::fill(cells.begin() + (last + 1 - count) * columns_, cells.begin() + (last + 1) * columns_, blank());
  for (int row = first; row <= last - count; ++row) wrapped[row] = wrapped[row + count];
  for (int row = last + 1 - count; row <= last; ++row) wrapped[row] = false;
}

void Terminal::scroll_down(int first, int last, int count) {
  count = std::clamp(count, 1, last - first + 1);
  auto& cells = screen().cells;
  auto& wrapped = screen().wrapped;
  std::move_backward(cells.begin() + first * columns_, cells.begin() + (last + 1 - count) * columns_, cells.begin() + (last + 1) * columns_);
  std::fill(cells.begin() + first * columns_, cells.begin() + (first + count) * columns_, blank());
  for (int row = last; row >= first + count; --row) wrapped[row] = wrapped[row - count];
  for (int row = first; row < first + count; ++row) wrapped[row] = false;
}

void Terminal::linefeed() {
  auto& state = screen(); state.wrap_pending = false;
  if (state.cursor.row == bottom_) scroll_up(top_, bottom_, 1, true);
  else state.cursor.row = std::min(rows_ - 1, state.cursor.row + 1);
}

void Terminal::repair_line(int row) {
  auto* line = screen().cells.data() + row * columns_;
  for (int col = 0; col < columns_; ++col) {
    if (line[col].width == 2) {
      if (col + 1 >= columns_) line[col] = blank();
      else { line[col + 1] = blank(); line[col + 1].width = 0; ++col; }
    } else if (line[col].width == 0) line[col] = blank();
  }
}

void Terminal::put(char32_t character) {
  auto& state = screen();
  int width = ::wcwidth(static_cast<wchar_t>(character));
  if (width < 0) width = 1;
  int previous = state.cursor.row * columns_ + state.cursor.col - (state.wrap_pending ? 0 : 1);
  if (previous >= 0 && state.cells[previous].width == 0) --previous;
  if (previous >= 0 && !state.cells[previous].chars[0]) previous = -1;
  const auto append = [&](Cell& cell) {
    for (auto& slot : cell.chars) if (!slot) { slot = character; break; }
  };
  bool joined = false;
  if (previous >= 0) {
    const auto& chars = state.cells[previous].chars;
    for (size_t i = 1; i < chars.size(); ++i) if (chars[i - 1] == 0x200d && !chars[i]) joined = true;
  }
  if (width == 0 || joined) { if (previous >= 0) append(state.cells[previous]); return; }
  const int natural_width = width;
  width = std::min(width, columns_);
  if (state.wrap_pending) {
    if (autowrap_) { state.wrapped[state.cursor.row] = true; state.cursor.col = 0; linefeed(); }
    state.wrap_pending = false;
  }
  if (width == 2 && state.cursor.col == columns_ - 1) {
    if (autowrap_) { state.wrapped[state.cursor.row] = true; state.cursor.col = 0; linefeed(); }
    else width = 1;
  }
  const int index = state.cursor.row * columns_ + state.cursor.col;
  if (insert_) {
    auto last = state.cells.begin() + (state.cursor.row + 1) * columns_;
    std::move_backward(state.cells.begin() + index, last - width, last);
  }
  if (state.cells[index].width == 0 && state.cursor.col > 0) state.cells[index - 1] = blank();
  if (state.cells[index].width == 2 && state.cursor.col + 1 < columns_) state.cells[index + 1] = blank();
  Cell cell = state.pen; cell.chars.fill(0); cell.chars[0] = character; cell.width = width; cell.natural_width = natural_width;
  state.cells[index] = cell;
  if (width == 2) { state.cells[index + 1] = blank(); state.cells[index + 1].width = 0; }
  const int next = state.cursor.col + width;
  state.cursor.col = std::min(columns_ - 1, next);
  state.wrap_pending = next >= columns_;
  if (insert_) repair_line(state.cursor.row);
}

void Terminal::erase(int first, int last) {
  auto& cells = screen().cells;
  first = std::clamp(first, 0, static_cast<int>(cells.size()));
  last = std::clamp(last, first, static_cast<int>(cells.size()));
  if (first < last && first % columns_ && cells[first].width == 0) cells[first - 1] = blank();
  if (last > first && last % columns_ && cells[last - 1].width == 2) cells[last] = blank();
  std::fill(cells.begin() + first, cells.begin() + last, blank());
  for (int row = (first + columns_ - 1) / columns_; row < last / columns_; ++row) screen().wrapped[row] = false;
}

void Terminal::sgr(const std::vector<int>& parameters) {
  auto& pen = screen().pen;
  for (size_t i = 0; i < parameters.size(); ++i) {
    const int value = parameters[i];
    switch (value) {
      case 0: pen = default_cell(); break;
      case 1: pen.attrs.bold = true; break;
      case 2: pen.attrs.dim = true; break;
      case 3: pen.attrs.italic = true; break;
      case 4: case 21: pen.attrs.underline = true; break;
      case 7: pen.attrs.reverse = true; break;
      case 8: pen.attrs.hidden = true; break;
      case 9: pen.attrs.strike = true; break;
      case 22: pen.attrs.bold = pen.attrs.dim = false; break;
      case 23: pen.attrs.italic = false; break;
      case 24: pen.attrs.underline = false; break;
      case 27: pen.attrs.reverse = false; break;
      case 28: pen.attrs.hidden = false; break;
      case 29: pen.attrs.strike = false; break;
      case 39: pen.fg = default_foreground_; pen.fg_index = -1; break;
      case 49: pen.bg = default_background_; pen.bg_index = -1; break;
      case 38: case 48:
        if (i + 2 < parameters.size() && parameters[i + 1] == 5) {
          const int index = std::clamp(parameters[i + 2], 0, 255);
          (value == 38 ? pen.fg : pen.bg) = palette(index, &palette_);
          (value == 38 ? pen.fg_index : pen.bg_index) = index;
          i += 2;
        } else if (i + 4 < parameters.size() && parameters[i + 1] == 2) {
          size_t start = i + 2;
          if (sequence_.find(':') != std::string::npos && parameters.size() > i + 5 && parameters[start] == 0) ++start;
          (value == 38 ? pen.fg : pen.bg) = {static_cast<std::uint8_t>(std::clamp(parameters[start], 0, 255)),
            static_cast<std::uint8_t>(std::clamp(parameters[start + 1], 0, 255)), static_cast<std::uint8_t>(std::clamp(parameters[start + 2], 0, 255))};
          (value == 38 ? pen.fg_index : pen.bg_index) = -2;
          i = start + 2;
        }
        break;
      default:
        if (value >= 30 && value <= 37) { pen.fg_index = value - 30; pen.fg = palette(pen.fg_index, &palette_); }
        if (value >= 40 && value <= 47) { pen.bg_index = value - 40; pen.bg = palette(pen.bg_index, &palette_); }
        if (value >= 90 && value <= 97) { pen.fg_index = value - 90 + 8; pen.fg = palette(pen.fg_index, &palette_); }
        if (value >= 100 && value <= 107) { pen.bg_index = value - 100 + 8; pen.bg = palette(pen.bg_index, &palette_); }
    }
  }
}

void Terminal::mode(int parameter, bool enabled, bool private_mode) {
  if (!private_mode) {
    if (parameter == 4) insert_ = enabled;
    if (parameter == 20) newline_ = enabled;
    return;
  }
  switch (parameter) {
    case 9: case 1000: case 1002: case 1003: {
      const auto requested = parameter == 9 ? MouseTracking::Press : parameter == 1000 ? MouseTracking::Clicks :
                             parameter == 1002 ? MouseTracking::Drag : MouseTracking::All;
      if (enabled) mouse_tracking_ = requested;
      else if (mouse_tracking_ == requested) mouse_tracking_ = MouseTracking::None;
      mouse_buttons_.fill(false); last_mouse_motion_.fill(-1);
      break;
    }
    case 1005: mouse_utf8_ = enabled; last_mouse_motion_.fill(-1); break;
    case 1006: mouse_sgr_ = enabled; last_mouse_motion_.fill(-1); break;
    case 1007: alternate_scroll_override_ = enabled; break;
    case 1015: mouse_urxvt_ = enabled; last_mouse_motion_.fill(-1); break;
    case 1016: mouse_pixels_ = enabled; last_mouse_motion_.fill(-1); break;
    case 1: application_cursor_ = enabled; break;
    case 6: origin_ = enabled; screen().cursor = {enabled ? top_ : 0, 0}; screen().wrap_pending = false; break;
    case 7: autowrap_ = enabled; if (!enabled) screen().wrap_pending = false; break;
    case 25: cursor_visible_ = enabled; break;
    case 47: case 1047: case 1049:
      if (alternate != enabled) {
        if (enabled) {
          primary_.saved = primary_.cursor; primary_.saved_pen = primary_.pen; primary_.saved_wrap_pending = primary_.wrap_pending;
          if (parameter != 47) {
            alternate_ = {}; alternate_.cells.resize(static_cast<size_t>(rows_) * columns_, default_cell());
            alternate_.pen = alternate_.saved_pen = default_cell();
            alternate_.wrapped.assign(rows_, false);
          }
        }
        alternate = enabled; top_ = 0; bottom_ = rows_ - 1; scroll_ = 0;
        clear_selection();
        if (!enabled && parameter == 1049) { primary_.cursor = primary_.saved; primary_.pen = primary_.saved_pen; primary_.wrap_pending = primary_.saved_wrap_pending; }
      }
      break;
    case 1048:
      if (enabled) { screen().saved = screen().cursor; screen().saved_pen = screen().pen; screen().saved_wrap_pending = screen().wrap_pending; }
      else { screen().cursor = screen().saved; screen().pen = screen().saved_pen; screen().wrap_pending = screen().saved_wrap_pending; }
      break;
    case 1004: focus_reporting_ = enabled; break;
    case 2004: bracketed_paste_ = enabled; break;
  }
}

void Terminal::csi(char command) {
  const bool private_mode = !sequence_.empty() && sequence_.front() == '?';
  if (!sequence_.empty() && (sequence_.front() == '>' || sequence_.front() == '<' || sequence_.front() == '=')) {
    if (sequence_.front() == '>' && command == 'c') output("\x1b[>0;1;0c");
    return;
  }
  if (private_mode && command != 'h' && command != 'l' && command != 'n' && command != 'c') return;
  const auto params = parameters(sequence_);
  const auto parameter = [&](size_t index, int fallback = 1) {
    return index >= params.size() || params[index] == 0 ? fallback : params[index];
  };
  auto& state = screen(); auto& cursor = state.cursor;
  const int first = origin_ ? top_ : 0, last = origin_ ? bottom_ : rows_ - 1;
  const int index = cursor.row * columns_ + cursor.col;
  const int count = std::min(parameter(0), columns_ - cursor.col);
  switch (command) {
    case 'A': cursor.row = std::max(first, cursor.row - parameter(0)); break;
    case 'B': case 'e': cursor.row = std::min(last, cursor.row + parameter(0)); break;
    case 'C': case 'a': cursor.col = std::min(columns_ - 1, cursor.col + parameter(0)); break;
    case 'D': cursor.col = std::max(0, cursor.col - parameter(0)); break;
    case 'E': cursor.col = 0; cursor.row = std::min(last, cursor.row + parameter(0)); break;
    case 'F': cursor.col = 0; cursor.row = std::max(first, cursor.row - parameter(0)); break;
    case 'G': case '`': cursor.col = std::clamp(parameter(0) - 1, 0, columns_ - 1); break;
    case 'd': cursor.row = std::clamp(parameter(0) - 1 + first, first, last); break;
    case 'H': case 'f': cursor = {std::clamp(parameter(0) - 1 + first, first, last), std::clamp(parameter(1) - 1, 0, columns_ - 1)}; break;
    case 'J':
      if (params[0] == 0) erase(index, rows_ * columns_);
      else if (params[0] == 1) erase(0, index + 1);
      else if (params[0] == 2) erase(0, rows_ * columns_);
      else if (params[0] == 3) { history_base_ += history_.size(); history_.clear(); scroll_ = 0; clear_selection(); }
      break;
    case 'K':
      if (params[0] == 0) erase(index, (cursor.row + 1) * columns_);
      else if (params[0] == 1) erase(cursor.row * columns_, index + 1);
      else if (params[0] == 2) erase(cursor.row * columns_, (cursor.row + 1) * columns_);
      break;
    case '@':
      std::move_backward(state.cells.begin() + index, state.cells.begin() + (cursor.row + 1) * columns_ - count, state.cells.begin() + (cursor.row + 1) * columns_);
      erase(index, index + count); repair_line(cursor.row); break;
    case 'P':
      std::move(state.cells.begin() + index + count, state.cells.begin() + (cursor.row + 1) * columns_, state.cells.begin() + index);
      erase((cursor.row + 1) * columns_ - count, (cursor.row + 1) * columns_); repair_line(cursor.row); break;
    case 'X': erase(index, index + count); break;
    case 'L': if (cursor.row >= top_ && cursor.row <= bottom_) scroll_down(cursor.row, bottom_, parameter(0)); break;
    case 'M': if (cursor.row >= top_ && cursor.row <= bottom_) scroll_up(cursor.row, bottom_, parameter(0), false); break;
    case 'S': scroll_up(top_, bottom_, parameter(0), true); break;
    case 'T': if (!private_mode) scroll_down(top_, bottom_, parameter(0)); break;
    case 'r':
      if (!private_mode) {
        const int top = parameter(0) - 1, bottom = parameter(1, rows_) - 1;
        if (top >= 0 && bottom < rows_ && top < bottom) { top_ = top; bottom_ = bottom; cursor = {origin_ ? top_ : 0, 0}; }
      }
      break;
    case 's': state.saved = cursor; state.saved_pen = state.pen; state.saved_wrap_pending = state.wrap_pending; break;
    case 'u': cursor = state.saved; state.pen = state.saved_pen; state.wrap_pending = state.saved_wrap_pending; break;
    case 'm': sgr(params); break;
    case 'h': case 'l': for (int param : params) mode(param, command == 'h', private_mode); break;
    case 'g': if (params[0] == 3) std::fill(tabs_.begin(), tabs_.end(), false); else if (params[0] == 0) tabs_[cursor.col] = false; break;
    case 'n':
      if (params[0] == 5) output("\x1b[0n");
      else if (params[0] == 6) output(std::string(private_mode ? "\x1b[?" : "\x1b[") + std::to_string(cursor.row - first + 1) + ";" + std::to_string(cursor.col + 1) + "R");
      break;
    case 'c': output(!sequence_.empty() && sequence_[0] == '>' ? "\x1b[>0;1;0c" : "\x1b[?1;2c"); break;
    case 't':
      if (params[0] == 18) output("\x1b[8;" + std::to_string(rows_) + ";" + std::to_string(columns_) + "t");
      else if (params[0] == 14) output("\x1b[4;" + std::to_string(rows_ * cell_pixel_height_) + ";" + std::to_string(columns_ * cell_pixel_width_) + "t");
      else if (params[0] == 16) output("\x1b[6;" + std::to_string(cell_pixel_height_) + ";" + std::to_string(cell_pixel_width_) + "t");
      break;
    case 'q':
      if (sequence_.find(' ') != std::string::npos && params[0] <= 6) {
        const int style = parameter(0);
        if (params[0] == 0) cursor_override_.reset();
        else cursor_override_ = CursorStyle{style <= 2 ? CursorShape::Block : style <= 4 ? CursorShape::Underline : CursorShape::Beam,
                                             style % 2 == 1};
      }
      break;
    case 'p':
      if (sequence_ == "!") { state.pen = default_cell(); top_ = 0; bottom_ = rows_ - 1; application_cursor_ = origin_ = insert_ = false; autowrap_ = cursor_visible_ = true; }
      break;
  }
  if (command != 'm' && command != 'n' && command != 'c' && command != 't' && command != 's' && command != 'u' &&
      command != 'h' && command != 'l' && command != 'q') screen().wrap_pending = false;
}

void Terminal::osc() {
  const auto separator = sequence_.find(';');
  if (separator == std::string::npos) return;
  const auto command = sequence_.substr(0, separator);
  if ((command == "0" || command == "2") && title) title(sequence_.substr(separator + 1));
}

void Terminal::feed(std::string_view bytes) {
  if (!bytes.empty()) { if (!alternate) primary_resize_tail_.clear(); alternate_resize_source_.reset(); }
  for (const unsigned char byte : bytes) {
    if (parser_ == Parser::Osc || parser_ == Parser::OscEscape) {
      if (byte == 7 || (parser_ == Parser::OscEscape && byte == '\\')) { osc(); sequence_.clear(); parser_ = Parser::Ground; }
      else if (byte == 27) parser_ = Parser::OscEscape;
      else {
        if (parser_ == Parser::OscEscape) { sequence_.clear(); parser_ = Parser::Escape; }
        else if (sequence_.size() < 8192) sequence_.push_back(byte);
        else { sequence_.clear(); parser_ = Parser::Ignore; }
      }
      continue;
    }
    if (parser_ == Parser::Ignore || parser_ == Parser::IgnoreEscape) {
      if (parser_ == Parser::IgnoreEscape && byte == '\\') parser_ = Parser::Ground;
      else parser_ = byte == 27 ? Parser::IgnoreEscape : Parser::Ignore;
      continue;
    }
    if (byte == 27) {
      if (utf8_left_) { put(0xfffd); utf8_left_ = 0; }
      parser_ = Parser::Escape; sequence_.clear(); continue;
    }
    if (byte == 0x18 || byte == 0x1a) { parser_ = Parser::Ground; sequence_.clear(); utf8_left_ = 0; continue; }
    if (byte < 32 || byte == 127) {
      auto& state = screen();
      switch (byte) {
        case 8: state.cursor.col = std::max(0, state.cursor.col - 1); state.wrap_pending = false; break;
        case 9:
          state.wrap_pending = false;
          do { ++state.cursor.col; } while (state.cursor.col < columns_ - 1 && !tabs_[state.cursor.col]);
          state.cursor.col = std::min(columns_ - 1, state.cursor.col); break;
        case 10: case 11: case 12: state.wrapped[state.cursor.row] = false; linefeed(); if (newline_) state.cursor.col = 0; break;
        case 13: state.cursor.col = 0; state.wrap_pending = false; break;
      }
      continue;
    }
    if (parser_ == Parser::Charset) {
      if (charset_target_ == '(') dec_graphics_ = byte == '0';
      parser_ = Parser::Ground; continue;
    }
    if (parser_ == Parser::Escape) {
      parser_ = Parser::Ground;
      auto& state = screen();
      switch (byte) {
        case '[': parser_ = Parser::Csi; sequence_.clear(); break;
        case ']': parser_ = Parser::Osc; sequence_.clear(); break;
        case 'P': case '_': case '^': parser_ = Parser::Ignore; break;
        case '(': case ')': case '*': case '+': charset_target_ = byte; parser_ = Parser::Charset; break;
        case '7': state.saved = state.cursor; state.saved_pen = state.pen; state.saved_wrap_pending = state.wrap_pending; break;
        case '8': state.cursor = state.saved; state.pen = state.saved_pen; state.wrap_pending = state.saved_wrap_pending; break;
        case 'D': state.wrapped[state.cursor.row] = false; linefeed(); break;
        case 'E': state.wrapped[state.cursor.row] = false; state.cursor.col = 0; linefeed(); break;
        case 'M':
          state.wrap_pending = false;
          if (state.cursor.row == top_) scroll_down(top_, bottom_, 1); else state.cursor.row = std::max(0, state.cursor.row - 1);
          break;
        case 'H': tabs_[state.cursor.col] = true; break;
        case 'c': reset(); break;
      }
      continue;
    }
    if (parser_ == Parser::Csi) {
      if (byte >= 0x40 && byte <= 0x7e) { csi(byte); sequence_.clear(); parser_ = Parser::Ground; }
      else if (sequence_.size() < 1024) sequence_.push_back(byte);
      else { sequence_.clear(); parser_ = Parser::Ground; }
      continue;
    }
    if (utf8_left_) {
      if ((byte & 0xc0) == 0x80) {
        utf8_ = (utf8_ << 6U) | (byte & 63U);
        if (--utf8_left_ == 0) put(utf8_ < utf8_min_ || utf8_ > 0x10ffff || (utf8_ >= 0xd800 && utf8_ <= 0xdfff) ? 0xfffd : utf8_);
        continue;
      }
      utf8_left_ = 0; put(0xfffd);
    }
    if (byte < 0x80) {
      char32_t character = byte;
      if (dec_graphics_ && byte >= 0x60 && byte <= 0x7e) {
        constexpr std::u32string_view graphics = U"◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
        character = graphics[byte - 0x60];
      }
      put(character);
    } else if (byte >= 0xc2 && byte <= 0xdf) { utf8_ = byte & 31U; utf8_left_ = 1; utf8_min_ = 0x80; }
    else if (byte >= 0xe0 && byte <= 0xef) { utf8_ = byte & 15U; utf8_left_ = 2; utf8_min_ = 0x800; }
    else if (byte >= 0xf0 && byte <= 0xf4) { utf8_ = byte & 7U; utf8_left_ = 3; utf8_min_ = 0x10000; }
    else put(0xfffd);
  }
  changed();
}

int Terminal::used_columns(std::span<const Cell> cells) const {
  for (int column = static_cast<int>(cells.size()) - 1; column >= 0; --column) {
    const auto& cell = cells[column];
    if (cell.width && (cell.chars[0] || cell.bg_index != -1 || cell.bg != default_background_ || cell.attrs != Attributes{}))
      return std::min(static_cast<int>(cells.size()), column + cell.width);
  }
  return 0;
}

void Terminal::reflow_primary(int rows, int columns) {
  struct Marker {
    size_t source_row;
    int source_column;
    size_t row = 0;
    int column = 0;
    bool pending = false, found = false;
  };
  const size_t old_history = history_.size();
  std::array<Marker, 3> markers{{
    {old_history + primary_.cursor.row, primary_.cursor.col + int(primary_.wrap_pending)},
    {old_history + primary_.saved.row, primary_.saved.col + int(primary_.saved_wrap_pending)},
    {old_history - std::min(old_history, static_cast<size_t>(scroll_)), 0}}};
  int used_rows = std::max(primary_.cursor.row, primary_.saved.row) + 1;
  for (int row = rows_ - 1; row >= used_rows; --row) {
    if (primary_.wrapped[row] || used_columns({primary_.cells.data() + row * columns_, static_cast<size_t>(columns_)})) { used_rows = row + 1; break; }
  }
  if (!primary_resize_tail_.empty()) used_rows = rows_;
  size_t visible_cells = static_cast<size_t>(used_rows) * columns_, visible_rows = used_rows;
  for (const auto& line : primary_resize_tail_) { visible_cells += line.cells.size(); ++visible_rows; }
  const size_t capacity = history_limit_ + rows + visible_cells / std::max(1, columns - 1) + visible_rows + 4;
  std::deque<HistoryLine> output;
  std::vector<Cell> line;
  size_t completed = 0, dropped = 0, source_row = 0;
  bool open = false;
  const auto emit = [&](bool wrapped) {
    output.push_back({std::move(line), wrapped}); line.clear(); ++completed;
    if (output.size() > capacity) { output.pop_front(); ++dropped; }
  };
  const auto consume = [&](std::span<const Cell> source, bool wrapped) {
    int used = used_columns(source);
    for (const auto& marker : markers) if (marker.source_row == source_row)
      used = std::max(used, std::min(static_cast<int>(source.size()), marker.source_column));
    open = true;
    for (int column = 0; column < used;) {
      Cell cell = source[column];
      if (!cell.width) { ++column; continue; }
      const int source_width = std::min(int(cell.width), static_cast<int>(source.size()) - column);
      const int width = std::min(columns, std::max(1, int(cell.natural_width)));
      if (static_cast<int>(line.size()) + width > columns) emit(true);
      for (auto& marker : markers) if (!marker.found && marker.source_row == source_row &&
          marker.source_column >= column && marker.source_column < column + source_width) {
        marker.row = completed; marker.column = static_cast<int>(line.size()) + std::min(width - 1, marker.source_column - column); marker.found = true;
      }
      cell.width = width; line.push_back(cell);
      if (width == 2) { Cell continuation = default_cell(); continuation.width = 0; line.push_back(continuation); }
      column += source_width;
    }
    for (auto& marker : markers) if (!marker.found && marker.source_row == source_row) {
      marker.row = completed; marker.pending = static_cast<int>(line.size()) == columns;
      marker.column = std::min(columns - 1, static_cast<int>(line.size())); marker.found = true;
    }
    if (!wrapped) { emit(false); open = false; }
    ++source_row;
  };
  for (const auto& source : history_) consume(source.cells, source.wrapped);
  for (int row = 0; row < used_rows; ++row)
    consume({primary_.cells.data() + row * columns_, static_cast<size_t>(columns_)}, primary_.wrapped[row]);
  for (const auto& source : primary_resize_tail_) consume(source.cells, source.wrapped);
  if (open) emit(false);
  size_t start = completed > static_cast<size_t>(rows) ? completed - rows : 0;
  start = std::max(dropped, std::min(start, markers[0].row));
  const size_t history_start = std::max(dropped, start > history_limit_ ? start - history_limit_ : 0);
  Screen next;
  next.cells.assign(static_cast<size_t>(rows) * columns, default_cell()); next.wrapped.assign(rows, false);
  next.pen = primary_.pen; next.saved_pen = primary_.saved_pen;
  const auto position = [&](const Marker& marker) {
    return Position{static_cast<int>(std::min(marker.row < start ? size_t{0} : marker.row - start, static_cast<size_t>(rows - 1))), marker.column};
  };
  next.cursor = position(markers[0]); next.wrap_pending = markers[0].pending;
  next.saved = position(markers[1]); next.saved_wrap_pending = markers[1].pending;
  for (int row = 0; row < rows && start + row < completed; ++row) {
    const auto& source = output[start + row - dropped];
    std::copy(source.cells.begin(), source.cells.end(), next.cells.begin() + row * columns);
    next.wrapped[row] = source.wrapped;
  }
  std::deque<HistoryLine> history;
  for (size_t row = history_start; row < start; ++row) history.push_back(std::move(output[row - dropped]));
  std::vector<HistoryLine> tail;
  for (size_t row = start + rows; row < completed; ++row) tail.push_back(std::move(output[row - dropped]));
  if (scroll_) scroll_ = static_cast<int>(start - std::clamp(markers[2].row, history_start, start));
  history_base_ += history_start; history_ = std::move(history); primary_ = std::move(next); primary_resize_tail_ = std::move(tail);
}

void Terminal::resize_alternate(int rows, int columns) {
  if (!alternate_resize_source_) alternate_resize_source_ = ResizeSnapshot{alternate_, rows_, columns_};
  const auto& source = *alternate_resize_source_;
  Screen next;
  next.cells.assign(static_cast<size_t>(rows) * columns, default_cell()); next.wrapped.assign(rows, false);
  next.pen = source.screen.pen; next.saved_pen = source.screen.saved_pen;
  for (int row = 0; row < std::min(rows, source.rows); ++row) {
    std::copy_n(source.screen.cells.begin() + row * source.columns, std::min(columns, source.columns), next.cells.begin() + row * columns);
    if (columns == source.columns) next.wrapped[row] = source.screen.wrapped[row];
    for (int column = 0; column < columns; ++column) {
      auto& cell = next.cells[row * columns + column];
      if ((cell.width == 2 && column == columns - 1) ||
          (cell.width == 0 && (column == 0 || next.cells[row * columns + column - 1].width != 2))) cell = default_cell();
    }
  }
  next.cursor = {std::clamp(source.screen.cursor.row, 0, rows - 1), std::clamp(source.screen.cursor.col, 0, columns - 1)};
  next.saved = {std::clamp(source.screen.saved.row, 0, rows - 1), std::clamp(source.screen.saved.col, 0, columns - 1)};
  next.wrap_pending = source.screen.wrap_pending && columns == source.columns && source.screen.cursor.row < rows;
  next.saved_wrap_pending = source.screen.saved_wrap_pending && columns == source.columns && source.screen.saved.row < rows;
  alternate_ = std::move(next);
}

void Terminal::resize(int rows, int columns) {
  validate_size(rows, columns);
  if (rows == rows_ && columns == columns_) return;
  clear_selection();
  reflow_primary(rows, columns); resize_alternate(rows, columns);
  const int old_columns = columns_;
  rows_ = rows; columns_ = columns; top_ = 0; bottom_ = rows - 1;
  tabs_.resize(columns, false);
  for (int col = old_columns; col < columns; ++col) if (col % 8 == 0) tabs_[col] = true;
  last_mouse_motion_.fill(-1);
  changed();
}

void Terminal::key(Key key, unsigned modifiers) {
  scroll_ = 0; clear_selection(); changed();
  const int parameter = 1 + (modifiers & ModShift ? 1 : 0) + (modifiers & ModAlt ? 2 : 0) + (modifiers & ModCtrl ? 4 : 0);
  const auto letter = [&](char final) {
    if (parameter != 1) output("\x1b[1;" + std::to_string(parameter) + final);
    else output(std::string(application_cursor_ ? "\x1bO" : "\x1b[") + final);
  };
  const auto tilde = [&](int number) {
    output("\x1b[" + std::to_string(number) + (parameter == 1 ? "" : ";" + std::to_string(parameter)) + "~");
  };
  switch (key) {
    case Key::Enter: if (modifiers & ModAlt) output("\x1b"); output(newline_ ? "\r\n" : "\r"); break;
    case Key::Backspace: if (modifiers & ModAlt) output("\x1b"); output(modifiers & ModCtrl ? "\x08" : "\x7f"); break;
    case Key::Tab: output(modifiers & ModShift ? "\x1b[Z" : "\t"); break;
    case Key::Escape: output("\x1b"); break;
    case Key::Up: letter('A'); break;
    case Key::Down: letter('B'); break;
    case Key::Right: letter('C'); break;
    case Key::Left: letter('D'); break;
    case Key::Home: letter('H'); break;
    case Key::End: letter('F'); break;
    case Key::Insert: tilde(2); break;
    case Key::Delete: tilde(3); break;
    case Key::PageUp: tilde(5); break;
    case Key::PageDown: tilde(6); break;
    default: {
      const unsigned function = static_cast<unsigned>(key) - static_cast<unsigned>(Key::Function0);
      if (function >= 1 && function <= 4) {
        const char final = 'P' + function - 1;
        if (parameter == 1) output(std::string("\x1bO") + final);
        else output("\x1b[1;" + std::to_string(parameter) + final);
      } else if (function >= 5 && function <= 12) {
        constexpr std::array<int, 8> numbers{15,17,18,19,20,21,23,24}; tilde(numbers[function - 5]);
      }
    }
  }
}

void Terminal::character(char32_t character, unsigned modifiers) {
  scroll_ = 0; clear_selection(); changed();
  if (modifiers & ModAlt) output("\x1b");
  if (modifiers & ModCtrl) {
    if ((character >= U'@' && character <= U'_') || (character >= U'a' && character <= U'z')) character &= 31U;
    else if (character == U' ' || character == U'2') character = 0;
    else if (character == U'?' || character == U'8') character = 127;
    else if (character >= U'3' && character <= U'7') character = character - U'3' + 27;
  }
  std::string encoded; utf8(encoded, character); output(encoded);
}

void Terminal::focus(bool focused) { if (focus_reporting_) output(focused ? "\x1b[I" : "\x1b[O"); }

void Terminal::set_default_colors(Color foreground, Color background) {
  if (foreground == default_foreground_ && background == default_background_) return;
  const auto update = [&](Cell& cell) {
    if (cell.fg_index == -1) cell.fg = foreground;
    if (cell.bg_index == -1) cell.bg = background;
  };
  for (auto* state : {&primary_, &alternate_}) {
    update(state->pen); update(state->saved_pen);
    for (auto& cell : state->cells) update(cell);
  }
  for (auto& line : history_) for (auto& cell : line.cells) update(cell);
  for (auto& line : primary_resize_tail_) for (auto& cell : line.cells) update(cell);
  if (alternate_resize_source_) {
    update(alternate_resize_source_->screen.pen); update(alternate_resize_source_->screen.saved_pen);
    for (auto& cell : alternate_resize_source_->screen.cells) update(cell);
  }
  default_foreground_ = foreground; default_background_ = background; changed();
}

std::array<Color, 16> Terminal::default_palette() {
  std::array<Color, 16> result{};
  for (unsigned i = 0; i < result.size(); ++i) result[i] = palette(i);
  return result;
}

void Terminal::set_palette(std::span<const Color> colors) {
  if (!colors.empty() && colors.size() != 16) throw std::invalid_argument("palette requires 16 colors");
  std::array<Color, 16> next{};
  for (size_t i = 0; i < next.size(); ++i) next[i] = colors.empty() ? palette(i) : colors[i];
  if (next == palette_) return;
  const auto update = [&](Cell& cell) {
    if (cell.fg_index >= 0 && cell.fg_index < 16) cell.fg = next[cell.fg_index];
    if (cell.bg_index >= 0 && cell.bg_index < 16) cell.bg = next[cell.bg_index];
  };
  for (auto* state : {&primary_, &alternate_}) {
    update(state->pen); update(state->saved_pen);
    for (auto& cell : state->cells) update(cell);
  }
  for (auto& line : history_) for (auto& cell : line.cells) update(cell);
  for (auto& line : primary_resize_tail_) for (auto& cell : line.cells) update(cell);
  if (alternate_resize_source_) {
    update(alternate_resize_source_->screen.pen); update(alternate_resize_source_->screen.saved_pen);
    for (auto& cell : alternate_resize_source_->screen.cells) update(cell);
  }
  palette_ = next; changed();
}

void Terminal::set_history_limit(std::size_t lines) {
  history_limit_ = std::min(lines, std::size_t{100000});
  while (history_.size() > history_limit_) { history_.pop_front(); ++history_base_; }
  scroll_ = std::min(scroll_, static_cast<int>(history_.size()));
  if (selection_first_.line < history_base_) clear_selection();
  changed();
}

void Terminal::set_cursor_style(CursorStyle style) { default_cursor_style_ = style; dirty = true; }

void Terminal::set_input_options(bool mouse_reporting, bool alternate_scroll) {
  mouse_enabled_ = mouse_reporting; default_alternate_scroll_ = alternate_scroll;
  mouse_buttons_.fill(false); last_mouse_motion_.fill(-1);
}

void Terminal::set_cell_size(unsigned width, unsigned height) {
  cell_pixel_width_ = std::clamp(width, 1U, 4096U); cell_pixel_height_ = std::clamp(height, 1U, 4096U);
}

bool Terminal::reports_mouse() const { return mouse_enabled_ && mouse_tracking_ != MouseTracking::None; }

void Terminal::mouse_report(unsigned button, bool release, MousePosition position, unsigned modifiers) {
  unsigned code = button | (modifiers & ModShift ? 4 : 0) | (modifiers & ModAlt ? 8 : 0) | (modifiers & ModCtrl ? 16 : 0);
  const int x = std::clamp(position.col, 0, columns_ - 1) + 1, y = std::clamp(position.row, 0, rows_ - 1) + 1;
  if (mouse_sgr_ || mouse_pixels_) {
    const int report_x = mouse_pixels_ ? std::clamp(position.x, 0, static_cast<int>(columns_ * cell_pixel_width_) - 1) + 1 : x;
    const int report_y = mouse_pixels_ ? std::clamp(position.y, 0, static_cast<int>(rows_ * cell_pixel_height_) - 1) + 1 : y;
    output("\x1b[<" + std::to_string(code) + ";" + std::to_string(report_x) + ";" + std::to_string(report_y) + (release ? "m" : "M"));
  } else {
    if (release) code = 3U | (code & 28U);
    if (mouse_urxvt_) output("\x1b[" + std::to_string(code + 32) + ";" + std::to_string(x) + ";" + std::to_string(y) + "M");
    else {
      std::string report = "\x1b[M";
      if (mouse_utf8_) { utf8(report, code + 32); utf8(report, std::min(x, 2015) + 32); utf8(report, std::min(y, 2015) + 32); }
      else { report.push_back(code + 32); report.push_back(std::min(x, 223) + 32); report.push_back(std::min(y, 223) + 32); }
      output(report);
    }
  }
}

bool Terminal::mouse_button(unsigned button, bool pressed, MousePosition position, unsigned modifiers) {
  if (button >= mouse_buttons_.size()) return false;
  mouse_buttons_[button] = pressed; last_mouse_motion_.fill(-1);
  if (!reports_mouse()) return false;
  if (!pressed && mouse_tracking_ == MouseTracking::Press) return true;
  if (pressed) { clear_selection(); if (scroll_) { scroll_ = 0; changed(); } }
  mouse_report(button < 3 ? button : 128 + button - 3, !pressed, position, modifiers);
  return true;
}

bool Terminal::mouse_motion(MousePosition position, unsigned modifiers) {
  if (!reports_mouse()) return false;
  const auto button = std::find(mouse_buttons_.begin(), mouse_buttons_.end(), true);
  if (mouse_tracking_ != MouseTracking::All && !(mouse_tracking_ == MouseTracking::Drag && button != mouse_buttons_.end())) return false;
  const unsigned number = button == mouse_buttons_.end() ? 3 : static_cast<unsigned>(button - mouse_buttons_.begin());
  const unsigned code = (button == mouse_buttons_.end() ? 3U : number < 3 ? number : 128 + number - 3) | 32U;
  const std::array<int, 5> key{mouse_pixels_ ? position.x : position.col, mouse_pixels_ ? position.y : position.row,
                              static_cast<int>(modifiers), static_cast<int>(code), 0};
  if (key != last_mouse_motion_) { mouse_report(code, false, position, modifiers); last_mouse_motion_ = key; }
  return true;
}

bool Terminal::mouse_wheel(bool horizontal, int steps, MousePosition position, unsigned modifiers) {
  if (!reports_mouse()) return false;
  clear_selection(); if (scroll_) { scroll_ = 0; changed(); }
  const unsigned button = (horizontal ? 66 : 64) + (steps > 0 ? 1 : 0);
  for (int count = 0; count < std::abs(std::clamp(steps, -64, 64)); ++count) mouse_report(button, false, position, modifiers);
  return true;
}

bool Terminal::alternate_scroll(int lines, bool horizontal, unsigned modifiers) {
  if (!alternate || !alternate_scroll_override_.value_or(default_alternate_scroll_)) return false;
  const auto direction = horizontal ? (lines > 0 ? Key::Right : Key::Left) : (lines > 0 ? Key::Down : Key::Up);
  for (int count = 0; count < std::abs(std::clamp(lines, -128, 128)); ++count) key(direction, modifiers);
  return true;
}

void Terminal::scroll(int lines) {
  if (alternate) return;
  const int value = std::clamp(scroll_ + lines, 0, static_cast<int>(history_.size()));
  if (value != scroll_) { scroll_ = value; changed(); }
}

std::int64_t Terminal::visible_origin() const {
  return alternate ? 0 : history_base_ + static_cast<std::int64_t>(history_.size()) - scroll_;
}

std::pair<Terminal::SelectionPoint, Terminal::SelectionPoint> Terminal::selection_extent(int row, int column) const {
  row = std::clamp(row, 0, rows_ - 1); column = std::clamp(column, 0, columns_ - 1);
  const auto line = visible_origin() + row;
  if (selection_mode_ == SelectionMode::Line) return {{line, 0}, {line, columns_ - 1}};
  while (column > 0 && cell(row, column).width == 0) --column;
  int first = column, last = std::min(columns_ - 1, column + std::max(1, int(cell(row, column).width)) - 1);
  if (selection_mode_ == SelectionMode::Word) {
    const auto category = [&](int col) {
      const auto character = cell(row, col).chars[0];
      if (!character || std::iswspace(character)) return 0;
      return std::iswalnum(character) || character == U'_' ? 1 : 2;
    };
    const auto wanted = category(column);
    while (first > 0) {
      int previous = first - 1;
      while (previous > 0 && cell(row, previous).width == 0) --previous;
      if (category(previous) != wanted) break;
      first = previous;
    }
    while (last + 1 < columns_) {
      const int next = last + 1;
      if (category(next) != wanted) break;
      last = std::min(columns_ - 1, next + std::max(1, int(cell(row, next).width)) - 1);
    }
  }
  return {{line, first}, {line, last}};
}

void Terminal::begin_selection(int row, int column, SelectionMode mode) {
  selection_mode_ = mode;
  const auto [first, last] = selection_extent(row, column);
  anchor_first_ = selection_first_ = first; anchor_last_ = selection_last_ = last;
  selection_anchor_valid_ = true; selection_ = mode != SelectionMode::Character;
  dirty = true;
}

void Terminal::extend_selection(int row, int column) {
  if (!selection_anchor_valid_) return;
  const auto [first, last] = selection_extent(row, column);
  selection_first_ = std::min(first, anchor_first_);
  selection_last_ = std::max(last, anchor_last_);
  selection_ = selection_first_ != selection_last_ || selection_mode_ != SelectionMode::Character;
  dirty = true;
}

void Terminal::clear_selection() {
  if (selection_) dirty = true;
  selection_ = selection_anchor_valid_ = false;
}

std::pair<int, int> Terminal::selection_range(int row) const {
  const auto line = visible_origin() + row;
  if (!selection_ || line < selection_first_.line || line > selection_last_.line) return {0, 0};
  return {line == selection_first_.line ? selection_first_.column : 0,
          line == selection_last_.line ? std::min(columns_, selection_last_.column + 1) : columns_};
}

std::string Terminal::selected_text() const {
  if (!selection_) return {};
  std::string result;
  for (auto line = selection_first_.line; line <= selection_last_.line; ++line) {
    const Cell* cells = nullptr;
    int columns = columns_;
    bool wrapped = false;
    const auto index = line - (alternate ? 0 : history_base_);
    if (index < 0) continue;
    const auto history = alternate ? 0 : static_cast<std::int64_t>(history_.size());
    if (index < history) {
      const auto& stored = history_[index]; cells = stored.cells.data(); columns = stored.cells.size(); wrapped = stored.wrapped;
    } else {
      const auto row = index - history;
      if (row >= rows_) break;
      cells = screen().cells.data() + row * columns_; wrapped = screen().wrapped[row];
    }
    const int first = line == selection_first_.line ? std::min(columns, selection_first_.column) : 0;
    const int last = line == selection_last_.line ? std::min(columns, selection_last_.column + 1) : columns;
    std::string text;
    for (int column = first; column < last; ++column) {
      const auto& cell = cells[column];
      if (!cell.width) continue;
      if (!cell.chars[0]) text.push_back(' ');
      else for (auto character : cell.chars) { if (!character) break; utf8(text, character); }
    }
    if (last == columns && !wrapped) while (!text.empty() && text.back() == ' ') text.pop_back();
    if (result.size() + text.size() >= 32 * 1024 * 1024) throw std::length_error("selection exceeds 32 MiB");
    result += text;
    if (line != selection_last_.line && !wrapped) result += '\n';
  }
  return result;
}

void Terminal::paste(std::string_view text) {
  if (text.empty()) return;
  if (text.size() > 4 * 1024 * 1024 - 12) throw std::length_error("paste exceeds 4 MiB");
  std::string encoded = bracketed_paste_ ? "\x1b[200~" : "";
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n' && i > 0 && text[i - 1] == '\r') continue;
    encoded.push_back(text[i] == '\n' ? '\r' : text[i]);
  }
  if (bracketed_paste_) encoded += "\x1b[201~";
  output(encoded); scroll_ = 0; clear_selection(); changed();
}

Cell Terminal::cell(int row, int column) const {
  if (row < 0 || row >= rows_ || column < 0 || column >= columns_) return default_cell();
  if (scroll_ && !alternate) {
    const int index = static_cast<int>(history_.size()) + row - scroll_;
    if (index < static_cast<int>(history_.size())) {
      const auto& line = history_[index].cells;
      return column < static_cast<int>(line.size()) ? line[column] : default_cell();
    }
    row -= scroll_;
  }
  return screen().cells[static_cast<size_t>(row) * columns_ + column];
}

bool Terminal::cursor_at(int row, int column) const {
  return scroll_ == 0 && cursor_visible_ && screen().cursor.row == row && screen().cursor.col == column;
}

std::string Terminal::take_output() { std::string result; result.swap(output_); return result; }

}  // namespace zterm
