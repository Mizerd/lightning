{
  description = "External production package for the checked-out Lightning source";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfreePredicate = pkg: (pkg.pname or "") == "lightning";
      };
      qt = pkgs.qt6;
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "lightning";
        version = (builtins.fromJSON (builtins.readFile ./dist/version.json)).nix_version;
        src = ./work/lightning;

        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          pkg-config
          rustc
          cargo
          qt.qttools
          qt.wrapQtAppsHook
        ];

        buildInputs = with pkgs; [
          qt.qtbase
          qt.qtdeclarative
          qt.qtsvg
          qt.qtwayland
          libsecret
          glib
          sqlite
          openssl
          xkeyboard_config
        ];

        configurePhase = ''
          runHook preConfigure
          export CARGO_HOME="$TMPDIR/cargo-home"
          mkdir -p "$CARGO_HOME"
          cp ${./work/cargo-config.toml} "$CARGO_HOME/config.toml"
          substituteInPlace "$CARGO_HOME/config.toml" \
            --replace-fail '@VENDOR_DIR@' '${./work/cargo-vendor}'
          cmake -S . -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$out" \
            -DBUILD_TESTING=OFF \
            -DENABLE_RUST_SDK_BACKEND=ON
          runHook postConfigure
        '';

        buildPhase = ''
          runHook preBuild
          cmake --build build --parallel "''${NIX_BUILD_CORES:-2}"
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          cmake --install build
          install -Dm0644 ${./packaging/common/lightning.desktop} \
            "$out/share/applications/lightning.desktop"
          install -Dm0644 ${./packaging/common/lightning.metainfo.xml} \
            "$out/share/metainfo/lightning.metainfo.xml"
          install -Dm0644 ${./packaging/common/copyright} \
            "$out/share/licenses/lightning/copyright"
          runHook postInstall
        '';

        doInstallCheck = true;
        installCheckPhase = ''
          "$out/bin/matrix-client" --version
        '';

        meta = {
          description = "Native Matrix desktop client";
          homepage = "https://gitlab.smetonis.net/Mizerd/lightning";
          license = pkgs.lib.licenses.unfree;
          mainProgram = "matrix-client";
          platforms = [ system ];
        };

        passthru.toolchain = {
          qt = qt.qtbase.version;
          cmake = pkgs.cmake.version;
          rust = pkgs.rustc.version;
        };
      };

      checks.${system}.default = self.packages.${system}.default;
    };
}
