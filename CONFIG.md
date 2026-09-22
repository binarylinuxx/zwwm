# zwwm Configuration Guide

zwwm uses `.zw` configuration files. The format supports variables, imports,
objects, arrays, repeated assignments, comments, and live reload.

> [!WARNING]
> Configuration is unstable while zwwm is under active development. Names,
> ranges, defaults, and behavior may change without backward compatibility.

## Configuration File

zwwm loads the first applicable configuration in this order:

1. The path in `ZWWM_CONFIG`, when the variable is set.
2. `$XDG_CONFIG_HOME/zwwm/config.zw`.
3. `$HOME/.config/zwwm/config.zw` when `XDG_CONFIG_HOME` is unset.
4. `/etc/xdg/zwwm/config.zw`.

The installed reference configuration is `zwwm/data/config.zw` in the source
tree and `/etc/xdg/zwwm/config.zw` after a system installation.

The active file is watched for changes. A successful edit is applied live. A
failed reload keeps the previous valid configuration and displays an error
popup. An invalid startup file uses built-in defaults and remains watched so it
can recover after the file is fixed.

## Language Basics

Comments begin with `--`:

```zw
-- This is a comment.
layout = {
  inner-gap = 12 -- Inline comments are allowed.
}
```

Values include strings, booleans, integers, `null`, arrays, lists, objects, and
variable references:

```zw
NAME = "value"
enabled = true
count = 3
items = ["one", "two"]
object = { name = "example" enabled = true }
```

Uppercase assignments define string variables. Reference them with `$NAME`:

```zw
TERMINAL = "ghostty"
MOD = "Mod4"

bind = $MOD, Return, exec, $TERMINAL
environment = { TERMINAL = $TERMINAL }
```

Variables must be defined somewhere in the merged configuration. Variables are
supported where a setting accepts a resolved string, such as bindings and
environment values.

## Imports

`import` accepts one path or an array:

```zw
import = "appearance.zw"
import = ["bindings.zw", "$HOME/.config/zwwm/outputs.zw"]
```

Relative paths are resolved beside the importing file. Import paths expand
process environment variables written as `$VAR` or `${VAR}`. Imports are
recursive, and cycles are errors.

Imported assignments are inserted where the `import` appears. For singleton
settings such as `shaders`, `input`, or `output`, the last assignment wins.
Repeated `bind`, `rule`, and `layer-rule` assignments accumulate in declaration
order.

```zw
import = "defaults.zw"

-- Replaces the complete input object selected from defaults.zw.
input = {
  focus-mode = "hover"
  reverse-mouse-scrolling = true
}
```

Singleton objects are not merged field by field. Include every non-default field
you want in the final object.

## Bindings

A binding contains exactly four comma-separated values:

```zw
bind = MODIFIERS, KEY, ACTION, ARGUMENT
```

Modifiers are XKB modifier names joined with `+`. Omit the first value or use an
empty string when no modifier is required. Keyboard keys use XKB keysym names.
`scrollback` and `scrollforward` match vertical mouse-wheel movement. Quote
values when needed.

```zw
MOD = "Mod4"

bind = $MOD, Return, exec, "ghostty"
bind = "Mod4+Shift", Escape, exit, ""
bind = $MOD, scrollback, zoomin, ""
bind = $MOD, scrollforward, zoomout, ""
bind = $MOD, "1", tag, "1"
bind = $MOD, Left, focus, "left"
bind = , Home, exec, "jes-cli screenpicker"
```

| Action | Argument | Behavior |
|---|---|---|
| `exec` | Shell command | Runs `sh -c ARGUMENT`, resolving `sh` through `PATH`. |
| `reload` | Ignored | Reloads the active configuration. |
| `exit` | Ignored | Exits zwwm. |
| `focus` | Empty or `"left"`, `"right"`, `"up"`, `"down"` | Empty cycles focus. A direction selects the nearest visible window in that direction on the same output. Canvas navigation uses world coordinates and is independent of camera pan and zoom. |
| `killactive` | Ignored | Requests that the focused window close. |
| `killsession` | Ignored | Terminates the compositor session, currently equivalent to `exit`. |
| `togglefloating` | Ignored | Toggles the focused window between tiled and floating. |
| `toggle-fullscreen` | Ignored | Toggles fullscreen only on the focused window. `fullscreen` remains an alias. |
| `tag` | `"1"` through `"9"` | Switches to a tag. |
| `movetotag` | `"1"` through `"9"` | Moves the focused window to a tag. |
| `zoomin` | Ignored | Zooms the active endless-canvas viewport in around the pointer. |
| `zoomout` | Ignored | Zooms the active endless-canvas viewport out around the pointer. |

