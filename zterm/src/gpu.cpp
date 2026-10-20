#include "gpu.hpp"

#include <linux-dmabuf-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace zterm {
namespace p = zwayland::generated;
namespace {

constexpr int atlas_size = 2048;
constexpr const char* vertex_shader = R"(#version 300 es
precision highp float;
layout(location=0) in vec4 rect;
layout(location=1) in vec4 texrect;
layout(location=2) in vec4 color;
layout(location=3) in float layer;
uniform vec2 viewport;
out vec3 uv;
out vec4 tint;
flat out float colored;
flat out float pixel_snap;
flat out vec4 uv_bounds;
void main() {
  vec2 corners[6] = vec2[](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
  vec2 q = corners[gl_VertexID];
  vec2 pixel = rect.xy + q * rect.zw;
  gl_Position = vec4(pixel.x / viewport.x * 2.0 - 1.0, pixel.y / viewport.y * 2.0 - 1.0, 0, 1);
  pixel_snap = layer >= 16.0 ? 1.0 : 0.0;
  uv = vec3(texrect.xy + q * abs(texrect.zw), layer - pixel_snap * 16.0);
  uv_bounds = vec4(texrect.xy, texrect.xy + abs(texrect.zw));
  tint = color;
  colored = texrect.z < 0.0 ? 1.0 : 0.0;
})";
constexpr const char* fragment_shader = R"(#version 300 es
precision highp float;
in vec3 uv;
in vec4 tint;
flat in float colored;
flat in float pixel_snap;
flat in vec4 uv_bounds;
uniform highp sampler2DArray atlas;
uniform highp sampler2DArray color_atlas;
uniform bool glyphs;
uniform float text_rounding;
out vec4 output_color;
vec3 coordinates(vec2 point) {
  vec2 inset = vec2(0.5 / 2048.0);
  vec2 first = uv_bounds.xy + inset, last = uv_bounds.zw - inset;
  return vec3(clamp(point, min(first, last), max(first, last)), uv.z);
}
float mask(vec2 point) { return texture(atlas, coordinates(point)).r; }
void main() {
  if (!glyphs) { output_color = vec4(tint.rgb * tint.a, tint.a); return; }
  if (colored > 0.5) { output_color = texture(color_atlas, coordinates(uv.xy)) * tint.a; return; }
  vec2 dx = dFdx(uv.xy), dy = dFdy(uv.xy);
  float coverage;
  if (pixel_snap > 0.5) {
    dx *= 0.25; dy *= 0.25;
    coverage = max(max(mask(uv.xy - dx - dy), mask(uv.xy + dx - dy)),
                   max(mask(uv.xy - dx + dy), mask(uv.xy + dx + dy)));
  } else if (text_rounding > 0.0) {
    vec2 x = dx * text_rounding * 0.8;
    vec2 y = dy * text_rounding * 0.8;
    vec2 d1 = (x + y) * 0.70710678;
    vec2 d2 = (x - y) * 0.70710678;
    coverage = mask(uv.xy) * 0.25 +
      (mask(uv.xy + x) + mask(uv.xy - x) + mask(uv.xy + y) + mask(uv.xy - y)) * 0.125 +
      (mask(uv.xy + d1) + mask(uv.xy - d1) + mask(uv.xy + d2) + mask(uv.xy - d2)) * 0.0625;
  } else {
    dx *= 0.25; dy *= 0.25;
    coverage = (mask(uv.xy - dx - dy) + mask(uv.xy + dx - dy) + mask(uv.xy - dx + dy) + mask(uv.xy + dx + dy)) * 0.25;
  }
  coverage = pow(clamp((coverage - 0.5) * 1.04 + 0.5, 0.0, 1.0), 0.95);
  float alpha = tint.a * coverage;
  output_color = vec4(tint.rgb * alpha, alpha);
})";

GLuint compile(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
  GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    std::array<char, 2048> log{}; glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
    glDeleteShader(shader); throw std::runtime_error(log.data());
  }
  return shader;
}

