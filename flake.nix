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
          ];

          # Qt QuickControls2 ships inside qtdeclarative in the current
          # nixpkgs qt6 attribute set — there is no separate qtquickcontrols2
          # attr, and referencing it makes evaluation fail.
          # libsecret + glib are used by src/storage/LibSecretStore.cpp for
          # Secret Service integration (v0.4).
          buildInputs = with pkgs; [
            qt.qtbase
            qt.qtdeclarative
            qt.qtsvg
            qt.qtwayland
            libsecret
            glib
          ];

          shellHook = ''
            export QT_QPA_PLATFORM_PLUGIN_PATH="${qt.qtbase}/lib/qt-6/plugins/platforms"
            export QML2_IMPORT_PATH="${qt.qtdeclarative}/lib/qt-6/qml"
            echo "matrix-client dev shell — Qt ${qt.qtbase.version}"
            echo "Configure (mock/http):  cmake -S . -B build -G Ninja"
            echo "Configure (+rust):      cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON"
            echo "Build:                  cmake --build build"
            echo "Run:                    ./build/matrix-client [--backend={mock,http,rust}]"
          '';
        };
      });
}
