#include "zwwm/renderer/opengl.hpp"
#include "shaders.hpp"

#include <png.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

#ifndef ZWWM_DATA_DIR
#define ZWWM_DATA_DIR "/usr/share/zwwm"
#endif

namespace zwwm::renderer {
namespace {

std::string default_wallpaper_path() {
  for (const auto& path : {
           std::filesystem::path{"/etc/xdg/zwwm/background/background.png"},
           std::filesystem::path{ZWWM_DATA_DIR} / "background/background.png",
           std::filesystem::path{"zwwm/data/background.png"}}) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) return path.string();
  }
  return "/etc/xdg/zwwm/background/background.png";
}

struct RenderDraw : DrawCall {
  FloatRect bounds, toplevel_bounds;
  std::optional<FloatRect> clip_bounds, texture_bounds;
  explicit RenderDraw(const DrawCall& draw) : DrawCall(draw), bounds(draw.bounds),
      toplevel_bounds(draw.toplevel_bounds) {
    if (draw.clip_bounds) clip_bounds = FloatRect(*draw.clip_bounds);
    if (draw.texture_bounds) texture_bounds = FloatRect(*draw.texture_bounds);
    if (draw.geometry) {
      bounds = draw.geometry->bounds;
      toplevel_bounds = draw.geometry->toplevel;
      if (draw.geometry->clip) clip_bounds = draw.geometry->clip;
      if (draw.geometry->texture) texture_bounds = draw.geometry->texture;
    }
  }
};

GLuint load_png_texture(const std::string& path, std::string& error,
                        std::uint32_t* width, std::uint32_t* height) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (png_image_begin_read_from_file(&image, path.c_str()) == 0) {
    error = "could not read background image " + path + ": " + image.message;
    return 0;
  }
  image.format = PNG_FORMAT_RGBA;
  if (image.width > static_cast<png_uint_32>(std::numeric_limits<GLsizei>::max()) ||
      image.height > static_cast<png_uint_32>(std::numeric_limits<GLsizei>::max())) {
    png_image_free(&image);
    error = "background image dimensions are too large";
    return 0;
  }
  std::vector<png_byte> pixels(PNG_IMAGE_SIZE(image));
  if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) == 0) {
    error = "could not decode background image " + path + ": " + image.message;
    png_image_free(&image);
    return 0;
  }

  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image.width),
               static_cast<GLsizei>(image.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  *width = image.width;
  *height = image.height;
  png_image_free(&image);
  return texture;
}

float rounded_rectangle_distance(float x, float y, float width, float height, float radius) {
  const float effective_radius = std::clamp(radius, 0.0F, 0.5F * std::min(width, height));
  const float half_width = width * 0.5F;
  const float half_height = height * 0.5F;
  const float corner_x = std::abs(x - half_width) - half_width + effective_radius;
  const float corner_y = std::abs(y - half_height) - half_height + effective_radius;
  return std::hypot(std::max(corner_x, 0.0F), std::max(corner_y, 0.0F)) +
         std::min(std::max(corner_x, corner_y), 0.0F) - effective_radius;
}

GLuint compile_shader(GLenum type, const char* source, std::string& error) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }

  std::array<GLchar, 1024> log{};
  GLsizei length = 0;
  glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &length, log.data());
  error.assign(log.data(), static_cast<std::size_t>(length));
  glDeleteShader(shader);
  return 0;
}

GLuint link_program(const char* vertex_source, const char* fragment_source, std::string& error) {
  const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source, error);
  if (vertex == 0) return 0;
  const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source, error);
  if (fragment == 0) { glDeleteShader(vertex); return 0; }
  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) return program;
  std::array<GLchar, 4096> log{};
  GLsizei length = 0;
  glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), &length, log.data());
  error.assign(log.data(), static_cast<std::size_t>(length));
  glDeleteProgram(program);
  return 0;
}

void uniform_1i(GLuint program, const char* name, GLint value) {
  const GLint location = glGetUniformLocation(program, name);
  if (location >= 0) glUniform1i(location, value);
}

void uniform_1f(GLuint program, const char* name, GLfloat value) {
  const GLint location = glGetUniformLocation(program, name);
  if (location >= 0) glUniform1f(location, value);
}

void uniform_2f(GLuint program, const char* name, GLfloat x, GLfloat y) {
  const GLint location = glGetUniformLocation(program, name);
  if (location >= 0) glUniform2f(location, x, y);
}

void uniform_4f(GLuint program, const char* name, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
  const GLint location = glGetUniformLocation(program, name);
  if (location >= 0) glUniform4f(location, x, y, z, w);
}

std::optional<GLenum> uniform_type(GLuint program, std::string_view requested) {
  GLint count = 0;
  glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
  std::array<GLchar, 256> name{};
  for (GLint index = 0; index < count; ++index) {
    GLsizei length = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(program, static_cast<GLuint>(index), static_cast<GLsizei>(name.size()),
                       &length, &size, &type, name.data());
    const std::string_view active(name.data(), static_cast<std::size_t>(length));
    if (active == requested || (active.ends_with("[0]") && active.substr(0, active.size() - 3) == requested))
      return type;
  }
  return std::nullopt;
}

bool compatible_value(GLenum type, const ShaderValue& value) {
  if (std::holds_alternative<bool>(value)) return type == GL_BOOL;
  if (std::holds_alternative<std::int32_t>(value))
    return type == GL_INT || type == GL_UNSIGNED_INT || type == GL_FLOAT;
  if (std::holds_alternative<std::array<float, 4>>(value)) return type == GL_FLOAT_VEC4;
  if (std::holds_alternative<std::vector<std::array<float, 4>>>(value)) return type == GL_FLOAT_VEC4;
  const auto* components = std::get_if<std::vector<std::int32_t>>(&value);
  if (components == nullptr) return false;
  return (components->size() == 2 && (type == GL_FLOAT_VEC2 || type == GL_INT_VEC2)) ||
         (components->size() == 3 && (type == GL_FLOAT_VEC3 || type == GL_INT_VEC3)) ||
         (components->size() == 4 && (type == GL_FLOAT_VEC4 || type == GL_INT_VEC4));
}

bool pipeline_shader_value(ShaderRole role, std::string_view name) {
  if (role == ShaderRole::border) return name == "width" || name == "radius";
  if (role == ShaderRole::window)
    return name == "blur_radius" || name == "blur_passes" || name == "blur_brightness" ||
           name == "blur_contrast" || name == "blur_saturation" || name == "blur_noise";
  return false;
}

