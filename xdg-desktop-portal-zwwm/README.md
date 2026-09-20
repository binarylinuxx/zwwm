# xdg-desktop-portal-zwwm

`xdg-desktop-portal-zwwm` is the compositor-spawned portal backend for zwwm. It
implements Screenshot, ScreenCast, Access, FileChooser, and Settings while GTK
handles unrelated portal interfaces.

The process is intentionally not D-Bus activated. zwwm starts one child and
authenticates its private Wayland client through an inherited capability socket
and kernel credentials. Child loss, socket loss, or client destruction revokes
capture authority. The private connection uses `libzwayland-client` and typed
generated protocol bindings rather than libwayland.

After acquiring its backend D-Bus name, the helper checks the public portal's
ScreenCast source mask. If the frontend started too early and cached stale or
zero capabilities, the helper asynchronously restarts only
`xdg-desktop-portal.service`. A frontend already advertising the expected
monitor and window mask is left untouched.

## Screenshot

- Noninteractive full-output capture to a private temporary file
- Interactive output, window, and region selection with Qt Widgets
- Decorated toplevel inventory and one-shot capture
- PNG, BMP, TGA, JPEG, and PPM output
- Cancellable request objects with one terminal response

The private screencopy path uses client-owned Wayland SHM buffers. zwwm also
publishes the canonical public screencopy protocol for direct tools such as
`grim`; that path is separate from portal authorization.

## ScreenCast

ScreenCast version 5 supports one monitor or managed toplevel source per
session. The chooser receives live native Wayland and Xwayland toplevel metadata
through the compositor's capability-restricted toplevel protocol. Toplevel
streams render only the selected client surface tree at its client-logical
size. They exclude compositor borders, rounding, shadows, blur, glass, opacity
rules, animations, other windows, and output transforms. Embedded cursor is the
only optional compositor overlay.

The PipeWire node advertises raw BGRx at the captured physical size, a `1..60
fps` range with 60 fps preferred, and a bounded 2-8 buffer pool with four
buffers preferred. Modifier-aware consumers receive linear GBM DMA-BUFs;
plain-format consumers retain the MemFd path. Capture is paced by compositor
presentation and released PipeWire capacity, with at most one request in flight.

Hidden cursor mode is always available. When embedded cursor capture is
available, the chooser provides an explicit `Include mouse cursor` option for
both monitor and toplevel sources. Metadata cursor, multiple simultaneous
sources, virtual outputs, persistence, and dynamic resize are not currently
implemented.

## Installation

CMake installs:

- `bin/xdg-desktop-portal-zwwm`
- `share/xdg-desktop-portal/portals/zwwm.portal`
- `share/xdg-desktop-portal/portals/zwwm-gtk.portal`
- `share/xdg-desktop-portal/zwwm-portals.conf`

The backend links Qt 6 Core, DBus, Gui, and Widgets, `libzwayland-client`,
PipeWire, and SPA. Image encoding uses the vendored `stb_image_write.h`; its
revision and license are recorded in `vendor/README.md`.
