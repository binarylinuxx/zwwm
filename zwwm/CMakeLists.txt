find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
option(XWAYLAND_ENABLE "Build Xwayland integration" ON)
if(DEFINED XWAYLAND-ENABLE)
  set(XWAYLAND_ENABLE "${XWAYLAND-ENABLE}" CACHE BOOL "Build Xwayland integration" FORCE)
endif()
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(LIBSEAT REQUIRED IMPORTED_TARGET libseat)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(XCURSOR REQUIRED IMPORTED_TARGET xcursor)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
if(XWAYLAND_ENABLE)
  pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
  pkg_check_modules(XCB_COMPOSITE REQUIRED IMPORTED_TARGET xcb-composite)
  pkg_check_modules(XCB_XFIXES REQUIRED IMPORTED_TARGET xcb-xfixes)
endif()

add_library(zwwm_master_stack STATIC src/layout/master_stack.cpp)
target_include_directories(zwwm_master_stack PUBLIC include)
target_link_libraries(zwwm_master_stack PUBLIC zwwm::renderer)
target_compile_options(zwwm_master_stack PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::master_stack ALIAS zwwm_master_stack)

add_library(zwwm_canvas STATIC src/layout/canvas.cpp)
target_include_directories(zwwm_canvas PUBLIC include)
target_compile_options(zwwm_canvas PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::canvas ALIAS zwwm_canvas)

add_library(zwwm_focus_fibonacci STATIC src/layout/focus_fibonacci.cpp)
target_include_directories(zwwm_focus_fibonacci PUBLIC include)
target_link_libraries(zwwm_focus_fibonacci PUBLIC zwwm::master_stack)
target_compile_options(zwwm_focus_fibonacci PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::focus_fibonacci ALIAS zwwm_focus_fibonacci)

add_library(zwwm_animation STATIC src/animation.cpp)
target_include_directories(zwwm_animation PUBLIC include)
target_link_libraries(zwwm_animation PUBLIC zwwm::renderer)
target_compile_options(zwwm_animation PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::animation ALIAS zwwm_animation)

add_library(zwwm_camera STATIC src/camera.cpp)
target_include_directories(zwwm_camera PUBLIC include)
target_compile_options(zwwm_camera PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::camera ALIAS zwwm_camera)

add_library(zwwm_runtime_config STATIC src/runtime_config.cpp)
target_include_directories(zwwm_runtime_config PUBLIC include)
target_link_libraries(zwwm_runtime_config PUBLIC zwayland-server PkgConfig::XKBCOMMON zwwm::lang zwwm::master_stack zwwm::animation)
target_compile_options(zwwm_runtime_config PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(zwwm_runtime_config PRIVATE
  ZWWM_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/zwwm"
)
add_library(zwwm::runtime_config ALIAS zwwm_runtime_config)

add_library(zwwm_compositor STATIC
  src/compositor_server.cpp
  src/compositor_server_buffers.cpp
  src/compositor_server_data_device.cpp
  src/compositor_server_input.cpp
  src/compositor_server_idle.cpp
  src/compositor_server_layer_shell.cpp
  src/compositor_server_session_lock.cpp
  src/compositor_server_surface_extensions.cpp
)
target_include_directories(zwwm_compositor PUBLIC include)
target_link_libraries(zwwm_compositor PUBLIC zwayland-server PkgConfig::XKBCOMMON zwwm::protocol_metadata zwwm::runtime_config zwwm::master_stack zwwm::canvas zwwm::focus_fibonacci zwwm::camera zwwm::renderer)
if(XWAYLAND_ENABLE)
  target_sources(zwwm_compositor PRIVATE
    src/compositor_server_xwayland.cpp
    src/compositor_server_xwayland_selection.cpp
    src/xwayland_spawn.cpp
  )
  target_link_libraries(zwwm_compositor PRIVATE PkgConfig::XCB PkgConfig::XCB_COMPOSITE PkgConfig::XCB_XFIXES)
  target_compile_definitions(zwwm_compositor PRIVATE ZWWM_XWAYLAND=1)
endif()
target_compile_options(zwwm_compositor PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(zwwm src/main.cpp src/error_popup.cpp src/manager_protocol.cpp src/nested_backend.cpp src/runtime_backend.cpp src/portal_capture.cpp)
target_include_directories(zwwm PRIVATE include)
target_link_libraries(zwwm PRIVATE
  zwayland-server
  zwayland-client
  PkgConfig::LIBDRM
  PkgConfig::LIBUDEV
  PkgConfig::LIBSEAT
  PkgConfig::LIBINPUT
  PkgConfig::GBM
  PkgConfig::XKBCOMMON
  PkgConfig::XCURSOR
  PkgConfig::FONTCONFIG
  PkgConfig::FREETYPE
  zwwm_compositor
  zwwm::lang
  zwwm::runtime_config
  zwwm::master_stack
  zwwm::renderer
  zwwm::animation
  zwwm::egl
)
target_compile_options(zwwm PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(zwwm xdg-desktop-portal-zwwm)
target_compile_definitions(zwwm PRIVATE
  ZWWM_PORTAL_EXECUTABLE="xdg-desktop-portal-zwwm"
  ZWWM_VERSION="${ZWWM_VERSION}"
)

install(TARGETS zwwm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS "${CMAKE_SOURCE_DIR}/zwwm-session" DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES data/config.zw DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/zwwm")
install(FILES data/background.png DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/zwwm/background")
install(DIRECTORY data/shaders/ DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/zwwm/shaders")
install(DIRECTORY data/shaders/ DESTINATION "${CMAKE_INSTALL_DATADIR}/zwwm/shaders")
install(FILES data/config.zw DESTINATION "${CMAKE_INSTALL_DATADIR}/zwwm")
install(FILES data/background.png DESTINATION "${CMAKE_INSTALL_DATADIR}/zwwm/background")
install(FILES data/zwwm.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/wayland-sessions")
install(FILES "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/CONFIG.md"
              "${CMAKE_SOURCE_DIR}/CHANGELOG.md" "${CMAKE_SOURCE_DIR}/LICENSE"
              "${CMAKE_SOURCE_DIR}/CONTRIBUTING.md" "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
