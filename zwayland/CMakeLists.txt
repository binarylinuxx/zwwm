# Modernized Wayland runtime (zwayland)
#
# This is a from-scratch, wire-compatible reimplementation of the Wayland
# server and client runtimes. It builds standalone as libzwayland-server and
# libzwayland-client around a shared C++23 wire codec. zwayland-scanner emits
# the typed C++ metadata and dispatch helpers consumed by both runtimes; zwwm's
# migration from libwayland onto these libraries is tracked separately.

set(ZWAYLAND_CORE_XML
    "${PROJECT_SOURCE_DIR}/zwwm-protocols/protocols/upstream/wayland/wayland.xml")
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
