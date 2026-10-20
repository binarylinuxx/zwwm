{ lib, stdenv, cmake, ninja, pkg-config, zwayland, zwayland-scanner, zwwm-protocols
, libzterm, zw-lang, pango, harfbuzz, libgbm, libglvnd, libepoxy, libdrm, libxkbcommon }:

stdenv.mkDerivation {
  pname = "zterm";
  version = "0.1.0-dev";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../LICENSE ../zterm/CMakeLists.txt ../zterm/src ../zterm/config.zw ../zterm/README.md ];
  };
  postUnpack = ''sourceRoot+=/zterm'';
  nativeBuildInputs = [ cmake ninja pkg-config zwayland-scanner ];
  buildInputs = [ zwayland zwwm-protocols libzterm zw-lang pango harfbuzz libgbm libglvnd libepoxy libdrm libxkbcommon ];
  meta = {
    description = "Experimental GPU-native zwayland terminal";
    homepage = "https://github.com/binarylinuxx/zwwm";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
    mainProgram = "zterm";
  };
}
