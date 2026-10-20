{ lib, stdenv, cmake, ninja }:

stdenv.mkDerivation {
  pname = "zw-lang";
  version = lib.strings.trim (builtins.readFile ../.version);
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../.version ../LICENSE ../zw-lang ];
  };
  postUnpack = ''sourceRoot+=/zw-lang'';
  nativeBuildInputs = [ cmake ninja ];
  cmakeFlags = [ "-DBUILD_SHARED_LIBS=ON" ];
  meta = {
    description = "Native zw configuration parser and validation library";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
