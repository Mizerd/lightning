# Non-flake entry point using an explicitly supplied host package set.
{ pkgs }:
let
  outputs = (import ./devShells.nix).perSystem { inherit pkgs; };
in
outputs.devShells.default
