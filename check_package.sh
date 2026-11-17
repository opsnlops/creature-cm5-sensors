#!/bin/bash
# Check a built creature-cm5-sensors or creature-cm5-audio .deb: control
# fields and file layout.
# Inspects the archive itself; on a usr-merged system an installed-file
# check cannot tell /lib from /usr/lib.
set -euo pipefail

deb=${1:?usage: check_package.sh <deb>}
fail() { echo "check_package: $*" >&2; exit 1; }

package=$(dpkg-deb -f "$deb" Package)
[ "$(dpkg-deb -f "$deb" Architecture)" = all ] || fail "Architecture is not all"

contents=$(dpkg-deb -c "$deb" | awk '{print $6}')

# Policy 10.1: nothing under aliased top-level directories.
if grep -E '^\./(bin|sbin|lib[^/]*)/' <<<"$contents"; then
    fail "files under an aliased path (Policy 10.1)"
fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
dpkg-deb -x "$deb" "$tmp"
version=$(dpkg-deb -f "$deb" Version)

require() {
    for f; do
        grep -qxF "$f" <<<"$contents" || fail "missing $f"
    done
}

check_audio() {
    dpkg-deb -f "$deb" Depends | grep -q dkms && fail "audio package must not depend on dkms"
    require ./usr/lib/creature-cm5-audio/creature-cm5-audio.dtbo \
        ./usr/lib/udev/rules.d/60-creature-cm5-audio.rules \
        ./usr/share/doc/creature-cm5-audio/copyright
    dts=$(dtc -I dtb -O dts "$tmp/usr/lib/creature-cm5-audio/creature-cm5-audio.dtbo" 2>/dev/null)
    grep -q 'ti,pcm5122' <<<"$dts" || fail "overlay has no ti,pcm5122 node"
    grep -q 'simple-audio-card' <<<"$dts" || fail "overlay has no simple-audio-card node"
    grep -q 'reg = <0x4d>' <<<"$dts" || fail "DAC is not at 0x4d"
    # The udev rule finds the analog card by its overlay node name.
    grep -q 'sound-creature-analog {' <<<"$dts" || fail "overlay sound node renamed; update the udev rule"
}

check_sensors() {
    dpkg-deb -f "$deb" Depends | grep -q 'linux-headers-rpi-2712' ||
        fail "missing linux-headers-rpi-2712 dependency (needed for DKMS on kernel upgrades)"
    dpkg-deb -f "$deb" Depends | grep -q 'dkms' || fail "missing dkms dependency"

    require \
        "./usr/src/pac1954-$version/pac1954.c" \
        "./usr/src/pac1954-$version/Makefile" \
        "./usr/src/pac1954-$version/dkms.conf" \
        ./usr/lib/creature-cm5-sensors/creature-cm5-sensors.dtbo \
        ./usr/lib/udev/rules.d/60-creature-cm5-sensors.rules \
        ./etc/sensors.d/creature-cm5-sensors.conf \
        ./usr/share/man/man4/pac1954.4.gz \
        ./usr/share/doc/creature-cm5-sensors/copyright

    grep -q "PACKAGE_VERSION=\"$version\"" "$tmp/usr/src/pac1954-$version/dkms.conf" ||
        fail "dkms.conf PACKAGE_VERSION is not $version"
    dtc -I dtb -O dts "$tmp/usr/lib/creature-cm5-sensors/creature-cm5-sensors.dtbo" 2>/dev/null |
        grep -q 'microchip,pac1954' || fail "overlay has no microchip,pac1954 node"
}

case "$package" in
    creature-cm5-sensors) check_sensors ;;
    creature-cm5-audio) check_audio ;;
    *) fail "unknown package $package" ;;
esac

echo "check_package: $deb OK"
