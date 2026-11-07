{
  inputs,
  ...
}:

{
  imports = [
    ./devShells.nix
  ];

  perSystem =
    { pkgs, ... }:
    let
      lightningPkgs = pkgs.extend (import ./overlay.nix);
    in
    {
      packages = rec {
        inherit (lightningPkgs) lightning-matrix-client lightning-matrix-client-debug;
        default = lightning-matrix-client;
      };
    };

  flake.overlays.default = import ./overlay.nix;

  flake.homeManagerModules = rec {
    lightning-matrix-client = import ./hm-module.nix inputs;
    default = lightning-matrix-client;
  };
}
