{ lib
, stdenv
, self
, xwaylandSupport ? true
, cmake
, ninja
, pkg-config
, makeWrapper
, wrapQtAppsHook
, dbus
, libepoxy
, fontconfig
, freetype
, libglvnd
, libdrm
, libgbm
, libinput
, libxml2
, libxkbcommon
, libXcursor
, libxcb
, pipewire
, qt6
, seatd
, systemd
, xdg-desktop-portal
, xdg-desktop-portal-gtk
, xwayland
}:

stdenv.mkDerivation {
  pname = "zwwm";
  version = "0.1.0-alpha.1";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../README.md
      ../CONFIG.md
      ../CHANGELOG.md
      ../CONTRIBUTING.md
      ../LICENSE
      ../SECURITY.md
      ../THIRD_PARTY.md
      ../zwwm-session
      ../build-aux
      ../xdg-desktop-portal-zwwm
      ../zw-lang
      ../zwayland-scanner
      ../zwayland
      ../zwwm-egl
      ../zwwm-protocols
      ../zwwm-renderer
      ../zwwm
      ../zwwmctl
    ];
  };

  nativeBuildInputs = [ cmake ninja pkg-config makeWrapper wrapQtAppsHook ];

  buildInputs = [
    dbus
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
    pipewire
    qt6.qtbase
    seatd
    systemd
    xdg-desktop-portal
  ] ++ lib.optional xwaylandSupport libxcb;

  cmakeFlags = [
    (lib.cmakeBool "BUILD_TESTING" false)
    (lib.cmakeBool "XWAYLAND_ENABLE" xwaylandSupport)
  ];

  dontWrapQtApps = true;

  postFixup = ''
    wrapQtApp "$out/bin/xdg-desktop-portal-zwwm"
    wrapProgram "$out/bin/zwwm-session" \
      --prefix PATH : "$out/bin:${lib.makeBinPath ([ xdg-desktop-portal xdg-desktop-portal-gtk ] ++ lib.optional xwaylandSupport xwayland)}" \
      --prefix XDG_DATA_DIRS : "$out/share" \
      --set NIX_XDG_DESKTOP_PORTAL_DIR "$out/share/xdg-desktop-portal/portals"
  '';

  passthru = {
    inherit xwaylandSupport;
    providedSessions = [ "zwwm" ];
  };

  meta = {
    description = "C++23 post-Wayland compositor";
    homepage = "https://github.com/binarylinuxx/zwwm";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
    mainProgram = "zwwm-session";
  };
}
