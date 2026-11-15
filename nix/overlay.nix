# Evaluate with the caller's package set, including its Qt and toolchain.
final: _prev:
let
  package = final.qt6.callPackage ./package.nix { };
in
{
  lightning-matrix-client = package;
  lightning-matrix-client-debug = package.overrideAttrs (old: {
    pname = "${old.pname}-debug";
    cmakeBuildType = "RelWithDebInfo";
    dontStrip = true;
    # CMake selects Cargo's dev profile for all non-Release builds.
    # Keep optimization and Rust line information in the debug package.
    env = (old.env or { }) // {
      CARGO_PROFILE_DEV_OPT_LEVEL = "2";
      CARGO_PROFILE_DEV_DEBUG = "1";
      CARGO_PROFILE_DEV_STRIP = "none";
      # Keep Rust symbols in the installed binary, not build-tree sidecars.
      CARGO_PROFILE_DEV_SPLIT_DEBUGINFO = "off";
    };
  });
}
