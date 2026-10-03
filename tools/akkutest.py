#!/usr/bin/env python3
"""Ergebnisse des Akku-Tests von der Uhr lesen und auswerten.

    python tools/akkutest.py               # Ergebnisse + Anteil je Verbraucher
    python tools/akkutest.py --log x.csv   # zusätzlich die Minutenwerte speichern
    python tools/akkutest.py --port COM6   # Port fest vorgeben

Die Uhr misst nur die Akkuspannung. Was ein Verbraucher kostet, ergibt sich
aus zwei Laeufen, die sich nur in diesem Verbraucher unterscheiden - z. B.
„Sensor + Display“ gegen „nur Sensor“ = Anteil des Displays.
Benötigt pyserial (pip install pyserial).
"""
import argparse
import csv
import io
import sys
import time

import serial
from serial.tools import list_ports

COMPONENTS = [("sensor", "Sensor/Erkennung"), ("display", "Display"), ("motor", "Vibration")]


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x303A:   # Espressif USB (ESP32-S3)
            return p.device
    sys.exit("Uhr nicht gefunden - Port mit --port angeben.")


def query(ser, cmd, timeout=20):
    """Befehl senden, Zeilen zwischen '--- …' und '--- ende ---' zurückgeben."""
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    lines, inside, t0 = [], False, time.time()
    while time.time() - t0 < timeout:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if line.startswith("--- ende"):
            return lines
        if line.startswith("--- "):
            inside = True
            continue
        if inside and line and not line.startswith("["):
            lines.append(line)
    sys.exit(f"Keine vollständige Antwort auf {cmd}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--log", help="Minutenwerte (SD-Karte) in diese CSV-Datei schreiben")
    args = ap.parse_args()

    ser = serial.Serial(args.port or find_port(), 115200, timeout=0.5)
    time.sleep(0.3)
    rows = list(csv.DictReader(io.StringIO("\n".join(query(ser, "AKKU")))))
    running = [r for r in rows if r.get("id", "").startswith("laeuft")]
    rows = [r for r in rows if r.get("id", "").isdigit()]

    if not rows:
        print("Noch keine Akku-Tests gespeichert.")
    else:
        print(f"{'Test':>4}  {'Min':>4}  {'Sensor':^6} {'Display':^7} {'Vibr.':^5}  {'Meld.':>5}  "
              f"{'Akku':>11}  {'%/h':>5}  {'voll':>6}")
        for r in rows:
            ph = float(r["pct_pro_h"]) if r["pct_pro_h"] else None   # leer = zu kurz
            voll = f"{100 / ph:5.1f} h" if ph is not None and ph >= 0.5 else "   -"
            note = {"1": "  abgebrochen", "2": "  Uhr ging aus"}.get(r["abbruch"], "")
            if ph is None:
                note += "  zu kurz"
            elif ph < 0:
                note += "  Akku stieg (am Kabel?)"
            ja = lambda k: "ja" if r[k] == "1" else "-"
            print(f"{r['id']:>4}  {r['minuten']:>4}  {ja('sensor'):^6} {ja('display'):^7} {ja('motor'):^5}  "
                  f"{r['meldungen']:>5}  {r['pct_start']:>3} -> {r['pct_ende']:>3} %  "
                  f"{(f'{ph:5.1f}' if ph is not None else '  -'):>5}  {voll}{note}")

        # Anteile: Paare, die sich nur in einem Verbraucher unterscheiden
        valid = [r for r in rows if r["pct_pro_h"] and float(r["pct_pro_h"]) >= 0 and int(r["minuten"]) >= 20]
        print("\nAnteil je Verbraucher (aus Laeufen >= 20 min, die sich nur darin unterscheiden):")
        found = False
        for key, name in COMPONENTS:
            others = [k for k, _ in COMPONENTS if k != key] + ["bt"]
            deltas = []
            for a in valid:
                for b in valid:
                    if a[key] == "1" and b[key] == "0" and all(a[o] == b[o] for o in others):
                        deltas.append(float(a["pct_pro_h"]) - float(b["pct_pro_h"]))
            if deltas:
                found = True
                print(f"  {name:<17} {sum(deltas) / len(deltas):5.1f} %/h  ({len(deltas)} Vergleich(e))")
        if not found:
            print("  noch keine passenden Paare - z. B. einen Lauf mit und einen ohne Display machen.")
        print("\nHinweis: Die Prozente stammen aus der Spannung (LiPo-Kurve) - bei kurzen Laeufen ungenau.")

    if running:
        print("\n" + running[0]["id"])

    if args.log:
        lines = query(ser, "AKKULOG", timeout=60)
        with open(args.log, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(lines) + "\n")
        print(f"\nMinutenwerte: {len(lines) - 1} Zeilen -> {args.log}")


if __name__ == "__main__":
    main()
