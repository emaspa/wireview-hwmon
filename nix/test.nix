# NixOS VM test: module loads, wireviewd serves its socket without a device,
# udev rules and group are in place, wireviewctl finds the bundled firmware,
# /etc/wireview belongs to the group and a settings change applies on reload.
{ module }:
{
  name = "wireview-hwmon";

  nodes.machine = { pkgs, ... }: {
    imports = [ module ];
    services.wireview-hwmon.enable = true;
    environment.systemPackages = [ pkgs.curl ];
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

    # The settings directory, then a config created the way a user would
    # and its mode and group from the tmpfiles rule.
    assert machine.succeed("stat -c '%a %U:%G' /etc/wireview").strip() == "750 root:wireview"
    machine.succeed("wireviewctl config set remote_enabled=1 bind=127.0.0.1 port=9876")
    machine.succeed("systemd-tmpfiles --create")
    assert machine.succeed("stat -c '%a %U:%G' /etc/wireview/config").strip() == "660 root:wireview"
    machine.wait_until_succeeds("curl -sf http://127.0.0.1:9876/sensors")
    machine.succeed("wireviewctl config set port=9877")
    machine.wait_until_succeeds("curl -sf http://127.0.0.1:9877/sensors")
    machine.fail("curl -sf http://127.0.0.1:9876/sensors")
    machine.succeed("sed -i s/9877/9878/ /etc/wireview/config && systemctl reload wireviewd")
    machine.wait_until_succeeds("curl -sf http://127.0.0.1:9878/sensors")
    machine.succeed("systemctl is-active wireviewd")
  '';
}
