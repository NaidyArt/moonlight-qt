#!/bin/bash

set -euo pipefail

app=${1:?usage: verify-macos-avsbdl-bundle.sh /path/to/Moonlight AVSampleBuffer.app}
expected_bundle_id=art.naidy.moonlight-avsamplebuffer
expected_display_name='Moonlight AVSampleBuffer'

fail()
{
  echo "FAIL: $1" >&2
  exit 1
}

[ -d "$app" ] || fail "app bundle not found: $app"
[ ! -L "$app" ] || fail "app bundle must not be a symlink"

plist=$app/Contents/Info.plist
binary=$app/Contents/MacOS/Moonlight
[ -f "$plist" ] || fail "Info.plist is missing"
[ -x "$binary" ] || fail "Moonlight executable is missing"

bundle_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")
display_name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleDisplayName' "$plist")
[ "$bundle_id" = "$expected_bundle_id" ] || fail "unexpected bundle identifier: $bundle_id"
[ "$display_name" = "$expected_display_name" ] || fail "unexpected display name: $display_name"

archs=$(lipo -archs "$binary")
case " $archs " in
  *' arm64 '*) ;;
  *) fail "arm64 slice is missing: $archs" ;;
esac
case " $archs " in
  *' x86_64 '*) ;;
  *) fail "x86_64 slice is missing: $archs" ;;
esac

codesign --verify --deep --strict --verbose=2 "$app"

signature_id=$(codesign -dv --verbose=4 "$app" 2>&1 | sed -n 's/^Identifier=//p')
[ "$signature_id" = "$expected_bundle_id" ] || fail "signature identifier mismatch: $signature_id"

echo "PASS: universal macOS bundle identity, executable and ad-hoc signature verified"
