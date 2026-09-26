#!/usr/bin/env python3
"""
Vergleicht das in die Firmware eingebettete C-Modell mit dem trainierten
Modell (modell_positionen.joblib).

Nach einem Neutraining (python3 trainieren_positionen.py) sollte man dieses
Skript laufen lassen, um sicherzustellen, dass die Koeffizienten in
UhrMeldezaehler/UhrMeldezaehler_core.h noch aktuell sind:

    python3 verify_model.py

Ausgabe: OK/MISMATCH je Modellteil.
"""

import re
import sys
import numpy as np
import joblib

HEADER = 'UhrMeldezaehler/UhrMeldezaehler_core.h'
MODEL = 'modell_positionen.joblib'

# Zuordnung: (Name im Header, Name im joblib-Modell)
# Die C-Klassenreihenfolge ist 0=kopf, 1=meldung, 2=tisch.
ARRAYS = [
    ('SCALER_MEAN', 'scaler_mean'),
    ('SCALER_SCALE', 'scaler_scale'),
    ('COEF_KOPF', 'coef_kopf'),
    ('COEF_MELDUNG', 'coef_meldung'),
    ('COEF_TISCH', 'coef_tisch'),
    ('INTERCEPT_KOPF', 'intercept_kopf'),
    ('INTERCEPT_MELDUNG', 'intercept_meldung'),
    ('INTERCEPT_TISCH', 'intercept_tisch'),
    ('REF_TISCH', 'ref_tisch'),
    ('REF_MELDUNG', 'ref_meldung'),
]


def parse_header():
    with open(HEADER, 'r', encoding='utf-8') as f:
        text = f.read()

    result = {}
    # Matcht "NAME[...] = { ... };" über mehrere Zeilen.
    for m in re.finditer(r'static const float ([\w]+)(?:\[[^\]]*\])?\s*=\s*\{(.*?)\};',
                         text, re.DOTALL):
        name = m.group(1)
        body = m.group(2)
        vals = re.findall(r'[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?', body)
        result[name] = np.array([float(v) for v in vals], dtype=np.float64)

    # Einzelwerte: "NAME = wert;" (z. B. INTERCEPT_*)
    for m in re.finditer(r'static const float ([\w]+)\s*=\s*([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)[fF]?\s*;',
                         text):
        result[m.group(1)] = np.array([float(m.group(2))], dtype=np.float64)
    return result


def load_model():
    d = joblib.load(MODEL)
    model = d['modell']
    sc = model.named_steps['standardscaler']
    lr = model.named_steps['logisticregression']

    klassen = list(d['klassen'])  # ['kopf', 'meldung', 'tisch']
    idx = {k: i for i, k in enumerate(klassen)}

    return {
        'scaler_mean': sc.mean_.astype(np.float64),
        'scaler_scale': sc.scale_.astype(np.float64),
        'coef_kopf': lr.coef_[idx['kopf']].astype(np.float64),
        'coef_meldung': lr.coef_[idx['meldung']].astype(np.float64),
        'coef_tisch': lr.coef_[idx['tisch']].astype(np.float64),
        'intercept_kopf': np.array([lr.intercept_[idx['kopf']]]),
        'intercept_meldung': np.array([lr.intercept_[idx['meldung']]]),
        'intercept_tisch': np.array([lr.intercept_[idx['tisch']]]),
        'ref_tisch': d['refs']['tisch'].astype(np.float64),
        'ref_meldung': d['refs']['meldung'].astype(np.float64),
    }


def main():
    try:
        header = parse_header()
    except OSError as e:
        print(f'FEHLER: {HEADER} nicht lesbar: {e}')
        return 1
    try:
        model = load_model()
    except OSError as e:
        print(f'FEHLER: {MODEL} nicht lesbar: {e}')
        return 1

    ok = True
    for hname, mname in ARRAYS:
        if hname not in header:
            print(f'MISSING  {hname:18s} (fehlt im Header)')
            ok = False
            continue
        a, b = header[hname], model[mname]
        if a.shape != b.shape or not np.allclose(a, b, atol=1e-6, rtol=1e-5):
            print(f'MISMATCH {hname:18s}  Header={a}  Modell={b}')
            ok = False
        else:
            print(f'OK       {hname:18s}  {a.shape[0]} Werte')

    print()
    if ok:
        print('Ergebnis: Eingebettetes Modell entspricht dem trainierten Modell.')
        return 0
    print('Ergebnis: ABWEICHUNG! Firmware-Koeffizienten in',
          'UhrMeldezaehler_core.h aktualisieren.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
