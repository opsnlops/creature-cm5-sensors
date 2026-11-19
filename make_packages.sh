#!/bin/bash
# Build the creature-cm5-sensors, creature-cm5-audio and creature-cm5-revb-workarounds .debs into dist/, check it, and report
# lintian findings. GitHub Actions runs this same script in debian:trixie.
#
# Build on the fleet's Debian release (trixie). Needs debhelper, dh-dkms,
# device-tree-compiler and lintian. All packages are arch: all; each host
# compiles the PAC1954 module itself with DKMS.
set -euo pipefail

cd "$(dirname "$0")"
rm -rf dist && mkdir -p dist

dpkg-buildpackage -us -uc -b
mv ../creature-cm5-sensors_*.deb ../creature-cm5-audio_*.deb ../creature-cm5-revb-workarounds_*.deb \
   ../creature-cm5-sensors_*.buildinfo ../creature-cm5-sensors_*.changes dist/

shopt -s nullglob
debs=(dist/*.deb)
[ "${#debs[@]}" -eq 3 ] || { echo "expected three .debs, found ${#debs[@]}" >&2; exit 1; }
for deb in "${debs[@]}"; do
    ./check_package.sh "$deb"
done

# Report only: lintian findings never fail the build. lintian should not
# run as root.
lintian_cmd=(lintian --display-info --tag-display-limit 0 "${debs[@]}")
if [ "$(id -u)" -eq 0 ]; then
    lintian_cmd=(runuser -u nobody -- "${lintian_cmd[@]}")
fi
if ! lintian_out=$("${lintian_cmd[@]}" 2>&1); then
    echo "lintian did not run cleanly" >&2
fi
if [ -n "$lintian_out" ]; then
    echo "$lintian_out"
    [ -n "${GITHUB_ACTIONS:-}" ] && echo "::warning::lintian reported findings (report-only)"
fi

ls -l "${debs[@]}"
