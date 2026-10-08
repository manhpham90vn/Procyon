#!/usr/bin/env bash
# Builds the core, CLI, tests and the GTK app with CMake + Ninja (the counterpart of
# build-windows.cmd), then copies the app to dist/linux/procyon.
#   scripts/build-linux.sh [release|debug] [--no-tests]
# release builds into build/linux, debug into build/linux-debug (no copy to dist).
set -euo pipefail
cd "$(dirname "$0")/.."

config=release
tests=1
for arg in "$@"; do
    case "$arg" in
        release | debug) config="$arg" ;;
        --no-tests) tests=0 ;;
        *) echo "usage: $0 [release|debug] [--no-tests]" >&2; exit 2 ;;
    esac
done

missing=()
for tool in cmake ninja pkg-config c++; do command -v "$tool" >/dev/null || missing+=("$tool"); done
if [ ${#missing[@]} -eq 0 ] && ! pkg-config --exists gtk4; then missing+=(gtk4); fi
if [ ${#missing[@]} -gt 0 ]; then
    echo "missing: ${missing[*]} (make tools lists the packages to install)" >&2
    exit 1
fi

if [ "$config" = release ]; then dir=build/linux; type=Release; else dir=build/linux-debug; type=Debug; fi
options=(-G Ninja -DCMAKE_BUILD_TYPE="$type")
# The release job passes the tag's version; otherwise apps/ui reads the VERSION file.
if [ -n "${PROCYON_VERSION:-}" ]; then options+=(-DPROCYON_VERSION="$PROCYON_VERSION"); else options+=(-UPROCYON_VERSION); fi
cmake -S core -B "$dir" "${options[@]}"
cmake --build "$dir"
if [ "$tests" = 1 ]; then ctest --test-dir "$dir" --output-on-failure; fi
if [ "$config" = release ]; then
    mkdir -p dist/linux
    cp "$dir/apps/linux/procyon" dist/linux/procyon
    echo "dist/linux/procyon"
fi