void apply_shader_values(GLuint program,
                         const std::vector<std::pair<std::string, ShaderValue>>& values) {
  for (const auto& [name, value] : values) {
    const std::string uniform = "zwwm_config_" + name;
    const GLint location = glGetUniformLocation(program, uniform.c_str());
    if (location < 0) continue;
    const GLenum type = *uniform_type(program, uniform);
    if (const auto* boolean = std::get_if<bool>(&value)) glUniform1i(location, *boolean ? GL_TRUE : GL_FALSE);
    else if (const auto* integer = std::get_if<std::int32_t>(&value)) {
      if (type == GL_FLOAT) glUniform1f(location, static_cast<float>(*integer));
      else if (type == GL_UNSIGNED_INT) glUniform1ui(location, static_cast<GLuint>(*integer));
      else glUniform1i(location, *integer);
    } else if (const auto* color = std::get_if<std::array<float, 4>>(&value)) {
      glUniform4f(location, (*color)[0], (*color)[1], (*color)[2], (*color)[3]);
    } else if (const auto* components = std::get_if<std::vector<std::int32_t>>(&value)) {
      if (type == GL_FLOAT_VEC2) glUniform2f(location, static_cast<float>((*components)[0]), static_cast<float>((*components)[1]));
      else if (type == GL_FLOAT_VEC3) glUniform3f(location, static_cast<float>((*components)[0]), static_cast<float>((*components)[1]), static_cast<float>((*components)[2]));
      else if (type == GL_FLOAT_VEC4) glUniform4f(location, static_cast<float>((*components)[0]), static_cast<float>((*components)[1]), static_cast<float>((*components)[2]), static_cast<float>((*components)[3]));
      else if (type == GL_INT_VEC2) glUniform2i(location, (*components)[0], (*components)[1]);
      else if (type == GL_INT_VEC3) glUniform3i(location, (*components)[0], (*components)[1], (*components)[2]);
      else if (type == GL_INT_VEC4) glUniform4i(location, (*components)[0], (*components)[1], (*components)[2], (*components)[3]);
    } else if (const auto* colors = std::get_if<std::vector<std::array<float, 4>>>(&value)) {
      glUniform4fv(location, static_cast<GLsizei>(colors->size()), colors->front().data());
      const std::string count_uniform = uniform + "_count";
      const GLint count_location = glGetUniformLocation(program, count_uniform.c_str());
      if (count_location >= 0) glUniform1i(count_location, static_cast<GLint>(colors->size()));
    }
  }
}

}  // namespace

bool border_ring_contains(Size size, float corner_radius, float border_width, float x, float y) {
  if (size.width == 0 || size.height == 0 || !std::isfinite(corner_radius) || !std::isfinite(border_width) ||
      corner_radius < 0.0F || border_width <= 0.0F) return false;
  const float width = static_cast<float>(size.width);
  const float height = static_cast<float>(size.height);
  if (rounded_rectangle_distance(x, y, width, height, corner_radius) > 0.0F) return false;
  const float inner_width = width - 2.0F * border_width;
  const float inner_height = height - 2.0F * border_width;
  return inner_width <= 0.0F || inner_height <= 0.0F ||
         rounded_rectangle_distance(x - border_width, y - border_width, inner_width, inner_height,
                                    std::max(corner_radius - border_width, 0.0F)) > 0.0F;
}

