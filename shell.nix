# Non-flake fallback dev shell.
# Prefer `nix develop` (flake.nix) when possible.
{ pkgs ? import <nixpkgs> { } }:

let
  qt = pkgs.qt6;
in
pkgs.mkShell {
  name = "matrix-client-dev";

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    gcc
    qt.qttools
    qt.wrapQtAppsHook
  ];

  buildInputs = [
    qt.qtbase
    qt.qtdeclarative
    qt.qtquickcontrols2
    qt.qtsvg
    qt.qtwayland
  ];

  shellHook = ''
    echo "matrix-client dev shell — Qt ${qt.qtbase.version}"
    echo "Configure:  cmake -S . -B build -G Ninja"
    echo "Build:      cmake --build build"
    echo "Run:        ./build/matrix-client"
  '';
}
