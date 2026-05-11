{
  description = "MacroSilicon ms912x USB display DRM kernel module";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { self
    , nixpkgs
    , flake-utils
    ,
    }:
    flake-utils.lib.eachDefaultSystem
      (
        system:
        let
          pkgs = import nixpkgs { inherit system; };

          # Extracts checkpatch.pl and its spelling dictionary from the kernel
          # source so the tool version matches the kernel being targeted.
          checkpatch = pkgs.runCommand "checkpatch-${pkgs.linux.version}"
            { nativeBuildInputs = [ pkgs.perl ]; }
            ''
              mkdir -p $out/bin
              tar xf ${pkgs.linux.src} \
                --wildcards --strip-components=1 \
                '*/scripts/checkpatch.pl' \
                '*/scripts/spelling.txt' \
                '*/scripts/const_structs.checkpatch'
              install -m755 scripts/checkpatch.pl $out/bin/checkpatch.pl
              install -m644 scripts/spelling.txt $out/bin/spelling.txt
              install -m644 scripts/const_structs.checkpatch \
                $out/bin/const_structs.checkpatch 2>/dev/null || true
            '';
        in
        {
          packages = {
            default = pkgs.linuxPackages.callPackage ./ms912x.nix { };
            inherit checkpatch;
          };

          devShells.default = pkgs.mkShell {
            packages = [
              checkpatch
              pkgs.lefthook
              pkgs.perl
              pkgs.shellcheck
              pkgs.nixpkgs-fmt
              pkgs.statix
              pkgs.git
            ];

            shellHook = ''
              if [ -d .git ] && [ -w .git/hooks ]; then
                lefthook install
              fi
            '';
          };
        }
      )
    // {
      overlays.default = final: prev: {
        linuxPackages = prev.linuxPackages.extend (
          lpFinal: lpPrev: {
            ms912x = lpPrev.callPackage ./ms912x.nix { };
          }
        );
      };

      nixosModules.default = import ./module.nix self;
    };
}