bool OpenGlRenderer::initialize() {
  if (ready()) {
    return true;
  }
  if (!epoxy_is_desktop_gl() && epoxy_gl_version() < 30) {
    last_error_ = "OpenGL ES 3.0 or newer is required";
    return false;
  }

  const GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, shader::surface_vertex, last_error_);
  if (vertex_shader == 0) {
    return false;
  }
  const GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, shader::surface_fragment, last_error_);
  if (fragment_shader == 0) {
    glDeleteShader(vertex_shader);
    return false;
  }

  program_ = glCreateProgram();
  glAttachShader(program_, vertex_shader);
  glAttachShader(program_, fragment_shader);
  glLinkProgram(program_);
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);

  GLint linked = GL_FALSE;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    std::array<GLchar, 1024> log{};
    GLsizei length = 0;
    glGetProgramInfoLog(program_, static_cast<GLsizei>(log.size()), &length, log.data());
    last_error_.assign(log.data(), static_cast<std::size_t>(length));
    shutdown();
    return false;
  }

  rect_location_ = glGetUniformLocation(program_, "rect");
  rect_size_location_ = glGetUniformLocation(program_, "rect_size");
  corner_radius_location_ = glGetUniformLocation(program_, "corner_radius");
  border_width_location_ = glGetUniformLocation(program_, "border_width");
  color_location_ = glGetUniformLocation(program_, "color");
  surface_texture_location_ = glGetUniformLocation(program_, "surface_texture");
  has_surface_texture_location_ = glGetUniformLocation(program_, "has_surface_texture");
  source_uv_location_ = glGetUniformLocation(program_, "source_uv");
  texture_rect_location_ = glGetUniformLocation(program_, "texture_rect");
  has_texture_rect_location_ = glGetUniformLocation(program_, "has_texture_rect");
  texture_corner_radius_location_ = glGetUniformLocation(program_, "texture_corner_radius");
  draw_origin_location_ = glGetUniformLocation(program_, "draw_origin");
  clip_rect_location_ = glGetUniformLocation(program_, "clip_rect");
  clip_radius_location_ = glGetUniformLocation(program_, "clip_radius");
  has_clip_location_ = glGetUniformLocation(program_, "has_clip");
  input_to_clip_row_0_location_ = glGetUniformLocation(program_, "input_to_clip_row_0");
  input_to_clip_row_1_location_ = glGetUniformLocation(program_, "input_to_clip_row_1");
  texture_transform_location_ = glGetUniformLocation(program_, "texture_transform");
  background_texture_location_ = glGetUniformLocation(program_, "background_texture");
  target_size_location_ = glGetUniformLocation(program_, "target_size");
  background_only_location_ = glGetUniformLocation(program_, "background_only");
  background_mask_alpha_threshold_location_ = glGetUniformLocation(program_, "background_mask_alpha_threshold");
  const GLuint glass_vertex = compile_shader(GL_VERTEX_SHADER, shader::surface_vertex, last_error_);
  const GLuint glass_fragment = glass_vertex == 0 ? 0 : compile_shader(GL_FRAGMENT_SHADER, shader::glass_fragment, last_error_);
  if (glass_vertex == 0 || glass_fragment == 0) {
    if (glass_vertex != 0) glDeleteShader(glass_vertex);
    shutdown();
    return false;
  }
  glass_program_ = glCreateProgram();
  glAttachShader(glass_program_, glass_vertex);
  glAttachShader(glass_program_, glass_fragment);
  glLinkProgram(glass_program_);
  glDeleteShader(glass_vertex);
  glDeleteShader(glass_fragment);
  glGetProgramiv(glass_program_, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    std::array<GLchar, 1024> log{};
    GLsizei length = 0;
    glGetProgramInfoLog(glass_program_, static_cast<GLsizei>(log.size()), &length, log.data());
    last_error_.assign(log.data(), static_cast<std::size_t>(length));
    shutdown();
    return false;
  }
  glass_rect_location_ = glGetUniformLocation(glass_program_, "rect");
  glass_rect_size_location_ = glGetUniformLocation(glass_program_, "rect_size");
  glass_corner_radius_location_ = glGetUniformLocation(glass_program_, "corner_radius");
  glass_background_texture_location_ = glGetUniformLocation(glass_program_, "background_texture");
  glass_reflection_texture_location_ = glGetUniformLocation(glass_program_, "reflection_texture");
  glass_target_size_location_ = glGetUniformLocation(glass_program_, "target_size");
  glass_draw_origin_location_ = glGetUniformLocation(glass_program_, "draw_origin");
  glass_has_clip_location_ = glGetUniformLocation(glass_program_, "has_clip");
  glass_clip_rect_location_ = glGetUniformLocation(glass_program_, "clip_rect");
  glass_clip_radius_location_ = glGetUniformLocation(glass_program_, "clip_radius");
  glass_quality_location_ = glGetUniformLocation(glass_program_, "glass_quality");
  glass_refraction_location_ = glGetUniformLocation(glass_program_, "glass_refraction");
  glass_dispersion_location_ = glGetUniformLocation(glass_program_, "glass_dispersion");
  glass_frost_location_ = glGetUniformLocation(glass_program_, "glass_frost");
  glass_time_location_ = glGetUniformLocation(glass_program_, "glass_time");
  glass_tint_location_ = glGetUniformLocation(glass_program_, "glass_tint");
  glass_ior_location_ = glGetUniformLocation(glass_program_, "glass_ior");
  glass_thickness_location_ = glGetUniformLocation(glass_program_, "glass_thickness");
  glass_roughness_location_ = glGetUniformLocation(glass_program_, "glass_roughness");
  glass_opacity_location_ = glGetUniformLocation(glass_program_, "glass_opacity");
  const GLuint kawase_vertex = compile_shader(GL_VERTEX_SHADER, shader::kawase_vertex, last_error_);
  const GLuint kawase_fragment = kawase_vertex == 0 ? 0 : compile_shader(GL_FRAGMENT_SHADER, shader::kawase_fragment, last_error_);
  if (kawase_vertex == 0 || kawase_fragment == 0) {
    if (kawase_vertex != 0) glDeleteShader(kawase_vertex);
    shutdown();
    return false;
  }
  kawase_program_ = glCreateProgram();
  glAttachShader(kawase_program_, kawase_vertex);
  glAttachShader(kawase_program_, kawase_fragment);
  glLinkProgram(kawase_program_);
  glDeleteShader(kawase_vertex);
  glDeleteShader(kawase_fragment);
  glGetProgramiv(kawase_program_, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    std::array<GLchar, 1024> log{};
    GLsizei length = 0;
    glGetProgramInfoLog(kawase_program_, static_cast<GLsizei>(log.size()), &length, log.data());
    last_error_.assign(log.data(), static_cast<std::size_t>(length));
    shutdown();
    return false;
  }
  kawase_source_location_ = glGetUniformLocation(kawase_program_, "source_texture");
  kawase_texel_size_location_ = glGetUniformLocation(kawase_program_, "texel_size");
  kawase_offset_location_ = glGetUniformLocation(kawase_program_, "sample_offset");
  kawase_upsample_location_ = glGetUniformLocation(kawase_program_, "upsample");
  kawase_finalize_location_ = glGetUniformLocation(kawase_program_, "finalize");
  kawase_brightness_location_ = glGetUniformLocation(kawase_program_, "brightness");
  kawase_contrast_location_ = glGetUniformLocation(kawase_program_, "contrast");
  kawase_saturation_location_ = glGetUniformLocation(kawase_program_, "saturation");
  kawase_noise_location_ = glGetUniformLocation(kawase_program_, "noise");
  glGenVertexArrays(1, &vertex_array_);
  glGenBuffers(1, &vertex_buffer_);
  if (vertex_array_ == 0 || vertex_buffer_ == 0 || rect_location_ < 0 || rect_size_location_ < 0 ||
       corner_radius_location_ < 0 || border_width_location_ < 0 || color_location_ < 0 || surface_texture_location_ < 0 ||
         has_surface_texture_location_ < 0 || texture_rect_location_ < 0 || has_texture_rect_location_ < 0 ||
           texture_corner_radius_location_ < 0 ||
           draw_origin_location_ < 0 || clip_rect_location_ < 0 || clip_radius_location_ < 0 || has_clip_location_ < 0 ||
             input_to_clip_row_0_location_ < 0 || input_to_clip_row_1_location_ < 0 || texture_transform_location_ < 0 ||
            background_texture_location_ < 0 || target_size_location_ < 0 || background_only_location_ < 0 ||
              background_mask_alpha_threshold_location_ < 0 ||
             glass_rect_location_ < 0 || glass_rect_size_location_ < 0 || glass_corner_radius_location_ < 0 ||
             glass_background_texture_location_ < 0 || glass_reflection_texture_location_ < 0 ||
             glass_target_size_location_ < 0 || glass_draw_origin_location_ < 0 ||
             glass_has_clip_location_ < 0 || glass_clip_rect_location_ < 0 || glass_clip_radius_location_ < 0 ||
             glass_quality_location_ < 0 || glass_refraction_location_ < 0 || glass_dispersion_location_ < 0 ||
             glass_frost_location_ < 0 || glass_time_location_ < 0 || glass_tint_location_ < 0 || glass_ior_location_ < 0 ||
             glass_thickness_location_ < 0 || glass_roughness_location_ < 0 || glass_opacity_location_ < 0 ||
           kawase_source_location_ < 0 || kawase_texel_size_location_ < 0 || kawase_offset_location_ < 0 || kawase_upsample_location_ < 0 ||
           kawase_finalize_location_ < 0 || kawase_brightness_location_ < 0 || kawase_contrast_location_ < 0 ||
           kawase_saturation_location_ < 0 || kawase_noise_location_ < 0) {
    last_error_ = "could not allocate OpenGL renderer resources";
    shutdown();
    return false;
  }

  constexpr std::array<float, 12> vertices{
      0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
      0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F,
  };
  glBindVertexArray(vertex_array_);
  glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  last_error_.clear();
  return true;
}

