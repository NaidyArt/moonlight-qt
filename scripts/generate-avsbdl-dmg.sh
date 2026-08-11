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

ORIGINAL_DMG=$INSTALLER_FOLDER/Moonlight-$VERSION.dmg
CUSTOM_DMG_BASENAME="Moonlight-AVSampleBuffer-$VERSION.dmg"
CUSTOM_DMG=$INSTALLER_FOLDER/$CUSTOM_DMG_BASENAME
CUSTOM_ZIP=$INSTALLER_FOLDER/Moonlight-AVSampleBuffer-$VERSION.app.zip

bash "$SOURCE_ROOT/scripts/generate-dmg.sh" "$BUILD_CONFIG"

[ -d "$ORIGINAL_APP" ] || fail "Upstream build did not produce Moonlight.app"
[ ! -e "$CUSTOM_APP" ] || fail "Custom app path already exists"
[ -f "$ORIGINAL_DMG" ] || fail "Upstream build did not produce its expected DMG"

mv "$ORIGINAL_APP" "$CUSTOM_APP"

# CI has no Apple Developer identity. Apply an explicit ad-hoc signature so the
# complete nested bundle has a coherent signature. This is not notarization.
codesign --force --deep --sign - "$CUSTOM_APP"
codesign --verify --deep --strict --verbose=2 "$CUSTOM_APP"

rm -f "$ORIGINAL_DMG"

if create-dmg "$CUSTOM_APP" "$INSTALLER_FOLDER" --no-version-in-filename; then
  CREATE_DMG_STATUS=0
else
  CREATE_DMG_STATUS=$?
fi
case $CREATE_DMG_STATUS in
  0|2) ;;
  *) fail "create-dmg failed with status $CREATE_DMG_STATUS" ;;
esac

GENERATED_DMG=$INSTALLER_FOLDER/Moonlight\ AVSampleBuffer.dmg
[ -f "$GENERATED_DMG" ] || fail "create-dmg did not produce the expected custom DMG"
mv "$GENERATED_DMG" "$CUSTOM_DMG"

ditto -c -k --sequesterRsrc --keepParent "$CUSTOM_APP" "$CUSTOM_ZIP"

(
  cd "$INSTALLER_FOLDER"
  shasum -a 256 "$CUSTOM_DMG_BASENAME" "$(basename "$CUSTOM_ZIP")" > SHA256SUMS.txt
)

echo "Build successful: $CUSTOM_DMG"
