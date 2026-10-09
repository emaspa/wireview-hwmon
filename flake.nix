{
  description = "Linux hwmon driver, daemon and CLI for the Thermal Grizzly WireView Pro II";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      # The package bundles Thermal Grizzly's firmware image, which is unfree;
      # allow exactly this one.
      pkgsFor = system: import nixpkgs {
        inherit system;
        config.allowUnfreePredicate = pkg: nixpkgs.lib.getName pkg == "wireview-hwmon";
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (pkgsFor system));
      version = nixpkgs.lib.fileContents ./VERSION;
      src = self;
    in
    {
      packages = forAllSystems (pkgs: rec {
        wireview-hwmon = pkgs.callPackage ./nix/package.nix { inherit src version; };
        # The module for the flake's default kernel; NixOS users get one built
        # for their own kernel from the NixOS module.
        wireview-hwmon-module = pkgs.linuxPackages.callPackage ./nix/kernel-module.nix { inherit src version; };
        default = wireview-hwmon;
      });

      nixosModules.default = import ./nix/module.nix { inherit src version; inherit (self) packages; };

      checks = forAllSystems (pkgs: {
        inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) wireview-hwmon wireview-hwmon-module;
      } // nixpkgs.lib.optionalAttrs (pkgs.stdenv.hostPlatform.system == "x86_64-linux") {
        nixos = pkgs.testers.runNixOSTest (import ./nix/test.nix { module = self.nixosModules.default; });
      });
    };
}