bool OpenGlRenderer::prepare_shaders(const ShaderSources& sources) {
  discard_shaders();
  if (!ready() || sources.programs.empty() || sources.window.empty() || sources.border.empty()) {
    last_error_ = "named window and border shaders are required";
    return false;
  }
  for (const auto& source : sources.programs) {
    if (source.name.empty() || pending_custom_programs_.contains(source.name)) {
      last_error_ = "shader names must be nonempty and unique";
      discard_shaders();
      return false;
    }
    const GLuint program = link_program(shader::surface_vertex, source.source.c_str(), last_error_);
    if (program == 0) { discard_shaders(); return false; }
    for (const auto& [name, value] : source.values) {
      const std::string uniform = "zwwm_config_" + name;
      const auto type = uniform_type(program, uniform);
      if (!type && pipeline_shader_value(source.role, name)) continue;
      if (!type || !compatible_value(*type, value)) {
        last_error_ = "shader " + source.name + " has no compatible active uniform " + uniform;
        glDeleteProgram(program);
        discard_shaders();
        return false;
      }
    }
    pending_custom_programs_.emplace(source.name,
        CustomProgram{program, source.role, source.values});
  }
  const auto valid_default = [&](const std::string& name, ShaderRole role) {
    const auto found = pending_custom_programs_.find(name);
    return found != pending_custom_programs_.end() && found->second.role == role;
  };
  if (!valid_default(sources.window, ShaderRole::window) ||
      !valid_default(sources.border, ShaderRole::border) ||
      (!sources.background.empty() && !valid_default(sources.background, ShaderRole::background))) {
    last_error_ = "default shader names must resolve to programs with matching roles";
    discard_shaders();
    return false;
  }
  pending_wallpaper_texture_ = load_png_texture(default_wallpaper_path(), last_error_,
                                               &pending_wallpaper_width_, &pending_wallpaper_height_);
  if (pending_wallpaper_texture_ == 0) {
    discard_shaders();
    return false;
  }
  pending_window_shader_ = sources.window;
  pending_border_shader_ = sources.border;
  pending_background_shader_ = sources.background;
  last_error_.clear();
  return true;
}

void OpenGlRenderer::commit_shaders() {
  if (pending_custom_programs_.empty()) return;
  for (const auto& [name, program] : custom_programs_) {
    (void)name;
    if (program.program != 0) glDeleteProgram(program.program);
  }
  custom_programs_ = std::move(pending_custom_programs_);
  window_shader_ = std::move(pending_window_shader_);
  border_shader_ = std::move(pending_border_shader_);
  background_shader_ = std::move(pending_background_shader_);
  if (wallpaper_texture_ != 0) glDeleteTextures(1, &wallpaper_texture_);
  wallpaper_texture_ = std::exchange(pending_wallpaper_texture_, 0);
  wallpaper_width_ = std::exchange(pending_wallpaper_width_, 0);
  wallpaper_height_ = std::exchange(pending_wallpaper_height_, 0);
}

void OpenGlRenderer::discard_shaders() {
  for (const auto& [name, program] : pending_custom_programs_) {
    (void)name;
    if (program.program != 0) glDeleteProgram(program.program);
  }
  pending_custom_programs_.clear();
  pending_window_shader_.clear();
  pending_border_shader_.clear();
  pending_background_shader_.clear();
  if (pending_wallpaper_texture_ != 0) {
    glDeleteTextures(1, &pending_wallpaper_texture_);
    pending_wallpaper_texture_ = 0;
  }
  pending_wallpaper_width_ = pending_wallpaper_height_ = 0;
}

void OpenGlRenderer::shutdown() {
  discard_shaders();
  for (const auto& [name, program] : custom_programs_) {
    (void)name;
    if (program.program != 0) glDeleteProgram(program.program);
  }
  custom_programs_.clear();
  if (wallpaper_texture_ != 0) {
    glDeleteTextures(1, &wallpaper_texture_);
    wallpaper_texture_ = 0;
  }
  wallpaper_width_ = wallpaper_height_ = 0;
  if (!kawase_textures_.empty()) {
    glDeleteTextures(static_cast<GLsizei>(kawase_textures_.size()), kawase_textures_.data());
    kawase_textures_.clear();
  }
  if (kawase_framebuffer_ != 0) { glDeleteFramebuffers(1, &kawase_framebuffer_); kawase_framebuffer_ = 0; }
  if (background_texture_ != 0) {
    glDeleteTextures(1, &background_texture_);
    background_texture_ = 0;
  }
  if (reflection_texture_ != 0) {
    glDeleteTextures(1, &reflection_texture_);
    reflection_texture_ = 0;
  }
  if (vertex_buffer_ != 0) {
    glDeleteBuffers(1, &vertex_buffer_);
    vertex_buffer_ = 0;
  }
  if (vertex_array_ != 0) {
    glDeleteVertexArrays(1, &vertex_array_);
    vertex_array_ = 0;
  }
  if (program_ != 0) {
    glDeleteProgram(program_);
    program_ = 0;
  }
  if (glass_program_ != 0) { glDeleteProgram(glass_program_); glass_program_ = 0; }
  if (kawase_program_ != 0) { glDeleteProgram(kawase_program_); kawase_program_ = 0; }
  rect_location_ = -1;
  rect_size_location_ = -1;
  corner_radius_location_ = -1;
  border_width_location_ = -1;
  color_location_ = -1;
  surface_texture_location_ = -1;
  has_surface_texture_location_ = -1;
  source_uv_location_ = -1;
  texture_rect_location_ = -1;
  has_texture_rect_location_ = -1;
  texture_corner_radius_location_ = -1;
  draw_origin_location_ = -1;
  clip_rect_location_ = -1;
  clip_radius_location_ = -1;
  has_clip_location_ = -1;
  input_to_clip_row_0_location_ = -1;
  input_to_clip_row_1_location_ = -1;
  texture_transform_location_ = -1;
  background_texture_location_ = -1;
  target_size_location_ = -1;
  background_only_location_ = -1;
  background_mask_alpha_threshold_location_ = -1;
  glass_rect_location_ = -1;
  glass_rect_size_location_ = -1;
  glass_corner_radius_location_ = -1;
  glass_background_texture_location_ = -1;
  glass_reflection_texture_location_ = -1;
  glass_target_size_location_ = -1;
  glass_draw_origin_location_ = -1;
  glass_has_clip_location_ = -1;
  glass_clip_rect_location_ = -1;
  glass_clip_radius_location_ = -1;
  glass_quality_location_ = -1;
  glass_refraction_location_ = -1;
  glass_dispersion_location_ = -1;
  glass_frost_location_ = -1;
  glass_time_location_ = -1;
  glass_tint_location_ = -1;
  glass_ior_location_ = -1;
  glass_thickness_location_ = -1;
  glass_roughness_location_ = -1;
  glass_opacity_location_ = -1;
  kawase_source_location_ = -1;
  kawase_texel_size_location_ = -1;
  kawase_offset_location_ = -1;
  kawase_upsample_location_ = -1;
  kawase_finalize_location_ = -1;
  kawase_brightness_location_ = -1;
  kawase_contrast_location_ = -1;
  kawase_saturation_location_ = -1;
  kawase_noise_location_ = -1;
  textures_.clear();
}

