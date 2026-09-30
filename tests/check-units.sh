#!/bin/sh
# Static checks on the systemd units. They guard settings that the unit and
# end-to-end tests cannot see, because those run wireviewd outside systemd.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
rc=0
for u in "$top/wireviewd.service" "$top/debian/wireviewd.service"; do
	# A private user namespace maps every command-socket peer except root to
	# uid 65534, which makes the wireview group check refuse group members.
	if grep -Eq '^[[:space:]]*PrivateUsers[[:space:]]*=[[:space:]]*(yes|true|1|on|self|identity)' "$u"; then
		echo "FAIL $u: PrivateUsers must stay off (SO_PEERCRED needs real peer uids)"
		rc=1
	fi
done
# The two units differ only in where the daemon binary lives.
a=$(grep -Ev '^(ExecStart=|#)' "$top/wireviewd.service")
b=$(grep -Ev '^(ExecStart=|#)' "$top/debian/wireviewd.service")
if [ "$a" != "$b" ]; then
	echo "FAIL the two wireviewd.service files differ in more than ExecStart"
	rc=1
fi
[ $rc -eq 0 ] && echo "units: OK"
exit $rc
