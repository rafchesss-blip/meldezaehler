#!/usr/bin/env bash
# Patches die Arduino-Bibliothek "TensorFlowLite_ESP32" (tanakamasayuki),
# damit sie mit dem ESP32-Core 3.x / GCC 14 kompiliert.
#
#    ./patch_tflite_lib.sh
#
# Führt folgende Anpassungen durch (idempotent):
#   1. Optionalen LCD/Kamera-Treiber (src/bus, src/screen) aus src/ verschieben
#      (sie brechen den Build, weil sie nur für die Beispiele nötig sind).
#   2. flatbuffers stl_emulation.h: const-Member von span entfernen
#      (GCC 14 verbietet Zuweisung an const-Member im Kopier-Zuweisungsoperator).
#   3. compatibility.h: TF_LITE_REMOVE_VIRTUAL_DELETE leeren
#      (der klassenspezifische operator delete bricht Placement-New unter GCC 14;
#       ESP32 stellt ein globales operator delete bereit).
set -euo pipefail

# Bibliothekspfad ermitteln (Arduino-Ordner)
for base in "${HOME}/Arduino/libraries" "${HOME}/Documents/Arduino/libraries"; do
  LIB="${base}/TensorFlowLite_ESP32"
  if [ -d "$LIB" ]; then break; fi
done

if [ ! -d "$LIB" ]; then
  echo "TensorFlowLite_ESP32 nicht gefunden. Zuerst installieren:"
  echo "  arduino-cli lib install TensorFlowLite_ESP32"
  exit 1
fi
echo "Bibliothek: $LIB"

SRC="$LIB/src"

# 1) bus/screen aus src/ verschieben
for d in bus screen; do
  if [ -d "$SRC/$d" ]; then
    mv "$SRC/$d" "$LIB/_unused_$d"
    echo "  verschoben: src/$d -> _unused_$d"
  fi
done

# 2) span-Member nicht-const machen
SPAN="$SRC/third_party/flatbuffers/stl_emulation.h"
if [ -f "$SPAN" ]; then
  python3 - "$SPAN" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = "  pointer const data_;\n  const size_type count_;"
new = "  pointer data_;\n  size_type count_;"
if old in s:
    open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
    print("  gepatcht: stl_emulation.h (const-Member entfernt)")
else:
    print("  stl_emulation.h: bereits gepatcht oder nicht gefunden")
PY
fi

# 3) TF_LITE_REMOVE_VIRTUAL_DELETE deaktivieren
COMPAT="$SRC/tensorflow/lite/micro/compatibility.h"
if [ -f "$COMPAT" ]; then
  python3 - "$COMPAT" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
pattern = re.compile(r"#ifdef TF_LITE_STATIC_MEMORY\n#define TF_LITE_REMOVE_VIRTUAL_DELETE.*?#endif", re.DOTALL)
replacement = ("// Note: TF_LITE_STATIC_MEMORY's class-specific operator delete is disabled\n"
               "// here because it breaks GCC 14 (placement-new accessibility check). ESP32\n"
               "// provides a global operator delete, so this links fine.\n"
               "#define TF_LITE_REMOVE_VIRTUAL_DELETE\n")
if pattern.search(s):
    s = pattern.sub(replacement, s, count=1)
    open(p, 'w', encoding='utf-8').write(s)
    print("  gepatcht: compatibility.h (TF_LITE_REMOVE_VIRTUAL_DELETE deaktiviert)")
else:
    print("  compatibility.h: bereits gepatcht")
PY
fi

echo "Fertig."
