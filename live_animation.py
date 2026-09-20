#!/usr/bin/env python3
"""
Live-Erkennung mit Schueler-Animation

Klassifiziert die Hand-Position (meldung / kopf / tisch) in Echtzeit,
zaehlt Meldungen ueber eine Zustandsmaschine und zeigt eine
Schueler-Figur, deren Arm der geschaetzten Hand-Position folgt.

Ausfuehren:
    python3 live_animation.py

VOR DEM START: Seriellen Monitor der Arduino IDE schliessen!
"""

import serial
import time
import sys
import numpy as np
from collections import deque
import joblib
import matplotlib
matplotlib.use('TkAgg')  # funktioniert auf dem Desktop
import matplotlib.pyplot as plt
import warnings
warnings.filterwarnings('ignore')

PORT = '/dev/ttyACM0'
BAUD = 115200

MIN_HALTEN = 0.5   # Sekunden ueber Kopf, damit es zaehlt
SPERRE = 2.5       # Mindestabstand zwischen zwei Meldungen

# Hand-Zielpositionen in der Figur
POS = {
    'tisch':   np.array([0.55, 0.75]),
    'kopf':    np.array([0.30, 2.50]),
    'meldung': np.array([0.00, 3.55]),
}
FARBE = {'meldung': 'red', 'kopf': 'orange', 'tisch': 'green'}


def read_sample(ser):
    try:
        line = ser.readline().decode('ascii', errors='replace').strip()
    except Exception:
        return None
    if not line:
        return None
    parts = line.split(',')
    if len(parts) == 6:
        try:
            return [int(p) for p in parts]
        except ValueError:
            return None
    return None


def merkmale(arr):
    a = arr[:, 0:3] / 16384.0
    g = arr[:, 3:6] / 32.8
    m = a.mean(axis=0)
    m = m / np.linalg.norm(m)
    acc_std = float(a.std(axis=0).sum())
    gyrmag = np.sqrt((g ** 2).sum(axis=1))
    return np.array([[m[0], m[1], m[2], acc_std, float(gyrmag.mean()), float(gyrmag.max())]])


def zeichne(ax, hand_pos, klasse, p, meldungen):
    ax.clear()
    ax.set_xlim(-1.2, 1.2)
    ax.set_ylim(-0.3, 3.9)
    ax.set_aspect('equal')
    ax.axis('off')

    # Tisch
    ax.plot([0.45, 1.0], [0.7, 0.7], color='saddlebrown', lw=5)
    ax.plot([0.5, 0.5], [0.0, 0.7], color='saddlebrown', lw=3)
    ax.plot([0.95, 0.95], [0.0, 0.7], color='saddlebrown', lw=3)

    # Koerper
    ax.add_patch(plt.Circle((0, 2.6), 0.32, color='#f4c89a', ec='black'))
    ax.plot([0, 0], [2.28, 1.0], color='navy', lw=5)
    ax.plot([0, -0.4], [1.0, 0.0], color='navy', lw=5)
    ax.plot([0, 0.4], [1.0, 0.0], color='navy', lw=5)

    # Arm (Schulter -> Hand)
    schulter = np.array([0.0, 2.1])
    farbe = FARBE.get(klasse, 'gray')
    ax.plot([schulter[0], hand_pos[0]], [schulter[1], hand_pos[1]], color=farbe, lw=6, solid_capstyle='round')
    ax.plot([hand_pos[0]], [hand_pos[1]], 'o', color=farbe, ms=14, mec='black')

    # Texte
    ax.set_title(f"Hand-Position: {klasse.upper()}    "
                 f"P(meldung)={p.get('meldung', 0):.2f}  "
                 f"P(kopf)={p.get('kopf', 0):.2f}  "
                 f"P(tisch)={p.get('tisch', 0):.2f}", fontsize=10)
    ax.text(0, 3.7, f"Meldungen: {meldungen}", fontsize=14, ha='center',
            color='red', fontweight='bold')
    ax.text(-1.15, -0.2, "Strg+C zum Beenden", fontsize=8, color='gray')


def main():
    daten = joblib.load('modell_positionen.joblib')
    model = daten['modell']
    FENSTER = daten['fenster']
    SCHRITT = daten['schritt']

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.05
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

    plt.ion()
    fig, ax = plt.subplots(figsize=(5.5, 7))
    fig.canvas.manager.set_window_title("Meldezaehler - Hand-Position")

    puffer = deque(maxlen=FENSTER)
    meldungen = 0
    letzte_meldung = 0
    hoch_seit = None
    im_hoch = False
    zaehler = 0
    p = {'meldung': 0.0, 'kopf': 0.0, 'tisch': 1.0}
    klasse = 'tisch'
    letzter_draw = 0

    print("Live-Erkennung laeuft. Schliesse das Animationsfenster oder Strg+C im Terminal.")
    print("Hand ueber den Kopf heben = Meldung\n")

    while True:
        # Alle verfuegbaren Samples lesen
        while ser.in_waiting:
            s = read_sample(ser)
            if s:
                puffer.append(s)
                zaehler += 1

        # Klassifizieren alle SCHRITT Samples
        if len(puffer) == FENSTER and zaehler % SCHRITT == 0:
            x = merkmale(np.array(puffer, dtype=float))
            proba = model.predict_proba(x)[0]
            p = dict(zip(model.classes_, proba))
            klasse = max(p, key=p.get)
            jetzt = time.time()

            # ---- Zustandsmaschine ----
            if klasse == 'meldung':
                if not im_hoch:
                    im_hoch = True
                    hoch_seit = jetzt
            else:
                if im_hoch:
                    dauer = jetzt - hoch_seit if hoch_seit else 0
                    if dauer >= MIN_HALTEN and (jetzt - letzte_meldung) > SPERRE:
                        meldungen += 1
                        letzte_meldung = jetzt
                        print(f">>> MELDUNG! (oben {dauer:.1f}s)  Gesamt: {meldungen}")
                    im_hoch = False

        # Animation aktualisieren (~10 Hz)
        jetzt = time.time()
        if jetzt - letzter_draw > 0.1:
            letzter_draw = jetzt
            hand = (p.get('tisch', 0) * POS['tisch'] +
                    p.get('kopf', 0) * POS['kopf'] +
                    p.get('meldung', 0) * POS['meldung'])
            zeichne(ax, hand, klasse, p, meldungen)
            plt.pause(0.01)

    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nBeendet.")
        sys.exit(0)
