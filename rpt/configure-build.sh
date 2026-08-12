#!/usr/bin/env bash
# Compiled and written by BG1KK.
# Privatization and closed-source use are strictly forbidden.
# GNU Radio components are copyrighted by their respective developers.
# All other code copyright © BG1KK.
# This copyright statement must be retained.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd "$script_dir/.." && pwd)"
build_dir="${1:-$source_dir/build-rpt}"
cmake_prefix="${CMAKE_PREFIX_PATH:-$HOME/.local}"
mbelib_root="${MBELIB_ROOT:-$HOME/.local}"
op25_source="${OP25_REPEATER_SOURCE_DIR:-$HOME/deps-src/op25-master/op25/gr-op25_repeater/lib}"

"$script_dir/check-environment.sh"
cmake -S "$source_dir" -B "$build_dir" -G 'Unix Makefiles' \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$cmake_prefix" \
    -DMBELIB_ROOT="$mbelib_root" \
    -DOP25_REPEATER_SOURCE_DIR="$op25_source" \
    -DDMR_B210_SOURCE_BUILD_RPT=ON \
    -DDMR_B210_SOURCE_BUILD_GUI=OFF \
    -DDMR_B210_SOURCE_BUILD_TESTS=ON
echo "Configured: $build_dir"
echo "Build with: cmake --build $build_dir -j\$(nproc)"
