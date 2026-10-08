#!/usr/bin/env bash
# Rewrites sources in place: swift-format (Swift) and clang-format (C/C++).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/dev-env.sh

python3 scripts/gen-tokens.py >/dev/null
if [ "$(uname -s)" = Darwin ]; then swift format format --in-place --recursive --parallel "${SWIFT_PATHS[@]}"; fi
if [ -n "$CLANG_FORMAT" ]; then
    "$CLANG_FORMAT" -i "${CPP_FILES[@]}"
else
    echo "clang-format not found (brew install clang-format, apt install clang-format); C/C++ left untouched" >&2
fi
echo "Formatted."
