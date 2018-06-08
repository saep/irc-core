let
  pkgs = import <nixpkgs> {};
  overridez = fetchTarball {
    url = "https://github.com/adetokunbo/haskell-overridez/archive/master.tar.gz";
    sha256 = "03a3fi1398ms0lbrmljphhgh4b6mlb80hw8ilfqdpjbg303z5cp1";
  };
in
  import overridez { inherit pkgs; }
