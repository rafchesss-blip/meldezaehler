#!/usr/bin/env python3
"""
Live-Test der Meldebewegungs-Erkennung am PC

Liest den Dauer-Stream des ESP32, haelt ein gleitendes
3-Sekunden-Fenster, berechnet die 8 wichtigsten Merkmale
und klassifiziert mit dem trainierten LogReg-Modell.

Anzeige:
  - eine Statuszeile mit aktueller Wahrscheinlichkeit p(melden)
  - ">>> MELDUNG ERKANNT!" wenn eine Meldung erkannt wird

Bedienung:
  - Sensor am Handgelenk, Arm normal bewegen
  - Meldebewegung machen -> sollte erkannt werden
  - Ctrl+C zum Beenden

VOR DEM START: Seriellen Monitor der Arduino IDE schliessen!
"""

import serial
import time
import sys
import numpy as np
import pandas as pd
from collections import deque
from sklearn.linear_model import LogisticRegression
from sklearn.preprocessing import StandardScaler
import warnings
warnings.filterwarnings('ignore')

PORT = '/dev/ttyACM0'
BAUD = 115200
FS = 100.0
FENSTER = 300            # 3 Sekunden
TOP_FEATURES = ['gz_std', 'winkel_gx', 'az_range', 'ax_std',
                'gx_range', 'gyrmag_std', 'gy_std', 'ay_range']
SCHWELLE = 0.5           # Wahrscheinlichkeit fuer "melden"
SPERRE_S = 2.5           # Mindestabstand zwischen zwei Erkennungen
KLASSIFIZIER_INTERVALL = 10  # alle 10 Samples (100 ms) klassifizieren


def features_aus_array(a, g):
    """a, g: (300,3) Arrays in physikalischen Einheiten (g bzw. dps)."""
    f = {}
    for i, n in enumerate(['ax', 'ay', 'az']):
        f[n + '_std'] = a[:, i].std()
        f[n + '_range'] = a[:, i].max() - a[:, i].min()
    for i, n in enumerate(['gx', 'gy', 'gz']):
        f[n + '_std'] = g[:, i].std()
        f[n + '_range'] = g[:, i].max() - g[:, i].min()
    acc_mag = np.sqrt((a ** 2).sum(axis=1))
    gyr_mag = np.sqrt((g ** 2).sum(axis=1))
    f['accmag_std'] = acc_mag.std()
    f['accmag_range'] = acc_mag.max() - acc_mag.min()
    f['accmag_dev'] = np.abs(acc_mag - acc_mag.mean()).mean()
    f['gyrmag_mean'] = gyr_mag.mean()
    f['gyrmag_std'] = gyr_mag.std()
    f['gyrmag_max'] = gyr_mag.max()
    f['rot_energie'] = (g ** 2).sum()
    f['acc_energie'] = ((a - a.mean(axis=0)) ** 2).sum()
    dt = 1.0 / FS
    for i, n in enumerate(['gx', 'gy', 'gz']):
        f['winkel_' + n] = np.abs(g[:, i].sum()) * dt
    return f


def modell_laden():
    """Trainiert das LogReg-Modell auf den gespeicherten Daten."""
    df = pd.read_csv('meldedaten/daten.csv')
    zeilen = []
    for (label, trial), gruppe in df.groupby(['label', 'trial']):
        a = gruppe[['ax', 'ay', 'az']].values.astype(float) / 16384.0
        g = gruppe[['gx', 'gy', 'gz']].values.astype(float) / 32.8
        f = features_aus_array(a, g)
        f['label'] = label
        zeilen.append(f)
    F = pd.DataFrame(zeilen)
    X = F[TOP_FEATURES].values
    y = (F['label'] == 'melden').astype(int).values

    scaler = StandardScaler().fit(X)
    model = LogisticRegression(max_iter=1000).fit(scaler.transform(X), y)
    return scaler, model


def main():
    scaler, model = modell_laden()
    print(f"Modell geladen ({len(TOP_FEATURES)} Merkmale).\n")

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()

    # ESP32 neu starten und Boot-Meldungen verwerfen
    time.sleep(0.3)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(True)
    time.sleep(0.6)

    print("=" * 55)
    print("  LIVE-ERKENNUNG  (Strg+C zum Beenden)")
    print("=" * 55)
    print("Sensor 2-3 s ruhig halten, dann Meldebewegung machen.")
    print()

    puffer = deque(maxlen=FENSTER)
    erkannt = 0
    letzte_erkennung = 0
    zaehler = 0
    p_aktuell = 0.0

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

        # Klassifizieren, wenn Fenster voll und Intervall erreicht
        if len(puffer) == FENSTER and zaehler % KLASSIFIZIER_INTERVALL == 0:
            arr = np.array(puffer, dtype=float)
            a = arr[:, 0:3] / 16384.0
            g = arr[:, 3:6] / 32.8
            f = features_aus_array(a, g)
            x = np.array([[f[n] for n in TOP_FEATURES]])
            x = scaler.transform(x)
            p = model.predict_proba(x)[0, 1]
            p_aktuell = p

            jetzt = time.time()
            if p >= SCHWELLE and (jetzt - letzte_erkennung) > SPERRE_S:
                erkannt += 1
                letzte_erkennung = jetzt
                print(f"\n>>> MELDUNG ERKANNT!  (p={p:.2f})  Gesamt: {erkannt} <<<\n")

        # Statuszeile
        if zaehler % 10 == 0:
            sys.stdout.write(f"\r  p(melden) = {p_aktuell:.2f}   erkannt = {erkannt}   ")
            sys.stdout.flush()

    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nBeendet.")
        sys.exit(0)