int render_node(dev_t device) {
  std::array<drmDevicePtr, 64> devices{};
  const int count = drmGetDevices2(0, devices.data(), devices.size());
  int result = -1;
  for (int i = 0; i < count && result < 0; ++i) {
    bool matches = false;
    for (int node = 0; node < DRM_NODE_MAX; ++node) {
      if (!(devices[i]->available_nodes & (1 << node))) continue;
      struct stat info{};
      if (stat(devices[i]->nodes[node], &info) == 0 && info.st_rdev == device) matches = true;
    }
    if (matches && (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
      result = open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
  }
  if (count > 0) drmFreeDevices(devices.data(), count);
  if (result < 0) throw std::runtime_error("cannot open the compositor GPU render node");
  return result;
}

}  // namespace

struct Gpu::Buffer {
  Gpu* owner = nullptr;
  gbm_bo* bo = nullptr;
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  GLuint texture = 0, framebuffer = 0;
  zwayland::client::Proxy* proxy = nullptr;
  bool busy = false, retired = false;
  ~Buffer() {
    if (proxy != nullptr) p::wl_buffer_destroy(owner->display_, proxy->id);
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (texture) glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(owner->egl_, image);
    if (bo != nullptr) gbm_bo_destroy(bo);
  }
};

Gpu::Gpu(zwayland::client::Display& display) : display_(display) {}

Gpu::~Gpu() {
  if (context_ != EGL_NO_CONTEXT) {
    (void)eglMakeCurrent(egl_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_);
    buffers_.clear();
    if (program_) glDeleteProgram(program_);
    if (atlas_) glDeleteTextures(1, &atlas_);
    if (color_atlas_) glDeleteTextures(1, &color_atlas_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (instances_) glDeleteBuffers(1, &instances_);
    if (text_instances_) glDeleteBuffers(1, &text_instances_);
    if (cursor_instances_) glDeleteBuffers(1, &cursor_instances_);
    if (selection_instances_) glDeleteBuffers(1, &selection_instances_);
    (void)eglMakeCurrent(egl_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl_, context_);
  }
  if (egl_ != EGL_NO_DISPLAY) eglTerminate(egl_);
  if (gbm_ != nullptr) gbm_device_destroy(gbm_);
  if (device_fd_ >= 0) close(device_fd_);
}

void Gpu::initialize(dev_t device, const std::vector<Format>& formats, std::string font, unsigned size,
                     unsigned weight, float text_rounding, float icon_scale) {
  font_ = std::move(font);
  font_weight_ = weight;
  text_rounding_ = text_rounding;
  icon_scale_ = icon_scale;
  device_fd_ = render_node(device);
  gbm_ = gbm_create_device(device_fd_);
  if (gbm_ == nullptr) throw std::runtime_error("cannot initialize GBM");
  egl_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
  if (egl_ == EGL_NO_DISPLAY || !eglInitialize(egl_, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API))
    throw std::runtime_error("cannot initialize GPU EGL");
  if (!epoxy_has_egl_extension(egl_, "EGL_KHR_surfaceless_context") ||
      !epoxy_has_egl_extension(egl_, "EGL_EXT_image_dma_buf_import_modifiers"))
    throw std::runtime_error("GPU requires surfaceless EGL and DMA-BUF modifiers");
  const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
  EGLConfig config{}; EGLint count = 0;
  if (!eglChooseConfig(egl_, attributes, &config, 1, &count) || count == 0)
    throw std::runtime_error("no GLES3 configuration");
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context_ = eglCreateContext(egl_, config, EGL_NO_CONTEXT, context_attributes);
  if (context_ == EGL_NO_CONTEXT || !eglMakeCurrent(egl_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_))
    throw std::runtime_error("cannot create GLES3 context");
  const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  if (renderer == nullptr || std::string_view(renderer).find("llvmpipe") != std::string_view::npos ||
      std::string_view(renderer).find("softpipe") != std::string_view::npos)
    throw std::runtime_error("a hardware GPU renderer is required");
  if (!epoxy_has_gl_extension("GL_OES_EGL_image")) throw std::runtime_error("EGLImage rendering is unavailable");
  for (const auto preferred : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888}) {
    for (const auto& entry : formats) if (entry.format == preferred && entry.modifier != DRM_FORMAT_MOD_INVALID)
      modifiers_.push_back(entry.modifier);
    if (!modifiers_.empty()) { format_ = preferred; break; }
  }
  if (!format_) throw std::runtime_error("no shared GPU buffer format");
  const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_shader);
  const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_shader);
  program_ = glCreateProgram(); glAttachShader(program_, vertex); glAttachShader(program_, fragment);
  glLinkProgram(program_); glDeleteShader(vertex); glDeleteShader(fragment);
  GLint linked = 0; glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (!linked) throw std::runtime_error("cannot link terminal shaders");
  glGenVertexArrays(1, &vao_); glBindVertexArray(vao_);
  glGenBuffers(1, &instances_); glBindBuffer(GL_ARRAY_BUFFER, instances_);
  glGenBuffers(1, &text_instances_); glGenBuffers(1, &cursor_instances_);
  glGenBuffers(1, &selection_instances_);
  for (unsigned i = 0; i < 3; ++i) {
    glEnableVertexAttribArray(i);
    glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), reinterpret_cast<void*>(i * 4 * sizeof(float)));
    glVertexAttribDivisor(i, 1);
  }
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Instance), reinterpret_cast<void*>(12 * sizeof(float)));
  glVertexAttribDivisor(3, 1);
  glGenTextures(1, &atlas_); glGenTextures(1, &color_atlas_);
  for (unsigned color = 0; color < 2; ++color) {
    glBindTexture(GL_TEXTURE_2D_ARRAY, color ? color_atlas_ : atlas_);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, color ? GL_RGBA8 : GL_R8, atlas_size, atlas_size, color ? 1 : 8);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  set_font_size(size);
}

