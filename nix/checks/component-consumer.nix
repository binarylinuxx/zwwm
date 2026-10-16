{ lib, stdenv, cmake, ninja, pkg-config, zwayland, zwayland-scanner, zwwm-protocols }:

stdenv.mkDerivation {
  pname = "zwayland-component-consumer-check";
  version = "1";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [ ./CMakeLists.txt ./component-consumer.cpp ./server-consumer.cpp ];
  };
  nativeBuildInputs = [ cmake ninja pkg-config zwayland-scanner ];
  buildInputs = [ zwayland zwwm-protocols ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ./cmake-consumer
    ./pkgconfig-consumer
    runHook postCheck
  '';
  installPhase = ''touch "$out"'';
}
