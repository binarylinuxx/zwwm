# zwwm
zwwm is a C++23 post-Wayland compositor with a Wayland stack rebuilt from the ground up —
even though in 2026 everyone seems to be rewriting everything in Rust.

> [!WARNING]
> `0.1.2-alpha.1` is the current public alpha. Configuration, private zwwm
> protocols, APIs, behavior, and internal architecture may change without
> backward compatibility. Use it only if you accept that instability.

## Features

- DRM/KMS and nested Wayland backends
- libinput and XKB keyboard support
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

Review `zwwm/data/config.zw`, then launch from a TTY:

```sh
./launch-zwwm.sh
```

zwwm-session adviced for proper startup.

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

## Components

- `zwwm/`: compositor, backends, input, layout, and Xwayland integration
- `zwwm-renderer/`: scene planning and OpenGL rendering
- `zwayland/`: independent Wayland-compatible client/server runtime and transport
- `zwayland-scanner/`: C++ XML-to-Wayland binding generator
- `zwwm-protocols/`: standard and project protocol XML plus generated bindings
- `zw-lang/`: configuration parser and validation library
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