Use `""` for actions that do not need an argument.

## User Shaders

`shaders` is a registry of named OpenGL ES 3 fragment shaders. Every entry has
a `role`, a source path, and optional reflected values. At least one `window`,
`border`, and `background` shader is required. The first shader declared for a
role is its default.

```zw
shaders = {
  blur = {
    role = "window"
    source = "shaders/window.frag"
    values = {
      radius = 10
      blur_radius = 8
      blur_passes = 3
      blur_brightness = 100
      blur_contrast = 100
      blur_saturation = 100
      blur_noise = 0
    }
  }
  border = {
    role = "border"
    source = "shaders/border.frag"
    values = {
      width = 3
      radius = 10
      gradient_colors = ["#2e3744", "#1f2731", "#2e3744"]
      focused_gradient_colors = ["#75c5ff", "#3da6f2", "#2474ae"]
    }
  }
  background = {
    role = "background"
    source = "shaders/background.frag"
    values = { top_color = "#20242b" bottom_color = "#13161b" }
  }
}
```

Relative paths are resolved beside the active `config.zw`. When neither
`ZWWM_CONFIG` nor a user config exists, zwwm creates
`$XDG_CONFIG_HOME/zwwm/config.zw` (or `$HOME/.config/zwwm/config.zw`) and copies
the three editable default shaders beside it. Existing files are never
overwritten. An explicit `ZWWM_CONFIG` path is never created automatically.

Shader files are read at startup, after a successful configuration reload, and
by this explicit transaction (required when only a `.frag` file changes):

```sh
zwwmctl rebuild-switch-shaders
```

Every registered shader is compiled and linked in every active GPU context
before any program is switched. If reading, compiling, or validating any shader
fails, all contexts keep the previous programs and the command reports the
diagnostic. Shader files are trusted GPU code; malformed source is recoverable,
but a GPU-hanging shader cannot be sandboxed by the compositor.

### Shader Values

Each value named `NAME` is uploaded to `zwwm_config_NAME`. Supported mappings
are:

| Configuration value | GLSL uniform |
|---|---|
| Boolean | `bool` |
| Integer | `int`, `uint`, or `float` |
| `#RRGGBB` or `#RRGGBBAA` | `vec4` with normalized components |
| Two to four integers | `ivec2..4` or `vec2..4` |
| One to twenty colors | `vec4 NAME[20]`; `int NAME_count` receives the active count |

Uppercase string variables may provide numeric shader values. For example,
`RADIUS = "20"` and `radius = $RADIUS` produce an integer value.

Some names also configure the compositor pipeline:

| Role | Value | Meaning |
|---|---|---|
| `border` | `width` | Border geometry width. |
| `border` | `radius` | Window corner radius before canvas and animation scaling. |
| `window` | `blur_radius` | Prepared backdrop blur radius, `0..64`. |
| `window` | `blur_passes` | Blur pass count, clamped to `1..8`. |
| `window` | `blur_brightness`, `blur_contrast`, `blur_saturation` | Percentage controls, clamped to `0..200`. |
| `window` | `blur_noise` | Noise percentage, clamped to `0..100`. |

All non-pipeline values must have a compatible active uniform in the shader;
otherwise the transactional rebuild fails and keeps the previous programs.
Backdrop blur is visible through transparent shader/client pixels.

Use renderer-provided `zwwm_corner_radius`, `zwwm_border_width`, and
`zwwm_clip_radius` for final geometry. They already include animation and
endless-canvas zoom scaling; using fixed config pixels directly makes rounding
change proportion at different zoom levels.

### Shader ABI

Window and border shaders use the compositor's `surface.vert` vertex stage and
may declare any subset of this ABI:

