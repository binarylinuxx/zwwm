cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(zwayland LANGUAGES CXX)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "zwayland requires Clang")
  endif()
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  find_program(ZWAYLAND_SCANNER_EXECUTABLE NAMES zwayland-scanner REQUIRED)
  add_executable(zwayland-scanner IMPORTED)
  set_target_properties(zwayland-scanner PROPERTIES IMPORTED_LOCATION "${ZWAYLAND_SCANNER_EXECUTABLE}")
endif()

# Modernized Wayland runtime (zwayland)
#
# This is a from-scratch, wire-compatible reimplementation of the Wayland
# server and client runtimes. It builds standalone as libzwayland-server and
# libzwayland-client around a shared C++23 wire codec. zwayland-scanner emits
# the typed C++ metadata and dispatch helpers consumed by both runtimes; zwwm's
# migration from libwayland onto these libraries is tracked separately.

set(ZWAYLAND_CORE_XML
    "${CMAKE_CURRENT_SOURCE_DIR}/../zwwm-protocols/protocols/upstream/wayland/wayland.xml"
    CACHE FILEPATH "Wayland core protocol XML used by zwayland-scanner")
set(ZWAYLAND_CORE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ZWAYLAND_CORE_SERVER_HEADER "${ZWAYLAND_CORE_GENERATED_DIR}/wayland-zwayland-server.h")
set(ZWAYLAND_CORE_CLIENT_HEADER "${ZWAYLAND_CORE_GENERATED_DIR}/wayland-zwayland-client.h")
add_custom_command(
  OUTPUT "${ZWAYLAND_CORE_SERVER_HEADER}" "${ZWAYLAND_CORE_CLIENT_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZWAYLAND_CORE_GENERATED_DIR}"
  COMMAND "$<TARGET_FILE:zwayland-scanner>" zwayland-server-header
          "${ZWAYLAND_CORE_XML}" "${ZWAYLAND_CORE_SERVER_HEADER}"
  COMMAND "$<TARGET_FILE:zwayland-scanner>" zwayland-client-header
          "${ZWAYLAND_CORE_XML}" "${ZWAYLAND_CORE_CLIENT_HEADER}"
  DEPENDS "${ZWAYLAND_CORE_XML}" zwayland-scanner
  VERBATIM)
add_custom_target(zwayland-core-protocol
  DEPENDS "${ZWAYLAND_CORE_SERVER_HEADER}" "${ZWAYLAND_CORE_CLIENT_HEADER}")

add_subdirectory(wire)
add_subdirectory(server)
add_subdirectory(client)

add_dependencies(zwayland-server zwayland-core-protocol)
add_dependencies(zwayland-client zwayland-core-protocol)
target_include_directories(zwayland-server PUBLIC
  $<BUILD_INTERFACE:${ZWAYLAND_CORE_GENERATED_DIR}>)
target_include_directories(zwayland-client PUBLIC
  $<BUILD_INTERFACE:${ZWAYLAND_CORE_GENERATED_DIR}>)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../.version" ZWAYLAND_VERSION LIMIT_COUNT 1)

  foreach(component wire client server)
    set_target_properties(zwayland-${component} PROPERTIES
      EXPORT_NAME ${component}
      VERSION "${ZWAYLAND_VERSION}")
    set(ZWAYLAND_PC_NAME "zwayland-${component}")
    set(ZWAYLAND_PC_REQUIRES "")
    if(NOT component STREQUAL "wire")
      set(ZWAYLAND_PC_REQUIRES "zwayland-wire = ${ZWAYLAND_VERSION}")
    endif()
    configure_file(zwayland.pc.in "${ZWAYLAND_PC_NAME}.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${ZWAYLAND_PC_NAME}.pc"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
    install(DIRECTORY "${component}/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  endforeach()
  install(FILES "${ZWAYLAND_CORE_SERVER_HEADER}" "${ZWAYLAND_CORE_CLIENT_HEADER}"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(TARGETS zwayland-wire zwayland-client zwayland-server EXPORT zwaylandTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(EXPORT zwaylandTargets NAMESPACE zwayland::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zwayland")
  configure_package_config_file(zwaylandConfig.cmake.in zwaylandConfig.cmake
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zwayland")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zwaylandConfig.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zwayland")
  install(FILES ../LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/zwayland")
endif()
