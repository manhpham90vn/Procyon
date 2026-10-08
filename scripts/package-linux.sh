#!/usr/bin/env bash
# Packages the release build as dist/Procyon-<version>-linux-<arch>.tar.gz (+ .sha256): the binary
# (fonts embedded), the desktop entry, AppStream metadata, the icon, and the licenses of Procyon
# and of what it embeds (Inter, Nunito: SIL OFL 1.1; Lucide: ISC). Unpacked into a prefix
# (~/.local, /usr/local) it installs like `make install`. Where dpkg-deb is available (Debian,
# Ubuntu), also dist/procyon_<version>_<arch>.deb, which installs the same files into /usr with GTK 4
# as a dependency, and its SHA-256 in the same .sha256 file.
#   scripts/package-linux.sh [version]
set -euo pipefail
cd "$(dirname "$0")/.."

version="${1:-$(tr -d '[:space:]' < VERSION)}"
arch="$(uname -m)"
[ -x build/linux/apps/linux/procyon ] || scripts/build-linux.sh release --no-tests

name="Procyon-$version-linux-$arch"
umask 022  # packaged files and directories readable by all, whatever the caller's umask
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
DESTDIR="$staging/$name" cmake --install build/linux --prefix / >/dev/null
cp README.md "$staging/$name/"
mkdir -p dist
tar -C "$staging" -czf "dist/$name.tar.gz" "$name"
echo "dist/$name.tar.gz"
sums=("$name.tar.gz")

if command -v dpkg-deb >/dev/null; then
    deb_arch="$(dpkg --print-architecture)"
    # Debian sorts "~" before the release, so 1.0.0~rc.1 comes before 1.0.0, like 1.0.0-rc.1 does in semver.
    deb_version="${version/-/\~}"
    deb="procyon_${deb_version}_$deb_arch.deb"
    root="$staging/deb"
    DESTDIR="$root" cmake --install build/linux --prefix /usr >/dev/null
    # dpkg-shlibdeps turns the libraries the binary links against into package dependencies.
    mkdir -p "$staging/shlibs/debian"
    printf 'Source: procyon\n\nPackage: procyon\nArchitecture: any\n' > "$staging/shlibs/debian/control"
    depends="$(cd "$staging/shlibs" && dpkg-shlibdeps -O "$root/usr/bin/procyon" 2>/dev/null)"
    depends="${depends#shlibs:Depends=}"
    mkdir -p "$root/DEBIAN"
    cat > "$root/DEBIAN/control" <<CONTROL
Package: procyon
Version: $deb_version
Architecture: $deb_arch
Maintainer: Manh Pham <manhpham90vn@icloud.com>
Installed-Size: $(du -sk --exclude=DEBIAN "$root" | cut -f1)
Depends: $depends
Recommends: systemd
Section: utils
Priority: optional
Homepage: https://github.com/manhpham90vn/Procyon
Description: Lightweight task manager
 See what is using your CPU, memory, disk, network and GPU, find out what a
 process is, and stop the ones you don't need.
CONTROL
    dpkg-deb --root-owner-group --build "$root" "dist/$deb" >/dev/null
    echo "dist/$deb"
    sums+=("$deb")
fi

(cd dist && sha256sum "${sums[@]}" > "$name.sha256")
