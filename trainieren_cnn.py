#!/usr/bin/env python3
"""
CNN-Training für die Melde-Erkennung (3-Zonen-Klassifikator: kopf/meldung/tisch)

Ersetzt die bisherige LogisticRegression durch ein 1D-CNN auf
1,5-s-Fenstern (150 Samples @ 100 Hz, 6 Kanäle: ax, ay, az, gx, gy, gz).

Fairer Vergleich: Beide Modelle werden mit demselben GroupKFold-Split
(nach Trial, kein Leakage zwischen Fenstern desselben Trials) bewertet.

Ausgabe:
  - Genauigkeit + Konfusionsmatrix je Modell und Fold
  - modell_cnn.keras          (finales, auf allen Daten trainiertes CNN)
  - modell_cnn_meta.joblib    (Klassen, Fenster-Parameter, Referenz-Richtungen)
"""

import os
import sys
import warnings
import numpy as np
import pandas as pd
import joblib

os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers
from sklearn.linear_model import LogisticRegression
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import confusion_matrix

warnings.filterwarnings('ignore')

DATEI = 'meldedaten/positionen.csv'
FENSTER = 150   # 1,5 s
SCHRITT = 25    # 0,25 s -> 7 Fenster/Trial
KLASSEN = ['meldung', 'nicht_meldung']
# Alte Labels (kopf/tisch/boden) werden automatisch zu "nicht_meldung".
LABEL_MAP = {'kopf': 'nicht_meldung', 'tisch': 'nicht_meldung', 'boden': 'nicht_meldung'}
SEED = 42
EPOCHEN = 80
BATCH = 32
FOLDS = 5

np.random.seed(SEED)
tf.random.set_seed(SEED)


def lade_df(datei):
    """Liest die CSV, mappt alte Labels auf die 2 Klassen und vergibt
    eindeutige Trial-IDs (damit z. B. kopf#1 + tisch#1 nach dem Mapping
    nicht fälschlich zu einem einzigen nicht_meldung-Trial verschmelzen)."""
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


def lade_fenster(datei=DATEI):
    """Lädt die CSV und schneidet (FENSTER,6)-Sequenzen aus.

    Erkennt automatisch, ob die Werte als rohe LSB (alte MPU6050-Daten)
    oder bereits physikalisch (g, dps – von der Uhr gesammelt) vorliegen.

    Rückgabe: X (n, FENSTER, 6) in physikalischen Einheiten (g, dps),
              y (n,) als Label-String, trials (n,) als eindeutige Trial-ID.
    """
    df = lade_df(datei)
    a = df[['ax', 'ay', 'az']].values.astype(float)
    g = df[['gx', 'gy', 'gz']].values.astype(float)
    if np.abs(a).max() > 50.0:      # rohe LSB (±16k) -> g / dps
        a = a / 16384.0
        g = g / 32.8
    df = df.copy()
    df[['ax', 'ay', 'az']] = a
    df[['gx', 'gy', 'gz']] = g

    X, y, trials = [], [], []
    for (label, trial), gruppe in df.groupby(['label', 'trial']):
        seq = gruppe[['ax', 'ay', 'az', 'gx', 'gy', 'gz']].values.astype(float)
        tid = f'{label}:{trial}'
        for i in range(0, len(seq) - FENSTER + 1, SCHRITT):
            X.append(seq[i:i + FENSTER])
            y.append(label)
            trials.append(tid)
    return np.array(X, dtype=np.float32), np.array(y), np.array(trials)


def gruppen_folds(y, trials, n_splits=FOLDS):
    """Stratifizierter Split nach Trial (Label + Nummer als eindeutige Gruppe)."""
    unique = np.unique(trials)
    # Label je Trial ermitteln (alle Fenster eines Trials tragen dasselbe Label)
    trial_label = {t: y[trials == t][0] for t in unique}

    folds = [[] for _ in range(n_splits)]
    for k in KLASSEN:
        ts = np.array([t for t in unique if trial_label[t] == k])
        np.random.shuffle(ts)
        for i, t in enumerate(ts):
            folds[i % n_splits].append(t)

    out = []
    for i in range(n_splits):
        test_trials = set(folds[i])
        test_idx = np.where(np.isin(trials, list(test_trials)))[0]
        train_idx = np.where(~np.isin(trials, list(test_trials)))[0]
        out.append((train_idx, test_idx))
    return out


def standardisiere(train, test):
    """Per-Kanal z-Score, Mittel/Std nur aus dem Train-Fold (kein Leakage)."""
    mean = train.mean(axis=(0, 1), keepdims=True)
    std = train.std(axis=(0, 1), keepdims=True) + 1e-6
    return (train - mean) / std, (test - mean) / std


def baue_cnn():
    model = keras.Sequential([
        layers.Input(shape=(FENSTER, 6)),
        layers.Conv1D(16, 5, activation='relu', padding='same'),
        layers.MaxPooling1D(2),
        layers.Conv1D(32, 5, activation='relu', padding='same'),
        layers.MaxPooling1D(2),
        layers.Conv1D(64, 3, activation='relu', padding='same'),
        layers.GlobalAveragePooling1D(),
        layers.Dropout(0.3),
        layers.Dense(len(KLASSEN), activation='softmax'),
    ])
    model.compile(
        optimizer=keras.optimizers.Adam(1e-3),
        loss='sparse_categorical_crossentropy',
        metrics=['accuracy'],
    )
    return model


