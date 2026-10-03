# Sourced by the format/lint scripts: locates tools and lists the files they work on.
# shellcheck shell=bash
# Variables here are consumed by the scripts that source this file.
# shellcheck disable=SC2034

find_tool() {
    # Homebrew's keg-only LLVM provides clang-format/clang-tidy when they aren't on PATH.
    command -v "$1" 2>/dev/null && return
    for prefix in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
        [ -x "$prefix/$1" ] && { echo "$prefix/$1"; return; }
    done
}

CLANG_FORMAT="$(find_tool clang-format)"
CLANG_TIDY="$(find_tool clang-tidy)"

SWIFT_PATHS=(Package.swift apps scripts/make-icon.swift scripts/bench-probe.swift)
CPP_FILES=()
while IFS= read -r file; do CPP_FILES+=("$file"); done < <(
    find core apps/windows/src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) |
        grep -v 'Tokens.generated.h' | sort
)
SHELL_FILES=(scripts/*.sh)
