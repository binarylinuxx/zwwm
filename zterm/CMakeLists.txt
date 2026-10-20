cmake_minimum_required(VERSION 3.25)
project(zterm LANGUAGES CXX)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "zterm requires Clang")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET zwayland-client libzterm zw-lang pangocairo harfbuzz gbm egl epoxy libdrm xkbcommon)
pkg_check_modules(PROTOCOLS REQUIRED zwwm-protocols)
pkg_get_variable(protocol_dir zwwm-protocols pkgdatadir)
find_program(scanner zwayland-scanner REQUIRED)
set(specs
  "xdg-shell|upstream/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
  "linux-dmabuf|upstream/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml"
  "viewporter|upstream/wayland-protocols/stable/viewporter/viewporter.xml"
  "fractional-scale|upstream/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml"
  "cursor-shape|upstream/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml"
  "ext-zwwm-manager-v1|ext-zwwm-manager-v1.xml"
  "zwwm-client-connection-v1|zwwm-client-connection-v1.xml")
foreach(spec IN LISTS specs)
  string(REPLACE "|" ";" parts "${spec}")
  list(GET parts 0 name)
  list(GET parts 1 xml)
  set(header "${CMAKE_CURRENT_BINARY_DIR}/${name}-zwayland-client.h")
  add_custom_command(OUTPUT "${header}"
    COMMAND "${scanner}" zwayland-client-header "${protocol_dir}/${xml}" "${header}"
    DEPENDS "${protocol_dir}/${xml}" VERBATIM)
  list(APPEND headers "${header}")
endforeach()
add_executable(zterm src/main.cpp src/gpu.cpp src/text.cpp src/clipboard.cpp src/config.cpp src/connection.cpp src/daemon.cpp ${headers})
target_include_directories(zterm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(zterm PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(zterm PRIVATE PkgConfig::DEPS util)
install(TARGETS zterm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES config.zw DESTINATION "${CMAKE_INSTALL_DATADIR}/zterm")
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/zterm")
