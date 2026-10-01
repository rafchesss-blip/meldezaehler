#!/usr/bin/env python3
"""
Sammelt CNN-Trainingsdaten direkt von der Uhr (QMI8658) – so viel du willst.

Die Uhr streamt nach dem Befehl "STREAM" kontinuierlich ~100 Hz die 6 Kanäle
(ax, ay, az, gx, gy, gz), bereits rotiert ins Trainings-Koordinatensystem und
in physikalischen Einheiten (g / dps).

Bedienung (Live-Aufnahme, ein Tastendruck, kein ENTER):

    1  =  MELDUNG        (Arm strecken, Hand über den Kopf)
    2  =  NICHT MELDEN    (normal bewegen, herumlaufen, kleine Bewegungen)
    q  =  beenden

Sobald du z. B. "1" drückst, wird ab sofort JEDES ankommende Sample als
"meldung" gespeichert – so lange, bis du eine andere Zahl drückst (dann wird
auf die neue Klasse umgeschaltet) oder "q" drückst. Jeder Tastendruck startet
einen neuen Trial der gedrückten Klasse.

Ausgabe: meldedaten/uhr_positionen.csv (physikalische Einheiten)
"""

import collections
import csv
import glob
import os
import sys
import threading
import time

import serial

BAUD = 115200
FENSTER = 150          # 1,5 s bei 100 Hz (muss zum CNN passen)
OUT_FILE = 'meldedaten/uhr_positionen.csv'
LABELS = {'1': 'meldung', '2': 'nicht_meldung'}
ANWEISUNG = {
    'meldung': 'Arm strecken, Hand UEBER den Kopf',
    'nicht_meldung': 'normal bewegen, herumlaufen, kleine Bewegungen',
}


def finde_port():
    kandidaten = sorted(glob.glob('/dev/ttyACM*') + glob.glob('/dev/ttyUSB*'))
    return kandidaten[0] if kandidaten else None


def getch():
    """Liest einen einzelnen Tastendruck ohne ENTER (Linux)."""
    try:
        import termios
        import tty
        fd = sys.stdin.fileno()
        alt = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            return sys.stdin.read(1)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, alt)
    except Exception:
        return sys.stdin.readline().strip()


def lade_trial_start(datei):
    """Höchster vorhandener Trial je Label, damit neue Läufe keine Nummern
    doppelt vergeben (wichtig für den GroupKFold-Split)."""
    starts = {k: 0 for k in LABELS.values()}
    if not os.path.exists(datei):
        return starts
    try:
        with open(datei, 'r') as f:
            for row in csv.DictReader(f):
                lbl = row.get('label')
                if lbl in starts:
                    try:
                        starts[lbl] = max(starts[lbl], int(row['trial']))
                    except (ValueError, KeyError):
                        pass
    except Exception:
        pass
    return starts


