#!/usr/bin/env bash
# Builds dist/Procyon.app. Usage: scripts/build-macos-app.sh [debug|release]
#   VERSION=1.2.3       marketing version (default 0.1.0)
#   BUILD_NUMBER=42     bundle build number (default 1)
#   SIGN_IDENTITY="Developer ID Application: …"  real signing with hardened runtime;
#                       default "-" signs ad hoc (runs locally, can't be notarized)
#   UNIVERSAL=1         build arm64 + x86_64
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
APP="dist/Procyon.app"
VERSION="${VERSION:-0.1.0}"
BUILD_NUMBER="${BUILD_NUMBER:-1}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
ARCH_FLAGS=()
[ "${UNIVERSAL:-0}" = 1 ] && ARCH_FLAGS=(--arch arm64 --arch x86_64)

swift build -c "$CONFIG" ${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"} --product Procyon
swift build -c "$CONFIG" ${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"} --product procyon-helper
BIN="$(swift build -c "$CONFIG" ${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"} --show-bin-path)"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Helpers"
cp "$BIN/Procyon" "$APP/Contents/MacOS/Procyon"
cp "$BIN/procyon-helper" "$APP/Contents/Helpers/procyon-helper"
cp apps/macos/Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
# SwiftPM resource bundles (none today) would be copied here.
for bundle in "$BIN"/*.bundle; do [ -e "$bundle" ] && cp -R "$bundle" "$APP/Contents/Resources/"; done

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Procyon</string>
  <key>CFBundleDisplayName</key><string>Procyon</string>
  <key>CFBundleIdentifier</key><string>dev.procyon.app</string>
  <key>CFBundleExecutable</key><string>Procyon</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${BUILD_NUMBER}</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticTermination</key><false/>
</dict>
</plist>
PLIST

# Inside-out: helper first, then the bundle. Real identities get the hardened runtime and a
# secure timestamp, both required for notarization.
SIGN_FLAGS=(--force --sign "$SIGN_IDENTITY")
[ "$SIGN_IDENTITY" != "-" ] && SIGN_FLAGS+=(--options runtime --timestamp)
codesign "${SIGN_FLAGS[@]}" --identifier dev.procyon.helper "$APP/Contents/Helpers/procyon-helper"
codesign "${SIGN_FLAGS[@]}" "$APP"
codesign --verify --strict --deep "$APP"
echo "Built $APP ($(du -sh "$APP" | cut -f1))"
