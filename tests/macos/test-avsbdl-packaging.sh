#!/bin/sh

set -eu

source_root=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
mock_bin=$fixture/mock-bin
mkdir -p "$mock_bin"

cat > "$mock_bin/codesign" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$mock_bin/PlistBuddy" <<'EOF'
#!/bin/sh
case "$*" in
  *CFBundleDisplayName*) echo 'Moonlight AVSampleBuffer' ;;
  *CFBundleShortVersionString*) echo '6.1.0' ;;
  *) exit 1 ;;
esac
EOF

cat > "$mock_bin/create-dmg" <<'EOF'
#!/bin/sh
set -eu
app=$1
output=$2
name=$(basename "$app" .app)
case "${CREATE_DMG_MODE:-normal}" in
  normal)
    : > "$output/$name 6.1.0.dmg"
    ;;
  zero)
    ;;
  multiple)
    : > "$output/$name 6.1.0.dmg"
    : > "$output/unexpected second.dmg"
    ;;
  *)
    exit 1
    ;;
esac
# create-dmg 6 returns 2 when the DMG is valid but no signing identity exists.
exit 2
EOF

cat > "$mock_bin/ditto" <<'EOF'
#!/bin/sh
set -eu
for last do :; done
: > "$last"
EOF

cat > "$mock_bin/shasum" <<'EOF'
#!/bin/sh
shift 2
for file do
  printf 'fixture  %s\n' "$file"
done
EOF

chmod 0755 "$mock_bin/"*

prepare_case()
{
  case_root=$1
  mkdir -p "$case_root/scripts" "$case_root/app"
  cp "$source_root/scripts/generate-avsbdl-dmg.sh" "$case_root/scripts/"
  printf '%s\n' '6.1.0' > "$case_root/app/version.txt"

  cat > "$case_root/scripts/generate-dmg.sh" <<'EOF'
#!/bin/sh
set -eu
mkdir -p build/build-Release/app/Moonlight.app/Contents/MacOS build/installer-Release
printf '#!/bin/sh\n' > build/build-Release/app/Moonlight.app/Contents/MacOS/Moonlight
chmod 0755 build/build-Release/app/Moonlight.app/Contents/MacOS/Moonlight
[ "${SKIP_DMG:-0}" = "1" ]
EOF
  chmod 0755 "$case_root/scripts/"*
}

success_root=$fixture/success
prepare_case "$success_root"
(
  cd "$success_root"
  PATH="$mock_bin:$PATH" PLIST_BUDDY="$mock_bin/PlistBuddy" \
    CREATE_DMG_MODE=normal CI_VERSION=test \
    bash scripts/generate-avsbdl-dmg.sh Release
)

installer=$success_root/build/installer-Release
[ -f "$installer/Moonlight-AVSampleBuffer-test.dmg" ]
[ ! -L "$installer/Moonlight-AVSampleBuffer-test.dmg" ]
[ -f "$installer/Moonlight-AVSampleBuffer-test.app.zip" ]
[ ! -e "$installer/Moonlight AVSampleBuffer 6.1.0.dmg" ]
[ -d "$success_root/build/build-Release/app/Moonlight AVSampleBuffer.app" ]
grep -Fq 'Moonlight-AVSampleBuffer-test.dmg' "$installer/SHA256SUMS.txt"
grep -Fq 'Moonlight-AVSampleBuffer-test.app.zip' "$installer/SHA256SUMS.txt"

zero_root=$fixture/zero
prepare_case "$zero_root"
if (
  cd "$zero_root"
  PATH="$mock_bin:$PATH" PLIST_BUDDY="$mock_bin/PlistBuddy" \
    CREATE_DMG_MODE=zero CI_VERSION=test \
    bash scripts/generate-avsbdl-dmg.sh Release
) >"$zero_root/output.log" 2>&1; then
  echo 'zero-candidate fixture unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'produced 0 DMG candidates instead of exactly one' "$zero_root/output.log"

multiple_root=$fixture/multiple
prepare_case "$multiple_root"
if (
  cd "$multiple_root"
  PATH="$mock_bin:$PATH" PLIST_BUDDY="$mock_bin/PlistBuddy" \
    CREATE_DMG_MODE=multiple CI_VERSION=test \
    bash scripts/generate-avsbdl-dmg.sh Release
) >"$multiple_root/output.log" 2>&1; then
  echo 'multiple-candidate fixture unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'produced 2 DMG candidates instead of exactly one' "$multiple_root/output.log"

echo 'PASS: custom packaging derives spaced/versioned name and rejects zero or multiple DMGs'
