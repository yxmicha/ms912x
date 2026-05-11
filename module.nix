self:
{ config, lib, pkgs, ... }:
{
  options.hardware.ms912x.enable = lib.mkEnableOption "MacroSilicon ms912x USB display DRM driver";

  config = lib.mkIf config.hardware.ms912x.enable {
    boot.extraModulePackages = [
      (config.boot.kernelPackages.callPackage (self + "/ms912x.nix") { })
    ];
  };
}
