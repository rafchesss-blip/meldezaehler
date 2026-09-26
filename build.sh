#!/usr/bin/env bash
# Build-Skript für den Meldezähler.
#
#   ./build.sh firmware   – Firmware kompilieren (UhrMeldezaehler)
#   ./build.sh app        – Flutter-APKs bauen und ins Projektverzeichnis kopieren
#   ./build.sh all        – beides (Standard)
#
# Voraussetzungen: arduino-cli + esp32-Core, Flutter SDK.

set -euo pipefail
cd "$(dirname "$0")"

FQBN="${FQBN:-esp32:esp32:esp32s3}"
SKETCH="UhrMeldezaehler"

build_firmware() {
  echo "==> Kompiliere Firmware (${FQBN}) ..."
  arduino-cli compile --fqbn "${FQBN}" "${SKETCH}"
}

build_app() {
  echo "==> Baue Flutter-APKs ..."
  (cd MeldeApp && flutter pub get && flutter build apk --release --split-per-abi && flutter build apk --release)

  echo "==> Kopiere APKs ins Projektverzeichnis ..."
  cp -v MeldeApp/build/app/outputs/flutter-apk/app-arm64-v8a-release.apk Meldezaehler-App-arm64.apk
  cp -v MeldeApp/build/app/outputs/flutter-apk/app-release.apk            Meldezaehler-App-universal.apk
}

case "${1:-all}" in
  firmware) build_firmware ;;
  app)      build_app ;;
  all)      build_firmware; build_app ;;
  *) echo "Unbekannt: $1 (erwartet: firmware, app, all)"; exit 2 ;;
esac

echo "==> Fertig."
