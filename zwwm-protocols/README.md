# zwwm-protocols

This target generates native zwayland client and server headers with the
repository's `zwayland-scanner`. Both upstream and project XML use the same
generator. Complete snapshots of Wayland 1.26.0 protocol data and
wayland-protocols 1.49 live in `protocols/upstream/`; project protocols live
directly in `protocols/`. Protocol generation does not depend on installed
Wayland protocol data packages.

## Project protocols

- `zwwm-tags-unstable-v1`: trusted tag observation and control
- `zwwm-screencopy-view-unstable-v1`: authenticated output and region capture
- `ext-zwwm-toplevels-unstable-v1`: authenticated toplevel inventory and capture
- `ext-zwwm-manager-v1`: typed compositor state, control, and event subscriptions
- `zwwm-data-control-v1`: compositor data-control integration
- `xwlr-layer-shell-v1`: zwwm layer-shell extensions
- `zwwm_xwayland_shell`: compositor-owned Xwayland surface association

Private capture globals are restricted to the compositor-authenticated portal
client. The canonical `zwlr_screencopy_manager_v1` remains public for direct
clients such as `grim`.

Generated sources are build artifacts and are not committed.
