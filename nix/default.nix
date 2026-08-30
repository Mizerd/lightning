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
}
