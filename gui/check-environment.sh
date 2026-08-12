#!/usr/bin/env bash
# Compiled and written by BG1KK.
# Privatization and closed-source use are strictly forbidden.
# GNU Radio components are copyrighted by their respective developers.
# All other code copyright © BG1KK.
# This copyright statement must be retained.
set -euo pipefail

fail=0
check_command() {
    if command -v "$1" >/dev/null 2>&1; then
        printf 'OK  command: %s\n' "$1"
    else
        printf 'NO  command: %s\n' "$1" >&2
        fail=1
    fi
}
check_pkg() {
    if pkg-config --exists "$1"; then
        printf 'OK  pkg-config: %s (%s)\n' "$1" "$(pkg-config --modversion "$1")"
    else
        printf 'NO  pkg-config: %s\n' "$1" >&2
        fail=1
    fi
}

echo 'DMR B210 Pi5 GUI build-environment check'
for tool in cmake make g++ pkg-config; do
    check_command "$tool"
done
for package in yaml-cpp sdl2 SDL2_ttf; do
    check_pkg "$package"
done
if [[ -e /dev/dri/card0 ]]; then
    echo 'OK  KMS/DRM device: /dev/dri/card0'
else
    echo 'WARN KMS/DRM device not visible; compilation is still possible'
fi
if [[ "$fail" -ne 0 ]]; then
    echo 'Environment check failed. Install the dependencies listed in ../README.md.' >&2
    exit 1
fi
echo 'Environment check passed.'
