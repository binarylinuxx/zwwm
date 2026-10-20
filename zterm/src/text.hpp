#pragma once

#include "font.hpp"
#include <pango/pangocairo.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace zterm {

class Text {
 public:
  struct Bounds { float x = 0, y = 0, width = 0, height = 0; };
  struct Bitmap {
    int width = 0, height = 0, x = 0, y = 0;
    bool color = false;
    unsigned scale = 1;
    std::vector<std::uint8_t> pixels;
    Bounds reference{};
  };
  Text();
  ~Text();
  Text(const Text&) = delete;
  Text& operator=(const Text&) = delete;
  void configure(const FontFamilies& families, unsigned size, unsigned weight = 400);
  Bitmap rasterize(std::string_view text, bool bold, bool italic, std::uint32_t rgb,
                   bool grid = false, bool separator = false, FontRole role = FontRole::Mono);
  unsigned width() const { return width_; }
  unsigned height() const { return height_; }
  static void append_utf8(std::string& text, char32_t character);
  static unsigned parse_weight(std::string_view value);

 private:
  PangoFontMap* map_ = nullptr;
  PangoContext* context_ = nullptr;
  PangoFontDescription* font_ = nullptr;
  unsigned width_ = 1, height_ = 1;
  unsigned weight_ = 400;
  int baseline_ = 1;
  FontFamilies families_;
  std::string family_;
  std::unordered_set<std::string> font_warnings_;
  std::unordered_set<char32_t> missing_glyphs_;
  std::string font_family(FontRole role, bool bold, bool italic) const;
};

}  // namespace zterm
