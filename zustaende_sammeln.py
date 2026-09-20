#!/usr/bin/env python3
"""
Sammelt Daten fuer die Arm-Zustands-Erkennung (unten / hoch)

Der Klassifikator soll lernen, ob der Arm OBEN oder UNTEN ist.
Dafuer werden 20x "HOCH" und 20x "UNTEN" aufgenommen (je 3 s).

Ausfuehren:
    python3 zustaende_sammeln.py

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
OUT_FILE = 'meldedaten/zustaende.csv'


def warte_auf_ready(ser, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ser.write(b'PING\n')
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line == 'READY':
            return True
        time.sleep(0.2)
    return False


def sammle(ser, label, index):
    print()
    print(f"===== {label}  {index}/{N_TRIALS} =====")
    print("  Position einnehmen ...")
    for i in range(3, 0, -1):
        print(f"  {i} ...", flush=True)
        time.sleep(1)
    print("  >>> AUFNEHMEN (Position ruhig halten!) <<<", flush=True)

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
    print("  ARM-ZUSTAENDE SAMMELN (unten / hoch)")
    print("=" * 55)
    print()
    print("  HOCH:  Arm nach oben heben und RUHUIG oben halten")
    print("         (so wie beim Melden, einfach halten)")
    print()
    print("  UNTEN: Arm normal unten / auf Tisch / im Schoss")
    print("         ruhig halten")
    print()
    input("ENTER druecken, wenn du bereit bist ...")

    os.makedirs('meldedaten', exist_ok=True)
    alle = []

    print("\n### TEIL 1: HOCH ###")
    for i in range(1, N_TRIALS + 1):
        samples = sammle(ser, "HOCH", i)
        for t, s in enumerate(samples):
            alle.append(['hoch', i, t] + s)

    print("\n\n### TEIL 2: UNTEN ###")
    input("ENTER druecken fuer Teil 2 ...")
    for i in range(1, N_TRIALS + 1):
        samples = sammle(ser, "UNTEN", i)
        for t, s in enumerate(samples):
            alle.append(['unten', i, t] + s)

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