bool OpenGlRenderer::ready() const {
  return program_ != 0 && glass_program_ != 0 && kawase_program_ != 0 && vertex_array_ != 0 && vertex_buffer_ != 0;
}

const std::string& OpenGlRenderer::last_error() const { return last_error_; }

bool OpenGlRenderer::bind_texture(TextureHandle handle, GLuint texture) {
  if (handle == 0 || texture == 0) {
    return false;
  }
  textures_[handle] = texture;
  return true;
}

void OpenGlRenderer::unbind_texture(TextureHandle handle) { textures_.erase(handle); }

bool OpenGlRenderer::render(const FramePlan& frame, Size target_size) const {
  if (!ready()) {
    return false;
  }
  if (target_size.width == 0 || target_size.height == 0) {
    return false;
  }
  if (std::any_of(frame.draws.begin(), frame.draws.end(), [](const DrawCall& draw) {
         return draw.bounds.empty() || !std::isfinite(draw.corner_radius) || draw.corner_radius < 0.0F ||
                 !std::isfinite(draw.border_width) || draw.border_width < 0.0F ||
                 !std::all_of(draw.source_uv.begin(), draw.source_uv.end(), [](float value) { return std::isfinite(value); }) ||
                 draw.source_uv[0] < 0.0F || draw.source_uv[1] < 0.0F ||
                 draw.source_uv[2] > 1.0F || draw.source_uv[3] > 1.0F ||
                 draw.source_uv[0] >= draw.source_uv[2] || draw.source_uv[1] >= draw.source_uv[3] ||
                  !std::isfinite(draw.clip_radius) || draw.clip_radius < 0.0F ||
                  !std::isfinite(draw.background_blur_radius) || draw.background_blur_radius < 0.0F;
      })) {
    return false;
  }

  auto prepare_blur = [&](const DrawCall& draw) {
    GLint previous_read = 0, previous_draw = 0, previous_viewport[4]{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    if (background_texture_ == 0) glGenTextures(1, &background_texture_);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, background_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, static_cast<GLsizei>(target_size.width),
                     static_cast<GLsizei>(target_size.height), 0);
    if (draw.glass) {
      if (reflection_texture_ == 0) glGenTextures(1, &reflection_texture_);
      glBindTexture(GL_TEXTURE_2D, reflection_texture_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, static_cast<GLsizei>(target_size.width),
                       static_cast<GLsizei>(target_size.height), 0);
      glBindTexture(GL_TEXTURE_2D, background_texture_);
    }

    const std::size_t requested = std::clamp<std::size_t>(draw.background_blur_passes, 1, 8);
    const float base_offset = std::clamp(draw.background_blur_radius / (2.0F * static_cast<float>(requested)), 0.5F, 8.0F);
    std::size_t iterations = 0;
    std::uint32_t width = target_size.width, height = target_size.height;
    std::vector<Size> sizes;
    while (iterations < requested && (width > 1 || height > 1)) {
      width = std::max(1U, width / 2U); height = std::max(1U, height / 2U);
      sizes.push_back({width, height}); ++iterations;
    }
    while (kawase_textures_.size() < iterations) {
      GLuint texture = 0; glGenTextures(1, &texture); kawase_textures_.push_back(texture);
    }
    if (kawase_framebuffer_ == 0) glGenFramebuffers(1, &kawase_framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, kawase_framebuffer_);
    glUseProgram(kawase_program_);
    glBindVertexArray(vertex_array_);
    glDisable(GL_BLEND);
    glUniform1i(kawase_source_location_, 0);

    GLuint source = background_texture_;
    Size source_size = target_size;
    bool complete = true;
    for (std::size_t index = 0; index < iterations; ++index) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, kawase_textures_[index]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(sizes[index].width),
                   static_cast<GLsizei>(sizes[index].height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, kawase_textures_[index], 0);
      complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
      glViewport(0, 0, static_cast<GLsizei>(sizes[index].width), static_cast<GLsizei>(sizes[index].height));
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, source);
      glUniform2f(kawase_texel_size_location_, 1.0F / source_size.width, 1.0F / source_size.height);
      glUniform1f(kawase_offset_location_, base_offset + static_cast<float>(index) * 0.5F);
      glUniform1i(kawase_upsample_location_, GL_FALSE);
      glUniform1i(kawase_finalize_location_, GL_FALSE);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      source = kawase_textures_[index]; source_size = sizes[index];
    }
    for (std::size_t index = iterations; index > 0; --index) {
      const std::size_t level = index - 1;
      const GLuint target = level == 0 ? background_texture_ : kawase_textures_[level - 1];
      const Size target_dimensions = level == 0 ? target_size : sizes[level - 1];
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
      complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
      glViewport(0, 0, static_cast<GLsizei>(target_dimensions.width), static_cast<GLsizei>(target_dimensions.height));
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, kawase_textures_[level]);
      glUniform2f(kawase_texel_size_location_, 1.0F / sizes[level].width, 1.0F / sizes[level].height);
      glUniform1f(kawase_offset_location_, base_offset * 0.5F + static_cast<float>(level) * 0.5F);
      glUniform1i(kawase_upsample_location_, GL_TRUE);
      glUniform1i(kawase_finalize_location_, level == 0 ? GL_TRUE : GL_FALSE);
      glUniform1f(kawase_brightness_location_, draw.background_blur_brightness);
      glUniform1f(kawase_contrast_location_, draw.background_blur_contrast);
      glUniform1f(kawase_saturation_location_, draw.background_blur_saturation);
      glUniform1f(kawase_noise_location_, draw.background_blur_noise);
      glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    glUseProgram(program_); glBindVertexArray(vertex_array_); glEnable(GL_BLEND);
    return complete;
  };

  glUseProgram(program_); glBindVertexArray(vertex_array_); glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUniform1i(background_texture_location_, 1);
  glUniform2f(target_size_location_, static_cast<float>(target_size.width), static_cast<float>(target_size.height));
  if (frame.draw_background && wallpaper_texture_ != 0 &&
      wallpaper_width_ != 0 && wallpaper_height_ != 0) {
    const float output_aspect = static_cast<float>(target_size.width) / target_size.height;
    const float image_aspect = static_cast<float>(wallpaper_width_) / wallpaper_height_;
    float left = 0.0F, top = 0.0F, right = 1.0F, bottom = 1.0F;
    if (output_aspect > image_aspect) {
      const float visible = image_aspect / output_aspect;
      top = (1.0F - visible) * 0.5F;
      bottom = 1.0F - top;
    } else {
      const float visible = output_aspect / image_aspect;
      left = (1.0F - visible) * 0.5F;
      right = 1.0F - left;
    }
    glDisable(GL_BLEND);
    glUniform4f(rect_location_, -1.0F, 1.0F, 2.0F, -2.0F);
    glUniform2f(rect_size_location_, static_cast<float>(target_size.width),
                static_cast<float>(target_size.height));
    glUniform4f(color_location_, 1.0F, 1.0F, 1.0F, 1.0F);
    glUniform1f(corner_radius_location_, 0.0F);
    glUniform1f(border_width_location_, 0.0F);
    glUniform1i(background_only_location_, GL_FALSE);
    glUniform1i(has_surface_texture_location_, GL_TRUE);
    glUniform1i(has_texture_rect_location_, GL_FALSE);
    glUniform1i(has_clip_location_, GL_FALSE);
    glUniform4f(source_uv_location_, left, top, right, bottom);
    glUniform2f(draw_origin_location_, 0.0F, 0.0F);
    glUniform1i(texture_transform_location_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, wallpaper_texture_);
    glUniform1i(surface_texture_location_, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  const auto custom_program = [&](const std::string& requested, const std::string& fallback,
                                  ShaderRole role) -> const CustomProgram* {
    const auto selected = custom_programs_.find(requested.empty() ? fallback : requested);
    if (selected == custom_programs_.end() || selected->second.role != role) return nullptr;
    return &selected->second;
  };
  const auto* background_program = custom_program({}, background_shader_, ShaderRole::background);
  if (frame.draw_background && background_program != nullptr) {
    glUseProgram(background_program->program);
    glEnable(GL_BLEND);
    uniform_4f(background_program->program, "rect", -1.0F, 1.0F, 2.0F, -2.0F);
    uniform_2f(background_program->program, "rect_size", static_cast<float>(target_size.width),
               static_cast<float>(target_size.height));
    uniform_4f(background_program->program, "source_uv", 0.0F, 0.0F, 1.0F, 1.0F);
    uniform_2f(background_program->program, "draw_origin", 0.0F, 0.0F);
    uniform_1i(background_program->program, "texture_transform", 0);
    uniform_2f(background_program->program, "zwwm_output_size", static_cast<float>(target_size.width),
               static_cast<float>(target_size.height));
    uniform_1f(background_program->program, "zwwm_time", frame.shader_time);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, wallpaper_texture_);
    uniform_1i(background_program->program, "zwwm_background_image", 0);
    apply_shader_values(background_program->program, background_program->values);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(program_);
  }
  const auto draw_surface = [&](const RenderDraw& draw, bool background_only) {
    const bool blend = background_only || !draw.opaque || draw.opacity < 1.0F || draw.corner_radius > 0.0F ||
                       draw.border_width > 0.0F || draw.clip_bounds.has_value();
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    const float left = (2.0F * static_cast<float>(draw.bounds.origin.x) / target_size.width) - 1.0F;
    const float top = 1.0F - (2.0F * static_cast<float>(draw.bounds.origin.y) / target_size.height);
    const float width = 2.0F * static_cast<float>(draw.bounds.size.width) / target_size.width;
    const float height = -2.0F * static_cast<float>(draw.bounds.size.height) / target_size.height;
    const float pixel_width = static_cast<float>(draw.bounds.size.width);
    const float pixel_height = static_cast<float>(draw.bounds.size.height);
    const float corner_radius = std::min(draw.corner_radius, 0.5F * std::min(pixel_width, pixel_height));
    glUniform4f(rect_location_, left, top, width, height);
    glUniform2f(rect_size_location_, pixel_width, pixel_height);
    glUniform1f(corner_radius_location_, corner_radius);
    glUniform1f(border_width_location_, background_only ? 0.0F : std::min(draw.border_width, 0.5F * std::min(pixel_width, pixel_height)));
    glUniform4f(color_location_, background_only ? 1.0F : draw.color[0], background_only ? 1.0F : draw.color[1],
                background_only ? 1.0F : draw.color[2],
                background_only ? draw.opacity : draw.color[3] * draw.opacity);
    glUniform1i(background_only_location_, background_only ? GL_TRUE : GL_FALSE);
    glUniform1f(background_mask_alpha_threshold_location_, draw.background_mask_alpha_threshold);
    const bool sample_surface = draw.texture.has_value() && (!background_only || draw.background_mask_texture);
    glUniform1i(has_surface_texture_location_, sample_surface ? GL_TRUE : GL_FALSE);
    glUniform4f(source_uv_location_, draw.source_uv[0], draw.source_uv[1], draw.source_uv[2], draw.source_uv[3]);
    glUniform1i(texture_transform_location_, sample_surface ? static_cast<GLint>(draw.texture_transform) : 0);
    glUniform1i(has_texture_rect_location_, draw.texture_bounds.has_value() ? GL_TRUE : GL_FALSE);
    glUniform1f(texture_corner_radius_location_, draw.texture_corner_radius);
    if (draw.texture_bounds) {
      glUniform4f(texture_rect_location_, static_cast<float>(draw.texture_bounds->origin.x),
                  static_cast<float>(draw.texture_bounds->origin.y), static_cast<float>(draw.texture_bounds->size.width),
                  static_cast<float>(draw.texture_bounds->size.height));
    }
    if (background_only) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, background_texture_);
      glActiveTexture(GL_TEXTURE0);
    }
    glUniform2f(draw_origin_location_, static_cast<float>(draw.bounds.origin.x), static_cast<float>(draw.bounds.origin.y));
    glUniform1i(has_clip_location_, draw.clip_bounds.has_value() ? GL_TRUE : GL_FALSE);
    if (draw.clip_bounds.has_value()) {
      glUniform4f(clip_rect_location_, static_cast<float>(draw.clip_bounds->origin.x), static_cast<float>(draw.clip_bounds->origin.y),
                  static_cast<float>(draw.clip_bounds->size.width), static_cast<float>(draw.clip_bounds->size.height));
      glUniform1f(clip_radius_location_, draw.clip_radius);
      const float clip_width = static_cast<float>(std::max(1.0, draw.clip_bounds->size.width));
      const float clip_height = static_cast<float>(std::max(1.0, draw.clip_bounds->size.height));
      glUniform3f(input_to_clip_row_0_location_, pixel_width / clip_width, 0.0F,
                  (static_cast<float>(draw.bounds.origin.x - draw.clip_bounds->origin.x)) / clip_width);
      glUniform3f(input_to_clip_row_1_location_, 0.0F, pixel_height / clip_height,
                  (static_cast<float>(draw.bounds.origin.y - draw.clip_bounds->origin.y)) / clip_height);
    } else {
      glUniform3f(input_to_clip_row_0_location_, 1.0F, 0.0F, 0.0F);
      glUniform3f(input_to_clip_row_1_location_, 0.0F, 1.0F, 0.0F);
    }
    if (sample_surface) {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, *draw.texture);
      glUniform1i(surface_texture_location_, 0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);
  };
  const auto draw_glass = [&](const RenderDraw& draw) {
    glUseProgram(glass_program_);
    glEnable(GL_BLEND);
    const float left = (2.0F * static_cast<float>(draw.bounds.origin.x) / target_size.width) - 1.0F;
    const float top = 1.0F - (2.0F * static_cast<float>(draw.bounds.origin.y) / target_size.height);
    const float width = 2.0F * static_cast<float>(draw.bounds.size.width) / target_size.width;
    const float height = -2.0F * static_cast<float>(draw.bounds.size.height) / target_size.height;
    const float pixel_width = static_cast<float>(draw.bounds.size.width);
    const float pixel_height = static_cast<float>(draw.bounds.size.height);
    const FloatRect clip = draw.clip_bounds.value_or(draw.bounds);
    glUniform4f(glass_rect_location_, left, top, width, height);
    glUniform2f(glass_rect_size_location_, pixel_width, pixel_height);
    glUniform1f(glass_corner_radius_location_, std::min(draw.corner_radius, 0.5F * std::min(pixel_width, pixel_height)));
    glUniform2f(glass_draw_origin_location_, static_cast<float>(draw.bounds.origin.x), static_cast<float>(draw.bounds.origin.y));
    glUniform1i(glass_has_clip_location_, draw.clip_bounds.has_value() ? GL_TRUE : GL_FALSE);
    glUniform4f(glass_clip_rect_location_, static_cast<float>(clip.origin.x), static_cast<float>(clip.origin.y),
                static_cast<float>(clip.size.width), static_cast<float>(clip.size.height));
    glUniform1f(glass_clip_radius_location_, draw.clip_radius);
    glUniform2f(glass_target_size_location_, static_cast<float>(target_size.width), static_cast<float>(target_size.height));
    glUniform1i(glass_quality_location_, static_cast<GLint>(draw.glass_quality));
    glUniform1f(glass_refraction_location_, draw.glass_refraction);
    glUniform1f(glass_dispersion_location_, draw.glass_dispersion);
    glUniform1f(glass_frost_location_, draw.glass_frost);
    glUniform1f(glass_time_location_, draw.glass_time);
    glUniform4f(glass_tint_location_, draw.glass_tint[0], draw.glass_tint[1], draw.glass_tint[2], draw.glass_tint[3]);
    glUniform1f(glass_ior_location_, draw.glass_ior);
    glUniform1f(glass_thickness_location_, draw.glass_thickness);
    glUniform1f(glass_roughness_location_, draw.glass_roughness);
    glUniform1f(glass_opacity_location_, draw.opacity);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, background_texture_);
    glUniform1i(glass_background_texture_location_, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, reflection_texture_);
    glUniform1i(glass_reflection_texture_location_, 2);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUseProgram(program_);
    glUniform1i(background_texture_location_, 1);
    glUniform2f(target_size_location_, static_cast<float>(target_size.width), static_cast<float>(target_size.height));
  };
  const auto draw_custom = [&](const RenderDraw& draw, const CustomProgram& selected,
                               bool blurred_backdrop_ready) {
    const GLuint custom_program = selected.program;
    const FloatRect toplevel = draw.toplevel_bounds.empty() ? draw.bounds : draw.toplevel_bounds;
    const float left = (2.0F * static_cast<float>(draw.bounds.origin.x) / target_size.width) - 1.0F;
    const float top = 1.0F - (2.0F * static_cast<float>(draw.bounds.origin.y) / target_size.height);
    const float width = 2.0F * static_cast<float>(draw.bounds.size.width) / target_size.width;
    const float height = -2.0F * static_cast<float>(draw.bounds.size.height) / target_size.height;
    glUseProgram(custom_program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    uniform_4f(custom_program, "rect", left, top, width, height);
    uniform_2f(custom_program, "rect_size", static_cast<float>(draw.bounds.size.width),
               static_cast<float>(draw.bounds.size.height));
    uniform_4f(custom_program, "source_uv", draw.source_uv[0], draw.source_uv[1],
               draw.source_uv[2], draw.source_uv[3]);
    uniform_2f(custom_program, "draw_origin", static_cast<float>(draw.bounds.origin.x),
               static_cast<float>(draw.bounds.origin.y));
    uniform_1i(custom_program, "texture_transform", static_cast<GLint>(draw.texture_transform));
    uniform_2f(custom_program, "zwwm_output_size", static_cast<float>(target_size.width),
               static_cast<float>(target_size.height));
    uniform_4f(custom_program, "zwwm_toplevel_rect", static_cast<float>(toplevel.origin.x),
               static_cast<float>(toplevel.origin.y), static_cast<float>(toplevel.size.width),
               static_cast<float>(toplevel.size.height));
    uniform_4f(custom_program, "zwwm_draw_rect", static_cast<float>(draw.bounds.origin.x),
               static_cast<float>(draw.bounds.origin.y), static_cast<float>(draw.bounds.size.width),
               static_cast<float>(draw.bounds.size.height));
    uniform_4f(custom_program, "zwwm_color", draw.color[0], draw.color[1], draw.color[2], draw.color[3]);
    uniform_1f(custom_program, "zwwm_opacity", draw.opacity);
    uniform_1f(custom_program, "zwwm_corner_radius", draw.corner_radius);
    uniform_1f(custom_program, "zwwm_border_width", draw.border_width);
    uniform_1f(custom_program, "zwwm_clip_radius", draw.clip_radius);
    const FloatRect clip = draw.clip_bounds.value_or(draw.bounds);
    const FloatRect texture_rect = draw.texture_bounds.value_or(draw.bounds);
    uniform_4f(custom_program, "zwwm_clip_rect", static_cast<float>(clip.origin.x),
               static_cast<float>(clip.origin.y), static_cast<float>(clip.size.width),
               static_cast<float>(clip.size.height));
    uniform_4f(custom_program, "zwwm_texture_rect", static_cast<float>(texture_rect.origin.x),
               static_cast<float>(texture_rect.origin.y), static_cast<float>(texture_rect.size.width),
               static_cast<float>(texture_rect.size.height));
    uniform_1f(custom_program, "zwwm_time", draw.shader_time);
    uniform_1i(custom_program, "zwwm_state", static_cast<GLint>(draw.toplevel_state));
    uniform_1i(custom_program, "zwwm_has_texture", draw.texture.has_value() ? GL_TRUE : GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, draw.texture.value_or(0));
    uniform_1i(custom_program, "zwwm_window_texture", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurred_backdrop_ready ? background_texture_ : reflection_texture_);
    uniform_1i(custom_program, "zwwm_blurred_backdrop_texture", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, reflection_texture_);
    uniform_1i(custom_program, "zwwm_backdrop_texture", 2);
    apply_shader_values(custom_program, selected.values);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUseProgram(program_);
    glUniform1i(background_texture_location_, 1);
    glUniform2f(target_size_location_, static_cast<float>(target_size.width), static_cast<float>(target_size.height));
  };
  NodeId backdrop_toplevel = 0;
  bool custom_toplevel_blur_ready = false;
  for (const DrawCall& item : frame.draws) {
    const RenderDraw draw(item);
    const auto* selected_window = draw.shader_role == ShaderRole::window ?
        custom_program(draw.shader_name, window_shader_, ShaderRole::window) : nullptr;
    const auto* selected_border = draw.shader_role == ShaderRole::border ?
        custom_program(draw.shader_name, border_shader_, ShaderRole::border) : nullptr;
    const bool custom_window = selected_window != nullptr;
    if (custom_window &&
        draw.toplevel_id != 0 && draw.toplevel_id != backdrop_toplevel) {
      backdrop_toplevel = draw.toplevel_id;
      if (reflection_texture_ == 0) glGenTextures(1, &reflection_texture_);
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, reflection_texture_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, static_cast<GLsizei>(target_size.width),
                       static_cast<GLsizei>(target_size.height), 0);
      glActiveTexture(GL_TEXTURE0);
      custom_toplevel_blur_ready = draw.background_blur_radius > 0.0F && prepare_blur(draw);
    }
    bool blurred_backdrop_ready = custom_window ? custom_toplevel_blur_ready : false;
    if (!custom_window && (draw.background_blur_radius > 0.0F || draw.glass)) {
      blurred_backdrop_ready = prepare_blur(draw);
      if (blurred_backdrop_ready && !custom_window) {
        glUniform1i(background_texture_location_, 1);
        glUniform2f(target_size_location_, static_cast<float>(target_size.width), static_cast<float>(target_size.height));
        if (draw.glass) draw_glass(draw); else draw_surface(draw, true);
      }
    }
    if (custom_window)
      draw_custom(draw, *selected_window, blurred_backdrop_ready);
    else if (selected_border != nullptr)
      draw_custom(draw, *selected_border, false);
    else
      draw_surface(draw, false);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_BLEND);
  glBindVertexArray(0);
  glUseProgram(0);
  return true;
}

