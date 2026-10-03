#!/usr/bin/env python3
"""Arduino-Bibliotheken für die Uhr-Firmware einrichten (Linux, macOS, Windows).

    python3 setup_libs.py              # Bibliotheken installieren + lv_conf.h kopieren
    python3 setup_libs.py --conf-only  # nur lv_conf.h abgleichen (ruft build.sh auf)

LVGL sucht seine Konfiguration als lv_conf.h NEBEN dem lvgl-Ordner, also im
libraries-Verzeichnis des Sketchbooks – nicht im Sketch. Die versionierte
lv_conf.h aus dem Repo wird deshalb dorthin kopiert (eine vorhandene wird
überschrieben). TensorFlowLite_ESP32 braucht danach noch patch_tflite_lib.sh.
"""
import json
import pathlib
import shutil
import subprocess
import sys

LIBS = ["lvgl@9.3.0", "XPowersLib", "TensorFlowLite_ESP32"]
ROOT = pathlib.Path(__file__).resolve().parent


def cli(*args):
    return subprocess.run(["arduino-cli", *args], check=True, capture_output=True, text=True).stdout


def sketchbook_libraries() -> pathlib.Path:
    cfg = json.loads(cli("config", "dump", "--format", "json"))
    cfg = cfg.get("config", cfg)
    user = cfg.get("directories", {}).get("user")
    if not user:   # ohne Konfigurationsdatei liefert dump {} – Standardwert abfragen
        user = cli("config", "get", "directories.user").strip()
    if not user:
        sys.exit("arduino-cli: directories.user nicht gefunden")
    return pathlib.Path(user) / "libraries"


def main():
    conf_only = "--conf-only" in sys.argv
    if not conf_only:
        for lib in LIBS:
            print(f"==> installiere {lib}")
            cli("lib", "install", lib)
    dst = sketchbook_libraries() / "lv_conf.h"
    src = ROOT / "lv_conf.h"
    if dst.exists() and dst.read_bytes() == src.read_bytes():
        print(f"lv_conf.h aktuell: {dst}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    print(f"lv_conf.h kopiert nach {dst}")


if __name__ == "__main__":
    main()
