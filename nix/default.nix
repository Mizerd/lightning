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
    {
      packages = rec {
        lightning-matrix-client = pkgs.qt6.callPackage ./package.nix { };
        default = lightning-matrix-client;
      };
    };

  flake.homeManagerModules = rec {
    lightning-matrix-client = import ./hm-module.nix inputs;
    default = lightning-matrix-client;
  };
}
