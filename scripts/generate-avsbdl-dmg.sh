#!/bin/bash

set -euo pipefail

BUILD_CONFIG=${1:-Release}

fail()
{
  echo "$1" >&2
  exit 1
}

if [ "$BUILD_CONFIG" != "Release" ]; then
  fail "This distribution script only accepts Release"
fi

SOURCE_ROOT=$PWD
BUILD_ROOT=$SOURCE_ROOT/build
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG
ORIGINAL_APP=$BUILD_FOLDER/app/Moonlight.app
CUSTOM_APP=$BUILD_FOLDER/app/Moonlight\ AVSampleBuffer.app

if [ -n "${CI_VERSION:-}" ]; then
  VERSION=$CI_VERSION
else
  VERSION=$(cat "$SOURCE_ROOT/app/version.txt")
fi
PLIST_BUDDY=${PLIST_BUDDY:-/usr/libexec/PlistBuddy}

CUSTOM_DMG_BASENAME="Moonlight-AVSampleBuffer-$VERSION.dmg"
CUSTOM_DMG=$INSTALLER_FOLDER/$CUSTOM_DMG_BASENAME
CUSTOM_ZIP=$INSTALLER_FOLDER/Moonlight-AVSampleBuffer-$VERSION.app.zip

SKIP_DMG=1 bash "$SOURCE_ROOT/scripts/generate-dmg.sh" "$BUILD_CONFIG"

[ -d "$ORIGINAL_APP" ] || fail "Upstream build did not produce Moonlight.app"
[ ! -e "$CUSTOM_APP" ] || fail "Custom app path already exists"

mv "$ORIGINAL_APP" "$CUSTOM_APP"

# CI has no Apple Developer identity. Apply an explicit ad-hoc signature so the
# complete nested bundle has a coherent signature. This is not notarization.
codesign --force --deep --sign - "$CUSTOM_APP"
codesign --verify --deep --strict --verbose=2 "$CUSTOM_APP"

[ -x "$PLIST_BUDDY" ] || fail "PlistBuddy is unavailable: $PLIST_BUDDY"
APP_DISPLAY_NAME=$("$PLIST_BUDDY" -c 'Print :CFBundleDisplayName' "$CUSTOM_APP/Contents/Info.plist") ||
  fail "CFBundleDisplayName is unavailable"
APP_SHORT_VERSION=$("$PLIST_BUDDY" -c 'Print :CFBundleShortVersionString' "$CUSTOM_APP/Contents/Info.plist") ||
  fail "CFBundleShortVersionString is unavailable"
for component in "$APP_DISPLAY_NAME" "$APP_SHORT_VERSION"; do
  [ -n "$component" ] || fail "create-dmg filename component is empty"
  case "$component" in
    */*|*$'\n'*|.|..) fail "unsafe create-dmg filename component: $component" ;;
  esac
done

shopt -s nullglob
DMG_CANDIDATES=("$INSTALLER_FOLDER"/*.dmg)
shopt -u nullglob
[ "${#DMG_CANDIDATES[@]}" -eq 0 ] || fail "installer directory already contains a DMG"

if create-dmg "$CUSTOM_APP" "$INSTALLER_FOLDER"; then
  CREATE_DMG_STATUS=0
else
  CREATE_DMG_STATUS=$?
fi
case $CREATE_DMG_STATUS in
  0|2) ;;
  *) fail "create-dmg failed with status $CREATE_DMG_STATUS" ;;
esac

GENERATED_DMG=$INSTALLER_FOLDER/$APP_DISPLAY_NAME\ $APP_SHORT_VERSION.dmg
shopt -s nullglob
DMG_CANDIDATES=("$INSTALLER_FOLDER"/*.dmg)
shopt -u nullglob
[ "${#DMG_CANDIDATES[@]}" -eq 1 ] || fail "create-dmg produced ${#DMG_CANDIDATES[@]} DMG candidates instead of exactly one"
[ "${DMG_CANDIDATES[0]}" = "$GENERATED_DMG" ] || fail "create-dmg produced an unexpected filename: ${DMG_CANDIDATES[0]}"
[ -f "$GENERATED_DMG" ] && [ ! -L "$GENERATED_DMG" ] || fail "create-dmg output is not one regular non-symlink file"
mv "$GENERATED_DMG" "$CUSTOM_DMG"
[ -f "$CUSTOM_DMG" ] && [ ! -L "$CUSTOM_DMG" ] || fail "release DMG rename failed"
[ ! -e "$GENERATED_DMG" ] && [ ! -L "$GENERATED_DMG" ] || fail "generated DMG source remains after rename"

ditto -c -k --sequesterRsrc --keepParent "$CUSTOM_APP" "$CUSTOM_ZIP"

(
  cd "$INSTALLER_FOLDER"
  shasum -a 256 "$CUSTOM_DMG_BASENAME" "$(basename "$CUSTOM_ZIP")" > SHA256SUMS.txt
)

echo "Build successful: $CUSTOM_DMG"
