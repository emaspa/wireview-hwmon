# The wireview_hwmon kernel module, built against one kernel. The NixOS module
# calls this through config.boot.kernelPackages.callPackage.
{ lib, stdenv, kernel, kernelModuleMakeFlags, src, version }:

stdenv.mkDerivation {
  pname = "wireview-hwmon-module";
  version = "${version}-${kernel.version}";
  inherit src;

  nativeBuildInputs = kernel.moduleBuildDependencies;
  makeFlags = kernelModuleMakeFlags ++ [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
  ];
  buildFlags = [ "module" ];

  installPhase = ''
    runHook preInstall
    install -Dm644 wireview_hwmon.ko \
      $out/lib/modules/${kernel.modDirVersion}/extra/wireview_hwmon.ko
    runHook postInstall
  '';

  meta = {
    description = "hwmon driver for the Thermal Grizzly WireView Pro II";
    homepage = "https://github.com/emaspa/wireview-hwmon";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
    broken = lib.versionOlder kernel.version "6.8";
  };
}
