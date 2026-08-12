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

echo 'DMR B210 repeater build-environment check'
for tool in cmake make g++ pkg-config uhd_config_info gnuradio-config-info; do
    check_command "$tool"
done
if command -v g++ >/dev/null 2>&1; then
    g++ --version | head -n 1
fi
for package in yaml-cpp lame; do
    check_pkg "$package"
done

mbelib_roots=()
[[ -n "${MBELIB_ROOT:-}" ]] && mbelib_roots+=("$MBELIB_ROOT")
mbelib_roots+=("$HOME/.local" "$HOME/deps-src/mbelib-master" "$HOME/gr-dsd" /usr /usr/local)
mbelib_header=''
mbelib_library=''
for root in "${mbelib_roots[@]}"; do
    for candidate in "$root/mbelib.h" "$root/include/mbelib.h" "$root/mbelib/mbelib.h"; do
        if [[ -f "$candidate" ]]; then mbelib_header="$candidate"; break 2; fi
    done
done
for root in "${mbelib_roots[@]}"; do
    for candidate in "$root/lib/libmbe.so" "$root/lib/libmbe.so.1.3" "$root/build/libmbe.so" "$root/build/libmbe.so.1.3" "$root/build/mbelib/libmbe.so" "$root/build/mbelib/libmbe.so.1.3"; do
        if [[ -f "$candidate" ]]; then mbelib_library="$candidate"; break 2; fi
    done
done
if [[ -n "$mbelib_header" ]]; then
    echo 'OK  mbelib headers'
else
    echo 'NO  mbelib headers; set MBELIB_ROOT to its installation or source root' >&2
    fail=1
fi
if [[ -n "$mbelib_library" ]]; then
    echo 'OK  libmbe runtime'
else
    echo 'NO  libmbe runtime; set MBELIB_ROOT to its installation or build root' >&2
    fail=1
fi
op25_config="${OP25_REPEATER_CONFIG:-$HOME/.local/lib/cmake/gnuradio-op25_repeater/gnuradio-op25_repeaterConfig.cmake}"
op25_source="${OP25_REPEATER_SOURCE_DIR:-$HOME/deps-src/op25-master/op25/gr-op25_repeater/lib}"
if [[ -f "$op25_config" && -f "$op25_source/bptc19696.cc" ]]; then
    echo 'OK  gr-op25-repeater runtime'
else
    echo 'NO  gr-op25-repeater package or source; set OP25_REPEATER_CONFIG and OP25_REPEATER_SOURCE_DIR' >&2
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo 'Environment check failed. Install the dependencies listed in ../README.md.' >&2
    exit 1
fi
echo 'Environment check passed.'
