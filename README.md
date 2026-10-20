# zwwm
zwwm is a C++23 post-Wayland compositor with a Wayland stack rebuilt from the ground up —
even though in 2026 everyone seems to be rewriting everything in Rust.

> [!WARNING]
> `0.2.0-alpha.2` is the current public alpha. Configuration, private zwwm
> protocols, APIs, behavior, and internal architecture may change without
> backward compatibility. Use it only if you accept that instability.

## Features

- Atomic DRM/KMS and nested Wayland backends
- libinput and XKB keyboard support
- Touchscreen input and a native virtual-keyboard protocol for on-screen keyboard clients
- Master-stack tiling, floating windows, fullscreen, and tags
- Xwayland windows integrated into layout, focus, actions, and clipboard
- custom shaders
- Public screencopy plus an authenticated XDG desktop portal backend
- Live configuration reload and Wayland-native `zwwmctl` control

### Nix Flake

Build or run the default Xwayland-enabled package directly:

```sh
nix build
nix run
```

The flake also exports `packages.<system>.zwwm-no-xwayland` and a NixOS module:

```nix
{
  inputs.zwwm.url = "github:binarylinuxx/zwwm";

  outputs = { nixpkgs, zwwm, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      modules = [
        zwwm.nixosModules.default
        {
          programs.zwwm = {
            enable = true;
            xwayland.enable = true;
            zterm = {
              enable = true;
              settings = {
                font = { family = "JetBrainsMono Nerd Font Mono"; size = 14; };
                cursor.shape = "beam";
              };
            };
          };
        }
      ];
    };
  };
}
```

Set `xwayland.enable = false` to select the package variant that does not
discover or link XCB. The module installs zwwm, registers its display-manager
session, enables graphics support, and configures the zwwm/GTK portal set.
`programs.zwwm.settings` and `programs.zwwm.zterm.settings` accept recursive native
`.zw` settings; `extraConfig` on either option appends literal `.zw` configuration.

### Reusable flake components

Available independently as `packages.<system>` outputs and through
`overlays.default`:

- `zwayland`: C++23 client/server/wire libraries, headers, CMake and pkg-config exports.
- `zwayland-scanner`: standalone native C++ binding generator.
- `zwwm-protocols`: project and upstream XML, usable with any language generator.

```sh
nix build .#zwayland .#zwayland-scanner .#zwwm-protocols
nix run .#zwayland-scanner -- zwayland-client-header protocol.xml protocol-client.h
```

CMake exports `zwayland::client`, `zwayland::server`, and `zwayland::wire`.
pkg-config provides `zwayland-client`, `zwayland-server`, `zwayland-wire`,
`zwwm-protocols` (variable `pkgdatadir`), and `zwayland-scanner` (variable `scanner`).

### CMake

```sh
nix develop
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_BUILD_TYPE=Release \
  -DXWAYLAND_ENABLE=ON
cmake --build build --target zwwm -j2
```

Set `XWAYLAND_ENABLE=OFF` if you not wish xwayland support

```sh
cmake --build build -j2
sudo "$(command -v cmake)" --install build
```

### Meson

The native Meson build uses the same in-tree zwayland scanner, generated
protocol bindings, embedded shaders, strict warnings, and Qt MOC pipeline:

```sh
nix develop
CC=clang CXX=clang++ meson setup build-meson --buildtype=release --prefix=/usr -Dxwayland=true
meson compile -C build-meson -j2
```

Set `-Dxwayland=false` if you not wish xwayland support. Install the same shipped binaries and data as the CMake build with:

```sh
sudo meson install -C build-meson
```

Use a packaging `DESTDIR` instead of `/` when staging an installation.

## Run

The direct backend requires atomic KMS support. It selects primary planes,
validates modesets with test-only atomic commits, and presents frames through
nonblocking atomic page flips. Output transforms and cursor drawing remain
compositor-rendered in the primary framebuffer.

Review `zwwm/data/config.zw`, then launch from a TTY:

```sh
./launch-zwwm.sh
```

zwwm-session adviced for proper startup.

### elogind and other non-systemd sessions

The direct DRM backend obtains its seat through libseat; it does not require
systemd. For elogind, install a libseat build with its logind backend enabled
and start zwwm from an elogind-managed graphical session or a PAM-enabled TTY
login. `pam_elogind` must provide `XDG_RUNTIME_DIR`. To explicitly select that
libseat backend:

```sh
LIBSEAT_BACKEND=logind zwwm-session
```

`zwwm-session` uses the existing session D-Bus bus when available, and runs
`dbus-run-session` when no bus is present and that command is installed. This
allows the optional desktop portal to activate on non-systemd sessions.
`launch-zwwm.sh` likewise works without `systemctl --user`.

## Control

```sh
zwwmctl status
zwwmctl outputs -j
zwwmctl clients -j
zwwmctl camera -j
zwwmctl keyboard -j
zwwmctl dispatch tag 2
zwwmctl reload
zwwmctl rebuild-switch-shaders
zwwmctl setcursor "Bibata-Modern-Classic" 24
```

See `CONFIG.md` for user shader paths, the atomic rebuild behavior, and the
complete GLSL interface.

## Community Projects

### [Just Enough Shell (JES)](https://github.com/ORFLEM/just_enough_shell)

A modular Wayland desktop shell with native zwwm support, a panel, launcher,
widgets, notifications, and a lock screen. Its plugin system supports additional
compositors and shell extensions.

Latest published screenshot — lock screen:

[![Just Enough Shell lock screen](https://raw.githubusercontent.com/ORFLEM/dots/main/images/10.webp)](https://github.com/ORFLEM/just_enough_shell)

[Desktop preview](https://raw.githubusercontent.com/ORFLEM/dots/main/images/7.webp)
· [Screenshot gallery](https://github.com/ORFLEM/just_enough_shell/blob/main/documentation/eng.md)

### [blxshell / dots](https://github.com/binarylinuxx/dots)

A Material You desktop shell built with Quickshell, featuring wallpaper-derived
colors, a panel, launcher, notifications, and a lock screen. Its native zwwm
plugin integrates tags, window state, workspace switching, and taskbar activation.

Latest repository screenshot:

[![blxshell desktop with zwwm integration](https://raw.githubusercontent.com/binarylinuxx/dots/main/20261011_10h30m37s_grim.png)](https://github.com/binarylinuxx/dots)

## Components

- `zwwm/`: compositor, backends, input, layout, and Xwayland integration
- `zwwm-renderer/`: scene planning and OpenGL rendering
- `zwayland/`: independent Wayland-compatible client/server runtime and transport
- `zwayland-scanner/`: C++ XML-to-Wayland binding generator
- `zwwm-protocols/`: standard and project protocol XML plus generated bindings
- `zw-lang/`: configuration parser and validation library
- [`zterm/`](zterm/README.md): Wayland-native GPU terminal with native zwwm integration
- `xdg-desktop-portal-zwwm/`: desktop portal
- `zwwmctl/`: `ext-zwwm-manager-v1` Wayland command-line client

## Maintenance Terms

zwwm has been developed for Clang from the beginning. Clang is the only
officially supported C and C++ toolchain, and the CMake and Meson builds reject
other compilers. Distribution maintainers may carry their own GCC adaptations,
but GCC-specific build failures and compiler quirks are outside the project's
normal support scope and will receive low priority.
## License

zwwm is licensed under the [BSD 3-Clause License](LICENSE). Redistribution and
modification are permitted under its notice-preservation and non-endorsement
conditions.
