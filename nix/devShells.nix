{
  perSystem =
    { pkgs, ... }:

    let
      qt = pkgs.qt6;
    in
    {
      devShells.default = pkgs.mkShell {
        name = "lightning-dev";

        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          pkg-config
          gcc
          # Opt-in build accelerators (see the hints printed below). Kept
          # out of CMakeLists.txt so official builds are unaffected and a
          # machine without the tool still builds.
          ccache
          mold
          qt.qttools
          qt.wrapQtAppsHook
          # Rust toolchain, used when ENABLE_RUST_SDK_BACKEND=ON.
          rustc
          cargo
        ];

        # QuickControls2 ships inside qtdeclarative (there is no separate
        # attribute). libsecret + glib back src/storage/LibSecretStore.cpp;
        # xkeyboard_config is needed when the xcb platform plugin loads.
        buildInputs = with pkgs; [
          qt.qtbase
          qt.qtdeclarative
          qt.qtsvg
          qt.qtwayland
          # Inline media playback; nixpkgs builds both the FFmpeg and
          # GStreamer backends.
          qt.qtmultimedia
          # Qt Multimedia dlopens libpipewire-0.3; without it Qt falls back
          # to the PulseAudio client, which crashed on FLAC playback
          # (pa_context_get_state assertion). See LD_LIBRARY_PATH below.
          pipewire
          libsecret
          glib
          xkeyboard_config
          # Call media engine: webrtcbin (gst-plugins-bad), linked directly.
          # libnice provides the nicesrc/nicesink elements webrtcbin needs.
          gst_all_1.gstreamer
          gst_all_1.gst-plugins-base
          gst_all_1.gst-plugins-good
          gst_all_1.gst-plugins-bad
          libnice
        ];

        shellHook = ''
          # Purge Qt paths inherited from the desktop session: a plugin
          # from the system Qt loaded into the flake's Qt fails the version
          # check and aborts silently, before any debug output.
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
          # nix develop does not put buildInputs on the loader path; expose
          # only pipewire so Qt Multimedia can dlopen libpipewire-0.3.
          export LD_LIBRARY_PATH="${pkgs.pipewire}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
          # nixpkgs ships the libnice GStreamer plugin in libnice's own
          # output, which the gst hook does not add.
          export GST_PLUGIN_SYSTEM_PATH_1_0="${pkgs.libnice.out}/lib/gstreamer-1.0''${GST_PLUGIN_SYSTEM_PATH_1_0:+:$GST_PLUGIN_SYSTEM_PATH_1_0}"

          echo "Lightning dev shell — Qt ${qt.qtbase.version}"
          echo "Configure (mock/http):  cmake -S . -B build -G Ninja"
          echo "Configure (+rust):      cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON"
          echo "Build:                  cmake --build build"
          echo "Run:                    ./build/lightning-matrix [--backend={mock,http,rust}]"
          echo "Faster local rebuilds (opt-in, add to any configure line):"
          echo "                        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_LINKER_TYPE=MOLD"
        '';
      };
    };
}
