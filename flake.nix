{
  description = "zwwm: an event-driven C++ Wayland compositor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          mkZwwm = { xwayland ? true }:
            pkgs.callPackage ./nix/package.nix {
              inherit self;
              xwaylandSupport = xwayland;
              wrapQtAppsHook = pkgs.qt6.wrapQtAppsHook;
            };
        in {
          default = mkZwwm { };
          zwwm = mkZwwm { };
          zwwm-no-xwayland = mkZwwm { xwayland = false; };
        });

      nixosModules.default = import ./nix/module.nix { inherit self; };
      nixosModules.zwwm = self.nixosModules.default;

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              meson
              ninja
              pkg-config
              clang
              dbus
              dbus.dev
              libffi
              libepoxy
              fontconfig
              freetype
              libglvnd
              libdrm
              libgbm
              libinput
              libxml2
              libxkbcommon
              libXcursor
              wayland
              libxcb
              pipewire
              pipewire.dev
              qt6.qtbase
              qt6.qtbase.dev
              seatd
              systemd
              xdg-desktop-portal
              xdg-desktop-portal-gtk
            ];
            shellHook = ''
              export CC=clang
              export CXX=clang++
              export XDG_CURRENT_DESKTOP=zwwm
              export XDG_SESSION_DESKTOP=zwwm
              export NIX_XDG_DESKTOP_PORTAL_DIR="$HOME/.local/share/xdg-desktop-portal/portals"
            '';
          };
        });
    };
}
