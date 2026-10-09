# wireviewd, wireviewctl, the udev rules and the bundled firmware image.
{ lib, stdenv, makeWrapper, dfu-util, python3, src, version }:

stdenv.mkDerivation {
  pname = "wireview-hwmon";
  inherit version src;

  nativeBuildInputs = [ makeWrapper ];
  nativeCheckInputs = [ python3 ];

  buildFlags = [ "wireviewd" "wireviewctl" ];

  # "wireviewctl flash" without a file argument flashes the image in this
  # package, not /usr/share.
  env.NIX_CFLAGS_COMPILE =
    ''-DDEFAULT_FIRMWARE_PATH="${placeholder "out"}/share/wireview/TG-WV-PRO2-FW.hex"'';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    make -C tests unit
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 -t $out/bin wireviewd wireviewctl
    # Installed ahead of 73-seat-late.rules, which grants the uaccess tag, so
    # the explicit uaccess builtin the 99- file needs elsewhere is dropped
    # (NixOS verifies rules with a udevadm that lacks it).
    mkdir -p $out/lib/udev/rules.d
    sed 's/, RUN{builtin}+="uaccess"//' 99-wireview-hwmon.rules \
      > $out/lib/udev/rules.d/70-wireview-hwmon.rules
    if grep -q 'RUN{builtin}' $out/lib/udev/rules.d/70-wireview-hwmon.rules; then
      echo "udev rule still calls a builtin" >&2; exit 1
    fi
    install -Dm644 firmware/TG-WV-PRO2-FW.hex $out/share/wireview/TG-WV-PRO2-FW.hex
    wrapProgram $out/bin/wireviewctl --prefix PATH : ${lib.makeBinPath [ dfu-util ]}
    runHook postInstall
  '';

  meta = {
    description = "Daemon and CLI for the Thermal Grizzly WireView Pro II";
    homepage = "https://github.com/emaspa/wireview-hwmon";
    # The tools are GPL-2.0; the bundled firmware image is Thermal Grizzly's,
    # redistributed unmodified (firmware/README.md).
    license = [ lib.licenses.gpl2Only lib.licenses.unfreeRedistributableFirmware ];
    platforms = lib.platforms.linux;
    mainProgram = "wireviewctl";
  };
}
