find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core DBus Gui Widgets)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SPA REQUIRED IMPORTED_TARGET libspa-0.2)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(XDG_DESKTOP_PORTAL REQUIRED xdg-desktop-portal)
pkg_get_variable(XDG_DESKTOP_PORTAL_INTERFACES_DIR xdg-desktop-portal interfaces_dir)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${XDG_DESKTOP_PORTAL_INTERFACES_DIR}/org.freedesktop.impl.portal.Screenshot.xml"
  "${XDG_DESKTOP_PORTAL_INTERFACES_DIR}/org.freedesktop.impl.portal.Request.xml"
  "${XDG_DESKTOP_PORTAL_INTERFACES_DIR}/org.freedesktop.impl.portal.ScreenCast.xml"
  "${XDG_DESKTOP_PORTAL_INTERFACES_DIR}/org.freedesktop.impl.portal.Session.xml"
  "${XDG_DESKTOP_PORTAL_INTERFACES_DIR}/org.freedesktop.impl.portal.Access.xml"
  "${XDG_DESKTOP_PORTAL_INTERFACES_DIR}/org.freedesktop.impl.portal.FileChooser.xml"
  "${XDG_DESKTOP_PORTAL_INTERFACES_DIR}/org.freedesktop.impl.portal.Settings.xml"
)

add_executable(xdg-desktop-portal-zwwm
  src/main.cpp
  src/access_portal.cpp
  src/access_portal.hpp
  src/capture_client.cpp
  src/capture_client.hpp
  src/file_chooser_portal.cpp
  src/file_chooser_portal.hpp
  src/image_writer.cpp
  src/image_writer.hpp
  src/screenshot_portal.cpp
  src/screenshot_portal.hpp
  src/screencast_portal.cpp
  src/screencast_portal.hpp
  src/settings_portal.cpp
  src/settings_portal.hpp
  src/pipewire_stream.cpp
  src/pipewire_stream.hpp
  vendor/stb_image_write.h
)
set_target_properties(xdg-desktop-portal-zwwm PROPERTIES AUTOMOC ON)
target_include_directories(xdg-desktop-portal-zwwm SYSTEM PRIVATE vendor)
target_link_libraries(xdg-desktop-portal-zwwm PRIVATE
  PkgConfig::PIPEWIRE
  PkgConfig::SPA
  PkgConfig::LIBDRM
  PkgConfig::GBM
  zwayland-client
  zwwm::protocol_metadata
  Qt6::Core
  Qt6::DBus
  Qt6::Gui
  Qt6::Widgets
)
target_compile_options(xdg-desktop-portal-zwwm PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(GNUInstallDirs)
install(TARGETS xdg-desktop-portal-zwwm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES data/zwwm.portal DESTINATION "${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals")
install(FILES data/zwwm-gtk.portal DESTINATION "${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals")
install(FILES data/zwwm-portals.conf DESTINATION "${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal")
