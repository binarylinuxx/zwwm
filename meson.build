project(
  'zwwm',
  ['c', 'cpp'],
  version: '0.1.0-alpha.1',
  meson_version: '>=1.4.0',
  default_options: ['cpp_std=c++23', 'cpp_eh=default', 'buildtype=release'],
)

strict_cpp = ['-Wall', '-Wextra', '-Wpedantic', '-Werror']
runtime_cpp = ['-Wall', '-Wextra', '-Wpedantic']
version_cpp = ['-DZWWM_VERSION="' + meson.project_version() + '"']
data_cpp = ['-DZWWM_DATA_DIR="' + (get_option('prefix') / get_option('datadir') / 'zwwm') + '"']
root_inc = include_directories('.')
python = import('python').find_installation()

libxml = dependency('libxml-2.0')

zwayland_scanner = executable(
  'zwayland-scanner',
  'zwayland-scanner/main.cpp',
  dependencies: libxml,
  cpp_args: strict_cpp,
  native: true,
)

wayland_xml = files('zwwm-protocols/protocols/upstream/wayland/wayland.xml')
core_server_header = custom_target(
  'wayland-zwayland-server-header',
  input: wayland_xml,
  output: 'wayland-zwayland-server.h',
  command: [zwayland_scanner, 'zwayland-server-header', '@INPUT@', '@OUTPUT@'],
)
core_client_header = custom_target(
  'wayland-zwayland-client-header',
  input: wayland_xml,
  output: 'wayland-zwayland-client.h',
  command: [zwayland_scanner, 'zwayland-client-header', '@INPUT@', '@OUTPUT@'],
)

wire_inc = include_directories('zwayland/wire/include')
server_inc = include_directories('zwayland/server/include')
client_inc = include_directories('zwayland/client/include')

zwayland_wire = static_library(
  'zwayland-wire',
  ['zwayland/wire/src/message.cpp', 'zwayland/wire/src/connection.cpp'],
  include_directories: wire_inc,
  cpp_args: runtime_cpp,
)
zwayland_wire_dep = declare_dependency(link_with: zwayland_wire, include_directories: wire_inc)

zwayland_server = static_library(
  'zwayland-server',
  ['zwayland/server/src/display.cpp', 'zwayland/server/src/event_loop.cpp', core_server_header],
  include_directories: [root_inc, server_inc, wire_inc],
  dependencies: zwayland_wire_dep,
  cpp_args: runtime_cpp,
)
zwayland_server_dep = declare_dependency(
  link_with: zwayland_server,
  include_directories: [root_inc, server_inc, wire_inc],
  dependencies: zwayland_wire_dep,
  sources: core_server_header,
)

zwayland_client = static_library(
  'zwayland-client',
  ['zwayland/client/src/display.cpp', core_client_header],
  include_directories: [root_inc, client_inc, wire_inc],
  dependencies: zwayland_wire_dep,
  cpp_args: runtime_cpp,
)
zwayland_client_dep = declare_dependency(
  link_with: zwayland_client,
  include_directories: [root_inc, client_inc, wire_inc],
  dependencies: zwayland_wire_dep,
  sources: core_client_header,
)

