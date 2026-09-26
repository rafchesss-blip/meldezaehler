# MeldeApp

Flutter-Begleit-App für den **Meldezähler** (Waveshare ESP32-S3-Touch-AMOLED-2.06).

## Funktionen

- Uhr per Bluetooth (BLE) finden und verbinden
- Zeit & Datum synchronisieren
- Live-Statistik anzeigen (alle 5 s automatisch aktualisiert):
  - Meldungen heute, Session, seit Kalibrierung
  - Drangenommen, Richtig/Falsch, Meldzeit
  - Akku-Stand
  - Verlauf pro Minute (Balkendiagramm)
  - Stunden-Statistik (Meldungen pro Unterrichtsstunde)
- Aktuell laufende Unterrichtsstunde anzeigen
- Stundenplan verwalten (7 Tage) und an die Uhr übertragen
- Statistik auf der Uhr löschen

## Build

```bash
flutter pub get
flutter build apk --release                # Universal-APK
flutter build apk --release --split-per-abi # eine APK pro ABI
```

## Tests

```bash
flutter test
dart analyze
```

## BLE-Protokoll

Siehe [../README.md](../README.md) im Projektstamm.
