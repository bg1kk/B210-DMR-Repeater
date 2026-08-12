#!/usr/bin/env bash
# Compiled and written by BG1KK.
# Privatization and closed-source use are strictly forbidden.
# GNU Radio components are copyrighted by their respective developers.
# All other code copyright © BG1KK.
# This copyright statement must be retained.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd "$script_dir/.." && pwd)"
build_dir="${1:-$source_dir/build-gui}"
cmake_prefix="${CMAKE_PREFIX_PATH:-$HOME/.local}"

"$script_dir/check-environment.sh"
cmake -S "$source_dir" -B "$build_dir" -G 'Unix Makefiles' \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$cmake_prefix" \
    -DDMR_B210_SOURCE_BUILD_RPT=OFF \
    -DDMR_B210_SOURCE_BUILD_GUI=ON \
    -DDMR_B210_SOURCE_BUILD_TESTS=ON
echo "Configured: $build_dir"
echo "Build with: cmake --build $build_dir -j\$(nproc)"
