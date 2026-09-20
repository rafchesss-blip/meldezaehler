#!/usr/bin/env python3
"""
Live-Erkennung mit Schueler-Animation und HOEHEN-Schaetzung

Neu:
  - Doppelte Integration der Beschleunigung -> vertikaler WEG (Hoehe)
  - Meldung zaehlt nur, wenn die Hand wirklich hoch gehoben wurde
    (Orientierung "meldung" UND Hoehe ueber Schwelle)
  - Serielles Lesen im Hintergrund-Thread (kein Haengen mehr)
  - Sitzende Schueler-Figur am Tisch

Ausfuehren:
    python3 live_animation.py
"""

import serial
import time
import sys
import threading
import queue
import numpy as np
from collections import deque
import joblib
import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
import warnings
warnings.filterwarnings('ignore')

PORT = '/dev/ttyACM0'
BAUD = 115200

MIN_HALTEN = 0.5      # Sekunden oben, damit es zaehlt
SPERRE = 2.5          # Mindestabstand zwischen zwei Meldungen
HOEHEN_SCHWELLE = 0.20  # Meter: Hand muss so hoch gehoben werden
PUFFER_GROESSE = 300   # 3 s fuer die Hoehen-Schaetzung

POS = {
    'tisch':   np.array([0.62, 1.00]),
    'kopf':    np.array([0.28, 2.05]),
    'meldung': np.array([0.00, 2.90]),
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


def schaetze_hoehe(puffer):
    """Doppelte Integration der Beschleunigung -> vertikaler Weg in Meter."""
    a = np.array(puffer, dtype=float)[:, 0:3] / 16384.0  # nominal g
    n = len(a)
    if n < 20:
        return 0.0

    # Schwerkraft-Richtung per Tiefpass (EMA) verfolgen
    alpha = 0.05
    g = np.zeros_like(a)
    g[0] = a[0]
    for i in range(1, n):
        g[i] = g[i - 1] + alpha * (a[i] - g[i - 1])

    gm = np.linalg.norm(g, axis=1)
    gm[gm < 1e-6] = 1e-6
    up = -g / gm[:, None]

    # Vertikale Bewegungsbeschleunigung (Gravitation entfernt)
    a_up = np.sum(a * up, axis=1) + gm   # in nominal g
    a_ms2 = a_up * 9.81                  # in m/s^2

    dt = 0.01
    v = np.cumsum(a_ms2) * dt

    # Drift entfernen (Anfang/Ende in Ruhe -> Geschwindigkeit 0)
    idx = np.arange(n)
    v = v - v[-1] * idx / max(n - 1, 1)

    s = np.cumsum(v) * dt
    return float(s.max() - s[0])          # Spitzen-Hoehe


def merkmale(arr):
    a = arr[:, 0:3] / 16384.0
    g = arr[:, 3:6] / 32.8
    m = a.mean(axis=0)
    m = m / np.linalg.norm(m)
    acc_std = float(a.std(axis=0).sum())
    gyrmag = np.sqrt((g ** 2).sum(axis=1))
    return np.array([[m[0], m[1], m[2], acc_std, float(gyrmag.mean()), float(gyrmag.max())]])


def zeichne(ax, hand_pos, klasse, p, meldungen, hoehe):
    ax.clear()
    ax.set_xlim(-1.2, 1.4)
    ax.set_ylim(-0.2, 3.2)
    ax.set_aspect('equal')
    ax.axis('off')

    # --- Tisch ---
    ax.plot([0.55, 1.15], [1.0, 1.0], color='saddlebrown', lw=6)
    ax.plot([0.6, 0.6], [0.0, 1.0], color='saddlebrown', lw=4)
    ax.plot([1.1, 1.1], [0.0, 1.0], color='saddlebrown', lw=4)

    # --- Stuhl ---
    ax.plot([-0.55, -0.05], [0.55, 0.55], color='gray', lw=4)      # Sitzflaeche
    ax.plot([-0.55, -0.55], [0.0, 0.55], color='gray', lw=3)       # Stuhlbein

    # --- Sitzende Figur ---
    # Torso (Huefte -> Schulter)
    ax.plot([0, 0], [0.95, 1.75], color='navy', lw=6)
    # Kopf
    ax.add_patch(plt.Circle((0, 2.05), 0.28, color='#f4c89a', ec='black'))
    # Oberschenkel (Huefte -> Knie, waagerecht)
    ax.plot([0, 0.42], [0.95, 0.95], color='navy', lw=6)
    # Unterschenkel (Knie -> Fuss, senkrecht)
    ax.plot([0.42, 0.42], [0.95, 0.1], color='navy', lw=6)
    # Fuss
    ax.plot([0.42, 0.62], [0.1, 0.1], color='navy', lw=4)

    # --- Arm (Schulter -> Hand) ---
    schulter = np.array([0.0, 1.72])
    farbe = FARBE.get(klasse, 'gray')
    ax.plot([schulter[0], hand_pos[0]], [schulter[1], hand_pos[1]],
            color=farbe, lw=6, solid_capstyle='round')
    ax.plot([hand_pos[0]], [hand_pos[1]], 'o', color=farbe, ms=13, mec='black')

    # --- Texte ---
    ax.set_title(f"Position: {klasse.upper()}    "
                 f"P(meldung)={p.get('meldung', 0):.2f}  "
                 f"P(kopf)={p.get('kopf', 0):.2f}  "
                 f"P(tisch)={p.get('tisch', 0):.2f}", fontsize=10)
    ax.text(0.35, 3.0, f"Hoehe: {hoehe:.2f} m", fontsize=12, ha='center', color='blue')
    ax.text(0.35, 2.7, f"Meldungen: {meldungen}", fontsize=13, ha='center',
            color='red', fontweight='bold')
    ax.text(-1.15, -0.15, "Strg+C zum Beenden", fontsize=8, color='gray')


def main():
    daten = joblib.load('modell_positionen.joblib')
    model = daten['modell']
    FENSTER = daten['fenster']    # 150
    SCHRITT = daten['schritt']    # 50

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

    # Hintergrund-Thread fuellt eine Queue, Hauptloop verarbeitet
    sample_queue = queue.Queue()
    stop = threading.Event()

    def leser():
        while not stop.is_set():
            s = read_sample(ser)
            if s:
                sample_queue.put(s)

    t = threading.Thread(target=leser, daemon=True)
    t.start()

    plt.ion()
    fig, ax = plt.subplots(figsize=(5.5, 6.5))
    fig.canvas.manager.set_window_title("Meldezaehler")

    puffer = deque(maxlen=PUFFER_GROESSE)
    meldungen = 0
    letzte_meldung = 0
    hoch_seit = None
    im_hoch = False
    zaehler = 0
    p = {'meldung': 0.0, 'kopf': 0.0, 'tisch': 1.0}
    klasse = 'tisch'
    hoehe = 0.0
    letzter_draw = 0

    print("Live-Erkennung laeuft. Hand hoch heben = Meldung.\n")

    while True:
        # Alle neuen Samples aus der Queue uebernehmen
        try:
            while True:
                s = sample_queue.get_nowait()
                puffer.append(s)
                zaehler += 1
        except queue.Empty:
            pass

        # Klassifizieren + Hoehe schaetzen alle SCHRITT Samples
        if len(puffer) >= FENSTER and zaehler >= SCHRITT:
            zaehler = 0
            arr = np.array(puffer, dtype=float)

            x = merkmale(arr[-FENSTER:])
            proba = model.predict_proba(x)[0]
            p = dict(zip(model.classes_, proba))
            klasse = max(p, key=p.get)

            hoehe = schaetze_hoehe(puffer)

            jetzt = time.time()
            # ---- Zustandsmaschine: nur zaehlen, wenn wirklich hoch ----
            if klasse == 'meldung' and hoehe >= HOEHEN_SCHWELLE:
                if not im_hoch:
                    im_hoch = True
                    hoch_seit = jetzt
            else:
                if im_hoch:
                    dauer = jetzt - hoch_seit if hoch_seit else 0
                    if dauer >= MIN_HALTEN and (jetzt - letzte_meldung) > SPERRE:
                        meldungen += 1
                        letzte_meldung = jetzt
                        print(f">>> MELDUNG! (Hoehe {hoehe:.2f}m, oben {dauer:.1f}s)  Gesamt: {meldungen}")
                    im_hoch = False

        # Animation (~10 Hz)
        jetzt = time.time()
        if jetzt - letzter_draw > 0.1:
            letzter_draw = jetzt
            hand = (p.get('tisch', 0) * POS['tisch'] +
                    p.get('kopf', 0) * POS['kopf'] +
                    p.get('meldung', 0) * POS['meldung'])
            zeichne(ax, hand, klasse, p, meldungen, hoehe)
        plt.pause(0.02)

    stop.set()
    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nBeendet.")
        sys.exit(0)
