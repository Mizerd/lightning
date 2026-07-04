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
    rustc
    cargo
  ];

  # QuickControls2 lives inside qtdeclarative in current nixpkgs — no
  # separate qtquickcontrols2 attribute is available.
  buildInputs = with pkgs; [
    qt.qtbase
    qt.qtdeclarative
    qt.qtsvg
    qt.qtwayland
    libsecret
    glib
  ];

  shellHook = ''
    echo "matrix-client dev shell — Qt ${qt.qtbase.version}"
    echo "Configure:  cmake -S . -B build -G Ninja"
    echo "Build:      cmake --build build"
    echo "Run:        ./build/matrix-client"
  '';
}
