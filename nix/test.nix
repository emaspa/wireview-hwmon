# NixOS VM test: module loads, wireviewd serves its socket without a device,
# udev rules and group are in place, wireviewctl finds the bundled firmware.
{ module }:
{
  name = "wireview-hwmon";

  nodes.machine = {
    imports = [ module ];
    services.wireview-hwmon.enable = true;
  };

  testScript = ''
    machine.wait_for_unit("multi-user.target")
    machine.succeed("lsmod | grep -q '^wireview_hwmon'")
    machine.succeed("test -c /dev/wireview-hwmon")
    machine.wait_for_unit("wireviewd.service")
    machine.wait_until_succeeds("test -S /run/wireviewd.sock")
    machine.succeed("getent group wireview")
    machine.succeed("test -f /etc/udev/rules.d/70-wireview-hwmon.rules")
    fw = machine.succeed("wireviewctl --help 2>&1 | grep -o '/nix/store/[^ ]*TG-WV-PRO2-FW.hex'").strip()
    machine.succeed(f"test -s {fw}")
    machine.succeed("grep -q dfu-util $(command -v wireviewctl)")
    machine.fail("wireviewctl info")
    machine.succeed("systemctl is-active wireviewd")
  '';
}
