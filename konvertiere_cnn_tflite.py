#!/usr/bin/env python3
"""
Konvertiert das trainierte CNN (modell_cnn.keras) nach TensorFlow Lite.

Ausgabe:
  - modell_cnn_int8.tflite   Full-Integer-Quantisierung (int8 in/out)
  - modell_cnn_float32.tflite (Referenz/Debug)

Die Quantisierung nutzt ein repräsentatives Datenset aus den
Trainingsfenstern (standardisiert, wie die Firmware es liefern wird).
"""

import os
import sys
import re
import warnings
import numpy as np
import pandas as pd
import joblib

os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'
import tensorflow as tf

warnings.filterwarnings('ignore')

DATEI = 'meldedaten/positionen.csv'
META = 'modell_cnn_meta.joblib'
FENSTER = 150
KLASSEN = ['meldung', 'nicht_meldung']
LABEL_MAP = {'kopf': 'nicht_meldung', 'tisch': 'nicht_meldung', 'boden': 'nicht_meldung'}


def lade_df(datei):
    """Liest CSV, mappt alte Labels und vergibt eindeutige Trial-IDs."""
    df = pd.read_csv(datei)
    seen = {}
    neue_trials = []
    for lbl, tr in zip(df['label'].astype(str), df['trial'].astype(int)):
        key = (lbl, tr)
        if key not in seen:
            seen[key] = len(seen) + 1
        neue_trials.append(seen[key])
    df['trial'] = neue_trials
    df['label'] = df['label'].map(lambda l: LABEL_MAP.get(l, l))
    return df


def lade_fenster_standardisiert(datei=DATEI):
    meta = joblib.load(META)
    mean = meta['scaler_mean'].reshape(1, 1, -1).astype(np.float32)
    std = meta['scaler_std'].reshape(1, 1, -1).astype(np.float32)

    df = lade_df(datei)
    a = df[['ax', 'ay', 'az']].values.astype(np.float32)
    g = df[['gx', 'gy', 'gz']].values.astype(np.float32)
    if np.abs(a).max() > 50.0:      # rohe LSB (±16k) -> g / dps
        a = a / 16384.0
        g = g / 32.8
    df = df.copy()
    df[['ax', 'ay', 'az']] = a
    df[['gx', 'gy', 'gz']] = g

    X = []
    for (label, trial), gruppe in df.groupby(['label', 'trial']):
        seq = gruppe[['ax', 'ay', 'az', 'gx', 'gy', 'gz']].values.astype(np.float32)
        for i in range(0, len(seq) - FENSTER + 1, 25):
            w = seq[i:i + FENSTER]
            X.append((w - mean[0]) / std[0])
    return np.array(X, dtype=np.float32)


def repr_dataset(X):
    for i in range(0, len(X), 1):
        yield [X[i:i + 1]]


def main():
    datei = sys.argv[1] if len(sys.argv) > 1 else DATEI
    print(f'Daten: {datei}')
    model = tf.keras.models.load_model('modell_cnn.keras')
    X = lade_fenster_standardisiert(datei)
    print(f'Repräsentative Fenster: {X.shape}')

    # --- Full-Integer-Quantisierung (int8) ---
    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = lambda: repr_dataset(X)
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tflite_int8 = conv.convert()
    with open('modell_cnn_int8.tflite', 'wb') as f:
        f.write(tflite_int8)
    print(f'int8-Modell: {len(tflite_int8)} Bytes')

    # --- float32 (Referenz) ---
    conv2 = tf.lite.TFLiteConverter.from_keras_model(model)
    tflite_f32 = conv2.convert()
    with open('modell_cnn_float32.tflite', 'wb') as f:
        f.write(tflite_f32)
    print(f'float32-Modell: {len(tflite_f32)} Bytes')

    # --- Sanity-Check: Genauigkeit des int8-Modells auf allen Fenstern ---
    interp = tf.lite.Interpreter(model_content=tflite_int8)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    print(f'\nEingabe: shape={inp["shape"]} dtype={inp["dtype"]} '
          f'scale={inp["quantization"][0]:.6f} zero={inp["quantization"][1]}')
    print(f'Ausgabe: shape={out["shape"]} dtype={out["dtype"]} '
          f'scale={out["quantization"][0]:.6f} zero={out["quantization"][1]}')

    labels = []
    df = lade_df(datei)
    for (label, trial), gruppe in df.groupby(['label', 'trial']):
        for _ in range(0, len(gruppe) - FENSTER + 1, 25):
            labels.append(label)
    labels = np.array(labels)

    y_true = np.array([KLASSEN.index(l) for l in labels])
    in_scale, in_zero = inp['quantization']
    y_pred = []
    for i in range(len(X)):
        q = np.clip(np.round(X[i:i + 1] / in_scale + in_zero), -128, 127).astype(np.int8)
        interp.set_tensor(inp['index'], q)
        interp.invoke()
        y_pred.append(interp.get_tensor(out['index']).argmax())
    y_pred = np.array(y_pred)
    acc = (y_pred == y_true).mean()
    print(f'\nint8-TFLite Genauigkeit (alle Fenster): {acc:.3f}')

    # --- C-Header für die Firmware erzeugen (modell_cnn_data.h) ---
    schreibe_c_header(tflite_int8)

    # --- Standardisierung (CNN_MEAN/CNN_STD) in der Firmware aktualisieren ---
    update_core_scaler()


def schreibe_c_header(data: bytes):
    zeilen = []
    zeilen.append('#pragma once')
    zeilen.append('')
    zeilen.append('// Int8-quantisiertes CNN (meldung/nicht_meldung), aus modell_cnn_int8.tflite erzeugt.')
    zeilen.append('alignas(8) const unsigned char modell_cnn_int8_tflite[] = {')
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        zeilen.append('  ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    zeilen.append('};')
    zeilen.append(f'const unsigned int modell_cnn_int8_tflite_len = {len(data)};')
    zeilen.append('')
    with open('UhrMeldezaehler/modell_cnn_data.h', 'w', encoding='utf-8') as f:
        f.write('\n'.join(zeilen))
    print(f'C-Header geschrieben: UhrMeldezaehler/modell_cnn_data.h ({len(data)} Bytes)')


def update_core_scaler():
    """Schreibt CNN_MEAN/CNN_STD aus modell_cnn_meta.joblib in die Firmware.

    Wichtig: Die Uhr standardisiert die Fenster vor der CNN-Inferenz mit genau
    diesen Konstanten. Nach einem Neutraining müssen sie daher zusammen mit dem
    Modell aktualisiert werden.
    """
    meta = joblib.load(META)
    mean = meta['scaler_mean']
    std = meta['scaler_std']

    def fmt6(vals):
        return ', '.join(f'{float(v):.9g}f' for v in vals)

    pfad = 'UhrMeldezaehler/UhrMeldezaehler_core.h'
    with open(pfad, 'r', encoding='utf-8', newline='') as f:   # Zeilenenden unverändert lassen
        text = f.read()

    text = re.sub(
        r'static const float CNN_MEAN\[6\] = \{[^}]*\};',
        'static const float CNN_MEAN[6] = {' + fmt6(mean) + '};',
        text)
    text = re.sub(
        r'static const float CNN_STD\[6\]  = \{[^}]*\};',
        'static const float CNN_STD[6]  = {' + fmt6(std) + '};',
        text)

    with open(pfad, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    print(f'CNN_MEAN/CNN_STD in {pfad} aktualisiert.')


if __name__ == '__main__':
    main()
