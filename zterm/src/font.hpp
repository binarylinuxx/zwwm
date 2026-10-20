#pragma once

#include <string>

namespace zterm {

struct FontFamilies {
  std::string mono = "monospace";
  std::string bold, italic, bold_italic, symbols, emoji;
  bool operator==(const FontFamilies&) const = default;
};

enum class FontRole { Mono, Symbols, Emoji };

}  // namespace zterm
