#!/usr/bin/env python3
"""
Live-Zustandsmaschine fuer die Meldebewegung

Prinzip:
  - Der Klassifikator liefert nur noch den ZUSTAND des Arms:
        "unten" oder "hoch"  (anhand der Schwerkraft-Richtung)
  - Die Zustandsmaschine zaehlt:
        unten -> hoch (halten) -> unten  =  1 Meldung
  - Andere Ablaeufe (z.B. unten -> runter -> unten) loesen nichts aus.

Ausfuehren:
    python3 live_zustand.py

VOR DEM START: Seriellen Monitor der Arduino IDE schliessen!
"""

import serial
import time
import sys
import numpy as np
import pandas as pd
from collections import deque
import warnings
warnings.filterwarnings('ignore')

PORT = '/dev/ttyACM0'
BAUD = 115200
FENSTER = 50            # 0,5 s
AUSWERT_INTERVALL = 20  # alle 20 Samples (0,2 s) auswerten

ENTER_HOCH = 0.90       # dot > Wert  -> Arm ist "hoch"
LEAVE_HOCH = 0.75       # dot < Wert  -> Arm ist wieder "unten"
MIN_HALTEN = 0.5        # Sekunden oben halten, damit es zaehlt
SPERRE = 2.5            # Mindestabstand zwischen zwei Meldungen


def lerne_hoch_referenz():
    """Lernt die Schwerkraft-Richtung bei 'Arm oben' aus den alten Daten."""
    df = pd.read_csv('meldedaten/daten.csv')
    melden = df[df['label'] == 'melden']
    refs = []
    for trial in sorted(melden['trial'].unique()):
        g = melden[melden['trial'] == trial]
        a = g[['ax', 'ay', 'az']].values.astype(float)
        start = a[:30].mean(axis=0)
        start = start / np.linalg.norm(start)
        mindot = 1.0
        best = None
        for i in range(0, len(a) - 50, 5):
            v = a[i:i + 50].mean(axis=0)
            v = v / np.linalg.norm(v)
            d = np.dot(start, v)
            if d < mindot:
                mindot = d
                best = v
        if mindot < 0.6:
            refs.append(best)
    H = np.array(refs).mean(axis=0)
    H = H / np.linalg.norm(H)
    return H


def main():
    H = lerne_hoch_referenz()
    print(f"'Arm oben'-Referenz gelernt: ({H[0]:+.3f}, {H[1]:+.3f}, {H[2]:+.3f})\n")

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
    print("  LIVE-ZUSTANDSMASCHINE  (Strg+C zum Beenden)")
    print("=" * 55)
    print("Arm ruhig unten halten, dann Meldebewegung machen.")
    print("Zustand: unten -> hoch -> unten = 1 Meldung\n")

    puffer = deque(maxlen=FENSTER)
    zustand = 'unten'
    hoch_seit = None
    meldungen = 0
    letzte_meldung = 0
    zaehler = 0
    dot_aktuell = 0.0

    while True:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            break
        if not line:
            continue
        parts = line.split(',')
        if len(parts) != 6:
            continue
        try:
            werte = [int(p) for p in parts]
        except ValueError:
            continue

        puffer.append(werte)
        zaehler += 1

        if len(puffer) == FENSTER and zaehler % AUSWERT_INTERVALL == 0:
            arr = np.array(puffer, dtype=float)
            a = arr[:, 0:3]
            v = a.mean(axis=0)
            norm = np.linalg.norm(v)
            if norm > 1e-6:
                v = v / norm
            dot = float(np.dot(v, H))
            dot_aktuell = dot

            jetzt = time.time()

            # ---- Zustandsmaschine ----
            if zustand == 'unten':
                if dot > ENTER_HOCH:
                    zustand = 'hoch'
                    hoch_seit = jetzt
                    print(f"\n  ^ ARM OBEN (dot={dot:.2f})")
            elif zustand == 'hoch':
                if dot < LEAVE_HOCH:
                    dauer = jetzt - hoch_seit if hoch_seit else 0
                    if dauer >= MIN_HALTEN and (jetzt - letzte_meldung) > SPERRE:
                        meldungen += 1
                        letzte_meldung = jetzt
                        print(f"\n>>> MELDUNG ERKANNT!  (oben gehalten {dauer:.1f}s)  Gesamt: {meldungen} <<<\n")
                    else:
                        print(f"\n  v arm wieder unten (zu kurz: {dauer:.1f}s) -> kein Zaehler")
                    zustand = 'unten'

            # Statuszeile
            sys.stdout.write(f"\r  Zustand={zustand:5s}  dot={dot:+.2f}  Meldungen={meldungen}   ")
            sys.stdout.flush()

    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nBeendet.")
        sys.exit(0)
