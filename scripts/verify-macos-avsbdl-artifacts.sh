#!/bin/bash

set -euo pipefail

staged_app=${1:?usage: verify-macos-avsbdl-artifacts.sh staged.app candidate.dmg candidate.app.zip}
dmg=${2:?usage: verify-macos-avsbdl-artifacts.sh staged.app candidate.dmg candidate.app.zip}
app_zip=${3:?usage: verify-macos-avsbdl-artifacts.sh staged.app candidate.dmg candidate.app.zip}

fail()
{
  echo "FAIL: $1" >&2
  exit 1
}

root=$(cd "$(dirname "$0")/.." && pwd)
verify_bundle=$root/scripts/verify-macos-avsbdl-bundle.sh
temp_root=$(mktemp -d)
mount_point=$temp_root/dmg
zip_root=$temp_root/zip
mounted=0

cleanup()
{
  if [ "$mounted" -eq 1 ]; then
    hdiutil detach "$mount_point" -quiet || true
  fi
  rm -rf "$temp_root"
}
trap cleanup EXIT HUP INT TERM

[ -d "$staged_app" ] || fail "staged app is missing"
[ -f "$dmg" ] || fail "DMG is missing"
[ -f "$app_zip" ] || fail "app ZIP is missing"

mkdir "$mount_point" "$zip_root"
hdiutil attach -readonly -nobrowse -mountpoint "$mount_point" "$dmg" >/dev/null
mounted=1
ditto -x -k "$app_zip" "$zip_root"

dmg_app=$mount_point/Moonlight\ AVSampleBuffer.app
zip_app=$zip_root/Moonlight\ AVSampleBuffer.app

bash "$verify_bundle" "$staged_app"
bash "$verify_bundle" "$dmg_app"
bash "$verify_bundle" "$zip_app"

staged_hash=$(shasum -a 256 "$staged_app/Contents/MacOS/Moonlight" | awk '{print $1}')
dmg_hash=$(shasum -a 256 "$dmg_app/Contents/MacOS/Moonlight" | awk '{print $1}')
zip_hash=$(shasum -a 256 "$zip_app/Contents/MacOS/Moonlight" | awk '{print $1}')

[ "$staged_hash" = "$dmg_hash" ] || fail "DMG executable differs from staged app"
[ "$staged_hash" = "$zip_hash" ] || fail "ZIP executable differs from staged app"

echo "PASS: staged, DMG, and ZIP executable SHA-256 are identical ($staged_hash)"
