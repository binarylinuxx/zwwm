{ self }:
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.zwwm;
  defaultPackage = if cfg.xwayland.enable
    then self.packages.${pkgs.stdenv.hostPlatform.system}.zwwm
    else self.packages.${pkgs.stdenv.hostPlatform.system}.zwwm-no-xwayland;
  package = if cfg.package == null then defaultPackage else cfg.package;
  ztermPackage = if cfg.zterm.package == null
    then self.packages.${pkgs.stdenv.hostPlatform.system}.zterm
    else cfg.zterm.package;
  zw = import ./zw-format.nix { inherit lib; };
  compositorConfig = zw.render cfg.settings + cfg.extraConfig;
  ztermConfig = zw.render cfg.zterm.settings + cfg.zterm.extraConfig;
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

    settings = lib.mkOption {
      type = zw.type;
      default = { };
      example = lib.literalExpression ''{
        layout = { default = "master-stack"; inner-gap = 8; };
        bind = [ [ "Mod4" "Return" "exec" "zterm" ] ];
      }'';
      description = "Recursive native zw settings written to /etc/xdg/zwwm/config.zw. Bindings may be a list of four-element lists.";
    };

    extraConfig = lib.mkOption {
      type = lib.types.lines;
      default = "";
      description = "Native zw configuration appended after programs.zwwm.settings.";
    };

    zterm = {
      enable = lib.mkEnableOption "the Wayland-native zterm terminal";

      package = lib.mkOption {
        type = lib.types.nullOr lib.types.package;
        default = null;
        description = "zterm package to install. Null selects the package from this flake.";
      };

      settings = lib.mkOption {
        type = zw.type;
        default = { };
        example = lib.literalExpression ''{
          font = { family = "JetBrainsMono Nerd Font Mono"; size = 14; weight = 400; };
          cursor = { shape = "beam"; blink = true; };
          colors = { red = "#e06c75"; bright-red = "#ff7a85"; };
        }'';
        description = "Recursive native zw settings written to /etc/xdg/zterm/config.zw. Supports font, rendering, colors, cursor, input, window, shell, and scrollback settings.";
      };

      extraConfig = lib.mkOption {
        type = lib.types.lines;
        default = "";
        description = "Native zw configuration appended after programs.zwwm.zterm.settings.";
      };
    };
  };

  config = lib.mkMerge [ (lib.mkIf cfg.enable {
    environment.systemPackages = [ package ];
    services.displayManager.sessionPackages = [ package ];
    programs.xwayland.enable = lib.mkIf cfg.xwayland.enable true;
    hardware.graphics.enable = true;
    xdg.portal = {
      enable = true;
      extraPortals = [ package pkgs.xdg-desktop-portal-gtk ];
      config.zwwm.default = [ "zwwm" "gtk" ];
    };
    environment.etc."xdg/zwwm/config.zw" = lib.mkIf (compositorConfig != "") { text = compositorConfig; };
  }) (lib.mkIf cfg.zterm.enable {
    environment.systemPackages = [ ztermPackage ];
    environment.etc."xdg/zterm/config.zw" = lib.mkIf (ztermConfig != "") { text = ztermConfig; };
  }) ];
}
