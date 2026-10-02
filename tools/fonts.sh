#!/usr/bin/env bash
# Schriften der Uhr neu erzeugen (Montserrat Medium + FontAwesome-Symbole von LVGL).
#   LVGL_DIR=<lvgl-Bibliothek> tools/fonts.sh
# Benötigt node/npx (lv_font_conv). Zeichenvorrat: ASCII, Umlaute/ß, ° · × – … „ “
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${LVGL_DIR:?LVGL_DIR auf den lvgl-Ordner setzen}/scripts/built_in_font"
OUT="$ROOT/UhrMeldezaehler/src/fonts"
SYMS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"
TXT="0x20-0x7E,0xB0,0xB7,0xC4,0xD6,0xD7,0xDC,0xDF,0xE4,0xF6,0xFC,0x2013,0x201C,0x201E,0x2026"
cd "$SRC"
for s in 16 20 24 32; do
  npx -y lv_font_conv@1.5.3 --bpp 4 --size $s --no-compress --font Montserrat-Medium.ttf -r "$TXT" \
    --font FontAwesome5-Solid+Brands+Regular.woff -r "$SYMS" --format lvgl --lv-include lvgl.h \
    --lv-font-name font_m$s -o "$OUT/font_m$s.c" --force-fast-kern-format
done
# Ziffernschriften für Uhrzeit und Zähler (nur 0-9 : - % . Leerzeichen)
for s in 48 64 96; do
  npx -y lv_font_conv@1.5.3 --bpp 4 --size $s --no-compress --font Montserrat-Medium.ttf \
    --symbols "0123456789:-%. " --format lvgl --lv-include lvgl.h --lv-font-name font_d$s -o "$OUT/font_d$s.c"
done
