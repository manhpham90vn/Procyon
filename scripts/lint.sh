#!/usr/bin/env bash
# Read-only checks used locally and in CI. Exits non-zero on the first failing group.
#   scripts/lint.sh            all checks
#   SKIP_TIDY=1 scripts/lint.sh  skip clang-tidy (slow; needs a CMake build for compile_commands.json)
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/dev-env.sh

step() { printf '\n==> %s\n' "$1"; }

step "Generated files are up to date (tokens, process catalog)"
python3 scripts/gen-tokens.py --check
python3 scripts/gen-catalog.py --check

if [ "$(uname -s)" = Darwin ]; then
    step "swift-format"
    swift format lint --strict --recursive --parallel "${SWIFT_PATHS[@]}"
fi

step "clang-format"
if [ -z "$CLANG_FORMAT" ]; then echo "clang-format not found (brew install clang-format, apt install clang-format)" >&2; exit 1; fi
"$CLANG_FORMAT" --dry-run -Werror "${CPP_FILES[@]}"

if [ "${SKIP_TIDY:-0}" != 1 ]; then
    step "clang-tidy"
    if [ -z "$CLANG_TIDY" ]; then echo "clang-tidy not found (brew install llvm, apt install clang-tidy)" >&2; exit 1; fi
    sources=()
    if [ "$(uname -s)" = Darwin ]; then
        cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Debug >/dev/null
        for file in "${CPP_FILES[@]}"; do
            # Windows and Linux sources are not in the macOS compile database.
            case "$file" in core/tools/*|core/src/platform/windows*|core/src/platform/linux*|apps/*) ;; *.cpp) sources+=("$file") ;; esac
        done
        "$CLANG_TIDY" -p build/core --quiet --extra-arg=-isysroot"$(xcrun --show-sdk-path)" "${sources[@]}"
    else
        # The Linux tree builds the shared UI and the GTK app too.
        cmake -S core -B build/linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
        for file in "${CPP_FILES[@]}"; do
            case "$file" in core/tools/*|core/src/platform/windows*|core/src/platform/macos*|core/src/helper_server.cpp|apps/windows/*) ;; *.cpp) sources+=("$file") ;; esac
        done
        "$CLANG_TIDY" -p build/linux-debug --quiet "${sources[@]}"
    fi
fi

if command -v shellcheck >/dev/null; then
    step "shellcheck"
    shellcheck -x "${SHELL_FILES[@]}"
fi

step "python"
python3 -m py_compile scripts/gen-tokens.py scripts/gen-catalog.py scripts/bench-macos.py scripts/bench-windows.py

printf '\nAll checks passed.\n'
