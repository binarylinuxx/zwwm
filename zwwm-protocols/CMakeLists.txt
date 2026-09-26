set(ZWWW_PROTOCOL_XML_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/protocols/upstream/wayland-protocols")
set(ZWWW_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

function(zwwm_add_standard_protocol name relative_xml)
  set(xml "${ZWWW_PROTOCOL_XML_DIR}/${relative_xml}")
  set(zw_server "${ZWWW_GENERATED_DIR}/${name}-zwayland-server.h")
  set(zw_client "${ZWWW_GENERATED_DIR}/${name}-zwayland-client.h")

  add_custom_command(
    OUTPUT "${zw_server}" "${zw_client}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZWWW_GENERATED_DIR}"
    COMMAND "$<TARGET_FILE:zwayland-scanner>" zwayland-server-header "${xml}" "${zw_server}"
    COMMAND "$<TARGET_FILE:zwayland-scanner>" zwayland-client-header "${xml}" "${zw_client}"
    DEPENDS "${xml}" zwayland-scanner
    VERBATIM
  )

  set(ZWWW_PROTOCOL_HEADERS "${ZWWW_PROTOCOL_HEADERS};${zw_server};${zw_client}" PARENT_SCOPE)
  set(ZWWW_ZWAYLAND_SERVER_HEADERS "${ZWWW_ZWAYLAND_SERVER_HEADERS};${zw_server}" PARENT_SCOPE)
  set(ZWWW_ZWAYLAND_CLIENT_HEADERS "${ZWWW_ZWAYLAND_CLIENT_HEADERS};${zw_client}" PARENT_SCOPE)
endfunction()

function(zwwm_add_project_protocol name relative_xml)
  set(xml "${CMAKE_CURRENT_SOURCE_DIR}/${relative_xml}")
  set(zw_server "${ZWWW_GENERATED_DIR}/${name}-zwayland-server.h")
  set(zw_client "${ZWWW_GENERATED_DIR}/${name}-zwayland-client.h")

  add_custom_command(
    OUTPUT "${zw_server}" "${zw_client}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZWWW_GENERATED_DIR}"
    COMMAND "$<TARGET_FILE:zwayland-scanner>" zwayland-server-header "${xml}" "${zw_server}"
    COMMAND "$<TARGET_FILE:zwayland-scanner>" zwayland-client-header "${xml}" "${zw_client}"
    DEPENDS "${xml}" zwayland-scanner
    VERBATIM
  )

  set(ZWWW_PROTOCOL_HEADERS "${ZWWW_PROTOCOL_HEADERS};${zw_server};${zw_client}" PARENT_SCOPE)
  set(ZWWW_ZWAYLAND_SERVER_HEADERS "${ZWWW_ZWAYLAND_SERVER_HEADERS};${zw_server}" PARENT_SCOPE)
  set(ZWWW_ZWAYLAND_CLIENT_HEADERS "${ZWWW_ZWAYLAND_CLIENT_HEADERS};${zw_client}" PARENT_SCOPE)
endfunction()

# Shell and surface lifecycle.
zwwm_add_standard_protocol(xdg-shell stable/xdg-shell/xdg-shell.xml)
zwwm_add_standard_protocol(xdg-decoration unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
zwwm_add_standard_protocol(xdg-activation staging/xdg-activation/xdg-activation-v1.xml)
zwwm_add_standard_protocol(xdg-dialog staging/xdg-dialog/xdg-dialog-v1.xml)

# Output, frame scheduling, and buffer transport.
zwwm_add_standard_protocol(viewporter stable/viewporter/viewporter.xml)
zwwm_add_standard_protocol(presentation-time stable/presentation-time/presentation-time.xml)
zwwm_add_standard_protocol(linux-dmabuf stable/linux-dmabuf/linux-dmabuf-v1.xml)
zwwm_add_standard_protocol(fractional-scale staging/fractional-scale/fractional-scale-v1.xml)
zwwm_add_standard_protocol(cursor-shape staging/cursor-shape/cursor-shape-v1.xml)
zwwm_add_standard_protocol(tearing-control staging/tearing-control/tearing-control-v1.xml)
zwwm_add_standard_protocol(explicit-sync unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml)

# Input and clipboard.
zwwm_add_standard_protocol(relative-pointer unstable/relative-pointer/relative-pointer-unstable-v1.xml)
zwwm_add_standard_protocol(pointer-constraints unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)
zwwm_add_standard_protocol(pointer-gestures unstable/pointer-gestures/pointer-gestures-unstable-v1.xml)
zwwm_add_standard_protocol(tablet unstable/tablet/tablet-unstable-v2.xml)
zwwm_add_standard_protocol(text-input-v3 unstable/text-input/text-input-unstable-v3.xml)
zwwm_add_standard_protocol(primary-selection unstable/primary-selection/primary-selection-unstable-v1.xml)
zwwm_add_standard_protocol(ext-idle-notify staging/ext-idle-notify/ext-idle-notify-v1.xml)

# Desktop policy and privileged compositor integration.
zwwm_add_standard_protocol(xdg-output unstable/xdg-output/xdg-output-unstable-v1.xml)
zwwm_add_standard_protocol(idle-inhibit unstable/idle-inhibit/idle-inhibit-unstable-v1.xml)
zwwm_add_standard_protocol(keyboard-shortcuts-inhibit unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml)
zwwm_add_standard_protocol(drm-lease staging/drm-lease/drm-lease-v1.xml)
zwwm_add_standard_protocol(session-lock staging/ext-session-lock/ext-session-lock-v1.xml)

# zwwm-specific protocols are retained as source XML under protocols/.
zwwm_add_project_protocol(zwwm-tags-unstable-v1 protocols/zwwm-tags-unstable-v1.xml)
zwwm_add_project_protocol(zwwm-data-control-v1 protocols/zwwm-data-control-v1.xml)
zwwm_add_project_protocol(wlr-data-control-unstable-v1 protocols/wlr-data-control-unstable-v1.xml)
zwwm_add_project_protocol(zwwm-layer-shell-v1 protocols/zwwm-layer-shell-v1.xml)
zwwm_add_project_protocol(xwlr-layer-shell-v1 protocols/xwlr-layer-shell-v1.xml)
zwwm_add_project_protocol(zwwm-screencopy-view-unstable-v1 protocols/zwwm-screencopy-view-unstable-v1.xml)
zwwm_add_project_protocol(ext-zwwm-toplevels-unstable-v1 protocols/ext-zwwm-toplevels-unstable-v1.xml)
zwwm_add_project_protocol(ext-zwwm-manager-v1 protocols/ext-zwwm-manager-v1.xml)
zwwm_add_project_protocol(xwayland-shell protocols/zwwm_xwayland_shell.xml)

add_custom_target(zwwm_protocol_headers DEPENDS ${ZWWW_PROTOCOL_HEADERS})
add_library(zwwm_protocol_metadata INTERFACE)
add_dependencies(zwwm_protocol_metadata zwwm_protocol_headers)
target_include_directories(zwwm_protocol_metadata INTERFACE "${ZWWW_GENERATED_DIR}")
add_library(zwwm::protocol_metadata ALIAS zwwm_protocol_metadata)

set(zwayland_server_header_check "")
foreach(header IN LISTS ZWWW_ZWAYLAND_SERVER_HEADERS)
  if(header STREQUAL "")
    continue()
  endif()
  get_filename_component(header_name "${header}" NAME)
  string(APPEND zwayland_server_header_check "#include <${header_name}>\n")
endforeach()
set(zwayland_client_header_check "")
foreach(header IN LISTS ZWWW_ZWAYLAND_CLIENT_HEADERS)
  if(header STREQUAL "")
    continue()
  endif()
  get_filename_component(header_name "${header}" NAME)
  string(APPEND zwayland_client_header_check "#include <${header_name}>\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/zwayland-server-headers.cpp"
  CONTENT "${zwayland_server_header_check}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/zwayland-client-headers.cpp"
  CONTENT "${zwayland_client_header_check}")

add_library(zwwm_zwayland_server_headers OBJECT
  "${CMAKE_CURRENT_BINARY_DIR}/zwayland-server-headers.cpp")
add_dependencies(zwwm_zwayland_server_headers zwwm_protocol_headers)
target_link_libraries(zwwm_zwayland_server_headers PRIVATE zwayland-server)
target_include_directories(zwwm_zwayland_server_headers PRIVATE "${ZWWW_GENERATED_DIR}")

add_library(zwwm_zwayland_client_headers OBJECT
  "${CMAKE_CURRENT_BINARY_DIR}/zwayland-client-headers.cpp")
add_dependencies(zwwm_zwayland_client_headers zwwm_protocol_headers)
target_link_libraries(zwwm_zwayland_client_headers PRIVATE zwayland-client)
target_include_directories(zwwm_zwayland_client_headers PRIVATE "${ZWWW_GENERATED_DIR}")
