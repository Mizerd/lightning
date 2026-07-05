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
    xkeyboard_config
  ];

  shellHook = ''
    # See flake.nix for the rationale — inherited KDE Plasma Qt env vars
    # can drag in a different qtbase major/minor and abort() the app at
    # plugin load. Purge, then set from this shell's qtbase.
    unset QT_PLUGIN_PATH
    unset QT_QPA_PLATFORM_PLUGIN_PATH
    unset QML_IMPORT_PATH
    unset QML2_IMPORT_PATH
    unset QT_QUICK_CONTROLS_STYLE
    unset QT_QUICK_CONTROLS_STYLE_PATH
    unset QT_QPA_PLATFORMTHEME

    export QT_PLUGIN_PATH="${qt.qtbase}/lib/qt-6/plugins:${qt.qtwayland}/lib/qt-6/plugins:${qt.qtsvg}/lib/qt-6/plugins:${qt.qtdeclarative}/lib/qt-6/plugins"
    export QT_QPA_PLATFORM_PLUGIN_PATH="${qt.qtbase}/lib/qt-6/plugins/platforms:${qt.qtwayland}/lib/qt-6/plugins/platforms"
    export QML_IMPORT_PATH="${qt.qtdeclarative}/lib/qt-6/qml"
    export QML2_IMPORT_PATH="$QML_IMPORT_PATH"
    export QT_XKB_CONFIG_ROOT="${pkgs.xkeyboard_config}/share/X11/xkb"

    echo "matrix-client dev shell — Qt ${qt.qtbase.version}"
    echo "Configure:  cmake -S . -B build -G Ninja"
    echo "Build:      cmake --build build"
    echo "Run:        ./build/matrix-client"
  '';
}
