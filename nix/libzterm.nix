{ lib, stdenv, cmake, ninja }:

stdenv.mkDerivation {
  pname = "libzterm";
  version = "0.1.0-dev";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../LICENSE ../zterm/libzterm ];
  };
  postUnpack = ''sourceRoot+=/zterm/libzterm'';
  nativeBuildInputs = [ cmake ninja ];
  cmakeFlags = [ "-DBUILD_SHARED_LIBS=ON" ];
  meta = {
    description = "Native terminal state and VT engine for zterm";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