bool OpenGlRenderer::render_pixels(const FramePlan& frame, Size target_size,
                                   std::vector<std::uint32_t>* pixels) const {
  if (pixels == nullptr || target_size.width == 0 || target_size.height == 0) return false;
  GLint previous_read = 0, previous_draw = 0, previous_viewport[4]{};
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
  glGetIntegerv(GL_VIEWPORT, previous_viewport);
  GLuint texture = 0, framebuffer = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(target_size.width),
               static_cast<GLsizei>(target_size.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  bool success = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (success) {
    glViewport(0, 0, static_cast<GLsizei>(target_size.width), static_cast<GLsizei>(target_size.height));
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    success = render(frame, target_size);
  }
  if (success) {
    pixels->resize(static_cast<std::size_t>(target_size.width) * target_size.height);
    glReadPixels(0, 0, static_cast<GLsizei>(target_size.width), static_cast<GLsizei>(target_size.height),
                 GL_BGRA, GL_UNSIGNED_BYTE, pixels->data());
    success = glGetError() == GL_NO_ERROR;
    if (success) for (std::uint32_t y = 0; y < target_size.height / 2; ++y) {
      auto top = pixels->begin() + static_cast<std::ptrdiff_t>(y * target_size.width);
      auto bottom = pixels->begin() + static_cast<std::ptrdiff_t>((target_size.height - y - 1) * target_size.width);
      std::swap_ranges(top, top + target_size.width, bottom);
    }
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
  glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteTextures(1, &texture);
  if (!success) pixels->clear();
  return success;
}

FramePlan OpenGlRenderer::build_frame(const Scene& scene, const DamageRegion& damage) const {
  FramePlan frame{damage.rectangles(), {}};
  if (damage.empty()) {
    return frame;
  }
  for (const SceneNode& node : scene.ordered_nodes()) {
    if (node.visible && node.opacity > 0.0F && damage.intersects(node.bounds)) {
      std::optional<GLuint> texture;
      if (node.texture.has_value()) {
        const auto registered = textures_.find(*node.texture);
        if (registered != textures_.end()) {
          texture = registered->second;
        }
      }
      const float maximum_radius = 0.5F * static_cast<float>(std::min(node.bounds.size.width, node.bounds.size.height));
      frame.draws.push_back({node.id, node.bounds, node.opacity, node.color, texture,
                             std::min(node.corner_radius, maximum_radius)});
    }
  }
  return frame;
}

}  // namespace zwwm::renderer