```glsl
in vec2 texture_coordinates;       // client texture UV for this draw
in vec2 local_pixel_coordinates;   // position within zwwm_draw_rect
in vec2 target_pixel_coordinates;  // output position, top-left origin
flat in vec4 source_uv_value;
flat in int texture_transform_value; // 0 normal, 1 90deg, 2 180deg, 3 270deg

uniform sampler2D zwwm_window_texture;
uniform sampler2D zwwm_backdrop_texture;         // sharp scene below this toplevel
uniform sampler2D zwwm_blurred_backdrop_texture; // configured prepared blur, or sharp
uniform vec2 zwwm_output_size;
uniform vec4 zwwm_toplevel_rect; // x, y, width, height
uniform vec4 zwwm_draw_rect;
uniform vec4 zwwm_texture_rect;
uniform vec4 zwwm_clip_rect;
uniform vec4 zwwm_color;
uniform float zwwm_opacity;
uniform float zwwm_corner_radius;
uniform float zwwm_border_width;
uniform float zwwm_clip_radius;
uniform float zwwm_time; // monotonic seconds, wrapping after 1000 seconds
uniform int zwwm_state;  // bit 0 focused, bit 1 fullscreen, bit 2 popup
uniform bool zwwm_has_texture;
```

Backdrop samplers use framebuffer coordinates. Sample them with
`gl_FragCoord.xy / zwwm_output_size`. `zwwm_toplevel_rect` gives every draw in
a managed surface tree the same root coordinate system, while
`zwwm_draw_rect` identifies the current client surface or border. The default
window shader demonstrates clipping and correct sampling of undersized client
buffers without stretching.

The compositor loads `background.png` beside the active configuration and uses
it as a center-cropped, full-output default wallpaper. The background shader
renders it before scene surfaces and before any layer-shell wallpaper client. It is
guaranteed `zwwm_output_size` and `zwwm_time`; it may also use the vertex-stage
varyings. Animating a shader does not itself schedule frames, so continuous
animation requires another active repaint source such as a configured animated
effect.

User shader alpha currently affects presentation only. Pointer hit testing
continues to use compositor-managed window geometry.

## Animations

```zw
animations = {
  enabled = true
  duration-ms = 320
  tag-duration-ms = 360
  spring = { stiffness = 200 damping = 18 mass-per-mille = 1000 }
  cubic-bezier = { x1 = 220 y1 = 0 x2 = 300 y2 = 1000 }
  open-window = true
  resize = true
  close = true
  open-scale-per-mille = 900
  close-scale-per-mille = 800
  open-offset-px = 0
  close-offset-px = 0
  tag-scale-per-mille = 1000
  tag-parallax-per-mille = 1000
  tag-fade-per-mille = 0
}
```

| Field | Default | Range |
|---|---:|---:|
| `enabled` | `false` | Boolean |
| `duration-ms` | `320` | `0` or greater |
| `tag-duration-ms` | `360` | `0..10000` |
| `open-window` | `true` | Boolean |
| `resize` | `true` | Boolean |
| `close` | `true` | Boolean |
| `open-scale-per-mille` | `900` | `500..1000` |
| `close-scale-per-mille` | `800` | `500..1000` |
| `open-offset-px` | `0` | `0..4096` |
| `close-offset-px` | `0` | `0..4096` |
| `tag-scale-per-mille` | `1000` | `500..1000` |
| `tag-parallax-per-mille` | `1000` | `0..1000` |
| `tag-fade-per-mille` | `0` | `0..1000` |

`spring.stiffness` and `spring.mass-per-mille` must be positive integers.
`spring.damping` may be zero. Cubic Bezier points are per-mille values from
`0..1000`.

User-driven window moves and resizes snap immediately even when `resize` is
enabled. Open, close, fullscreen, and tag transitions remain animated.

## Layout

zwwm provides `master-stack`, `focus-fibonacci`, and `endless-canvas` layouts.

```zw
layout = {
  default = "master-stack"
  master-count = 1
  master-ratio = 60
  outer-gap = 16
  inner-gap = 12
  min-zoom-per-mille = 250
  max-zoom-per-mille = 1350
  smart-gaps = false
}
```

| Field | Default | Range or values |
|---|---:|---|
| `default` | `"master-stack"` | `"master-stack"`, `"focus-fibonacci"`, or `"endless-canvas"` |
| `master-count` | `1` | Positive integer |
| `master-ratio` | `60` | `10..90` percent |
| `outer-gap` | `16` | `0` or greater, pixels |
| `inner-gap` | `12` | `0` or greater, pixels |
| `gaps` | Not set | Fallback for both gap fields when their explicit fields are absent |
| `min-zoom-per-mille` | `250` | Positive canvas zoom minimum; `1000` is 1.0x |
| `max-zoom-per-mille` | `1350` | Positive canvas zoom maximum; must be at least the minimum |
| `smart-gaps` | `false` | Removes outer gaps when only one tiled window is visible |

