#!/usr/bin/env bash
# Packages dist/Procyon.app into a DMG and a zip with SHA-256 checksums.
#   scripts/package-macos.sh <version>
# With NOTARY_PROFILE set (an `xcrun notarytool store-credentials` profile), or APPLE_ID +
# APPLE_TEAM_ID + APPLE_APP_PASSWORD, the DMG is notarized and stapled.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: package-macos.sh <version>}"
APP="dist/Procyon.app"
DMG="dist/Procyon-${VERSION}.dmg"
ZIP="dist/Procyon-${VERSION}.zip"
[ -d "$APP" ] || { echo "$APP not found: run scripts/build-macos-app.sh first" >&2; exit 1; }

rm -f "$DMG" "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"

staging="$(mktemp -d)"
cp -R "$APP" "$staging/"
ln -s /Applications "$staging/Applications"
# hdiutil now and then fails with "Resource busy" while something else holds the image; retry.
for attempt in 1 2 3 4 5; do
    hdiutil create -volname "Procyon ${VERSION}" -srcfolder "$staging" -ov -format UDZO "$DMG" >/dev/null && break
    [ "$attempt" -lt 5 ] || exit 1
    sleep $((attempt * 2))
done
rm -rf "$staging"

if [ -n "${SIGN_IDENTITY:-}" ] && [ "$SIGN_IDENTITY" != "-" ]; then
    codesign --force --sign "$SIGN_IDENTITY" --timestamp "$DMG"
fi

notary_args=()
if [ -n "${NOTARY_PROFILE:-}" ]; then
    notary_args=(--keychain-profile "$NOTARY_PROFILE")
elif [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
    notary_args=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD")
fi
if [ ${#notary_args[@]} -gt 0 ]; then
    echo "Notarizing ${DMG}..."
    xcrun notarytool submit "$DMG" "${notary_args[@]}" --wait
    xcrun stapler staple "$DMG"
else
    echo "Skipping notarization (no credentials); Gatekeeper will warn on other Macs."
fi

(cd dist && shasum -a 256 "$(basename "$DMG")" "$(basename "$ZIP")" > "Procyon-${VERSION}.sha256")
ls -lh "$DMG" "$ZIP"
