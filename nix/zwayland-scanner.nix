{ lib, stdenv, cmake, ninja, pkg-config, libxml2 }:

stdenv.mkDerivation {
  pname = "zwayland-scanner";
  version = lib.strings.trim (builtins.readFile ../.version);

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../.version ../LICENSE ../zwayland-scanner ];
  };
  postUnpack = ''sourceRoot+=/zwayland-scanner'';

  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [ libxml2 ];

  meta = {
    description = "Native C++23 Wayland client/server binding generator";
    homepage = "https://github.com/binarylinuxx/zwwm";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
    mainProgram = "zwayland-scanner";
  };
}
