#!/usr/bin/env python3
"""
Training der Meldebewegungs-Erkennung (am PC)

Liest meldedaten/daten.csv, extrahiert Merkmale pro Bewegung,
trainiert Klassifikatoren und bewertet sie per Kreuzvalidierung.

Ausgabe:
  - Merkmals-Statistik pro Klasse
  - Genauigkeit (RandomForest + LogisticRegression)
  - Wichtigste Merkmale
"""

import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.model_selection import cross_val_score, StratifiedKFold
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import make_pipeline
import warnings
warnings.filterwarnings('ignore')

DATEI = 'meldedaten/daten.csv'
FS = 100.0  # Abtastrate in Hz


def features_aus_trial(df_trial):
    """Berechnet Merkmale aus einem 300x6-Fenster."""
    a = df_trial[['ax', 'ay', 'az']].values.astype(float)
    g = df_trial[['gx', 'gy', 'gz']].values.astype(float)

    # Physikalische Umrechnung
    a = a / 16384.0        # -> g
    g = g / 32.8           # -> dps (bei +/-1000 dps)

    f = {}

    # --- Kanalweise Merkmale ---
    for i, name in enumerate(['ax', 'ay', 'az']):
        f[f'{name}_std'] = a[:, i].std()
        f[f'{name}_range'] = a[:, i].max() - a[:, i].min()
    for i, name in enumerate(['gx', 'gy', 'gz']):
        f[f'{name}_std'] = g[:, i].std()
        f[f'{name}_range'] = g[:, i].max() - g[:, i].min()

    # --- Betraege ---
    acc_mag = np.sqrt((a ** 2).sum(axis=1))          # sollte ~1g in Ruhe
    gyr_mag = np.sqrt((g ** 2).sum(axis=1))          # Drehraten-Betrag

    f['accmag_std'] = acc_mag.std()
    f['accmag_range'] = acc_mag.max() - acc_mag.min()
    f['accmag_dev'] = np.abs(acc_mag - acc_mag.mean()).mean()  # Abw. vom Mittel
    f['gyrmag_mean'] = gyr_mag.mean()
    f['gyrmag_std'] = gyr_mag.std()
    f['gyrmag_max'] = gyr_mag.max()

    # --- Rotations-Energie (Drehung = wichtig fuer Armheben) ---
    f['rot_energie'] = (g ** 2).sum()
    f['acc_energie'] = ((a - a.mean(axis=0)) ** 2).sum()

    # --- Integrierte Drehung (Gesamtwinkel) ---
    dt = 1.0 / FS
    for i, name in enumerate(['gx', 'gy', 'gz']):
        f[f'winkel_{name}'] = np.abs(g[:, i].sum()) * dt

    return f


def main():
    df = pd.read_csv(DATEI)
    print(f"Daten: {len(df)} Zeilen, {df['trial'].nunique()} Trials\n")

    # Merkmale pro Trial berechnen
    zeilen = []
    for (label, trial), gruppe in df.groupby(['label', 'trial']):
        f = features_aus_trial(gruppe)
        f['label'] = label
        f['trial'] = trial
        zeilen.append(f)

    F = pd.DataFrame(zeilen)
    feature_spalten = [c for c in F.columns if c not in ('label', 'trial')]
    X = F[feature_spalten].values
    y = (F['label'] == 'melden').astype(int).values

    print(f"Merkmale: {len(feature_spalten)}, Samples: {len(F)}")
    print(f"  melden: {y.sum()}, nicht_melden: {(1-y).sum()}\n")

    # --- Klassen-Statistik (Mittelwerte) ---
    print("=== Merkmals-Mittelwerte pro Klasse ===")
    stat = F.groupby('label')[feature_spalten].mean().T
    stat['diff'] = stat['melden'] - stat['nicht_melden']
    stat = stat.reindex(stat['diff'].abs().sort_values(ascending=False).index)
    print(stat.head(12).round(3).to_string())
    print()

    cv = StratifiedKFold(n_splits=5, shuffle=True, random_state=42)

    # --- RandomForest ---
    rf = RandomForestClassifier(n_estimators=200, random_state=42)
    scores_rf = cross_val_score(rf, X, y, cv=cv, scoring='accuracy')
    print(f"RandomForest:   Genauigkeit {scores_rf.mean():.3f}  (+/- {scores_rf.std():.3f})")

    # --- LogisticRegression (einfach, gut auf MCU uebertragbar) ---
    lr = make_pipeline(StandardScaler(),
                       LogisticRegression(max_iter=1000, C=1.0))
    scores_lr = cross_val_score(lr, X, y, cv=cv, scoring='accuracy')
    print(f"LogRegression:  Genauigkeit {scores_lr.mean():.3f}  (+/- {scores_lr.std():.3f})")

    # --- Wichtigste Merkmale (RandomForest, auf allen Daten) ---
    rf.fit(X, y)
    imp = pd.Series(rf.feature_importances_, index=feature_spalten)
    print("\n=== Wichtigste Merkmale (RandomForest) ===")
    print(imp.sort_values(ascending=False).head(10).round(3).to_string())

    # --- Modell speichern (fuer spaetere MCU-Uebertragung) ---
    import joblib
    rf.fit(X, y)
    joblib.dump({'modell': rf, 'merkmale': feature_spalten},
                'modell_randomforest.joblib')
    print("\nModell gespeichert: modell_randomforest.joblib")


if __name__ == '__main__':
    main()