def merkmale_lr(seq):
    """Die 6 Handmerkmale der bisherigen LogisticRegression aus einem Fenster."""
    a = seq[:, :3]
    g = seq[:, 3:]
    m = a.mean(axis=0)
    n = np.linalg.norm(m)
    m = m / n if n > 0 else m
    acc_std = float(a.std(axis=0).sum())
    gyrmag = np.sqrt((g ** 2).sum(axis=1))
    return [m[0], m[1], m[2], acc_std, float(gyrmag.mean()), float(gyrmag.max())]


def main():
    datei = sys.argv[1] if len(sys.argv) > 1 else DATEI
    print(f'Daten: {datei}')
    X, y, trials = lade_fenster(datei)
    y_idx = np.array([KLASSEN.index(k) for k in y])
    print(f'Fenster: {X.shape}, Trials: {len(np.unique(trials))}')
    print(f'Klassen: {dict(zip(*np.unique(y, return_counts=True)))}\n')

    folds = gruppen_folds(y, trials)

    # Hinweis bei zu wenigen Trials pro Klasse (GroupKFold braucht mehrere)
    for k in KLASSEN:
        nt = len(set(trials[y == k]))
        if nt < 2:
            print(f'Hinweis: Klasse "{k}" hat nur {nt} Trial(s) – '
                  f'für ein aussagekräftiges CV besser 2-3 Trials sammeln.')
    print()

    cnn_acc, lr_acc = [], []
    n_k = len(KLASSEN)
    cm_sum_cnn = np.zeros((n_k, n_k), dtype=int)
    cm_sum_lr = np.zeros((n_k, n_k), dtype=int)

    for fi, (tr, te) in enumerate(folds, 1):
        # --- CNN ---
        Xtr, Xte = standardisiere(X[tr], X[te])
        tf.random.set_seed(SEED + fi)
        model = baue_cnn()
        model.fit(Xtr, y_idx[tr], validation_data=(Xte, y_idx[te]),
                  epochs=EPOCHEN, batch_size=BATCH, verbose=0,
                  callbacks=[keras.callbacks.EarlyStopping(
                      monitor='val_accuracy', patience=15, restore_best_weights=True)])
        _, acc = model.evaluate(Xte, y_idx[te], verbose=0)
        p_cnn = model.predict(Xte, verbose=0).argmax(axis=1)
        cnn_acc.append(acc)
        cm_sum_cnn += confusion_matrix(y_idx[te], p_cnn, labels=list(range(n_k)))

        # --- LogisticRegression (Baseline, gleiche Fenster) ---
        if len(set(y_idx[tr])) >= 2:
            Ftr = np.array([merkmale_lr(s) for s in X[tr]])
            Fte = np.array([merkmale_lr(s) for s in X[te]])
            lr = make_pipeline(StandardScaler(), LogisticRegression(max_iter=1000))
            lr.fit(Ftr, y_idx[tr])
            p_lr = lr.predict(Fte)
            lr_acc.append((p_lr == y_idx[te]).mean())
            cm_sum_lr += confusion_matrix(y_idx[te], p_lr, labels=list(range(n_k)))
        else:
            lr_acc.append(np.nan)
            print(f'  Fold {fi}: LogReg übersprungen (nur 1 Klasse im Train-Fold)')

        print(f'Fold {fi}: CNN={acc:.3f}   LogReg={lr_acc[-1]:.3f}')

    print(f'\n=== Ergebnis ({FOLDS}-fold GroupKFold) ===')
    print(f'CNN:            {np.mean(cnn_acc):.3f}  (+/- {np.std(cnn_acc):.3f})')
    print(f'LogRegression:  {np.nanmean(lr_acc):.3f}  (+/- {np.nanstd(lr_acc):.3f})')

    print('\nKonfusionsmatrix CNN (Zeilen=wahr, Spalten=vorhergesagt):')
    print(pd.DataFrame(cm_sum_cnn, index=KLASSEN, columns=KLASSEN).to_string())

    # --- Finales Modell auf allen Daten trainieren und speichern ---
    print('\nTrainiere finales CNN auf allen Daten ...')
    Xall = X.copy()
    mean = Xall.mean(axis=(0, 1), keepdims=True)
    std = Xall.std(axis=(0, 1), keepdims=True) + 1e-6
    Xall = (Xall - mean) / std
    final = baue_cnn()
    final.fit(Xall, y_idx, epochs=EPOCHEN, batch_size=BATCH, verbose=0)

    final.save('modell_cnn.keras')
    refs = {}
    df = lade_df(datei)
    a_all = df[['ax', 'ay', 'az']].values.astype(float)
    if np.abs(a_all).max() > 50.0:
        a_all = a_all / 16384.0
    df = df.copy()
    df[['ax', 'ay', 'az']] = a_all
    for label in KLASSEN:
        a = df[df['label'] == label][['ax', 'ay', 'az']].values.astype(float)
        m = a.mean(axis=0)
        refs[label] = (m / np.linalg.norm(m)).astype(float)
    joblib.dump({'klassen': KLASSEN, 'fenster': FENSTER, 'schritt': SCHRITT,
                 'scaler_mean': mean.reshape(-1), 'scaler_std': std.reshape(-1),
                 'refs': refs},
                'modell_cnn_meta.joblib')
    print('Gespeichert: modell_cnn.keras, modell_cnn_meta.joblib')


if __name__ == '__main__':
    main()
