{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  gcc,
  ccache,
  mold,
  rustc,
  cargo,
  rustPlatform,
  qt6,
  kdePackages,
  pipewire,
  libsecret,
  glib,
  xkeyboard_config,
  gst_all_1,
  libnice,
  version ? "git",
  withBuildAccelerators ? false,
  withRustBackend ? true,
}:

let
  gst = with gst_all_1; [
    gstreamer
    gst-plugins-base
    gst-plugins-good
    gst-plugins-bad
    libnice # makeSearchPathOutput falls back to .out
    # pipewire ships libgstpipewire (pipewiresrc); screen share needs it.
    pipewire
  ];
in
stdenv.mkDerivation {
  pname = "lightning-matrix-client";
  src = ../.;
  inherit version;

  cargoRoot = "rust";
  cargoDeps = rustPlatform.importCargoLock {
    lockFile = ../rust/Cargo.lock;
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    gcc
    qt6.qttools
    qt6.wrapQtAppsHook
    rustPlatform.cargoSetupHook
  ]
  ++ lib.optionals withRustBackend [
    rustc
    cargo
  ]
  ++ lib.optionals withBuildAccelerators [
    ccache
    mold
  ];
  buildInputs = [
    qt6.qtbase
    qt6.qtdeclarative
    qt6.qtsvg
    qt6.qtwayland
    qt6.qtmultimedia
    pipewire
    libsecret
    glib
    xkeyboard_config
    # image format support, test using `lightning-matrix --image-format-status`
    qt6.qtimageformats # webp
    kdePackages.kimageformats # jxl
  ]
  ++ gst;

  cmakeFlags = [
    (lib.cmakeBool "ENABLE_RUST_SDK_BACKEND" withRustBackend)
  ]
  ++ lib.optionals withBuildAccelerators [
    (lib.cmakeFeature "CMAKE_CXX_COMPILER_LAUNCHER" "ccache")
    (lib.cmakeFeature "CMAKE_LINKER_TYPE" "MOLD")
  ];

  qtWrapperArgs = [
    "--prefix LD_LIBRARY_PATH : ${
      lib.makeLibraryPath [
        # Fix qt.multimedia.symbolsresolver not loading pipewire library
        # from https://github.com/NixOS/nixpkgs/issues/409850#issuecomment-3795701197
        pipewire
      ]
    }"
    "--prefix GST_PLUGIN_PATH : ${lib.makeSearchPathOutput "lib" "lib/gstreamer-1.0" gst}"
  ];

  meta = {
    homepage = "https://www.lightning-matrix.org";
    description = "Native Qt 6/QML Matrix desktop client (C++20 + official Rust Matrix SDK)";
    license = lib.licenses.gpl3Plus;
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
      "x86_64-darwin"
      "aarch64-darwin"
    ];
    mainProgram = "lightning-matrix";
  };
}
