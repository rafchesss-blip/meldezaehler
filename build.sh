#!/usr/bin/env bash
# Build-Skript für den Meldezähler.
#
#   ./build.sh firmware   – Firmware kompilieren + automatisch auf die Uhr flashen
#   ./build.sh upload     – Firmware kompilieren + flashen (explizit, ggf. PORT=...)
#   ./build.sh app        – Flutter-APKs bauen und ins Projektverzeichnis kopieren
#   ./build.sh all        – Firmware (mit Auto-Upload) + App (Standard)
#
# Der Port wird automatisch erkannt (/dev/ttyACM* bzw. /dev/ttyUSB*).
# Mit PORT=/dev/ttyACM0 kann ein fester Port erzwungen werden.
#
# Voraussetzungen: arduino-cli + esp32-Core, Flutter SDK.
# Bibliotheken einmalig: python3 setup_libs.py && ./patch_tflite_lib.sh

set -euo pipefail
cd "$(dirname "$0")"

# Waveshare ESP32-S3-Touch-AMOLED-2.06: 16 MB Flash + 8 MB OPI-PSRAM.
# Die Firmware (LVGL, Schriften, CNN) braucht die 3-MB-App-Partition.
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi}"
# -O2 statt -Os: Rendern und CNN ~30 % schneller (gemessen 02.10.2026)
OPT=(--build-property "compiler.optimization_flags=-O2")
SKETCH="UhrMeldezaehler"

# Port automatisch finden: PORT-Env hat Vorrang, sonst erstes /dev/ttyACM* bzw. /dev/ttyUSB*.
detect_port() {
  if [ -n "${PORT:-}" ]; then
    echo "$PORT"
    return 0
  fi
  local p
  for p in /dev/ttyACM* /dev/ttyUSB*; do
    if [ -e "$p" ]; then
      echo "$p"
      return 0
    fi
  done
  return 1
}

# lv_conf.h muss neben der lvgl-Bibliothek liegen (siehe setup_libs.py)
sync_lv_conf() {
  python3 setup_libs.py --conf-only 2>/dev/null || python setup_libs.py --conf-only
}

build_firmware() {
  sync_lv_conf
  echo "==> Kompiliere Firmware (${FQBN}) ..."
  arduino-cli compile "${OPT[@]}" --fqbn "${FQBN}" "${SKETCH}"

  local port
  if port="$(detect_port)"; then
    echo "==> Uhr gefunden auf ${port} -> flashe automatisch ..."
    arduino-cli upload --fqbn "${FQBN}" -p "${port}" "${SKETCH}"
  else
    echo "==> Keine Uhr gefunden (/dev/ttyACM*, /dev/ttyUSB*). Nur kompiliert."
    echo "    Flashen mit: ./build.sh upload   oder  PORT=/dev/... ./build.sh firmware"
  fi
}

upload_firmware() {
  local port
  if [ -n "${PORT:-}" ]; then
    port="$PORT"
  elif port="$(detect_port)"; then
    :
  else
    echo "FEHLER: Keine Uhr gefunden. Port mit PORT=/dev/ttyACM0 angeben." >&2
    exit 1
  fi
  sync_lv_conf
  echo "==> Kompiliere + flashe Firmware (${FQBN}) auf ${port} ..."
  arduino-cli compile "${OPT[@]}" --fqbn "${FQBN}" "${SKETCH}"
  arduino-cli upload  --fqbn "${FQBN}" -p "${port}" "${SKETCH}"
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
  upload)   upload_firmware ;;
  app)      build_app ;;
  all)      build_firmware; build_app ;;
  *) echo "Unbekannt: $1 (erwartet: firmware, upload, app, all)"; exit 2 ;;
esac

echo "==> Fertig."
