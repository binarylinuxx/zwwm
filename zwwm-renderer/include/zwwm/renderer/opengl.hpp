#pragma once

#include "zwwm/renderer/scene.hpp"

#include <epoxy/gl.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace zwwm::renderer {

enum class ShaderRole : std::uint8_t { builtin, window, border, background };
using ShaderValue = std::variant<bool, std::int32_t, std::array<float, 4>,
                                  std::vector<std::int32_t>,
                                  std::vector<std::array<float, 4>>>;

struct NamedShaderSource {
  std::string name;
  ShaderRole role = ShaderRole::window;
  std::string source;
  std::vector<std::pair<std::string, ShaderValue>> values;
};

struct ShaderSources {
  std::vector<NamedShaderSource> programs;
  std::string window;
  std::string border;
  std::string background;
  std::string background_image;
};

// Pixel-center geometry shared by ring planning tests and the shader contract.
[[nodiscard]] bool border_ring_contains(Size size, float corner_radius, float border_width,
                                        float x, float y);

struct DrawCall {
  DrawCall() = default;
  DrawCall(NodeId node_id_value, Rect bounds_value, float opacity_value, std::array<float, 4> color_value,
             std::optional<GLuint> texture_value, float corner_radius_value = 0.0F,
             float border_width_value = 0.0F,
             std::array<float, 4> source_uv_value = {0.0F, 0.0F, 1.0F, 1.0F},
             std::optional<Rect> clip_bounds_value = std::nullopt, float clip_radius_value = 0.0F)
      : node_id(node_id_value),
        bounds(bounds_value),
        opacity(opacity_value),
        color(color_value),
         texture(texture_value),
         corner_radius(corner_radius_value),
         border_width(border_width_value),
         source_uv(source_uv_value), clip_bounds(clip_bounds_value), clip_radius(clip_radius_value) {}

  NodeId node_id = 0;
  Rect bounds;
  float opacity = 1.0F;
  std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
  std::optional<GLuint> texture;
  float corner_radius = 0.0F;
  // Nonzero draws a rounded ring instead of a filled rectangle.
  float border_width = 0.0F;
  // Normalized source rectangle: left, top, right, bottom.
  std::array<float, 4> source_uv{0.0F, 0.0F, 1.0F, 1.0F};
  // Screen-space destination for source_uv, independent of decoration bounds.
  std::optional<Rect> texture_bounds;
  float texture_corner_radius = 0.0F;
  std::optional<Rect> clip_bounds;
  float clip_radius = 0.0F;
  float background_blur_radius = 0.0F;
  std::uint32_t background_blur_passes = 3;
  float background_blur_brightness = 1.0F;
  float background_blur_contrast = 1.0F;
  float background_blur_saturation = 1.0F;
  float background_blur_noise = 0.0F;
  // Use the client texture's alpha as backdrop coverage for shaped layer surfaces.
  bool background_mask_texture = false;
  float background_mask_alpha_threshold = 0.0F;
  bool glass = false;
  std::uint32_t glass_quality = 2;
  float glass_refraction = 0.0F;
  float glass_dispersion = 0.0F;
  float glass_frost = 0.0F;
  float glass_time = 0.0F;
  float glass_ior = 1.5F;
  float glass_thickness = 12.0F;
  float glass_roughness = 0.18F;
  std::array<float, 4> glass_tint{0.0F, 0.0F, 0.0F, 0.0F};
  std::uint32_t texture_transform = 0;
  bool opaque = false;
  ShaderRole shader_role = ShaderRole::builtin;
  std::string shader_name;
  NodeId toplevel_id = 0;
  Rect toplevel_bounds;
  std::uint32_t toplevel_state = 0;
  float shader_time = 0.0F;
};

struct FramePlan {
  std::vector<Rect> damage;
  std::vector<DrawCall> draws;
  bool draw_background = false;
  float shader_time = 0.0F;
};

class OpenGlRenderer {
 public:
  OpenGlRenderer() = default;
  OpenGlRenderer(const OpenGlRenderer&) = delete;
  OpenGlRenderer& operator=(const OpenGlRenderer&) = delete;

  // A desktop OpenGL 3.3 context must be current on this thread.
  [[nodiscard]] bool initialize();
  // Compiles a complete candidate set without changing the active programs.
  [[nodiscard]] bool prepare_shaders(const ShaderSources& sources);
  void commit_shaders();
  void discard_shaders();
  // The same context used by initialize() must be current on this thread.
  void shutdown();
  [[nodiscard]] bool ready() const;
  [[nodiscard]] const std::string& last_error() const;

