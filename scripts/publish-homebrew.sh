#!/usr/bin/env bash
# Points the procyon cask in the Homebrew tap at a release, from packaging/homebrew/procyon.rb.
#   HOMEBREW_TAP_TOKEN=… scripts/publish-homebrew.sh 0.2.0
# Uses dist/Procyon-<version>.dmg when it is there (the release job), otherwise downloads it from
# the GitHub release. HOMEBREW_TAP_TOKEN needs contents write access to the tap.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=${1:?usage: publish-homebrew.sh VERSION (without the v prefix)}
VERSION=${VERSION#v}
TAP_REPO=${TAP_REPO:-manhpham90vn/homebrew-tap}
TAP_BRANCH=${TAP_BRANCH:-main}

: "${HOMEBREW_TAP_TOKEN:?publish-homebrew.sh: HOMEBREW_TAP_TOKEN must hold a token with contents write access to $TAP_REPO.}"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/procyon-homebrew.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

DMG="dist/Procyon-$VERSION.dmg"
if [ ! -f "$DMG" ]; then
    gh release download "v$VERSION" --repo manhpham90vn/Procyon --pattern "Procyon-$VERSION.dmg" --dir "$WORK"
    DMG="$WORK/Procyon-$VERSION.dmg"
fi
SHA256=$(shasum -a 256 "$DMG" | cut -d' ' -f1)

git clone --quiet --depth 1 "https://x-access-token:$HOMEBREW_TAP_TOKEN@github.com/$TAP_REPO.git" "$WORK/tap"
mkdir -p "$WORK/tap/Casks"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@SHA256@/$SHA256/g" packaging/homebrew/procyon.rb > "$WORK/tap/Casks/procyon.rb"

cd "$WORK/tap"
git add Casks/procyon.rb
if git diff --cached --quiet; then
    echo "[ok]      $TAP_REPO already at $VERSION"
    exit 0
fi
git -c user.name="github-actions[bot]" \
    -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
    commit --quiet -m "procyon $VERSION"
git push --quiet origin "HEAD:refs/heads/$TAP_BRANCH"
echo "[ok]      $TAP_REPO -> $VERSION"
