{
  description = "Native C++/Qt Matrix desktop client";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        qt = pkgs.qt6;
      in {
        devShells.default = pkgs.mkShell {
          name = "matrix-client-dev";

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            gcc
            qt.qttools
            qt.wrapQtAppsHook
            # Rust toolchain — only used when ENABLE_RUST_SDK_BACKEND=ON.
            # Kept in the default shell so contributors can flip the flag
            # without a second shell.
            rustc
            cargo
          ];

          # Qt QuickControls2 ships inside qtdeclarative in the current
          # nixpkgs qt6 attribute set — there is no separate qtquickcontrols2
          # attr, and referencing it makes evaluation fail.
          # libsecret + glib are used by src/storage/LibSecretStore.cpp for
          # Secret Service integration (v0.4).
          # xkeyboard_config is needed by libxkbcommon / xcb-cursor when the
          # xcb platform plugin loads.
          buildInputs = with pkgs; [
            qt.qtbase
            qt.qtdeclarative
            qt.qtsvg
            qt.qtwayland
            # v0.7: native inline video/audio playback (QMediaPlayer /
            # VideoOutput). nixpkgs builds qtmultimedia with BOTH the
            # default FFmpeg backend (ffmpeg-full linked via RPATH — no
            # plugin-path setup needed) and the GStreamer backend.
            qt.qtmultimedia
            libsecret
            glib
            xkeyboard_config
          ];

          shellHook = ''
            # ------------------------------------------------------------
            # v0.4.3: Purge Qt env inherited from a running KDE Plasma /
            # GNOME / etc. session before setting the flake-consistent
            # values.
            #
            # Root cause of the reported crash: a KDE Plasma session on
            # NixOS exports QT_PLUGIN_PATH pointing at the system-wide
            # qtbase (e.g. 6.11.0). Our flake pulls qtbase 6.11.1 from
            # nixos-unstable and the executable is linked against it.
            # When Qt initialises its platform plugin, it walks
            # QT_PLUGIN_PATH first, loads a helper plugin from the 6.11.0
            # tree into the 6.11.1 process, hits the version check, and
            # aborts BEFORE it can print any error — that is why
            # QT_DEBUG_PLUGINS=1 produced no output. Purging first, then
            # setting from ${qt.qtbase} keeps a single Qt in the picture.
            # ------------------------------------------------------------
            unset QT_PLUGIN_PATH
            unset QT_QPA_PLATFORM_PLUGIN_PATH
            unset QML_IMPORT_PATH
            unset QML2_IMPORT_PATH
            unset QT_QUICK_CONTROLS_STYLE
            unset QT_QUICK_CONTROLS_STYLE_PATH
            unset QT_QPA_PLATFORMTHEME

            export QT_PLUGIN_PATH="${qt.qtbase}/lib/qt-6/plugins:${qt.qtwayland}/lib/qt-6/plugins:${qt.qtsvg}/lib/qt-6/plugins:${qt.qtdeclarative}/lib/qt-6/plugins:${qt.qtmultimedia}/lib/qt-6/plugins"
            export QT_QPA_PLATFORM_PLUGIN_PATH="${qt.qtbase}/lib/qt-6/plugins/platforms:${qt.qtwayland}/lib/qt-6/plugins/platforms"
            export QML_IMPORT_PATH="${qt.qtdeclarative}/lib/qt-6/qml:${qt.qtmultimedia}/lib/qt-6/qml"
            export QML2_IMPORT_PATH="$QML_IMPORT_PATH"
            export QT_XKB_CONFIG_ROOT="${pkgs.xkeyboard_config}/share/X11/xkb"
            # Split-prefix Qt: let Qt6Qml's CMake machinery see qtmultimedia's
            # QML-plugin configs (silences the harmless "quickmultimediaplugin
            # ... will not be linked" configure warning; loading stays dynamic).
            export QT_ADDITIONAL_PACKAGES_PREFIX_PATH="${qt.qtmultimedia}"

            echo "matrix-client dev shell — Qt ${qt.qtbase.version}"
            echo "Configure (mock/http):  cmake -S . -B build -G Ninja"
            echo "Configure (+rust):      cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON"
            echo "Build:                  cmake --build build"
            echo "Run:                    ./build/matrix-client [--backend={mock,http,rust}]"
          '';
        };
      });
}
