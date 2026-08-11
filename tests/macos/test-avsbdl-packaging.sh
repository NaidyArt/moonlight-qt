#!/bin/sh

set -eu

source_root=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT HUP INT TERM

mkdir -p "$fixture/scripts" "$fixture/mock-bin" "$fixture/app"
cp "$source_root/scripts/generate-avsbdl-dmg.sh" "$fixture/scripts/"
printf '%s\n' '6.1.0' > "$fixture/app/version.txt"

cat > "$fixture/scripts/generate-dmg.sh" <<'EOF'
#!/bin/sh
set -eu
mkdir -p build/build-Release/app/Moonlight.app/Contents/MacOS build/installer-Release
printf '#!/bin/sh\n' > build/build-Release/app/Moonlight.app/Contents/MacOS/Moonlight
chmod 0755 build/build-Release/app/Moonlight.app/Contents/MacOS/Moonlight
[ "${SKIP_DMG:-0}" = "1" ]
EOF

cat > "$fixture/mock-bin/codesign" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$fixture/mock-bin/create-dmg" <<'EOF'
#!/bin/sh
set -eu
app=$1
output=$2
name=$(basename "$app" .app)
: > "$output/$name.dmg"
EOF

cat > "$fixture/mock-bin/ditto" <<'EOF'
#!/bin/sh
set -eu
for last do :; done
: > "$last"
EOF

cat > "$fixture/mock-bin/shasum" <<'EOF'
#!/bin/sh
shift 2
for file do
  printf 'fixture  %s\n' "$file"
done
EOF

chmod 0755 "$fixture/scripts/generate-dmg.sh" "$fixture/scripts/generate-avsbdl-dmg.sh" "$fixture/mock-bin/"*

(
  cd "$fixture"
  PATH="$fixture/mock-bin:$PATH" CI_VERSION=test bash scripts/generate-avsbdl-dmg.sh Release
)

installer=$fixture/build/installer-Release
[ -f "$installer/Moonlight-AVSampleBuffer-test.dmg" ]
[ -f "$installer/Moonlight-AVSampleBuffer-test.app.zip" ]
[ ! -e "$installer/Moonlight-test.dmg" ]
[ -d "$fixture/build/build-Release/app/Moonlight AVSampleBuffer.app" ]
grep -Fq 'Moonlight-AVSampleBuffer-test.dmg' "$installer/SHA256SUMS.txt"
grep -Fq 'Moonlight-AVSampleBuffer-test.app.zip' "$installer/SHA256SUMS.txt"

echo 'PASS: custom packaging fixture'
