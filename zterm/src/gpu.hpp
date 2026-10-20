#pragma once

#include <zterm/terminal.hpp>
#include "text.hpp"
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gbm.h>
#include <zwayland/client/display.hpp>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zterm {

struct Format { std::uint32_t format, padding; std::uint64_t modifier; };
static_assert(sizeof(Format) == 16);

class Gpu {
 public:
  explicit Gpu(zwayland::client::Display& display);
  ~Gpu();
  Gpu(const Gpu&) = delete;
  Gpu& operator=(const Gpu&) = delete;
  void initialize(dev_t device, const std::vector<Format>& formats, FontFamilies fonts, unsigned size,
                  unsigned weight = 400, float text_rounding = 0.5f, float icon_scale = 1.3f);
  void resize(unsigned width, unsigned height, unsigned font_size);
  void configure(FontFamilies fonts, unsigned weight, float rounding, float icon_scale,
                 Color background, std::array<float, 4> selection, float background_opacity);
  bool draw(Terminal& terminal, zwayland::client::Proxy& surface,
            zwayland::client::Proxy& dmabuf, bool cursor);
  unsigned cell_width() const { return cell_width_; }
  unsigned cell_height() const { return cell_height_; }
  std::function<void()> released;

 private:
  enum class GlyphFit { Text, Icon, Emoji, Grid, Separator };
  struct Glyph {
    float x, y, width, height, left, top, layer, sample_width, sample_height;
    bool color;
    Text::Bounds reference{};
    bool pixel_snap = false;
  };
  struct AtlasCursor { int x = 1, y = 1, row = 0, layer = 0; };
  struct Instance { float x, y, width, height, u, v, uw, vh, r, g, b, a, layer = 0; };
  struct Buffer;
  zwayland::client::Display& display_;
  int device_fd_ = -1;
  gbm_device* gbm_ = nullptr;
  EGLDisplay egl_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  Text text_renderer_;
  FontFamilies fonts_;
  unsigned font_size_ = 0, cell_width_ = 1, cell_height_ = 1;
  unsigned font_weight_ = 400;
  float text_rounding_ = 0.5f;
  float icon_scale_ = 1.3f;
  float background_opacity_ = 1.f;
  Color background_{16,19,25};
  std::array<float, 4> selection_color_{0.22f,0.48f,0.9f,0.45f};
  unsigned width_ = 0, height_ = 0;
  std::uint32_t format_ = 0;
  std::vector<std::uint64_t> modifiers_;
  std::vector<std::unique_ptr<Buffer>> buffers_;
  std::unordered_map<std::string, Glyph> glyphs_;
  GLuint program_ = 0, atlas_ = 0, color_atlas_ = 0, vao_ = 0, instances_ = 0, text_instances_ = 0, cursor_instances_ = 0, selection_instances_ = 0;
  std::uint64_t cached_revision_ = 0;
  std::array<AtlasCursor, 2> atlas_cursors_{};
  unsigned atlas_generation_ = 0;
  std::vector<Instance> backgrounds_, text_, selection_rects_;
  Glyph glyph(const std::string& text, bool bold, bool italic, std::uint32_t rgb,
              bool grid = false, bool separator = false, FontRole role = FontRole::Mono);
  Glyph fit_glyph(Glyph glyph, GlyphFit fit, unsigned cells, unsigned available) const;
  void append_glyph(const Glyph& glyph, float x, float y, Color color);
  void build_text(Terminal& terminal);
  void set_font_size(unsigned size);
  void batch(const std::vector<Instance>& instances, bool text, bool upload, GLuint buffer = 0);
  std::unique_ptr<Buffer> create_buffer(zwayland::client::Proxy& dmabuf);
};

}  // namespace zterm
