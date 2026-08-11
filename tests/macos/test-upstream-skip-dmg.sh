#!/bin/bash

set -euo pipefail

source_root=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT HUP INT TERM

mock_bin=$fixture/mock-bin
mkdir -p "$mock_bin"

cat > "$mock_bin/python3" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$mock_bin/qmake" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$mock_bin/sysctl" <<'EOF'
#!/bin/sh
echo 2
EOF

cat > "$mock_bin/make" <<'EOF'
#!/bin/sh
set -eu
mkdir -p app/Moonlight.app/Contents/MacOS
printf '#!/bin/sh\n' > app/Moonlight.app/Contents/MacOS/Moonlight
chmod 0755 app/Moonlight.app/Contents/MacOS/Moonlight
EOF

cat > "$mock_bin/dsymutil" <<'EOF'
#!/bin/sh
set -eu
output=
while [ "$#" -gt 0 ]; do
  if [ "$1" = '-o' ]; then
    shift
    output=$1
  fi
  shift
done
[ -n "$output" ]
mkdir -p "$output"
EOF

cat > "$mock_bin/macdeployqt" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$mock_bin/create-dmg" <<'EOF'
#!/bin/sh
set -eu
output=$2
: > "$FIXTURE_MARKER"
: > "$output/Moonlight.dmg"
EOF

chmod 0755 "$mock_bin/"*

prepare_case()
{
  case_root=$1
  mkdir -p "$case_root/scripts" "$case_root/app/gui"
  cp "$source_root/scripts/generate-dmg.sh" "$case_root/scripts/"
  printf '%s\n' '6.1.0' > "$case_root/app/version.txt"
}

default_root=$fixture/default
prepare_case "$default_root"
(
  cd "$default_root"
  PATH="$mock_bin:$PATH" FIXTURE_MARKER="$default_root/create-dmg.called" \
    CI_VERSION=test bash scripts/generate-dmg.sh Release
)
[ -f "$default_root/create-dmg.called" ]
[ -f "$default_root/build/installer-Release/Moonlight-test.dmg" ]

skip_root=$fixture/skip
prepare_case "$skip_root"
(
  cd "$skip_root"
  PATH="$mock_bin:$PATH" FIXTURE_MARKER="$skip_root/create-dmg.called" \
    CI_VERSION=test SKIP_DMG=1 bash scripts/generate-dmg.sh Release
)
[ ! -e "$skip_root/create-dmg.called" ]
[ -d "$skip_root/build/build-Release/app/Moonlight.app" ]
[ ! -e "$skip_root/build/installer-Release/Moonlight-test.dmg" ]

echo 'PASS: upstream DMG path is default-on and SKIP_DMG=1 stops after deployment'
