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
if [[ -e "$trash" ]]; then
  trash="$HOME/.Trash/Moonlight AVSampleBuffer-$(date -u +%Y%m%dT%H%M%SZ).app"
fi

/bin/mv "$target" "$trash"
print "Desinstalado de forma recuperable: $trash"
print 'Moonlight oficial y sus ajustes compartidos no se han modificado.'
