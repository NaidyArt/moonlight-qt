#!/bin/zsh

set -euo pipefail

target='/Applications/Moonlight AVSampleBuffer.app'
expected_bundle_id='art.naidy.moonlight-avsamplebuffer'

if [[ ! -e "$target" ]]; then
  print 'Moonlight AVSampleBuffer no está instalado en /Applications.'
  exit 0
fi

if [[ -L "$target" || ! -d "$target" ]]; then
  print -u2 'El destino no es el paquete de aplicación esperado; no se ha tocado nada.'
  exit 1
fi

actual_bundle_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$target/Contents/Info.plist" 2>/dev/null || true)
if [[ "$actual_bundle_id" != "$expected_bundle_id" ]]; then
  print -u2 "Bundle ID inesperado ($actual_bundle_id); no se ha tocado nada."
  exit 1
fi

/usr/bin/pkill -f '^/Applications/Moonlight AVSampleBuffer\.app/Contents/MacOS/Moonlight($| )' >/dev/null 2>&1 || true
/bin/sleep 1
if /usr/bin/pgrep -f '^/Applications/Moonlight AVSampleBuffer\.app/Contents/MacOS/Moonlight($| )' >/dev/null; then
  print -u2 'Moonlight AVSampleBuffer sigue abierto; ciérralo y repite el rollback.'
  exit 1
fi

trash="$HOME/.Trash/Moonlight AVSampleBuffer.app"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
attempt=0
while [[ -e "$trash" || -L "$trash" ]]; do
  (( attempt += 1 ))
  if (( attempt > 100 )); then
    print -u2 'No se encontró un nombre libre en la Papelera; no se ha tocado nada.'
    exit 1
  fi
  trash="$HOME/.Trash/Moonlight AVSampleBuffer-$stamp-$attempt.app"
done

# -n closes the race between the existence check and mv. The postconditions
# fail closed if another process creates the destination at the same instant.
/bin/mv -n "$target" "$trash"
nested_collision="$trash/Moonlight AVSampleBuffer.app"
if [[ -e "$nested_collision" || -L "$nested_collision" ]]; then
  print -u2 "Otro proceso creó el destino durante el traslado. La app quedó recuperable en: $nested_collision"
  exit 1
fi
if [[ -e "$target" || -L "$target" || ! -d "$trash" || -L "$trash" ]]; then
  print -u2 'El traslado recuperable no terminó de forma inequívoca; revisa Aplicaciones y la Papelera.'
  exit 1
fi

moved_bundle_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$trash/Contents/Info.plist" 2>/dev/null || true)
if [[ "$moved_bundle_id" != "$expected_bundle_id" ]]; then
  print -u2 'El paquete movido no conserva la identidad esperada; no continúes hasta revisarlo.'
  exit 1
fi
print "Desinstalado de forma recuperable: $trash"
print 'Moonlight oficial y sus ajustes compartidos no se han modificado.'
