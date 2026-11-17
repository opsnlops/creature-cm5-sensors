#!/bin/bash
# Build the creature-cm5-sensors .deb into dist/, check it, and report
# lintian findings. GitHub Actions runs this same script in debian:trixie.
#
# Build on the fleet's Debian release (trixie). Needs debhelper, dh-dkms,
# device-tree-compiler and lintian. The package is arch: all; each host
# compiles the kernel module itself with DKMS.
set -euo pipefail

cd "$(dirname "$0")"
rm -rf dist && mkdir -p dist

dpkg-buildpackage -us -uc -b
mv ../creature-cm5-sensors_*.deb ../creature-cm5-sensors_*.buildinfo \
   ../creature-cm5-sensors_*.changes dist/

shopt -s nullglob
debs=(dist/creature-cm5-sensors_*.deb)
[ "${#debs[@]}" -eq 1 ] || { echo "expected one .deb, found ${#debs[@]}" >&2; exit 1; }
./check_package.sh "${debs[0]}"

# Report only: lintian findings never fail the build. lintian should not
# run as root.
lintian_cmd=(lintian --display-info --tag-display-limit 0 "${debs[0]}")
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

ls -l "${debs[0]}"
