#!/usr/bin/env python3
"""
Anlern-Programm fuer die Meldebewegung (ESP32-C3 + MPU6050)

Sammelt 20x "MELDEN" und 20x "NICHT MELDEN" Bewegungen.
Pro Bewegung zeichnet der ESP32 3 Sekunden lang mit 100 Hz auf:
  ax, ay, az (Beschleunigung) + gx, gy, gz (Drehrate)

Die Daten werden als CSV in ./meldedaten/daten.csv gespeichert
und danach am PC zum Trainieren benutzt.

VOR DEM START:
  - Seriellen Monitor in der Arduino IDE SCHLIESSEN
  - Dieses Skript in einem Terminal ausfuehren:
        python3 anlernen.py
"""

import serial
import time
import csv
import os
import sys

PORT = '/dev/ttyACM0'
BAUD = 115200
N_TRIALS = 20
OUT_DIR = 'meldedaten'
OUT_FILE = os.path.join(OUT_DIR, 'daten.csv')


def warte_auf_ready(ser, timeout=8):
    """Wartet, bis der ESP32 'READY' sendet."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ser.write(b'PING\n')
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line == 'READY':
            return True
        time.sleep(0.2)
    return False


def sammle_bewegung(ser, label, index):
    """Fuehrt Countdown durch und zeichnet eine Bewegung auf."""
    print()
    print(f"===== {label}  {index}/{N_TRIALS} =====")
    print("  Arm in Ausgangsposition bringen (ruhig, unten) ...")
    for i in range(3, 0, -1):
        print(f"  {i} ...", flush=True)
        time.sleep(1)

    print("  >>> JETZT! Bewegung ausfuehren! <<<", flush=True)

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

    print(f"  -> {len(samples)} Samples aufgenommen")
    return samples


def main():
    if not os.path.exists(PORT):
        print(f"FEHLER: {PORT} nicht gefunden. Ist der ESP32 angeschlossen?")
        sys.exit(1)

    # Port oeffnen (ohne Reset auszuloesen)
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()

    # ESP32 sauber neu starten und auf READY warten
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
        print("FEHLER: ESP32 antwortet nicht.")
        print("  - Ist das DatenSammler-Sketch geflasht?")
        print("  - Ist der Serielle Monitor der Arduino IDE geschlossen?")
        ser.close()
        sys.exit(1)
    print("ESP32 bereit!\n")

    print("=" * 55)
    print("  ANLERNEN DER MELDEBEWEGUNG")
    print("=" * 55)
    print()
    print("  MELDEN (Arm melden in der Schule):")
    print("    - Arm zuegig nach oben heben (~1 s)")
    print("    - kurz oben halten / leicht schwingen")
    print("    - Arm zuegig wieder nach unten (~1 s)")
    print()
    print("  NICHT MELDEN (normale Bewegungen):")
    print("    - schreiben, kratzen, Arm leicht bewegen,")
    print("    - aufstehen, hinsetzen, Sachen greifen ...")
    print("    (aber NICHT die Meldebewegung machen!)")
    print()
    input("ENTER druecken, wenn du bereit bist ...")

    os.makedirs(OUT_DIR, exist_ok=True)
    alle_zeilen = []

    # ---- Teil 1: MELDEN ----
    print("\n### TEIL 1: MELDEN ###")
    for i in range(1, N_TRIALS + 1):
        samples = sammle_bewegung(ser, "MELDEN", i)
        for t, s in enumerate(samples):
            alle_zeilen.append(['melden', i, t] + s)

    # ---- Teil 2: NICHT MELDEN ----
    print("\n\n### TEIL 2: NICHT MELDEN ###")
    print("Jetzt normale Alltagsbewegungen machen (KEIN Melden!).")
    input("ENTER druecken fuer Teil 2 ...")
    for i in range(1, N_TRIALS + 1):
        samples = sammle_bewegung(ser, "NICHT MELDEN", i)
        for t, s in enumerate(samples):
            alle_zeilen.append(['nicht_melden', i, t] + s)

    # ---- Speichern ----
    with open(OUT_FILE, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['label', 'trial', 't', 'ax', 'ay', 'az', 'gx', 'gy', 'gz'])
        w.writerows(alle_zeilen)

    n_melden = sum(1 for z in alle_zeilen if z[0] == 'melden')
    n_nicht = sum(1 for z in alle_zeilen if z[0] == 'nicht_melden')
    print()
    print("=" * 55)
    print(f"FERTIG! Gespeichert in: {OUT_FILE}")
    print(f"  {n_melden} Samples MELDEN, {n_nicht} Samples NICHT MELDEN")
    print("=" * 55)

    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nAbgebrochen.")
        sys.exit(0)