  // Texture ownership remains with the importing backend.
  [[nodiscard]] bool bind_texture(TextureHandle handle, GLuint texture);
  void unbind_texture(TextureHandle handle);

  // Draws solid or sampled rectangles with an optional pixel corner radius.
  [[nodiscard]] bool render(const FramePlan& frame, Size target_size) const;
  // Renders an isolated frame into top-down XRGB8888 CPU storage.
  [[nodiscard]] bool render_pixels(const FramePlan& frame, Size target_size,
                                   std::vector<std::uint32_t>* pixels) const;
  [[nodiscard]] FramePlan build_frame(const Scene& scene, const DamageRegion& damage) const;
  [[nodiscard]] static constexpr GLenum texture_target() { return GL_TEXTURE_2D; }

 private:
  GLuint program_ = 0;
  GLuint vertex_array_ = 0;
  GLuint vertex_buffer_ = 0;
  GLint rect_location_ = -1;
  GLint rect_size_location_ = -1;
  GLint corner_radius_location_ = -1;
  GLint border_width_location_ = -1;
  GLint color_location_ = -1;
  GLint surface_texture_location_ = -1;
  GLint has_surface_texture_location_ = -1;
  GLint source_uv_location_ = -1;
  GLint texture_rect_location_ = -1;
  GLint has_texture_rect_location_ = -1;
  GLint texture_corner_radius_location_ = -1;
  GLint draw_origin_location_ = -1;
  GLint clip_rect_location_ = -1;
  GLint clip_radius_location_ = -1;
  GLint has_clip_location_ = -1;
  GLint input_to_clip_row_0_location_ = -1;
  GLint input_to_clip_row_1_location_ = -1;
  GLint texture_transform_location_ = -1;
  GLint background_texture_location_ = -1;
  GLint target_size_location_ = -1;
  GLint background_only_location_ = -1;
  GLint background_mask_alpha_threshold_location_ = -1;
  GLuint glass_program_ = 0;
  struct CustomProgram {
    GLuint program = 0;
    ShaderRole role = ShaderRole::window;
    std::vector<std::pair<std::string, ShaderValue>> values;
  };
  std::unordered_map<std::string, CustomProgram> custom_programs_;
  std::unordered_map<std::string, CustomProgram> pending_custom_programs_;
  std::string window_shader_;
  std::string border_shader_;
  std::string background_shader_;
  std::string pending_window_shader_;
  std::string pending_border_shader_;
  std::string pending_background_shader_;
  GLuint wallpaper_texture_ = 0;
  GLuint pending_wallpaper_texture_ = 0;
  GLint glass_rect_location_ = -1;
  GLint glass_rect_size_location_ = -1;
  GLint glass_corner_radius_location_ = -1;
  GLint glass_background_texture_location_ = -1;
  GLint glass_reflection_texture_location_ = -1;
  GLint glass_target_size_location_ = -1;
  GLint glass_draw_origin_location_ = -1;
  GLint glass_has_clip_location_ = -1;
  GLint glass_clip_rect_location_ = -1;
  GLint glass_clip_radius_location_ = -1;
  GLint glass_quality_location_ = -1;
  GLint glass_refraction_location_ = -1;
  GLint glass_dispersion_location_ = -1;
  GLint glass_frost_location_ = -1;
  GLint glass_time_location_ = -1;
  GLint glass_tint_location_ = -1;
  GLint glass_ior_location_ = -1;
  GLint glass_thickness_location_ = -1;
  GLint glass_roughness_location_ = -1;
  GLint glass_opacity_location_ = -1;
  mutable GLuint background_texture_ = 0;
  mutable GLuint reflection_texture_ = 0;
  GLuint kawase_program_ = 0;
  GLint kawase_source_location_ = -1;
  GLint kawase_texel_size_location_ = -1;
  GLint kawase_offset_location_ = -1;
  GLint kawase_upsample_location_ = -1;
  GLint kawase_finalize_location_ = -1;
  GLint kawase_brightness_location_ = -1;
  GLint kawase_contrast_location_ = -1;
  GLint kawase_saturation_location_ = -1;
  GLint kawase_noise_location_ = -1;
  mutable GLuint kawase_framebuffer_ = 0;
  mutable std::vector<GLuint> kawase_textures_;
  std::unordered_map<TextureHandle, GLuint> textures_;
  std::string last_error_;
};

}  // namespace zwwm::renderer
