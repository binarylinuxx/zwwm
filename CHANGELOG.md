# Changelog

All notable changes to zwwm are documented here.

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
