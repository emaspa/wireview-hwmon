#!/bin/sh
# Boot the running kernel in QEMU with a busybox initramfs, load the freshly
# built wireview_hwmon.ko and run bind_test plus rmmod/insmod cycles under
# concurrent sysfs reads. Needs qemu-system-x86_64, a static busybox, static
# libc, sudo to read /boot/vmlinuz (root-only on Ubuntu), and the module
# built for the running kernel (make module).
#
#   tests/kmod/run-vm-test.sh [stress-rounds]
#
# SPDX-License-Identifier: GPL-2.0
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
rounds=${1:-2000}
kver=$(uname -r)
ko=$top/wireview_hwmon.ko
[ -f "$ko" ] || { echo "build the module first: make module" >&2; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

sudo -n cat "/boot/vmlinuz-$kver" > "$work/vmlinuz"
cc -static -O2 -Wall -Wextra -o "$work/bind_test" "$here/bind_test.c"

root=$work/root
mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys" "$root/tmp"
cp "$(command -v busybox)" "$root/bin/busybox"
for a in sh mount cat ls insmod rmmod lsmod echo sleep poweroff grep seq kill dmesg true; do
	ln -s busybox "$root/bin/$a"
done
cp "$ko" "$root/wireview_hwmon.ko"
cp "$work/bind_test" "$root/bind_test"
cat > "$root/init" <<INIT
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev
echo 1 > /proc/sys/kernel/panic_on_oops
rc=0
insmod /wireview_hwmon.ko || rc=1
/bind_test $rounds || rc=1
# rmmod is refused while a file is open, and works under readers.
( exec 3>/dev/wireview-hwmon; rmmod wireview_hwmon 2>/dev/null && echo "FAIL rmmod with an open file" ) || true
lsmod | grep -q wireview_hwmon && echo "ok   rmmod refused while a file is open" || { echo "FAIL module gone"; rc=1; }
for i in 1 2 3 4; do ( while true; do cat /sys/class/hwmon/hwmon*/* >/dev/null 2>&1; done ) & done
for i in \$(seq 1 50); do
	rmmod wireview_hwmon || { echo "FAIL rmmod cycle \$i"; rc=1; break; }
	insmod /wireview_hwmon.ko || { echo "FAIL insmod cycle \$i"; rc=1; break; }
done
echo "ok   50 rmmod/insmod cycles under concurrent sysfs reads"
dmesg | grep -E "BUG|WARNING|Oops|refcount|use-after|general protection" && { echo "FAIL kernel complained"; rc=1; }
echo "VMTEST-RESULT \$rc"
poweroff -f
INIT
chmod +x "$root/init"
(cd "$root" && find . | cpio -o -H newc --quiet) | gzip > "$work/initrd.gz"

kvm=""
[ -w /dev/kvm ] && kvm="-enable-kvm -cpu host"
timeout 600 qemu-system-x86_64 $kvm -m 1024 -smp 4 -nographic -no-reboot \
	-kernel "$work/vmlinuz" -initrd "$work/initrd.gz" \
	-append "console=ttyS0 quiet panic=-1 slab_debug=FZPU init=/init" \
	| tee "$work/console.log" | grep -E '^(ok|FAIL|bind_test|VMTEST)|BUG|Oops|WARNING'
grep -q 'VMTEST-RESULT 0' "$work/console.log"
