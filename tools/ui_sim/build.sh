#!/usr/bin/env bash
# Host-Simulator der Uhr-Oberfläche bauen und ausführen (Linux, gcc, python3+Pillow).
#
#   tools/ui_sim/build.sh            -> tools/ui_sim/out/*.png, Prüfprotokoll
#
# LVGL 9.3.0 wird beim ersten Lauf nach tools/ui_sim/.lvgl geklont (nicht im
# Repo). Es gilt dieselbe lv_conf.h wie für die Uhr.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SKETCH="$ROOT/UhrMeldezaehler"
LVGL="${LVGL_DIR:-$HERE/.lvgl}"
BUILD="$HERE/.build"
OUT="$HERE/out"

if [ ! -d "$LVGL/src" ]; then
  git clone --depth 1 --branch v9.3.0 https://github.com/lvgl/lvgl.git "$LVGL"
fi
mkdir -p "$BUILD/obj" "$OUT"

CFLAGS=(-O1 -DLV_CONF_INCLUDE_SIMPLE -I"$ROOT" -I"$LVGL" -I"$LVGL/src")
if [ ! -f "$BUILD/liblvgl.a" ]; then
  echo "==> baue LVGL ..."
  find "$LVGL/src" -name '*.c' | while read -r f; do
    echo "$f"
  done | xargs -P "$(nproc)" -I{} sh -c 'gcc '"${CFLAGS[*]}"' -c "{}" -o "'"$BUILD"'/obj/$(echo "{}" | md5sum | cut -c1-16).o"'
  ar rcs "$BUILD/liblvgl.a" "$BUILD"/obj/*.o
fi

echo "==> baue Simulator ..."
for f in "$SKETCH"/src/fonts/*.c; do
  gcc "${CFLAGS[@]}" -DLV_LVGL_H_INCLUDE_SIMPLE -c "$f" -o "$BUILD/$(basename "$f" .c).o"
done
g++ -std=c++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
  "${CFLAGS[@]}" -I"$SKETCH" -I"$HERE" -include "$HERE/sim_model.h" \
  "$HERE/sim_main.cpp" "$BUILD"/font_*.o "$BUILD/liblvgl.a" -lm -rdynamic -g -o "$BUILD/ui_sim"

echo "==> starte ..."
rm -f "$OUT"/*.ppm "$OUT"/*.png
set +e
(cd "$HERE" && "$BUILD/ui_sim" "$OUT")
rc=$?
set -e
python3 - "$OUT" <<'PY'
import sys, glob, os
from PIL import Image
for p in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(p).save(p[:-4] + ".png")
    os.remove(p)
PY
exit $rc
