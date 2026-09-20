#!/usr/bin/env python3
"""
Training des 3-Zonen-Klassifikators (meldung / kopf / tisch)

Liest meldedaten/positionen.csv, extrahiert Merkmale aus
1,5-Sekunden-Fenstern und trainiert eine logistische Regression.

Merkmale (6):
  - normierte Schwerkraft-Richtung (3)
  - Beschleunigungs-Streuung gesamt (1)
  - Drehraten-Betrag Mittel und Maximum (2)

Speichert das Modell als modell_positionen.joblib
(mit Referenz-Richtungen fuer die Animation).
"""

import numpy as np
import pandas as pd
from sklearn.linear_model import LogisticRegression
from sklearn.model_selection import StratifiedKFold, cross_val_predict
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import make_pipeline
from sklearn.metrics import confusion_matrix
import joblib
import warnings
warnings.filterwarnings('ignore')

DATEI = 'meldedaten/positionen.csv'
FENSTER = 150   # 1,5 s
SCHRITT = 50    # 0,5 s


def merkmale(a, g):
    """a, g: (FENSTER,3) in g bzw. dps."""
    m = a.mean(axis=0)
    m = m / np.linalg.norm(m)
    acc_std = float(a.std(axis=0).sum())
    gyrmag = np.sqrt((g ** 2).sum(axis=1))
    return [m[0], m[1], m[2], acc_std, float(gyrmag.mean()), float(gyrmag.max())]


def main():
    df = pd.read_csv(DATEI)

    X, y = [], []
    for (label, trial), gruppe in df.groupby(['label', 'trial']):
        a = gruppe[['ax', 'ay', 'az']].values.astype(float) / 16384.0
        g = gruppe[['gx', 'gy', 'gz']].values.astype(float) / 32.8
        for i in range(0, len(a) - FENSTER + 1, SCHRITT):
            X.append(merkmale(a[i:i + FENSTER], g[i:i + FENSTER]))
            y.append(label)

    X = np.array(X)
    y = np.array(y)
    klassen = ['kopf', 'meldung', 'tisch']
    print(f"Fenster: {X.shape}, Klassen: {dict(zip(*np.unique(y, return_counts=True)))}")

    # Referenz-Richtungen (Mittelwert) fuer die Animation
    refs = {}
    for label in klassen:
        gg = df[df['label'] == label]
        a = gg[['ax', 'ay', 'az']].values.astype(float)
        m = a.mean(axis=0)
        refs[label] = m / np.linalg.norm(m)

    # Kreuzvalidierung
    cv = StratifiedKFold(n_splits=5, shuffle=True, random_state=42)
    model = make_pipeline(StandardScaler(), LogisticRegression(max_iter=1000))
    yp = cross_val_predict(model, X, y, cv=cv)
    acc = (yp == y).mean()
    print(f"\nGenauigkeit (5-fold CV): {acc:.3f}")

    cm = confusion_matrix(y, yp, labels=klassen)
    print("\nKonfusionsmatrix (Zeilen=wahr, Spalten=vorhergesagt):")
    print(pd.DataFrame(cm, index=klassen, columns=klassen).to_string())

    # Finales Modell auf allen Daten trainieren und speichern
    model.fit(X, y)
    joblib.dump({'modell': model, 'klassen': klassen, 'refs': refs,
                 'fenster': FENSTER, 'schritt': SCHRITT},
                'modell_positionen.joblib')
    print("\nModell gespeichert: modell_positionen.joblib")


if __name__ == '__main__':
    main()