wayland_protocols_dir = join_paths(
  meson.project_source_root(),
  'zwwm-protocols/protocols/upstream/wayland-protocols',
)
protocol_specs = [
  ['xdg-shell', join_paths(wayland_protocols_dir, 'stable/xdg-shell/xdg-shell.xml')],
  ['xdg-decoration', join_paths(wayland_protocols_dir, 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml')],
  ['xdg-activation', join_paths(wayland_protocols_dir, 'staging/xdg-activation/xdg-activation-v1.xml')],
  ['xdg-dialog', join_paths(wayland_protocols_dir, 'staging/xdg-dialog/xdg-dialog-v1.xml')],
  ['viewporter', join_paths(wayland_protocols_dir, 'stable/viewporter/viewporter.xml')],
  ['presentation-time', join_paths(wayland_protocols_dir, 'stable/presentation-time/presentation-time.xml')],
  ['linux-dmabuf', join_paths(wayland_protocols_dir, 'stable/linux-dmabuf/linux-dmabuf-v1.xml')],
  ['fractional-scale', join_paths(wayland_protocols_dir, 'staging/fractional-scale/fractional-scale-v1.xml')],
  ['cursor-shape', join_paths(wayland_protocols_dir, 'staging/cursor-shape/cursor-shape-v1.xml')],
  ['tearing-control', join_paths(wayland_protocols_dir, 'staging/tearing-control/tearing-control-v1.xml')],
  ['explicit-sync', join_paths(wayland_protocols_dir, 'unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml')],
  ['relative-pointer', join_paths(wayland_protocols_dir, 'unstable/relative-pointer/relative-pointer-unstable-v1.xml')],
  ['pointer-constraints', join_paths(wayland_protocols_dir, 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml')],
  ['pointer-gestures', join_paths(wayland_protocols_dir, 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml')],
  ['tablet', join_paths(wayland_protocols_dir, 'unstable/tablet/tablet-unstable-v2.xml')],
  ['text-input-v3', join_paths(wayland_protocols_dir, 'unstable/text-input/text-input-unstable-v3.xml')],
  ['primary-selection', join_paths(wayland_protocols_dir, 'unstable/primary-selection/primary-selection-unstable-v1.xml')],
  ['ext-idle-notify', join_paths(wayland_protocols_dir, 'staging/ext-idle-notify/ext-idle-notify-v1.xml')],
  ['xdg-output', join_paths(wayland_protocols_dir, 'unstable/xdg-output/xdg-output-unstable-v1.xml')],
  ['idle-inhibit', join_paths(wayland_protocols_dir, 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml')],
  ['keyboard-shortcuts-inhibit', join_paths(wayland_protocols_dir, 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml')],
  ['drm-lease', join_paths(wayland_protocols_dir, 'staging/drm-lease/drm-lease-v1.xml')],
  ['session-lock', join_paths(wayland_protocols_dir, 'staging/ext-session-lock/ext-session-lock-v1.xml')],
  ['zwwm-tags-unstable-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/zwwm-tags-unstable-v1.xml')],
  ['zwwm-data-control-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/zwwm-data-control-v1.xml')],
  ['wlr-data-control-unstable-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/wlr-data-control-unstable-v1.xml')],
  ['wlr-layer-shell-unstable-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/wlr-layer-shell-unstable-v1.xml')],
  ['xwlr-layer-shell-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/xwlr-layer-shell-v1.xml')],
  ['zwwm-screencopy-view-unstable-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/zwwm-screencopy-view-unstable-v1.xml')],
  ['ext-zwwm-toplevels-unstable-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/ext-zwwm-toplevels-unstable-v1.xml')],
  ['ext-zwwm-manager-v1', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/ext-zwwm-manager-v1.xml')],
  ['xwayland-shell', join_paths(meson.project_source_root(), 'zwwm-protocols/protocols/zwwm_xwayland_shell.xml')],
]

protocol_server_headers = []
protocol_client_headers = []
foreach spec : protocol_specs
  protocol_input = files(spec[1])
  protocol_server_headers += custom_target(
    spec[0] + '-zwayland-server-header',
    input: protocol_input,
    output: spec[0] + '-zwayland-server.h',
    command: [zwayland_scanner, 'zwayland-server-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_client_headers += custom_target(
    spec[0] + '-zwayland-client-header',
    input: protocol_input,
    output: spec[0] + '-zwayland-client.h',
    command: [zwayland_scanner, 'zwayland-client-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach
protocol_server_dep = declare_dependency(include_directories: root_inc, sources: protocol_server_headers)
protocol_client_dep = declare_dependency(include_directories: root_inc, sources: protocol_client_headers)

zw_lang_inc = include_directories('zw-lang/include')
zw_lang = static_library(
  'zw_lang',
  'zw-lang/src/config.cpp',
  include_directories: zw_lang_inc,
  cpp_args: strict_cpp,
)
zw_lang_dep = declare_dependency(link_with: zw_lang, include_directories: zw_lang_inc)

shader_sources = files(
  'zwwm-renderer/shaders/surface.vert',
  'zwwm-renderer/shaders/surface.frag',
  'zwwm-renderer/shaders/kawase.vert',
  'zwwm-renderer/shaders/kawase.frag',
  'zwwm-renderer/shaders/glass.frag',
  'zwwm-renderer/shaders/glass_physics.glsl',
  'zwwm-renderer/shaders/rounded_rect.glsl',
)
embedded_shaders = custom_target(
  'embedded-shaders',
  input: shader_sources,
  output: 'shaders.hpp',
  command: [python, files('build-aux/embed-shaders.py'), '@OUTPUT@', '@INPUT@'],
)

egl = dependency('egl')
gles = dependency('glesv2')
epoxy = dependency('epoxy')
libdrm = dependency('libdrm')
renderer_inc = include_directories('zwwm-renderer/include')
zwwm_renderer = static_library(
  'zwwm_renderer',
  [
    'zwwm-renderer/src/dmabuf.cpp',
    'zwwm-renderer/src/egl_dmabuf_importer.cpp',
    'zwwm-renderer/src/scene.cpp',
    'zwwm-renderer/src/opengl.cpp',
    embedded_shaders,
  ],
  include_directories: [renderer_inc, root_inc],
  dependencies: [egl, epoxy, libdrm],
  cpp_args: strict_cpp,
)
zwwm_renderer_dep = declare_dependency(
  link_with: zwwm_renderer,
  include_directories: [renderer_inc, root_inc],
  dependencies: [egl, epoxy, libdrm],
  sources: embedded_shaders,
)

zwwm_egl_inc = include_directories('zwwm-egl/include')
zwwm_egl = static_library(
  'zwwm-egl',
  'zwwm-egl/src/window.cpp',
  include_directories: zwwm_egl_inc,
  dependencies: [zwayland_client_dep, egl, gles],
  cpp_args: strict_cpp,
)
zwwm_egl_dep = declare_dependency(
  link_with: zwwm_egl,
  include_directories: zwwm_egl_inc,
  dependencies: [zwayland_client_dep, egl, gles],
)

zwwm_inc = include_directories('zwwm/include')
xkbcommon = dependency('xkbcommon')
zwwm_master_stack = static_library(
  'zwwm_master_stack',
  'zwwm/src/master_stack.cpp',
  include_directories: zwwm_inc,
  dependencies: zwwm_renderer_dep,
  cpp_args: strict_cpp,
)
zwwm_master_stack_dep = declare_dependency(
  link_with: zwwm_master_stack,
  include_directories: zwwm_inc,
  dependencies: zwwm_renderer_dep,
)

zwwm_animation = static_library(
  'zwwm_animation',
  'zwwm/src/animation.cpp',
  include_directories: zwwm_inc,
  dependencies: zwwm_renderer_dep,
  cpp_args: strict_cpp,
)
zwwm_animation_dep = declare_dependency(
  link_with: zwwm_animation,
  include_directories: zwwm_inc,
  dependencies: zwwm_renderer_dep,
)

zwwm_runtime_config = static_library(
  'zwwm_runtime_config',
  'zwwm/src/runtime_config.cpp',
  include_directories: zwwm_inc,
  dependencies: [zwayland_server_dep, xkbcommon, zw_lang_dep, zwwm_master_stack_dep, zwwm_animation_dep],
  cpp_args: strict_cpp + data_cpp,
)
zwwm_runtime_config_dep = declare_dependency(
  link_with: zwwm_runtime_config,
  include_directories: zwwm_inc,
  dependencies: [zwayland_server_dep, xkbcommon, zw_lang_dep, zwwm_master_stack_dep, zwwm_animation_dep],
)

compositor_sources = files(
  'zwwm/src/compositor_server.cpp',
  'zwwm/src/compositor_server_buffers.cpp',
  'zwwm/src/compositor_server_data_device.cpp',
  'zwwm/src/compositor_server_input.cpp',
  'zwwm/src/compositor_server_idle.cpp',
  'zwwm/src/compositor_server_layer_shell.cpp',
  'zwwm/src/compositor_server_session_lock.cpp',
  'zwwm/src/compositor_server_surface_extensions.cpp',
)
compositor_deps = [
  zwayland_server_dep,
  protocol_server_dep,
  xkbcommon,
  zwwm_runtime_config_dep,
  zwwm_master_stack_dep,
  zwwm_renderer_dep,
]
compositor_args = strict_cpp
if get_option('xwayland')
  compositor_sources += files(
    'zwwm/src/compositor_server_xwayland.cpp',
    'zwwm/src/compositor_server_xwayland_selection.cpp',
    'zwwm/src/xwayland_spawn.cpp',
  )
  compositor_deps += [dependency('xcb'), dependency('xcb-composite'), dependency('xcb-xfixes')]
  compositor_args += ['-DZWWM_XWAYLAND=1']
endif
zwwm_compositor = static_library(
  'zwwm_compositor',
  compositor_sources,
  include_directories: [zwwm_inc, root_inc],
  dependencies: compositor_deps,
  cpp_args: compositor_args,
)
zwwm_compositor_dep = declare_dependency(
  link_with: zwwm_compositor,
  include_directories: [zwwm_inc, root_inc],
  dependencies: compositor_deps,
)

pipewire = dependency('libpipewire-0.3', include_type: 'system')
spa = dependency('libspa-0.2', include_type: 'system')
qt6_dep = dependency('qt6', modules: ['Core', 'DBus', 'Gui', 'Widgets'])
qt6 = import('qt6')
portal_moc = qt6.preprocess(
  moc_headers: files(
    'xdg-desktop-portal-zwwm/src/access_portal.hpp',
    'xdg-desktop-portal-zwwm/src/capture_client.hpp',
    'xdg-desktop-portal-zwwm/src/file_chooser_portal.hpp',
    'xdg-desktop-portal-zwwm/src/pipewire_stream.hpp',
    'xdg-desktop-portal-zwwm/src/screenshot_portal.hpp',
    'xdg-desktop-portal-zwwm/src/screencast_portal.hpp',
    'xdg-desktop-portal-zwwm/src/settings_portal.hpp',
  ),
  dependencies: qt6_dep,
)
portal_helper = executable(
  'xdg-desktop-portal-zwwm',
  [
    'xdg-desktop-portal-zwwm/src/main.cpp',
    'xdg-desktop-portal-zwwm/src/access_portal.cpp',
    'xdg-desktop-portal-zwwm/src/capture_client.cpp',
    'xdg-desktop-portal-zwwm/src/file_chooser_portal.cpp',
    'xdg-desktop-portal-zwwm/src/image_writer.cpp',
    'xdg-desktop-portal-zwwm/src/screenshot_portal.cpp',
    'xdg-desktop-portal-zwwm/src/screencast_portal.cpp',
    'xdg-desktop-portal-zwwm/src/settings_portal.cpp',
    'xdg-desktop-portal-zwwm/src/pipewire_stream.cpp',
    portal_moc,
  ],
  include_directories: [root_inc, include_directories('xdg-desktop-portal-zwwm/vendor', is_system: true)],
  dependencies: [pipewire, spa, libdrm, dependency('gbm'), zwayland_client_dep, protocol_client_dep, qt6_dep],
  cpp_args: strict_cpp,
  install: true,
)

zwwm = executable(
  'zwwm',
  [
    'zwwm/src/main.cpp',
    'zwwm/src/error_popup.cpp',
    'zwwm/src/manager_protocol.cpp',
    'zwwm/src/nested_backend.cpp',
    'zwwm/src/runtime_backend.cpp',
    'zwwm/src/portal_capture.cpp',
  ],
  include_directories: [zwwm_inc, root_inc],
  dependencies: [
    zwayland_server_dep,
    zwayland_client_dep,
    protocol_server_dep,
    protocol_client_dep,
    dependency('libdrm'),
    dependency('libudev'),
    dependency('libseat'),
    dependency('libinput'),
    dependency('gbm'),
    xkbcommon,
    dependency('xcursor'),
    dependency('fontconfig'),
    dependency('freetype2'),
    zwwm_compositor_dep,
    zw_lang_dep,
    zwwm_runtime_config_dep,
    zwwm_master_stack_dep,
    zwwm_renderer_dep,
    zwwm_animation_dep,
    zwwm_egl_dep,
  ],
  cpp_args: strict_cpp + version_cpp + ['-DZWWM_PORTAL_EXECUTABLE="xdg-desktop-portal-zwwm"'],
  install: true,
)

zwwmctl = executable(
  'zwwmctl',
  'zwwmctl/main.cpp',
  include_directories: root_inc,
  dependencies: [zwayland_client_dep, protocol_client_dep],
  cpp_args: strict_cpp + version_cpp,
  install: true,
)

install_data(
  'zwwm-session',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)

install_data(
  ['xdg-desktop-portal-zwwm/data/zwwm.portal', 'xdg-desktop-portal-zwwm/data/zwwm-gtk.portal'],
  install_dir: join_paths(get_option('datadir'), 'xdg-desktop-portal/portals'),
)
install_data(
  'xdg-desktop-portal-zwwm/data/zwwm-portals.conf',
  install_dir: join_paths(get_option('datadir'), 'xdg-desktop-portal'),
)
install_data(
  'xdg-desktop-portal-zwwm/data/org.freedesktop.impl.portal.desktop.zwwm.service',
  install_dir: join_paths(get_option('datadir'), 'dbus-1/services'),
)
install_data('zwwm/data/config.zw', install_dir: '/etc/xdg/zwwm')
install_data('zwwm/data/config.zw', install_dir: get_option('datadir') / 'zwwm')
install_data(
  'zwwm/data/shaders/window.frag',
  'zwwm/data/shaders/border.frag',
  'zwwm/data/shaders/background.frag',
  install_dir: '/etc/xdg/zwwm/shaders',
)
install_data(
  'zwwm/data/shaders/window.frag',
  'zwwm/data/shaders/border.frag',
  'zwwm/data/shaders/background.frag',
  install_dir: get_option('datadir') / 'zwwm/shaders',
)
install_data('zwwm/data/zwwm.desktop', install_dir: get_option('datadir') / 'wayland-sessions')
install_data(
  'README.md',
  'CONFIG.md',
  'CHANGELOG.md',
  'CONTRIBUTING.md',
  'LICENSE',
  'SECURITY.md',
  'THIRD_PARTY.md',
  install_dir: get_option('datadir') / 'doc/zwwm',
)
