# zterm

An experimental **Wayland-native, GPU-accelerated terminal** built for the
**zwwm ecosystem**. It uses the native C++23 zwayland client runtime and its own
libzterm terminal engine.

Text is shaped with Pango/HarfBuzz, cached in a glyph atlas, and drawn with
instanced OpenGL ES. GPU framebuffers are presented through DMA-BUF without
framebuffer readback.

## zwwm integration

- Uses zwwm's native client-connection bootstrap when available and accepts
  preconnected sockets through `WAYLAND_SOCKET`.
- Uses the separate `zwwm-client-connection-v1` protocol for native connections
  and named daemon registration.
- Shares the `.zw` configuration language, imports, and variables with zwwm.
- Reloads its configuration on zwwm configuration events in native mode.
- Can read a `zterm = { ... }` block from a shared `.zw` file with `--config`.

For other Wayland compositors, use `--standalone`. This uses standard Wayland
and xdg-shell, with linux-dmabuf v4 feedback, a GPU render node, and OpenGL ES 3.

## Run

With this flake's NixOS module:

```nix
programs.zwwm.zterm = {
  enable = true;
  settings = {
    font = { mono = "JetBrainsMono Nerd Font Mono"; size = 14; weight = 400; };
    colors = { red = "#e06c75"; bright-red = "#ff7a85"; };
    cursor.shape = "beam";
  };
};
```

Settings are installed to `/etc/xdg/zterm/config.zw`; user configuration and CLI
options take precedence. `package` selects a custom build and `extraConfig`
appends literal `.zw` configuration.

From the repository root:

```sh
nix run .#zterm
```

Example with explicit options:

```sh
nix run .#zterm -- --standalone \
  --font "JetBrainsMono Nerd Font Mono" --font-size 14 \
  --font-weight 400 --text-rounding 0.2 --icon-scale 1.3 --cursor beam
```

Use `-e COMMAND [ARGS...]` to run a program instead of your login shell.

### Daemon

Start the named zterm service on a zwwm session:

```sh
nix run .#zterm -- daemon
```

It runs in the foreground, registers as `zterm` on the compositor's Wayland
socket, and accepts activation channels through `zwwm-client-connection-v1`.
Each activation launches a client window with a preconnected Wayland socket.
Registration is removed when the daemon exits or the compositor disconnects.

## Configuration and colors

Create `~/.config/zterm/config.zw`, or select a file with `--config FILE` or
`ZTERM_CONFIG`. Command-line options override file settings. See
[config.zw](config.zw) for the complete example.

### Font roles and transparency

```zw
font = {
  mono = "JetBrainsMono Nerd Font Mono"
  symbols = "Symbols Nerd Font Mono"
  emoji = "Noto Color Emoji"
  size = 14
  weight = 400
}

rendering = { background-opacity-per-mille = 850 }
```

`font.bold`, `font.italic`, and `font.bold-italic` select separate variant
families. Empty role names use automatic variants/fallback; `font.family` is
also accepted as an alias for `font.mono`. Cell dimensions come from the mono
font, so role changes preserve a shared terminal grid.

CLI equivalents include `--font-mono`, `--font-bold`, `--font-italic`,
`--font-bold-italic`, `--font-symbols`, and `--font-emoji`; `--font` selects mono.
Use `--background-opacity 0.85` for an 85%-opaque default background. Glyphs and
application-selected background colors remain opaque; the compositor shows
content behind transparent areas.

### ANSI palette

All **16 ANSI colors** can be set independently:

```zw
colors = {
  foreground = "#dce1e8"
  background = "#101319"
  selection = "#387ae673"
  red = "#e06c75"
  bright-red = "#ff7a85"
}
```

| Index | Setting | Index | Setting |
| --- | --- | --- | --- |
| 0 | `black` | 8 | `bright-black` |
| 1 | `red` | 9 | `bright-red` |
| 2 | `green` | 10 | `bright-green` |
| 3 | `yellow` | 11 | `bright-yellow` |
| 4 | `blue` | 12 | `bright-blue` |
| 5 | `magenta` | 13 | `bright-magenta` |
| 6 | `cyan` | 14 | `bright-cyan` |
| 7 | `white` | 15 | `bright-white` |

Colors use `#RRGGBB`; selection also accepts `#RRGGBBAA`. A 16-entry
`colors.palette` array is also accepted in index order. Later assignments win.
Palette reloads update indexed colors in the screen and scrollback.

Font, rendering, shell, scrollback, window, cursor, and mouse settings are also
available in `.zw`. Rendering fractions use per-mille integers: `200` means
`0.2`, and `1300` means `1.3`.

## Controls

- **Ctrl+Shift+R:** reload configuration.
- **Ctrl+Shift+C / V:** copy and paste.
- **Drag / double-click / triple-click:** select text, a word, or a line.
- **Shift-drag:** select in mouse-aware applications.
- **Wheel / trackpad:** scroll history or send application mouse input.
- **Shift+PageUp / PageDown:** browse scrollback.

`cursor.shape` accepts `beam`, `line` (underline), or `block`; applications can
also request cursor shapes through terminal escape sequences.
