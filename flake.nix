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

          buildInputs = [
            qt.qtbase
            qt.qtdeclarative
            qt.qtquickcontrols2
            qt.qtsvg
            qt.qtwayland
          ];

          shellHook = ''
            export QT_QPA_PLATFORM_PLUGIN_PATH="${qt.qtbase}/lib/qt-6/plugins/platforms"
            export QML2_IMPORT_PATH="${qt.qtdeclarative}/lib/qt-6/qml:${qt.qtquickcontrols2}/lib/qt-6/qml"
            echo "matrix-client dev shell — Qt ${qt.qtbase.version}"
            echo "Configure:  cmake -S . -B build -G Ninja"
            echo "Build:      cmake --build build"
            echo "Run:        ./build/matrix-client"
          '';
        };

        packages.default = qt.callPackage ./nix/package.nix { };
      });
}
