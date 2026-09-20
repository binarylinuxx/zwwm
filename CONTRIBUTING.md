# Contributing

zwwm is an early-stage C++23 compositor. Focused bug reports and small,
well-scoped changes are preferred.

## Development Environment

Enter the pinned Nix development shell:

```sh
nix develop
```

Build a release configuration with CMake:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DXWAYLAND_ENABLE=ON
cmake --build build -j2
```

The equivalent Meson build is:

```sh
meson setup build-meson --buildtype=release --prefix=/usr -Dxwayland=true
meson compile -C build-meson -j2
```

## Changes

- Keep changes direct and narrowly scoped; fix root causes rather than adding wrappers.
- Keep both CMake and Meson production builds working.
- Do not add generated build output or machine-specific configuration.
- Document user-visible configuration and protocol changes.
- large changes will require large evidence to be accepted

For rendering changes, failures in optional blur or glass preparation must fall
back safely and must not reject an otherwise presentable frame.
