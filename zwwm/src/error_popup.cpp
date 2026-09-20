#include "zwwm/error_popup.hpp"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>

namespace zwwm {
namespace {

constexpr int kFontSize = 14;
constexpr int kMaximumTextWidth = 456;
constexpr int kMaximumMessageLines = 4;

std::uint32_t source_over(std::uint32_t destination, std::uint32_t source) {
  const std::uint32_t alpha = source >> 24U;
  const std::uint32_t inverse = 255U - alpha;
  const auto blend = [inverse](std::uint32_t foreground, std::uint32_t background) {
    return std::min(255U, foreground + (background * inverse + 127U) / 255U);
  };
  return blend(alpha, destination >> 24U) << 24U |
         blend((source >> 16U) & 0xffU, (destination >> 16U) & 0xffU) << 16U |
         blend((source >> 8U) & 0xffU, (destination >> 8U) & 0xffU) << 8U |
         blend(source & 0xffU, destination & 0xffU);
}

std::uint32_t premultiplied(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                            std::uint8_t alpha) {
  const auto channel = [alpha](std::uint8_t value) {
    return (static_cast<std::uint32_t>(value) * alpha + 127U) / 255U;
  };
  return static_cast<std::uint32_t>(alpha) << 24U | channel(red) << 16U |
         channel(green) << 8U | channel(blue);
}

int text_width(FT_Face face, std::string_view text) {
  int width = 0;
  for (const unsigned char character : text) {
    if (FT_Load_Char(face, character < 128 ? character : '?', FT_LOAD_DEFAULT) == 0)
      width += static_cast<int>(face->glyph->advance.x >> 6);
  }
  return width;
}

std::vector<std::string> wrap_message(FT_Face face, std::string_view message) {
  std::string normalized(message.substr(0, 512));
  bool truncated = message.size() > normalized.size();
  for (char& character : normalized)
    if (character == '\n' || character == '\r' || character == '\t') character = ' ';

  std::istringstream words(normalized);
  std::vector<std::string> lines;
  std::string line;
  std::string word;
  while (words >> word) {
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (text_width(face, candidate) <= kMaximumTextWidth) {
      line = candidate;
      continue;
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
      line.clear();
      if (lines.size() == kMaximumMessageLines) {
        truncated = true;
        break;
      }
    }
    while (!word.empty() && text_width(face, word) > kMaximumTextWidth) {
      std::size_t count = 1;
      while (count < word.size() && text_width(face, std::string_view(word).substr(0, count + 1)) <= kMaximumTextWidth)
        ++count;
      lines.push_back(word.substr(0, count));
      word.erase(0, count);
      if (lines.size() == kMaximumMessageLines) {
        truncated = true;
        break;
      }
    }
    if (lines.size() == kMaximumMessageLines) break;
    line = std::move(word);
  }
  if (lines.size() < kMaximumMessageLines && !line.empty()) lines.push_back(std::move(line));
  if (lines.empty()) lines.emplace_back("Configuration could not be loaded");
  if (truncated) {
    std::string& last = lines.back();
    while (!last.empty() && text_width(face, last + "...") > kMaximumTextWidth) last.pop_back();
    last += "...";
  }
  return lines;
}

void draw_text(ErrorPopupImage& image, FT_Face face, std::string_view text, int x, int baseline,
               std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
  int pen = x;
  for (const unsigned char character : text) {
    if (FT_Load_Char(face, character < 128 ? character : '?', FT_LOAD_RENDER) != 0) continue;
    const FT_GlyphSlot glyph = face->glyph;
    const int left = pen + glyph->bitmap_left;
    const int top = baseline - glyph->bitmap_top;
    for (unsigned row = 0; row < glyph->bitmap.rows; ++row) {
      for (unsigned column = 0; column < glyph->bitmap.width; ++column) {
        const int target_x = left + static_cast<int>(column);
        const int target_y = top + static_cast<int>(row);
        if (target_x < 0 || target_y < 0 || target_x >= static_cast<int>(image.width) ||
            target_y >= static_cast<int>(image.height)) continue;
        const auto coverage = glyph->bitmap.buffer[static_cast<std::ptrdiff_t>(row) * glyph->bitmap.pitch + column];
        auto& target = image.pixels[static_cast<std::size_t>(target_y) * image.width + target_x];
        target = source_over(target, premultiplied(red, green, blue, coverage));
      }
    }
    pen += static_cast<int>(glyph->advance.x >> 6);
  }
}

}  // namespace

ErrorPopupImage render_error_popup(std::string_view message) {
  ErrorPopupImage image;
  if (!FcInit()) return image;
  FcPattern* pattern = FcPatternCreate();
  if (pattern == nullptr) return image;
  FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif"));
  FcPatternAddDouble(pattern, FC_PIXEL_SIZE, kFontSize);
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult match_result = FcResultNoMatch;
  FcPattern* match = FcFontMatch(nullptr, pattern, &match_result);
  FcPatternDestroy(pattern);
  FcChar8* font_path = nullptr;
  if (match == nullptr || FcPatternGetString(match, FC_FILE, 0, &font_path) != FcResultMatch) {
    if (match != nullptr) FcPatternDestroy(match);
    return image;
  }

  FT_Library library = nullptr;
  FT_Face face = nullptr;
  const bool loaded = FT_Init_FreeType(&library) == 0 &&
                      FT_New_Face(library, reinterpret_cast<const char*>(font_path), 0, &face) == 0 &&
                      FT_Set_Pixel_Sizes(face, 0, kFontSize) == 0;
  FcPatternDestroy(match);
  if (!loaded) {
    if (face != nullptr) FT_Done_Face(face);
    if (library != nullptr) FT_Done_FreeType(library);
    return image;
  }

  const auto message_lines = wrap_message(face, message);
  std::vector<std::string> lines{"Configuration error"};
  lines.insert(lines.end(), message_lines.begin(), message_lines.end());
  int content_width = 0;
  for (const auto& line : lines) content_width = std::max(content_width, text_width(face, line));
  const int line_height = std::max(17, static_cast<int>(face->size->metrics.height >> 6));
  const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);
  image.width = static_cast<std::uint32_t>(std::clamp(content_width + 40, 280, 496));
  image.height = static_cast<std::uint32_t>(24 + line_height * static_cast<int>(lines.size()));
  image.pixels.assign(static_cast<std::size_t>(image.width) * image.height, 0);

