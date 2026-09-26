#!/usr/bin/env bash
# Build-Skript für den Meldezähler.
#
#   ./build.sh firmware   – Firmware kompilieren (UhrMeldezaehler)
#   ./build.sh upload     – Firmware kompilieren + auf die Uhr flashen
#   ./build.sh app        – Flutter-APKs bauen und ins Projektverzeichnis kopieren
#   ./build.sh all        – Firmware + App (Standard)
#
# Voraussetzungen: arduino-cli + esp32-Core, Flutter SDK.

set -euo pipefail
cd "$(dirname "$0")"

# Waveshare ESP32-S3-Touch-AMOLED-2.06: 16 MB Flash + 8 MB OPI-PSRAM.
# Mit der Standard-4-MB-Partition waere die Firmware zu 99% voll und der
# Rekorder (PSRAM-Puffer) wuerde nicht funktionieren.
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi}"
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

upload_firmware() {
  local port="${PORT:-/dev/ttyACM0}"
  echo "==> Kompiliere + flashe Firmware (${FQBN}) auf ${port} ..."
  arduino-cli compile --fqbn "${FQBN}" "${SKETCH}"
  arduino-cli upload  --fqbn "${FQBN}" -p "${port}" "${SKETCH}"
}

case "${1:-all}" in
  firmware) build_firmware ;;
  upload)   upload_firmware ;;
  app)      build_app ;;
  all)      build_firmware; build_app ;;
  *) echo "Unbekannt: $1 (erwartet: firmware, upload, app, all)"; exit 2 ;;
esac

echo "==> Fertig."
