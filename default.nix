let
  overridez = import ./nix/haskell-overridez.nix;
  config = rec {
    packageOverrides = pkgs: rec {
      haskellPackages =
        with pkgs.haskell.lib; let
          manualOverrides = haskellPackagesNew: haskellPackagesOld: {
          };
        in
          pkgs.haskellPackages.override {
            overrides = overridez.combineAllIn ./nix [ manualOverrides ];
          };
    };
  };
  pkgs = import <nixpkgs> { inherit config; };
in
  { glirc = pkgs.haskellPackages.callPackage ./default-gen.nix {};
  }
