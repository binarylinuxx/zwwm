find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.vert"
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.frag"
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/kawase.vert"
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/kawase.frag"
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glass.frag"
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glass_physics.glsl"
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rounded_rect.glsl"
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rounded_rect.glsl" ZWWM_ROUNDED_RECT_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glass_physics.glsl" ZWWM_GLASS_PHYSICS_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.vert" ZWWM_SURFACE_VERTEX_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.frag" ZWWM_SURFACE_FRAGMENT_SHADER)
string(REPLACE "#include \"rounded_rect.glsl\"" "${ZWWM_ROUNDED_RECT_SHADER}" ZWWM_SURFACE_FRAGMENT_SHADER "${ZWWM_SURFACE_FRAGMENT_SHADER}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/kawase.vert" ZWWM_KAWASE_VERTEX_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/kawase.frag" ZWWM_KAWASE_FRAGMENT_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glass.frag" ZWWM_GLASS_FRAGMENT_SHADER)
string(REPLACE "#include \"rounded_rect.glsl\"" "${ZWWM_ROUNDED_RECT_SHADER}" ZWWM_GLASS_FRAGMENT_SHADER "${ZWWM_GLASS_FRAGMENT_SHADER}")
string(REPLACE "#include \"glass_physics.glsl\"" "${ZWWM_GLASS_PHYSICS_SHADER}" ZWWM_GLASS_FRAGMENT_SHADER "${ZWWM_GLASS_FRAGMENT_SHADER}")
configure_file(src/shaders.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/zwwm/renderer/shaders.hpp" @ONLY)

add_library(zwwm_renderer STATIC
  src/dmabuf.cpp
  src/egl_dmabuf_importer.cpp
  src/scene.cpp
  src/opengl.cpp
)

target_include_directories(zwwm_renderer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)
target_include_directories(zwwm_renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/zwwm/renderer")
target_link_libraries(zwwm_renderer PUBLIC PkgConfig::EGL PkgConfig::EPOXY PkgConfig::LIBDRM PkgConfig::PNG)
target_compile_definitions(zwwm_renderer PRIVATE ZWWM_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/zwwm")
target_compile_options(zwwm_renderer PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::renderer ALIAS zwwm_renderer)
