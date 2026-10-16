{ lib, stdenv, cmake, ninja, zwayland-scanner, zwwm-protocols }:

stdenv.mkDerivation {
  pname = "zwayland";
  version = lib.strings.trim (builtins.readFile ../.version);

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../.version ../LICENSE ../zwayland ];
  };
  postUnpack = ''sourceRoot+=/zwayland'';

  nativeBuildInputs = [ cmake ninja zwayland-scanner ];
  cmakeFlags = [
    "-DBUILD_SHARED_LIBS=ON"
    "-DZWAYLAND_CORE_XML=${zwwm-protocols}/share/zwwm-protocols/upstream/wayland/wayland.xml"
  ];

  meta = {
    description = "Independent C++23 Wayland client, server, and wire libraries";
    homepage = "https://github.com/binarylinuxx/zwwm";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
