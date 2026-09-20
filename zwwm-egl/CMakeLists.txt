find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)

add_library(zwwm-egl src/window.cpp)
add_library(zwwm::egl ALIAS zwwm-egl)

target_include_directories(zwwm-egl PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(zwwm-egl PUBLIC
  zwayland-client
  PkgConfig::EGL
  PkgConfig::GLES)
target_compile_features(zwwm-egl PUBLIC cxx_std_23)
target_compile_options(zwwm-egl PRIVATE -Wall -Wextra -Wpedantic -Werror)
