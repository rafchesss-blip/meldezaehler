# Meldezähler

Ein Schulsystem, das Handheben („Meldungen") automatisch zählt – läuft komplett
auf einer **Waveshare ESP32-S3-Touch-AMOLED-2.06** Uhr. Eine Flutter-App
verbindet sich per Bluetooth (BLE), synchronisiert Zeit/Datum und zeigt
Statistiken sowie den Stundenplan an.

## Übersicht

```
Meldezähler/
├── UhrMeldezaehler/           Firmware für die Uhr (Arduino, ESP32-S3)
│   ├── UhrMeldezaehler.ino    setup() + loop()
│   ├── UhrMeldezaehler_core.h gesamte App-Logik (UI, Sensor, BLE, Audio)
│   └── audio_codec.h          ES8311/ES7210 Audio-Treiber (Rekorder)
├── MeldeApp/                  Flutter-Begleit-App (Android)
├── meldedaten/                aufgenommene Trainingsdaten (CSV)
├── trainieren.py              Training Meldebewegung (RandomForest/LogReg)
├── trainieren_positionen.py   Training 3-Zonen-Klassifikator (kopf/meldung/tisch)
├── anlernen*.py               Datenerfassung (ältere ESP32-C3 + MPU6050-Hardware)
├── live_*.py                  Live-Erkennung/Animation am PC (ältere Hardware)
├── VibrationTest/             Test-Sketch für den Vibrationsmotor
└── firmware_backup/           Werks-Firmware der Uhr (Backup)
```

## Hardware

- **Waveshare ESP32-S3-Touch-AMOLED-2.06** (ESP32-S3, 16 MB Flash, 8 MB PSRAM)
  - QMI8658 (Accel + Gyro) → Handhebe-Erkennung
  - CO5300 (AMOLED 410×502) → UI
  - FT3168 (Touch) → Bedienung
  - AXP2101 (PMU) → Akku
  - PCF85063 (RTC) → Uhrzeit
  - ES8311 + ES7210 → Lautsprecher/Mikrofon (Rekorder)
  - Vibrationsmotor an GPIO18

## Firmware bauen & flashen

Voraussetzungen: `arduino-cli`, ESP32-Core (`esp32:esp32`), Bibliotheken
`Arduino_GFX_Library` und `XPowersLib`.

```bash
# Waveshare ESP32-S3-Touch-AMOLED-2.06 (16 MB Flash + 8 MB OPI-PSRAM):
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" UhrMeldezaehler
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" -p /dev/ttyACM0 UhrMeldezaehler

# oder bequem per Skript:
./build.sh firmware
```

> **Wichtig:**
> - Mit der Standard-4-MB-Partition wäre die Firmware zu **99 % voll** und
>   der Rekorder hätte keinen PSRAM-Puffer. Deshalb immer `FlashSize=16M`,
>   `PartitionScheme=huge_app` (3 MB APP) und `PSRAM=opi` verwenden.
> - Ohne PSRAM deaktiviert sich der Rekorder sauber selbst, der Rest läuft trotzdem.

### Serielle Befehle (USB, 115200 Baud)

| Befehl | Wirkung |
|--------|---------|
| `RESET` | Tageszähler auf 0 |
| `CAL` | Kalibrierung neu starten |
| `CALCLEAR` | Kalibrierung löschen + Neustart |
| `CALIB` | Kalibrierwerte ausgeben |
| `STATS` | Statistik ausgeben |
| `TIME HH:MM:SS` | Uhrzeit setzen |
| `DATE DD.MM.YY` | Datum setzen |
| `TT` | Stundenplan ausgeben |
| `RESTORE …` | Kalibrierung wiederherstellen |
| `VIB` | Vibrationstest |
| `TON` / `REC` / `STOP` / `PEAK` | Audio-Tests |

## Flutter-App bauen

```bash
cd MeldeApp
flutter pub get
flutter build apk --release                # Universal-APK
flutter build apk --release --split-per-abi # eine APK pro ABI
```

Die APKs landen unter `MeldeApp/build/app/outputs/flutter-apk/`.

### Funktionen der App

- Uhr per BLE finden (`Meldezaehler` / Service-UUID `4fafc201-…`)
- Zeit & Datum synchronisieren
- Statistik anzeigen (heute, Session, seit Kalibrierung, Akku, Minuten-Verlauf)
- Stunden-Statistik (Meldungen pro Unterrichtsstunde)
- Stundenplan verwalten und an die Uhr übertragen
- Statistik auf der Uhr löschen

## BLE-Protokoll

Service-UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`

| Charakteristik | UUID | Typ | Zweck |
|----------------|------|-----|-------|
| Zeit | `beb5483e-…-b26a8` | Write | 6 Bytes: Jahr-2000, Monat, Tag, Std, Min, Sek |
| Statistik | `beb5483e-…-b26a9` | Read | JSON: Skalare + `min[60]` |
| Löschen | `beb5483e-…-b26aa` | Write | `"1"` setzt die Tages-Statistik zurück |
| Stundenplan | `beb5483e-…-b26ab` | Write | Befehle, je einer pro Write |
| Stunden-Stat. | `beb5483e-…-b26ac` | Read | JSON: `lessonCounts` |

Stundenplan-Befehle (jeweils als eigener Write):

```
CLEAR
P|<Tag 0-6>|<Idx>|<Name>|<sh>|<sm>|<eh>|<em>
SAVE
```

> Die Statistik ist bewusst auf **zwei** Charakteristiken aufgeteilt, weil eine
> einzelne BLE-Charakteristik auf 517 Bytes begrenzt ist (`ESP_GATT_MAX_ATTR_LEN`)
> und die JSON bei vollem Stundenplan sonst abgeschnitten würde.

## Erkennung (wie es funktioniert)

1. **Kalibrierung beim ersten Start:** Arm UNTEN und Arm HOCH halten (je 10×),
   optional TISCH. Daraus werden die personenabhängigen Schwerkraft-Richtungen
   gelernt und eine Rotation in das Trainings-Koordinatensystem berechnet.
2. **Live:** 100 Hz Sensor → Merkmale (Schwerkraft-Richtung, Beschleunigungs-
   Streuung, Drehraten) → eingebettetes LogisticRegression-Modell (3 Klassen:
   `kopf`/`meldung`/`tisch`).
3. **Zustandsmaschine:** Arm unten → oben (≥ 0,5 s halten) → unten zählt eine
   Meldung – aber nur, wenn die Position überwiegend `meldung` war (Kopfkratzen
   zählt nicht). Mindestabstand zwischen Meldungen: 2,5 s.

## Training (PC, historische ESP32-C3+MPU6050-Pipeline)

```bash
python3 trainieren.py            # Meldebewegung (RandomForest + LogReg)
python3 trainieren_positionen.py # 3-Zonen-Klassifikator → modell_positionen.joblib
```

Die Koeffizienten des finalen Modells sind als C-Arrays in
`UhrMeldezaehler_core.h` eingebettet (`SCALER_*`, `COEF_*`, `INTERCEPT_*`).

## Werks-Firmware wiederherstellen

```bash
esptool.py --port /dev/ttyACM0 write_flash 0x0 \
  firmware_backup/ESP32-S3-Touch-AMOLED-2.06-xiaozhi-251104.bin
```