  constexpr int radius = 12;
  for (int y = 0; y < static_cast<int>(image.height); ++y) {
    for (int x = 0; x < static_cast<int>(image.width); ++x) {
      const int closest_x = std::clamp(x, radius, static_cast<int>(image.width) - radius - 1);
      const int closest_y = std::clamp(y, radius, static_cast<int>(image.height) - radius - 1);
      const int dx = x - closest_x;
      const int dy = y - closest_y;
      if (dx * dx + dy * dy <= radius * radius)
        image.pixels[static_cast<std::size_t>(y) * image.width + x] = premultiplied(35, 27, 31, 246);
    }
  }
  for (int y = 12; y < static_cast<int>(image.height) - 12; ++y)
    for (int x = 5; x < 8; ++x)
      image.pixels[static_cast<std::size_t>(y) * image.width + x] = premultiplied(239, 82, 96, 255);

  int baseline = 12 + ascender;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    draw_text(image, face, lines[index], 20, baseline,
              index == 0 ? 255 : 235, index == 0 ? 116 : 232, index == 0 ? 126 : 235);
    baseline += line_height;
  }
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return image;
}

void composite_error_popup(const ErrorPopupImage& popup, std::vector<std::uint32_t>& destination,
                           std::uint32_t destination_width, std::uint32_t destination_height,
                           std::int32_t x, std::int32_t y) {
  if (!popup || destination.size() < static_cast<std::size_t>(destination_width) * destination_height) return;
  for (std::uint32_t source_y = 0; source_y < popup.height; ++source_y) {
    const auto target_y = y + static_cast<std::int32_t>(source_y);
    if (target_y < 0 || target_y >= static_cast<std::int32_t>(destination_height)) continue;
    for (std::uint32_t source_x = 0; source_x < popup.width; ++source_x) {
      const auto target_x = x + static_cast<std::int32_t>(source_x);
      if (target_x < 0 || target_x >= static_cast<std::int32_t>(destination_width)) continue;
      auto& target = destination[static_cast<std::size_t>(target_y) * destination_width + target_x];
      target = source_over(target, popup.pixels[static_cast<std::size_t>(source_y) * popup.width + source_x]);
    }
  }
}

}  // namespace zwwm
