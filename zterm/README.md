# zterm

An experimental **Wayland-native, GPU-accelerated terminal** for the **zwwm
ecosystem**, built with zwayland and libzterm.

## zwwm integration

Native connections and daemon registration use `zwwm-client-connection-v1`.
Configuration uses `.zw`, with imports, variables, and zwwm reload events.
Shared files can include a `zterm = { ... }` block.

## Run

From the repository root:

```sh
nix run .#zterm
```

Add `-- --standalone` for other Wayland compositors, or `-- -e COMMAND` to run
a program. Requires OpenGL ES 3, a GPU render node, and linux-dmabuf v4 support.

### Daemon

```sh
nix run .#zterm -- daemon
```

Runs the terminal engine in-process and keeps a renderer, fonts, and GPU
resources warm for activations through zwwm's connection protocol.

## Configuration

Create `~/.config/zterm/config.zw`, or select a file with `--config FILE` or
`ZTERM_CONFIG`. CLI options take precedence. See [config.zw](config.zw) for all
settings.

```zw
font = {
  mono = "JetBrainsMono Nerd Font Mono"
  symbols = "Symbols Nerd Font Mono"
  emoji = "Noto Color Emoji"
  size = 14
  weight = 400
}

rendering = { background-opacity-per-mille = 850 }

colors = {
  foreground = "#dce1e8"
  background = "#101319"
  selection = "#387ae673"
  red = "#e06c75"
  bright-red = "#ff7a85"
}

cursor = { shape = "beam" blink = true }
```

Font roles also include `bold`, `italic`, and `bold-italic`. All 16 ANSI colors
are configurable: `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`,
`white`, and their `bright-` variants. Colors use `#RRGGBB`; selection also
accepts `#RRGGBBAA`. Per-mille values use `1000 = 100%`.

## NixOS

With this flake's NixOS module:

```nix
programs.zwwm.zterm = {
  enable = true;
  settings = {
    font.mono = "JetBrainsMono Nerd Font Mono";
    rendering.background-opacity-per-mille = 850;
  };
};
```

Settings are written to `/etc/xdg/zterm/config.zw`.

## Controls

- **Ctrl+Shift+R:** reload configuration.
- **Ctrl+Shift+C / V:** copy and paste.
- **Drag / double-click / triple-click:** select text, a word, or a line.
- **Shift-drag:** select in mouse-aware applications.
- **Wheel / trackpad / Shift+PageUp / PageDown:** scroll.

Cursor shapes: `beam`, `line` (underline), or `block`.
