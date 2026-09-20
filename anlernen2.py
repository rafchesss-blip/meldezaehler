#!/usr/bin/env python3
"""
Anlernen der Hand-POSITIONEN (3 Zonen) fuer die Meldebewegung

Sammelt je 20 Aufnahmen:
  MELDUNG : Arm strecken, Hand UEBER den Kopf, ruhig oben halten
  KOPF    : Hand am Kopf (kratzen, Nase bohren) - natuerliche Bewegung
  TISCH   : Haende am Tisch / haengend, normale Bewegung

Der Klassifikator soll spaeter die aktuelle Hand-POSITION erkennen,
damit nur "Hand ueber Kopf" als Meldung zaehlt.

Ausfuehren:
    python3 anlernen2.py

VOR DEM START: Seriellen Monitor der Arduino IDE schliessen!
"""

import serial
import time
import csv
import os
import sys

PORT = '/dev/ttyACM0'
BAUD = 115200
N_TRIALS = 20
OUT_FILE = 'meldedaten/positionen.csv'


def warte_auf_ready(ser, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ser.write(b'PING\n')
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line == 'READY':
            return True
        time.sleep(0.2)
    return False


def sammle(ser, label, index, anweisung):
    print()
    print(f"===== {label}  {index}/{N_TRIALS} =====")
    print(f"  {anweisung}")
    for i in range(3, 0, -1):
        print(f"  {i} ...", flush=True)
        time.sleep(1)
    print("  >>> AUFNEHMEN! <<<", flush=True)

    ser.reset_input_buffer()
    ser.write(b'GO\n')

    samples = []
    while True:
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line == 'DONE':
            break
        if not line:
            continue
        parts = line.split(',')
        if len(parts) == 6:
            try:
                samples.append([int(p) for p in parts])
            except ValueError:
                pass

    print(f"  -> {len(samples)} Samples")
    return samples


def main():
    if not os.path.exists(PORT):
        print(f"FEHLER: {PORT} nicht gefunden.")
        sys.exit(1)

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()

    time.sleep(0.3)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(True)
    time.sleep(0.5)

    print("Warte auf ESP32 ...")
    if not warte_auf_ready(ser):
        print("FEHLER: ESP32 antwortet nicht. Sketch geflasht? Monitor geschlossen?")
        ser.close()
        sys.exit(1)
    print("ESP32 bereit!\n")

    print("=" * 55)
    print("  HAND-POSITIONEN ANLERNEN (3 Zonen)")
    print("=" * 55)
    print()
    print("  MELDUNG : Arm strecken, Hand UEBER den Kopf,")
    print("            ruhig oben halten")
    print("  KOPF    : Hand am Kopf (kratzen, Nase bohren)")
    print("  TISCH   : Haende am Tisch, schreiben, normal bewegen")
    print()
    input("ENTER druecken, wenn du bereit bist ...")

    os.makedirs('meldedaten', exist_ok=True)
    alle = []

    klassen = [
        ("meldung", "Arm strecken, Hand UEBER den Kopf, oben halten"),
        ("kopf",    "Hand am Kopf: kratzen / Nase bohren"),
        ("tisch",   "Haende am Tisch: schreiben, normal bewegen"),
    ]

    for nr, (label, anweisung) in enumerate(klassen):
        print(f"\n\n### TEIL {nr + 1}: {label.upper()} ###")
        if nr > 0:
            input("ENTER druecken fuer diesen Teil ...")
        for i in range(1, N_TRIALS + 1):
            samples = sammle(ser, label.upper(), i, anweisung)
            for t, s in enumerate(samples):
                alle.append([label, i, t] + s)

    with open(OUT_FILE, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['label', 'trial', 't', 'ax', 'ay', 'az', 'gx', 'gy', 'gz'])
        w.writerows(alle)

    print()
    print("=" * 55)
    print(f"FERTIG! Gespeichert: {OUT_FILE}")
    print("=" * 55)
    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nAbgebrochen.")
        sys.exit(0)
