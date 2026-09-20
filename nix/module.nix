{ self }:
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.zwwm;
  defaultPackage = if cfg.xwayland.enable
    then self.packages.${pkgs.stdenv.hostPlatform.system}.zwwm
    else self.packages.${pkgs.stdenv.hostPlatform.system}.zwwm-no-xwayland;
  package = if cfg.package == null then defaultPackage else cfg.package;
in {
  options.programs.zwwm = {
    enable = lib.mkEnableOption "the zwwm Wayland compositor";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "zwwm package to install. Null selects the package from this flake.";
    };

    xwayland.enable = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Build zwwm with Xwayland integration and enable the Xwayland runtime.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ package ];
    services.displayManager.sessionPackages = [ package ];
    programs.xwayland.enable = lib.mkIf cfg.xwayland.enable true;
    hardware.graphics.enable = true;
    xdg.portal = {
      enable = true;
      extraPortals = [ package pkgs.xdg-desktop-portal-gtk ];
      config.zwwm.default = [ "zwwm" "gtk" ];
    };
  };
}
