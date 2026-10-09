# NixOS module: services.wireview-hwmon.enable = true; loads the kernel
# module, installs the udev rules and runs wireviewd.
{ src, version, packages }:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.wireview-hwmon;
in
{
  options.services.wireview-hwmon = {
    enable = lib.mkEnableOption "the WireView Pro II hwmon driver and the wireviewd daemon";

    package = lib.mkOption {
      type = lib.types.package;
      default = packages.${pkgs.stdenv.hostPlatform.system}.wireview-hwmon;
      defaultText = lib.literalExpression "wireview-hwmon from this flake";
      description = "Package providing wireviewd, wireviewctl and the udev rules.";
    };

    kernelModule = lib.mkOption {
      type = lib.types.package;
      default = config.boot.kernelPackages.callPackage ./kernel-module.nix { inherit src version; };
      defaultText = lib.literalExpression "wireview_hwmon built for config.boot.kernelPackages";
      description = "The wireview_hwmon kernel module package.";
    };
  };

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [ cfg.kernelModule ];
    boot.kernelModules = [ "wireview_hwmon" ];

    # Serial port and DFU bootloader: dialout + uaccess for the local seat,
    # and ModemManager leaves the device alone.
    services.udev.packages = [ cfg.package ];
    environment.systemPackages = [ cfg.package ];

    # Members may send privileged daemon commands (config writes, NVM,
    # flashing, the GUI's serial handover).
    users.groups.wireview = { };

    # Same unit as the distro packages. /etc/wireview/config is optional and
    # may hold the LAN secret, so it is not generated into the store.
    systemd.services.wireviewd = {
      description = "WireView Pro II hwmon daemon";
      after = [ "local-fs.target" "systemd-modules-load.service" ];
      wantedBy = [ "multi-user.target" ];
      serviceConfig = {
        Type = "simple";
        ExecStart = "${cfg.package}/bin/wireviewd";
        Restart = "on-failure";
        RestartSec = 5;
        ProtectSystem = "strict";
        ReadWritePaths = [ "/run" ];
        LogsDirectory = "wireview";
        LogsDirectoryMode = "0750";
        ConfigurationDirectory = "wireview";
        ConfigurationDirectoryMode = "0700";
        ProtectHome = true;
        PrivateTmp = true;
        UMask = "0077";
        DevicePolicy = "closed";
        DeviceAllow = [ "char-ttyACM rw" "char-misc w" ];
        CapabilityBoundingSet = "";
        NoNewPrivileges = true;
        ProtectKernelTunables = true;
        ProtectKernelModules = true;
        ProtectKernelLogs = true;
        ProtectControlGroups = true;
        ProtectClock = true;
        ProtectHostname = true;
        ProtectProc = "invisible";
        ProcSubset = "pid";
        PrivateIPC = true;
        RestrictNamespaces = true;
        RestrictRealtime = true;
        RestrictSUIDSGID = true;
        LockPersonality = true;
        MemoryDenyWriteExecute = true;
        RestrictAddressFamilies = [ "AF_UNIX" "AF_INET" "AF_INET6" ];
        SystemCallArchitectures = "native";
        SystemCallFilter = [ "@system-service" "~@privileged @resources" ];
        SystemCallErrorNumber = "EPERM";
      };
    };
  };
}