def oeffne_seriell(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()

    # Native-USB-Reset der ESP32-S3-Uhr (wie in den alten Sammel-Skripten)
    time.sleep(0.3)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(True)
    time.sleep(0.5)
    return ser


def warte_auf_stream(ser, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ser.reset_input_buffer()
        ser.write(b'STREAM\n')
        t0 = time.time()
        while time.time() - t0 < 2.0:
            line = ser.readline().decode('ascii', errors='replace').strip()
            if line == 'STREAMING':
                return True
        time.sleep(0.5)
    return False


def main():
    port = finde_port()
    if not port:
        print('FEHLER: Keine Uhr gefunden (/dev/ttyACM* oder /dev/ttyUSB*).')
        print('Uhr per USB anschliessen und ggf. die Firmware neu flashen.')
        sys.exit(1)

    print(f'Oeffne serielle Verbindung zu {port} ...')
    ser = oeffne_seriell(port)

    print('Warte auf Uhr und starte Stream ...')
    time.sleep(2.5)
    if not warte_auf_stream(ser):
        print('FEHLER: Uhr antwortet nicht auf STREAM.')
        print('  - Ist die Firmware geflasht?')
        print('  - Ist die Uhr bereits kalibriert (beim ersten Start einmal machen)?')
        print('  - Ist der serielle Monitor der Arduino IDE geschlossen?')
        ser.close()
        sys.exit(1)

    print('=' * 62)
    print('  CNN-TRAININGSDATEN SAMMELN (Live-Aufnahme)')
    print('=' * 62)
    print()
    print('  1 = MELDUNG        : ' + ANWEISUNG['meldung'])
    print('  2 = NICHT MELDEN   : ' + ANWEISUNG['nicht_meldung'])
    print('  q = beenden')
    print()
    print('  Taste druecken = Aufnahme mit dieser Klasse STARTEN.')
    print('  Eine andere Zahl = auf die neue Klasse UMSCHALTEN.')
    print('  Es laeuft so lange, bis du umschaltest oder q drueckst.')
    print('  (Jede Position mindestens ~1,5 s halten, sonst ist der')
    print('   Trial zu kurz fuer das Training.)')
    print()
    input('ENTER druecken, um zu starten ...')

    os.makedirs('meldedaten', exist_ok=True)
    neu = not os.path.exists(OUT_FILE)
    starts = lade_trial_start(OUT_FILE)
    next_trial = {k: v + 1 for k, v in starts.items()}

    # Aktueller Aufnahme-Zustand (von Reader-Thread + Tastatur geteilt)
    lock = threading.Lock()
    cur = {'label': None, 'trial': 0, 't': 0}
    recorded = {k: {'trials': 0, 'samples': 0} for k in LABELS.values()}
    stop = threading.Event()

    with open(OUT_FILE, 'a', newline='') as f:
        w = csv.writer(f)
        if neu:
            w.writerow(['label', 'trial', 't', 'ax', 'ay', 'az', 'gx', 'gy', 'gz'])
            f.flush()

        def leser():
            n_rows = 0
            while not stop.is_set():
                try:
                    line = ser.readline().decode('ascii', errors='replace').strip()
                except Exception:
                    break
                if not line:
                    continue
                if line.startswith('OK stream stop'):
                    continue
                teile = line.split(',')
                if len(teile) != 6:
                    continue
                try:
                    werte = [float(p) for p in teile]
                except ValueError:
                    continue

                with lock:
                    if cur['label'] is None:
                        row = None
                    else:
                        row = [cur['label'], cur['trial'], cur['t']] + werte
                        cur['t'] += 1

                if row is not None:
                    w.writerow(row)
                    n_rows += 1
                    if n_rows % 10 == 0:
                        f.flush()
            f.flush()

        t = threading.Thread(target=leser, daemon=True)
        t.start()
        t0 = time.time()

        def beende(info):
            lbl = info['label']
            if lbl is None:
                return
            recorded[lbl]['trials'] += 1
            recorded[lbl]['samples'] += info['t']
            hinweis = ''
            if info['t'] < FENSTER:
                hinweis = f'   (zu kurz! < {FENSTER} Samples -> zaehlt nicht im Training)'
            print(f'  beendet: {lbl:8s} Trial {info["trial"]:3d}  '
                  f'({info["t"]} Samples){hinweis}')

        print('\nLaeuft! Erste Taste druecken, um eine Aufnahme zu starten.\n')

        try:
            while True:
                taste = getch()
                if taste in ('q', 'Q', '\x03', '\x04'):
                    break

                if taste not in LABELS:
                    if taste in ('\n', '\r'):
                        continue
                    print(f'  Unbekannte Taste: {taste!r}  (1/2/3 oder q)')
                    continue

                label = LABELS[taste]
                with lock:
                    prev = dict(cur)
                    cur['label'] = label
                    cur['trial'] = next_trial[label]
                    next_trial[label] += 1
                    cur['t'] = 0
                    neu_trial = cur['trial']

                beende(prev)
                f.flush()
                el = time.time() - t0
                print(f'  >>> AUFNAHME {label:8s} Trial {neu_trial:3d} '
                      f'gestartet  [{el:.0f}s]')
        finally:
            stop.set()
            t.join(timeout=1.5)

            with lock:
                final = dict(cur)
            beende(final)
            f.flush()

            print()
            print('=' * 62)
            print('Beende Stream ...')
            try:
                ser.write(b'STOPSTREAM\n')
                time.sleep(0.3)
                ser.reset_input_buffer()
            except Exception:
                pass
            try:
                ser.close()
            except Exception:
                pass

    gesamt_trials = sum(v['trials'] for v in recorded.values())
    gesamt_samples = sum(v['samples'] for v in recorded.values())
    print(f'Fertig! Gespeichert: {OUT_FILE}')
    print(f'Dieser Lauf: {gesamt_trials} Trials, {gesamt_samples} Samples')
    for lbl, v in recorded.items():
        print(f'  {lbl:8s}: {v["trials"]:3d} Trials, {v["samples"]:5d} Samples')
    if gesamt_trials:
        print()
        print('Naechster Schritt (CNN neu trainieren + in die Uhr einbetten):')
        print(f'  python3 trainieren_cnn.py {OUT_FILE}')
        print(f'  python3 konvertiere_cnn_tflite.py {OUT_FILE}')
        print('  ./build.sh firmware')
    print('=' * 62)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\nAbgebrochen.')
        sys.exit(0)
