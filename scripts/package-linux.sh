#!/usr/bin/env bash
# Packages the release build as dist/Procyon-<version>-linux-<arch>.tar.gz (+ .sha256): the binary
# (fonts embedded), the desktop entry, AppStream metadata, the icon, and the licenses of Procyon
# and of what it embeds (Inter, Nunito: SIL OFL 1.1; Lucide: ISC). Unpacked into a prefix
# (~/.local, /usr/local) it installs like `make install`.
#   scripts/package-linux.sh [version]
set -euo pipefail
cd "$(dirname "$0")/.."

version="${1:-$(tr -d '[:space:]' < VERSION)}"
arch="$(uname -m)"
[ -x build/linux/apps/linux/procyon ] || scripts/build-linux.sh release --no-tests

name="Procyon-$version-linux-$arch"
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
DESTDIR="$staging/$name" cmake --install build/linux --prefix / >/dev/null
cp README.md "$staging/$name/"
mkdir -p dist
tar -C "$staging" -czf "dist/$name.tar.gz" "$name"
(cd dist && sha256sum "$name.tar.gz" > "$name.sha256")
echo "dist/$name.tar.gz"
