#!/bin/bash
# Check a built creature-cm5-sensors .deb: control fields and file layout.
# Inspects the archive itself; on a usr-merged system an installed-file
# check cannot tell /lib from /usr/lib.
set -euo pipefail

deb=${1:?usage: check_package.sh <deb>}
fail() { echo "check_package: $*" >&2; exit 1; }

[ "$(dpkg-deb -f "$deb" Package)" = creature-cm5-sensors ] || fail "wrong Package"
[ "$(dpkg-deb -f "$deb" Architecture)" = all ] || fail "Architecture is not all"
dpkg-deb -f "$deb" Depends | grep -q 'linux-headers-rpi-2712' ||
    fail "missing linux-headers-rpi-2712 dependency (needed for DKMS on kernel upgrades)"
dpkg-deb -f "$deb" Depends | grep -q 'dkms' || fail "missing dkms dependency"

contents=$(dpkg-deb -c "$deb" | awk '{print $6}')

# Policy 10.1: nothing under aliased top-level directories.
if grep -E '^\./(bin|sbin|lib[^/]*)/' <<<"$contents"; then
    fail "files under an aliased path (Policy 10.1)"
fi

version=$(dpkg-deb -f "$deb" Version)
for f in \
    "./usr/src/pac1954-$version/pac1954.c" \
    "./usr/src/pac1954-$version/Makefile" \
    "./usr/src/pac1954-$version/dkms.conf" \
    ./usr/lib/creature-cm5-sensors/creature-cm5-sensors.dtbo \
    ./usr/lib/udev/rules.d/60-creature-cm5-sensors.rules \
    ./etc/sensors.d/creature-cm5-sensors.conf \
    ./usr/share/man/man4/pac1954.4.gz \
    ./usr/share/doc/creature-cm5-sensors/copyright; do
    grep -qxF "$f" <<<"$contents" || fail "missing $f"
done

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
dpkg-deb -x "$deb" "$tmp"
grep -q "PACKAGE_VERSION=\"$version\"" "$tmp/usr/src/pac1954-$version/dkms.conf" ||
    fail "dkms.conf PACKAGE_VERSION is not $version"
dtc -I dtb -O dts "$tmp/usr/lib/creature-cm5-sensors/creature-cm5-sensors.dtbo" 2>/dev/null |
    grep -q 'microchip,pac1954' || fail "overlay has no microchip,pac1954 node"

echo "check_package: $deb OK"
