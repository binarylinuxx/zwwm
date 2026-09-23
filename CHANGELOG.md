# Changelog

All notable changes to zwwm are documented here.

## 0.1.3-alpha.1 - 2026-09-23
nothing really interesting another bug fixes

### Highlights
- geometry for endless canvas now fractional preserving clusters intact without visual drift on panning zooming

## 0.1.2-alpha.1 - 2026-09-22

This is intended to become zwwm's first minimally stable release: a usable
baseline for regular installation and testing while configuration, private
protocols, and internal APIs remain subject to change.

### Highlights

- Reimplemented endless-canvas camera zoom with frame-driven smoothing, stable
  viewport ownership, centered scaling, and clean interaction with canvas pan.
- Restricted canvas tag transitions to windows visible through each tag's
  actual camera position and zoom, preventing off-screen canvas content from
  entering the transition.
- Added `zwwmctl listen`, which streams live JSON snapshots containing clients,
  active tags, camera state, and the effective keyboard layout.
- Extended the manager protocol to version 6 with combined live snapshots and
  camera and keyboard event subscriptions.
- Made Clang the required C and C++ compiler in both CMake and Meson, matching
  the project's supported-toolchain policy.

### Stability

This release establishes a more dependable runtime and control interface, but
it is not API-stable. Breaking changes may still be made when needed to correct
behavior or simplify the architecture.

## 0.1.1-alpha.1 - 2026-09-21

- Added magnetic endless-canvas window snapping with configured gaps.
- Added directional arrow-key focus navigation.
- Added runtime cursor theme control and camera state reporting to `zwwmctl`.
- Improved zoom-consistent shader rounding and cursor handling.

## 0.1.0-alpha.1 - 2026-09-20

Basically first public alpha release.

### Highlights

- Independent C++23 zwayland client/server runtime and typed protocol scanner.
- Direct DRM/KMS and nested Wayland backends with Xwayland support.
- Master-stack, focus-fibonacci, and endless-canvas layouts.
- Native and Xwayland window management, tags, focus, clipboard, and drag-and-drop.
- Layer shell, session lock, pointer constraints, relative pointer, and idle notify.
- OpenGL animations, backdrop blur, glass, and atomically reloadable user shaders.
- Screencopy and compositor-supervised XDG desktop portal integration.
- Wayland-native `zwwmctl` status, dispatch, event, reload, and shader controls.

### Stability

This release is an alpha. Configuration fields, private zwwm protocols, and
internal APIs may change before a stable release. Wayland client compatibility
remains a core project goal.