`focus-fibonacci` is a persistent, focus-driven split layout. A newly tiled
window divides the currently focused tiled window into equal halves. Split
orientation alternates horizontally and vertically at each tree depth, forming
a Fibonacci-style spiral as windows are added along the focused branch. If the
focused window is not tiled on that output and tag, the newest tiled leaf is
split. Closing or floating a window collapses its sibling branch without
rearranging unrelated branches. `outer-gap`, `inner-gap`, and `smart-gaps`
apply normally; `master-count` and `master-ratio` only affect `master-stack`.

`endless-canvas` places managed native and Xwayland windows in persistent world
coordinates instead of fitting them into the output work area. Drag an empty
background with the left mouse button to pan the active output and tag. Existing
Super+left window dragging moves a window in world space, while Super+right
resizes it. Window movement and viewport panning use the configured window
geometry animation.

While a window is moved, its edges magnetically snap beside visible windows on
the same output and tag. The resulting separation is `inner-gap`. The capture
threshold is 16 output pixels and is converted to world coordinates, so snapping
feels consistent at every zoom level. Snapped windows remain independent; moving
one does not move its neighbors.

To prevent zooming in beyond 1.0x, set `max-zoom-per-mille = 1000`.

A new native toplevel first receives an unconstrained `0x0` configure. Its first
committed positive `set_window_geometry` size becomes its canvas size; without
one, zwwm uses half of the output work area. New windows are centered in the
current viewport. Each output and tag retains its own pan position. Fullscreen
temporarily bypasses the canvas transform and never changes that saved pan or
the window's world geometry, so leaving fullscreen restores the window against
the same viewport position.

```zw
layout = {
  default = "focus-fibonacci"
  gaps = 10
  smart-gaps = true
}
```

## Input

```zw
input = {
  focus-mode = "by-click"
  reverse-mouse-scrolling = false
}
```

| Field | Default | Values |
|---|---:|---|
| `focus-mode` | `"by-click"` | `"by-click"` or `"hover"` |
| `reverse-mouse-scrolling` | `false` | Reverses mouse wheel and wheel-tilt scrolling without changing touchpad finger scrolling |

`"click"` is accepted as an alias for `"by-click"`.

## Keyboard

Keyboard fields are XKB rule names. Empty strings use libxkbcommon defaults.
The complete combination is validated before it is applied.

```zw
keyboard = {
  rules = ""
  model = ""
  layout = "us"
  variant = ""
  options = ""
}
```

Multiple layouts and variants are comma-separated. For example:

```zw
keyboard = {
  rules = ""
  model = ""
  layout = "us,de"
  variant = ",nodeadkeys"
  options = "grp:alt_shift_toggle"
}
```

## Output

The current output configuration is global rather than connector-specific.

```zw
output = {
  mode = "preferred"
  scale-per-mille = 1000
  bit-depth = 8
  transform = "normal"
}
```

| Field | Default | Range or values |
|---|---:|---|
| `mode` | `"preferred"` | `"preferred"` or `"WIDTHxHEIGHT@HZ"` |
| `scale-per-mille` | `1000` | `250..8000`; `1250` means 1.25 scale |
| `bit-depth` | `8` | `8` or `10` |
| `transform` | `"normal"` | `"normal"`, `"90"`, `"180"`, `"270"` |

Refresh rates may contain decimals:

```zw
output = { mode = "1920x1080@179.82" scale-per-mille = 1000 bit-depth = 10 transform = "normal" }
```

## Environment

`environment` sets variables in the compositor process. Programs launched by
bindings inherit them. Reloading applies the newly configured values, but
variables removed from the object are not unset from the already running
process.

```zw
TERMINAL = "ghostty"

environment = {
  XDG_CURRENT_DESKTOP = "zwwm"
  XDG_SESSION_DESKTOP = "zwwm"
  XDG_SESSION_TYPE = "wayland"
  XCURSOR_SIZE = "24"
  XCURSOR_THEME = "Bibata-Modern-Classic"
  TERMINAL = $TERMINAL
}
```

Names must be valid shell identifiers. Values must be strings or uppercase
string-variable references.

`XCURSOR_THEME` and `XCURSOR_SIZE` provide the startup cursor theme. They use
the standard Xcursor theme format used by Wayland compositors. To change both
for the running compositor without editing the environment, use:

```sh
zwwmctl setcursor "Bibata-Modern-Classic" 24
```

This runtime override is not written back to `config.zw`.

## Autostart

