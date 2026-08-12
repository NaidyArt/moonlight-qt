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
base_commit=2e13ed9977bc31c73caf8428f08f58d793313ece

git -C "$root" cat-file -e "$base_commit^{commit}" 2>/dev/null ||
  fail "pinned upstream base is unavailable"
git -C "$root" merge-base --is-ancestor "$base_commit" HEAD ||
  fail "candidate is not descended from the pinned upstream base"
git -C "$root" diff --check "$base_commit"..HEAD ||
  fail "candidate diff has whitespace errors"

unexpected=0
while IFS= read -r changed_path; do
  case "$changed_path" in
    .github/workflows/build-macos-avsbdl.yml|\
    .github/workflows/build.yml|\
    app/Info.plist|\
    app/app.pro|\
    app/gui/SettingsView.qml|\
    app/settings/streamingpreferences.cpp|\
    app/settings/streamingpreferences.h|\
    app/streaming/session.cpp|\
    app/streaming/session.h|\
    app/streaming/video/ffmpeg.cpp|\
    app/streaming/video/ffmpeg.h|\
    app/streaming/video/overlaymanager.cpp|\
    app/streaming/video/overlaymanager.h|\
    app/streaming/video/statstelemetry.cpp|\
    app/streaming/video/statstelemetry.h|\
    docs/MACOS_AVSBDL_INSTALLABLE.md|\
    docs/MACOS_AVSBDL_TELEMETRY.md|\
    docs/MACOS_AVSBDL_TELEMETRY_REVIEW.md|\
    'release-macos/Desinstalar Moonlight AVSampleBuffer.command'|\
    release-macos/README.txt|\
    scripts/generate-avsbdl-dmg.sh|\
    scripts/generate-dmg.sh|\
    scripts/verify-macos-avsbdl-artifacts.sh|\
    scripts/verify-macos-avsbdl-bundle.sh|\
    scripts/verify-macos-avsbdl-source.sh|\
    scripts/verify-macos-avsbdl-telemetry-source.py|\
    tests/macos/test-avsbdl-packaging.sh|\
    tests/macos/test-upstream-skip-dmg.sh|\
    tests/macos/telemetry/telemetry.pro|\
    tests/macos/telemetry/test-statstelemetry.cpp)
      ;;
    *)
      echo "Unexpected path changed from pinned base: $changed_path" >&2
      unexpected=1
      ;;
  esac
done < <(git -C "$root" diff --name-only "$base_commit"..HEAD)
[ "$unexpected" -eq 0 ] || fail "candidate source allowlist failed"

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
grep -Fq 'if [ "${SKIP_DMG:-0}" = "1" ]; then' "$root/scripts/generate-dmg.sh" ||
  fail "official macOS build script lacks the isolated packaging handoff"
grep -Fq "APP_DISPLAY_NAME=\$(\"\$PLIST_BUDDY\" -c 'Print :CFBundleDisplayName'" "$root/scripts/generate-avsbdl-dmg.sh" ||
  fail "custom packager does not derive the create-dmg name from its signed bundle"
grep -Fq '[ "${#DMG_CANDIDATES[@]}" -eq 1 ]' "$root/scripts/generate-avsbdl-dmg.sh" ||
  fail "custom packager does not require exactly one DMG candidate"

metal_experiment_matches=$(find "$root/app" "$root/scripts" -type f \
  ! -name 'verify-macos-avsbdl-source.sh' \
  ! -name 'verify-macos-avsbdl-telemetry-source.py' \
  -exec grep -n -E 'VT_METAL_(ASYNC|MAX)|vt_metal_latency_config' {} + || true)
if [ -n "$metal_experiment_matches" ]; then
  echo "$metal_experiment_matches" >&2
  fail "experimental Metal tuning leaked into the clean AVSampleBuffer branch"
fi

python3 "$root/scripts/verify-macos-avsbdl-telemetry-source.py"

echo "PASS: source defaults to persistent AVSampleBuffer with a separate macOS bundle identity"
