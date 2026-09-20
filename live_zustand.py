#!/usr/bin/env python3
"""
Live-Zustandsmaschine fuer die Meldebewegung (mit Personen-Kalibrierung)

Warum Kalibrierung?
  - Jede Person traegt das Band anders (Drehung am Handgelenk)
  - Jede Person hebt den Arm etwas anders
  -> Die Schwerkraft-Richtung bei "Arm oben" ist personenabhaengig.
  -> Deshalb wird sie beim Start fuer die tragende Person gelernt.

Ablauf:
  1. Kalibrierung: "Arm unten" und "Arm oben" je 2 s halten
  2. Zustandsmaschine:
       unten -> hoch (halten) -> unten  =  1 Meldung

Ausfuehren:
    python3 live_zustand.py

VOR DEM START: Seriellen Monitor der Arduino IDE schliessen!
"""

import serial
import time
import sys
import numpy as np
from collections import deque

PORT = '/dev/ttyACM0'
BAUD = 115200
FENSTER = 50            # 0,5 s
AUSWERT_INTERVALL = 20  # alle 20 Samples (0,2 s) auswerten

ENTER_HOCH = 0.10       # score > Wert  -> Arm ist "hoch"
LEAVE_HOCH = 0.00       # score < Wert  -> Arm ist wieder "unten"
MIN_HALTEN = 0.5        # Sekunden oben halten, damit es zaehlt
SPERRE = 2.5            # Mindestabstand zwischen zwei Meldungen


def read_sample(ser):
    """Liest eine gueltige Sample-Zeile (6 ints) vom seriellen Port."""
    while True:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            return None
        if not line:
            continue
        parts = line.split(',')
        if len(parts) == 6:
            try:
                return [int(p) for p in parts]
            except ValueError:
                continue


def kalibriere(ser, anweisung, dauer=2.0):
    """Laesst die Person eine Position einnehmen und lernt die Richtung."""
    print(f"\n  {anweisung}")
    for i in range(3, 0, -1):
        print(f"     {i} ...", flush=True)
        time.sleep(1)
    print("     >>> Position RUHUIG HALTEN <<<", flush=True)
    ser.reset_input_buffer()

    acc = []
    t0 = time.time()
    while time.time() - t0 < dauer:
        s = read_sample(ser)
        if s:
            acc.append(s[0:3])
    if len(acc) < 20:
        print("  FEHLER: zu wenige Daten, versuche es erneut!")
        return kalibriere(ser, anweisung, dauer)
    v = np.array(acc, dtype=float).mean(axis=0)
    v = v / np.linalg.norm(v)
    return v


def main():
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
    time.sleep(0.6)

    print("=" * 55)
    print("  MELDEZAEHLER - Kalibrierung fuer die tragende Person")
    print("=" * 55)

    N = kalibriere(ser, "1) Arm UNTEN halten (ruhig)")
    H = kalibriere(ser, "2) Arm HOCH heben und halten (ruhig)")

    winkel = np.degrees(np.arccos(np.clip(np.dot(N, H), -1, 1)))
    print(f"\n  Kalibriert:  unten={N}  hoch={H}")
    print(f"  Winkel zwischen unten und hoch: {winkel:.0f} Grad")

    # Warten, bis der Arm wieder unten ist (verhindert Fehlzaehlung nach Kalibrierung)
    print("\n  Arm jetzt wieder UNTEN halten ...")
    puffer = deque(maxlen=FENSTER)
    while True:
        s = read_sample(ser)
        if s is None:
            break
        puffer.append(s)
        if len(puffer) == FENSTER:
            arr = np.array(puffer, dtype=float)
            v = arr[:, 0:3].mean(axis=0)
            v = v / np.linalg.norm(v)
            score = float(np.dot(v, H) - np.dot(v, N))
            if score < LEAVE_HOCH:
                break

    print("\n" + "=" * 55)
    print("  BEREIT! Meldebewegung machen (Strg+C zum Beenden)")
    print("  unten -> hoch -> unten = 1 Meldung")
    print("=" * 55 + "\n")

    zustand = 'unten'
    hoch_seit = None
    meldungen = 0
    letzte_meldung = 0
    zaehler = 0
    score_aktuell = 0.0

    while True:
        s = read_sample(ser)
        if s is None:
            break
        puffer.append(s)
        zaehler += 1

        if len(puffer) == FENSTER and zaehler % AUSWERT_INTERVALL == 0:
            arr = np.array(puffer, dtype=float)
            v = arr[:, 0:3].mean(axis=0)
            norm = np.linalg.norm(v)
            if norm > 1e-6:
                v = v / norm
            score = float(np.dot(v, H) - np.dot(v, N))
            score_aktuell = score

            jetzt = time.time()

            # ---- Zustandsmaschine ----
            if zustand == 'unten':
                if score > ENTER_HOCH:
                    zustand = 'hoch'
                    hoch_seit = jetzt
                    print(f"\n  ^ ARM OBEN (score={score:+.2f})")
            elif zustand == 'hoch':
                if score < LEAVE_HOCH:
                    dauer = jetzt - hoch_seit if hoch_seit else 0
                    if dauer >= MIN_HALTEN and (jetzt - letzte_meldung) > SPERRE:
                        meldungen += 1
                        letzte_meldung = jetzt
                        print(f"\n>>> MELDUNG ERKANNT!  (oben {dauer:.1f}s)  Gesamt: {meldungen} <<<\n")
                    else:
                        print(f"\n  v arm unten (zu kurz: {dauer:.1f}s) -> kein Zaehler")
                    zustand = 'unten'

            sys.stdout.write(f"\r  Zustand={zustand:5s}  score={score:+.2f}  Meldungen={meldungen}   ")
            sys.stdout.flush()

    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nBeendet.")
        sys.exit(0)