`exec-sh-on-startup` runs commands once after zwwm initializes its backend,
Wayland socket, Xwayland integration, and portal helper. Commands inherit the
configured environment, `WAYLAND_DISPLAY`, and, when enabled, `DISPLAY`.

```zw
PANEL = "waybar"
exec-sh-on-startup = [$PANEL, "swww-daemon"]
```

Each nonempty entry is executed as `sh -c COMMAND`, with `sh` resolved through
`PATH` for NixOS compatibility. Live configuration reloads do not rerun startup
commands; restart zwwm to apply changes to this setting.

## Window Rules

Window rules use shell-style glob matching. A rule may match `app-id`, `title`,
or both. When both are present, both must match. Rules are evaluated in
declaration order; later matching values override earlier values.

```zw
rule = {
  match = { app-id = "org.example.*" title = "Preferences" }
  set = {
    floating = true
    size = "60%x70%"
    window-shader = "blur"
    border-shader = "border"
    opacity = 95
  }
}
```

| Match field | Type | Meaning |
|---|---|---|
| `app-id` | String glob | Native Wayland app ID or Xwayland application class |
| `title` | String glob | Window title |

| Set field | Type | Range or format |
|---|---|---|
| `floating` | Boolean | Select tiled or floating placement |
| `size` | String | `WIDTHxHEIGHT` pixels or `WIDTH%xHEIGHT%` of the work area |
| `window-shader` | Registered shader name | Selects a shader with role `window` |
| `border-shader` | Registered shader name | Selects a shader with role `border` |
| `opacity` | Integer | `0..100` percent |

Examples:

```zw
rule = { match = { app-id = "foot" } set = { opacity = 92 window-shader = "blur" } }
rule = { match = { title = "Picture-in-Picture" } set = { floating = true size = "30%x30%" } }
```

Rules apply to native XDG toplevels. Xwayland also has metadata-based automatic
floating for transient, modal, splash, dialog, utility, menu, and fixed-size
windows.

## Layer Rules

Layer rules match layer-shell namespaces with shell-style globs. Layer blur is
disabled unless a matching rule enables it or assigns a nonzero radius.

```zw
layer-rule = {
  match = { namespace = "waybar" }
  set = { blur = true blur-radius = 10 ignore-alpha = 10 opacity = 92 }
}
```

| Field | Type | Range |
|---|---|---|
| `match.namespace` | Nonempty string glob | Layer-shell namespace |
| `set.blur` | Boolean | `true` selects radius 8; `false` selects radius 0 |
| `set.blur-radius` | Integer | `0..64`; an explicit radius overrides the radius selected by `set.blur` |
| `set.ignore-alpha` | Integer | `0..100`; pixels at or below this alpha percentage do not receive backdrop blur |
| `set.opacity` | Integer | `0..100` percent |

Later matching layer rules replace the complete accumulated layer effect.

## Reloading

Saving the active file triggers live reload. A configured binding can also
reload explicitly:

```zw
bind = Mod4, R, reload, ""
```

The equivalent Wayland manager command is:

```sh
zwwmctl reload
```

Other control and inspection commands include:

```sh
zwwmctl status
zwwmctl outputs
zwwmctl clients
zwwmctl tags
zwwmctl layers
zwwmctl camera
zwwmctl camera -j
zwwmctl keyboard
zwwmctl keyboard -j
zwwmctl listen
zwwmctl dispatch focus left
zwwmctl rebuild-switch-shaders
zwwmctl setcursor "Bibata-Modern-Classic" 24
```

`camera` reports one row per output for its active tag: connector, output ID,
tag, world X, world Y, zoom level, and whether the output is active. JSON output
returns these as named fields in a `cameras` array. Camera reporting requires
manager protocol version 4.

`listen` writes one JSON object per state update. Each object contains the live
client list, active tags, camera position and zoom for every output, and the
effective keyboard layout.

`keyboard` reports the effective XKB layout name and zero-based group. It
requires manager protocol version 5.

Successful reloads update layout, rendering, input, keyboard state, logical
output scale and transform, bindings, rules, animations, and environment
values. Existing processes keep their own inherited environment. Restart zwwm
after changing the requested DRM mode or bit depth.

## Current Caveats

- Configuration compatibility is not guaranteed yet.
- Output settings are global and do not currently match individual connectors.
- Unknown settings produce diagnostics; misspelled fields should not be relied
  upon being ignored.

For a complete ready-to-run configuration, see `zwwm/data/config.zw`.
