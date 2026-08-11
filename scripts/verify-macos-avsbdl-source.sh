#!/bin/bash

set -euo pipefail

fail()
{
  echo "FAIL: $1" >&2
  exit 1
}

root=$(cd "$(dirname "$0")/.." && pwd)
prefs=$root/app/settings/streamingpreferences.cpp
plist=$root/app/Info.plist
main=$root/app/main.cpp

grep -Fq 'constexpr auto defaultRenderer = RendererSelection::RS_AVSBDL;' "$prefs" ||
  fail "macOS AVSampleBuffer default is missing"
grep -Fq 'static_cast<int>(defaultRenderer)' "$prefs" ||
  fail "renderer setting does not use the variant default"
grep -Fq '#define SER_RENDERER "rendererAvsbdlVariant"' "$prefs" ||
  fail "custom renderer preference is not isolated from official Moonlight"
grep -Fq '<string>art.naidy.moonlight-avsamplebuffer</string>' "$plist" ||
  fail "custom bundle identifier is missing"
grep -Fq '<string>Moonlight AVSampleBuffer</string>' "$plist" ||
  fail "custom display name is missing"
grep -Fq 'QCoreApplication::setApplicationName("Moonlight");' "$main" ||
  fail "upstream QSettings identity changed; paired hosts would not be preserved"

metal_experiment_matches=$(find "$root/app" "$root/scripts" -type f \
  ! -name 'verify-macos-avsbdl-source.sh' \
  -exec grep -n -E 'VT_METAL_(ASYNC|MAX)|vt_metal_latency_config' {} + || true)
if [ -n "$metal_experiment_matches" ]; then
  echo "$metal_experiment_matches" >&2
  fail "experimental Metal tuning leaked into the clean AVSampleBuffer branch"
fi

echo "PASS: source defaults to persistent AVSampleBuffer with a separate macOS bundle identity"