void Gpu::set_font_size(unsigned size) {
  if (font_size_ == size) return;
  font_size_ = size;
  text_renderer_.configure(font_, size, font_weight_);
  cell_width_ = text_renderer_.width(); cell_height_ = text_renderer_.height();
  glyphs_.clear(); atlas_cursors_.fill({}); ++atlas_generation_;
  cached_revision_ = 0;
}

void Gpu::resize(unsigned width, unsigned height, unsigned font_size) {
  set_font_size(font_size);
  if (width == width_ && height == height_) return;
  width_ = width; height_ = height;
  for (auto& buffer : buffers_) buffer->retired = true;
}

void Gpu::configure(std::string font, unsigned weight, float rounding, float icon_scale,
                     Color background, std::array<float, 4> selection) {
  if (font != font_ || weight != font_weight_) font_size_ = 0;
  font_ = std::move(font); font_weight_ = weight; text_rounding_ = rounding; icon_scale_ = icon_scale;
  background_ = background; selection_color_ = selection; cached_revision_ = 0;
}

Gpu::Glyph Gpu::glyph(const std::string& text, bool bold, bool italic, std::uint32_t rgb, bool grid, bool separator) {
  std::string key = text;
  key.push_back('\0'); key.push_back(bold); key.push_back(italic); key.push_back(grid); key.push_back(separator);
  if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
  std::string colored_key = key; colored_key.push_back('\1'); colored_key.append(reinterpret_cast<const char*>(&rgb), sizeof(rgb));
  if (auto it = glyphs_.find(colored_key); it != glyphs_.end()) return it->second;
  const auto bitmap = text_renderer_.rasterize(text, bold, italic, rgb, grid, separator);
  const bool color = bitmap.color;
  Glyph result{};
  if (bitmap.width > 0 && bitmap.height > 0) {
    auto& atlas = atlas_cursors_[color ? 1 : 0];
    if (atlas.x + bitmap.width + 1 >= atlas_size) {
      atlas.x = 1; atlas.y += atlas.row + 1; atlas.row = 0;
    }
    if (atlas.y + bitmap.height + 1 >= atlas_size) {
      ++atlas.layer; atlas.x = atlas.y = 1; atlas.row = 0;
    }
    if (atlas.layer >= (color ? 1 : 8) || glyphs_.size() >= 32768) {
      glyphs_.clear(); atlas_cursors_.fill({}); ++atlas_generation_;
    }
    glActiveTexture(color ? GL_TEXTURE1 : GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, color ? color_atlas_ : atlas_);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, atlas.x, atlas.y, atlas.layer, bitmap.width, bitmap.height, 1,
                    color ? GL_RGBA : GL_RED, GL_UNSIGNED_BYTE, bitmap.pixels.data());
    result = {float(atlas.x), float(atlas.y), float(bitmap.width) / bitmap.scale, float(bitmap.height) / bitmap.scale,
              float(bitmap.x), float(bitmap.y), float(atlas.layer), float(bitmap.width), float(bitmap.height), color};
    atlas.x += bitmap.width + 1; atlas.row = std::max(atlas.row, bitmap.height);
  }
  result.reference = bitmap.reference;
  glyphs_.emplace(color ? colored_key : key, result);
  return result;
}

std::unique_ptr<Gpu::Buffer> Gpu::create_buffer(zwayland::client::Proxy& dmabuf) {
  auto buffer = std::make_unique<Buffer>(); buffer->owner = this;
  buffer->bo = gbm_bo_create_with_modifiers2(gbm_, width_, height_, format_, modifiers_.data(), modifiers_.size(), GBM_BO_USE_RENDERING);
  if (buffer->bo == nullptr) throw std::runtime_error("cannot allocate a shared GPU framebuffer");
  const int planes = gbm_bo_get_plane_count(buffer->bo);
  if (planes < 1 || planes > 4) throw std::runtime_error("unsupported GPU plane count");
  constexpr std::array<EGLint, 4> fd_attr{EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
  constexpr std::array<EGLint, 4> offset_attr{EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
  constexpr std::array<EGLint, 4> stride_attr{EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
  constexpr std::array<EGLint, 4> lo_attr{EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
  constexpr std::array<EGLint, 4> hi_attr{EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
  std::vector<EGLint> attributes{EGL_WIDTH, static_cast<EGLint>(width_), EGL_HEIGHT, static_cast<EGLint>(height_), EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(format_)};
  const auto modifier = gbm_bo_get_modifier(buffer->bo);
  std::array<int, 4> fds{-1, -1, -1, -1};
  for (int plane = 0; plane < planes; ++plane) {
    fds[plane] = gbm_bo_get_fd_for_plane(buffer->bo, plane);
    if (fds[plane] < 0) {
      for (int fd : fds) if (fd >= 0) close(fd);
      throw std::runtime_error("cannot export a GPU buffer plane");
    }
    attributes.insert(attributes.end(), {fd_attr[plane], fds[plane], offset_attr[plane], static_cast<EGLint>(gbm_bo_get_offset(buffer->bo, plane)),
      stride_attr[plane], static_cast<EGLint>(gbm_bo_get_stride_for_plane(buffer->bo, plane)),
      lo_attr[plane], static_cast<EGLint>(modifier), hi_attr[plane], static_cast<EGLint>(modifier >> 32U)});
  }
  attributes.push_back(EGL_NONE);
  buffer->image = eglCreateImageKHR(egl_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
  if (buffer->image == EGL_NO_IMAGE_KHR) {
    for (int fd : fds) if (fd >= 0) close(fd);
    throw std::runtime_error("cannot import the GPU framebuffer as an EGLImage");
  }
  const auto params = p::zwp_linux_dmabuf_v1_create_params(display_, dmabuf.id);
  for (int plane = 0; plane < planes; ++plane) {
    p::zwp_linux_buffer_params_v1_add(display_, params, fds[plane], plane,
      gbm_bo_get_offset(buffer->bo, plane), gbm_bo_get_stride_for_plane(buffer->bo, plane), modifier >> 32U, modifier);
    close(fds[plane]);
  }
  const auto id = p::zwp_linux_buffer_params_v1_create_immed(display_, params, width_, height_, format_, 0);
  p::zwp_linux_buffer_params_v1_destroy(display_, params);
  buffer->proxy = display_.find_proxy(id);
  struct Release {
    Buffer* buffer;
    void release(zwayland::client::Proxy&) const {
      buffer->busy = false;
      if (buffer->owner->released) buffer->owner->released();
    }
  };
  buffer->proxy->set_observer(p::wl_buffer_observer(Release{buffer.get()}));
  glGenTextures(1, &buffer->texture); glBindTexture(GL_TEXTURE_2D, buffer->texture);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buffer->image);
  glGenFramebuffers(1, &buffer->framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, buffer->framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer->texture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    throw std::runtime_error("GPU buffer is not renderable");
  return buffer;
}

void Gpu::batch(const std::vector<Instance>& instances, bool text, bool upload, GLuint buffer) {
  if (instances.empty()) return;
  glUniform1i(glGetUniformLocation(program_, "glyphs"), text);
  glBindBuffer(GL_ARRAY_BUFFER, buffer ? buffer : text ? text_instances_ : instances_);
  for (unsigned i = 0; i < 3; ++i)
    glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), reinterpret_cast<void*>(i * 4 * sizeof(float)));
  glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Instance), reinterpret_cast<void*>(12 * sizeof(float)));
  if (upload) glBufferData(GL_ARRAY_BUFFER, instances.size() * sizeof(Instance), instances.data(), GL_DYNAMIC_DRAW);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, instances.size());
}

Gpu::Glyph Gpu::fit_glyph(Glyph glyph, GlyphFit fit, unsigned cells, unsigned available) const {
  if (glyph.width <= 0 || glyph.height <= 0) return glyph;
  const float width = cells * cell_width_, height = cell_height_, span = available * cell_width_;
  const float ink_width = std::max(1.f, glyph.width - 2), ink_height = std::max(1.f, glyph.height - 2);
  if (fit == GlyphFit::Grid || fit == GlyphFit::Separator) {
    auto reference = glyph.reference;
    if (reference.width <= 0 || reference.height <= 0) reference = {0,0,float(cell_width_),float(cell_height_)};
    const float sx = width / reference.width, sy = height / reference.height;
    glyph.left = (glyph.left - reference.x) * sx; glyph.top = (glyph.top - reference.y) * sy;
    glyph.width *= sx; glyph.height *= sy; glyph.pixel_snap = true;
  } else if (fit == GlyphFit::Icon) {
    const float scale = std::min({icon_scale_, span * 0.96f / ink_width, height * 0.85f / ink_height});
    glyph.left = std::max(0.f, std::floor((width - ink_width * scale) / 2)) - scale;
    glyph.top = std::floor((height - ink_height * scale) / 2) - scale;
    glyph.width *= scale; glyph.height *= scale;
  }
  const auto clip = [](float& origin, float& extent, float& sample, float& sample_extent, float limit) {
    const float first = std::max(0.f, origin), last = std::min(limit, origin + extent);
    if (last <= first) { extent = sample_extent = 0; return; }
    sample += (first - origin) / extent * sample_extent;
    sample_extent *= (last - first) / extent;
    origin = first; extent = last - first;
  };
  clip(glyph.left,glyph.width,glyph.x,glyph.sample_width,fit == GlyphFit::Icon || fit == GlyphFit::Text ? span : width);
  clip(glyph.top,glyph.height,glyph.y,glyph.sample_height,height);
  return glyph;
}

void Gpu::append_glyph(const Glyph& glyph, float x, float y, Color color) {
  if (glyph.width <= 0 || glyph.height <= 0) return;
  text_.push_back({x+glyph.left,y+glyph.top,glyph.width,glyph.height,
    glyph.x / atlas_size,glyph.y / atlas_size,(glyph.color ? -glyph.sample_width : glyph.sample_width) / atlas_size,
    glyph.sample_height / atlas_size,color.red / 255.f,color.green / 255.f,color.blue / 255.f,1,
    glyph.layer + (glyph.pixel_snap ? 16.f : 0.f)});
}

void Gpu::build_text(Terminal& terminal) {
  backgrounds_.clear(); text_.clear();
  for (int row = 0; row < terminal.rows(); ++row) {
    for (int col = 0; col < terminal.columns(); ++col) {
      auto cell = terminal.cell(row, col);
      if (cell.width == 0 || cell.chars[0] == UINT32_MAX) continue;
      if (cell.attrs.reverse) std::swap(cell.fg, cell.bg);
      if (cell.attrs.hidden) cell.fg = cell.bg;
      if (cell.attrs.dim) { cell.fg.red /= 2; cell.fg.green /= 2; cell.fg.blue /= 2; }
      const float x = col * cell_width_, y = row * cell_height_;
      const float width = std::max(1, int(cell.width)) * cell_width_;
      backgrounds_.push_back({x,y,width,float(cell_height_),0,0,0,0, cell.bg.red / 255.f,cell.bg.green / 255.f,cell.bg.blue / 255.f,1});
      const std::uint32_t foreground = (std::uint32_t(cell.fg.red) << 16U) | (std::uint32_t(cell.fg.green) << 8U) | cell.fg.blue;
      if (cell.attrs.underline || cell.attrs.strike) {
        const float line_y = cell.attrs.strike ? y + cell_height_ / 2 : y + cell_height_ - 2;
        backgrounds_.push_back({x,line_y,width,1,0,0,0,0,cell.fg.red / 255.f,cell.fg.green / 255.f,cell.fg.blue / 255.f,1});
      }
      if (cell.attrs.hidden || !cell.chars[0] || (cell.chars[0] == U' ' && !cell.chars[1])) continue;
      const auto character = cell.chars[0];
      const bool grid = (character >= 0x2500 && character <= 0x259f) || (character >= 0x2800 && character <= 0x28ff) ||
                        (character >= 0x1fb00 && character <= 0x1fbff) || (character >= 0x1cc00 && character <= 0x1cebf);
      const bool separator = character >= 0xe0b0 && character <= 0xe0d7;
      const bool icon = (character >= 0xe000 && character <= 0xf8ff) ||
                        (character >= 0xf0000 && character <= 0xffffd) ||
                        (character >= 0x100000 && character <= 0x10fffd);
      std::string cluster;
      for (const auto codepoint : cell.chars) { if (!codepoint) break; Text::append_utf8(cluster, codepoint); }
      unsigned available = cell.width;
      if (!grid && !separator && col + cell.width < terminal.columns()) {
        const auto next = terminal.cell(row, col + cell.width);
        const auto next_background = next.attrs.reverse ? next.fg : next.bg;
        if ((next.chars[0] == 0 || next.chars[0] == U' ') && next.width == 1 && next_background == cell.bg) ++available;
      }
      const auto fit = grid ? GlyphFit::Grid : separator ? GlyphFit::Separator : icon ? GlyphFit::Icon : GlyphFit::Text;
      append_glyph(fit_glyph(glyph(cluster,cell.attrs.bold,grid ? false : cell.attrs.italic,foreground,grid,separator),fit,cell.width,available),x,y,cell.fg);
    }
  }
}

bool Gpu::draw(Terminal& terminal, zwayland::client::Proxy& surface, zwayland::client::Proxy& dmabuf, bool cursor) {
  std::erase_if(buffers_, [](const auto& buffer) { return buffer->retired && !buffer->busy; });
  Buffer* target = nullptr;
  unsigned current = 0;
  for (auto& buffer : buffers_) if (!buffer->retired) {
    ++current; if (!buffer->busy) target = buffer.get();
  }
  if (target == nullptr && current < 3) {
    buffers_.push_back(create_buffer(dmabuf)); target = buffers_.back().get();
  }
  if (target == nullptr) return false;
  const bool rebuild = cached_revision_ != terminal.revision();
  if (rebuild) {
    for (int attempt = 0; ; ++attempt) {
      const auto generation = atlas_generation_;
      build_text(terminal);
      if (generation == atlas_generation_) break;
      if (attempt >= 2) throw std::runtime_error("visible text exceeds the GPU cache");
    }
    cached_revision_ = terminal.revision();
  }
  glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer); glViewport(0, 0, width_, height_);
  glClearColor(background_.red / 255.f,background_.green / 255.f,background_.blue / 255.f,1); glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(program_); glUniform2f(glGetUniformLocation(program_, "viewport"), width_, height_);
  glUniform1f(glGetUniformLocation(program_, "text_rounding"), text_rounding_);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, atlas_);
  glUniform1i(glGetUniformLocation(program_, "atlas"), 0);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D_ARRAY, color_atlas_);
  glUniform1i(glGetUniformLocation(program_, "color_atlas"), 1);
  glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, instances_);
  glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  batch(backgrounds_, false, rebuild);
  selection_rects_.clear();
  if (terminal.has_selection()) for (int row = 0; row < terminal.rows(); ++row) {
    const auto [first, last] = terminal.selection_range(row);
    if (last > first) selection_rects_.push_back({first * float(cell_width_), row * float(cell_height_),
      (last - first) * float(cell_width_), float(cell_height_),0,0,0,0,
      selection_color_[0],selection_color_[1],selection_color_[2],selection_color_[3]});
  }
  batch(text_, true, rebuild);
  batch(selection_rects_, false, true, selection_instances_);
  const auto position = terminal.cursor_position();
  const auto style = terminal.cursor_style();
  if ((cursor || !style.blink) && !terminal.has_selection() && terminal.cursor_at(position.row, position.col)) {
    const float width = std::max(1, int(terminal.cell(position.row, position.col).width)) * cell_width_;
    const float thickness = std::max(1.f, std::round(cell_height_ * 0.09f));
    const auto color = terminal.foreground_color();
    Instance cursor_instance{position.col * float(cell_width_), position.row * float(cell_height_), width, float(cell_height_),
                             0,0,0,0,color.red / 255.f,color.green / 255.f,color.blue / 255.f,1};
    if (style.shape == CursorShape::Block) {
      glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
      cursor_instance.r = cursor_instance.g = cursor_instance.b = 1;
    } else if (style.shape == CursorShape::Beam) cursor_instance.width = thickness;
    else { cursor_instance.y += cell_height_ - thickness; cursor_instance.height = thickness; }
    batch({cursor_instance}, false, true, cursor_instances_);
  }
  glFlush();
  p::wl_surface_attach(display_, surface.id, target->proxy, 0, 0);
  p::wl_surface_damage_buffer(display_, surface.id, 0, 0, width_, height_);
  target->busy = true;
  return true;
}

}  // namespace zterm
