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
│   ├── UhrMeldezaehler_core.h Logik: Sensor, Erkennung, Kalibrierung, BLE, Tasten
│   ├── hal_display.h          Display (esp_lcd, DMA) + LVGL-Anbindung
│   ├── esp_lcd_sh8601.c/.h    Panel-Treiber CO5300 (aus dem Waveshare-BSP)
│   ├── ui_ctl.h               Bedien-Logik: Navigation, Timer, Stoppuhr, …
│   ├── ui.h, ui_screens.h,    Oberfläche (LVGL 9.3): Masken, Zifferblätter,
│   │   ui_watchfaces.h,       Gestaltung
│   │   ui_theme.h
│   └── src/fonts/             Schriften mit Umlauten (tools/fonts.sh)
├── lv_conf.h                  LVGL-Konfiguration (wird neben lvgl kopiert)
├── setup_libs.py              Arduino-Bibliotheken + lv_conf.h einrichten
├── tools/ui_sim/              Host-Simulator der Oberfläche (Screenshots, Abläufe)
├── MeldeApp/                  Flutter-Begleit-App (Android)
├── meldedaten/                aufgenommene Trainingsdaten (CSV)
├── trainieren.py              Training Meldebewegung (RandomForest/LogReg)
├── trainieren_positionen.py   Training 3-Zonen-Klassifikator (kopf/meldung/tisch)
├── trainieren_cnn.py          Training CNN (1D-ConvNet, 2 Klassen)
├── konvertiere_cnn_tflite.py  CNN → int8-TFLite (für die Uhr)
├── patch_tflite_lib.sh        Patches für die TensorFlowLite_ESP32-Bibliothek
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
  - Vibrationsmotor an GPIO18

## Firmware bauen & flashen

Voraussetzungen: `arduino-cli`, ESP32-Core (`esp32:esp32` 3.x), Bibliotheken
`lvgl` 9.3.0, `XPowersLib` und `TensorFlowLite_ESP32`. Einmalig einrichten:

```bash
python3 setup_libs.py      # installiert die Bibliotheken, kopiert lv_conf.h neben lvgl
./patch_tflite_lib.sh      # s. u.
```

> LVGL liest seine Konfiguration nur aus `lv_conf.h` **neben** dem
> `lvgl`-Bibliotheksordner. `build.sh` gleicht diese Datei vor jedem Build mit
> der versionierten `lv_conf.h` ab.

> **TensorFlowLite_ESP32 patchen:** Die Bibliothek braucht zwei kleine Patches,
> um mit dem ESP32-Core 3.x / GCC 14 zu kompilieren. Einmalig ausführen:
>
> ```bash
> ./patch_tflite_lib.sh
> ```

```bash
# Waveshare ESP32-S3-Touch-AMOLED-2.06 (16 MB Flash + 8 MB OPI-PSRAM):
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" UhrMeldezaehler
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi" -p /dev/ttyACM0 UhrMeldezaehler

# oder bequem per Skript:
./build.sh firmware    # kompiliert + flasht AUTOMATISCH (Port wird erkannt)
```

> `./build.sh firmware` sucht den Port selbst (/dev/ttyACM* oder /dev/ttyUSB*)
> und flasht direkt nach dem Kompilieren. Fester Port erzwingbar mit
> `PORT=/dev/ttyACM0 ./build.sh firmware`. Ohne angeschlossene Uhr wird nur
> kompiliert.

> **Wichtig:** Immer `FlashSize=16M`, `PartitionScheme=huge_app` (3 MB App)
> und `PSRAM=opi` verwenden – die Firmware (LVGL, Schriften, CNN) passt nicht
> in die Standard-Partition.

### Display-Anbindung und Rückfallebene

Das Display wird über `esp_lcd` (Quad-SPI mit DMA) angesteuert; LVGL rendert in
zwei Teilpuffer und überträgt nur geänderte Flächen, während `loop()`
weiterläuft. Bleibt der Bildschirm nach dem Flashen schwarz, in
`UhrMeldezaehler/hal_display.h` `#define USE_ESP_LCD 0` setzen: dann überträgt
LVGL über Arduino_GFX (langsamer, Bibliothek `GFX Library for Arduino` nötig).
Im seriellen Log zeigt `[perf] max loop=… max ui=…` alle 5 s die längste
Schleifendauer und Render-Zeit.

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
| `BATT` | Akku-Diagnose (Spannung, Ladezustand) |
| `BTN` | Zustand der Power-Taste (GPIO10) |
| `SLEEP` | sofort in den Deep-Sleep |
| `STREAM` / `STOPSTREAM` | Rohdaten-Stream für PC-Training (100 Hz, rotiert, g/dps) |
| `SENSOR` / `SENSOR ON` / `SENSOR OFF` | Erkennung anzeigen / ein- / ausschalten |
| `RECSD <klasse>` / `STOPSD` | Sensor-Aufnahme auf SD starten/stoppen (`meldung`/`nicht_meldung`) |
| `SDCHECK` | Größe + erste Zeilen von `/aufnahme.csv` anzeigen |

## Bedienung der Uhr

| Wo | Geste / Taste | Wirkung |
|----|---------------|---------|
| Zifferblatt | nach oben wischen | Apps |
| Zifferblatt | 2 s halten (vibriert) | Zifferblatt wählen (6 Stück) |
| überall | **Power** kurz | zurück (wie ‹); auf dem Zifferblatt: Standby |
| überall | **BOOT** 1× | Meldungen bearbeiten (hinzufügen, drangenommen, richtig/falsch, löschen) |
| überall | **BOOT** 2× | Tisch-Lage neu lernen (Uhr liegt flach) |
| Apps / Auswahl | nach unten wischen | Zifferblatt |
| Melden | seitlich wischen | Zähler · Statistik · Kalibrierung |
| Melden (Zähler) | lange drücken | Tageszählung zurücksetzen (mit Rückfrage) |

Apps: **Melden**, **Zeit** (Timer, Stoppuhr), **Einstellungen** (Helligkeit,
Bluetooth, Sensor, Vibration, Stumm im Unterricht), **Aufnahme**
(Trainingsdaten auf SD), **Test** (Motor, Sensor, Akku). Zurück aus einer App
führt zu den Apps, von dort zum Zifferblatt. Löschen fragt immer nach.

### Oberfläche ohne Uhr testen

```bash
tools/ui_sim/build.sh     # baut LVGL + Simulator, schreibt tools/ui_sim/out/*.png
```

Der Simulator übersetzt dieselben `ui*.h`-Dateien gegen eine Stub-Logik
(`tools/ui_sim/sim_model.h`), rendert jede Maske als PNG und spielt
Touch-Abläufe durch (`sim_main.cpp`); er endet mit „ALLES OK“ oder listet die
Abweichungen. Schriften neu erzeugen: `LVGL_DIR=<lvgl> tools/fonts.sh`.

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
2. **Live:** 100 Hz Sensor → 1,5-s-Fenster (150 Samples × 6 Kanäle) wird in das
   Trainings-Koordinatensystem rotiert, standardisiert und an ein eingebettetes
   **int8-quantisiertes 1D-CNN** (TensorFlow Lite Micro) übergeben
   (2 Klassen: `meldung`/`nicht_meldung`). Umschaltbar per `USE_CNN` in
   `UhrMeldezaehler_core.h` (0 = bisherige LogisticRegression).
3. **Zustandsmaschine:** Arm unten → oben (≥ 0,5 s halten) → unten zählt eine
   Meldung – aber nur, wenn die Position überwiegend `meldung` war (Kopfkratzen
   zählt nicht). Mindestabstand zwischen Meldungen: 2,5 s.

## Training (PC, historische ESP32-C3+MPU6050-Pipeline)

```bash
python3 trainieren.py            # Meldebewegung (RandomForest + LogReg)
python3 trainieren_positionen.py # 3-Zonen-Klassifikator → modell_positionen.joblib
python3 verify_model.py          # prüft: eingebettetes C-Modell == trainiertes Modell
python3 trainieren_cnn.py        # CNN (2 Klassen) → modell_cnn.keras
python3 konvertiere_cnn_tflite.py # CNN → modell_cnn_int8.tflite + modell_cnn_data.h
```

### Trainingsdaten mit der Uhr sammeln (so viel du willst)

Mit `uhr_daten_sammeln.py` kannst du direkt auf der Uhr beliebig viele
gelabelte Daten aufnehmen und danach das CNN auf dem PC neu trainieren.
Die Uhr streamt die Sensorwerte bereits **rotiert ins Trainings-
Koordinatensystem** und in **physikalischen Einheiten** (g / dps) – exakt der
Datenpfad, den auch die CNN-Inferenz verwendet.

```bash
python3 uhr_daten_sammeln.py
```

Bedienung am PC (ein Tastendruck, kein ENTER):

| Taste | Bedeutung |
|-------|-----------|
| `1` | **MELDUNG** – Arm strecken, Hand über den Kopf, oben halten |
| `2` | **NICHT MELDEN** – normal bewegen, herumlaufen, kleine Bewegungen |
| `q` | beenden |

> Das CNN hat bewusst nur **2 Klassen**: `meldung` und `nicht_meldung`.
> Für „nicht melden“ einfach ganz normal bewegen (herumlaufen, kleine
> Bewegungen) – so lernt das Modell, dass diese Dinge **keine** Meldung sind.
> Alte Labels (`kopf`/`tisch`/`boden`) werden beim Training automatisch zu
> `nicht_meldung` zusammengefasst.

**Live-Aufnahme:** Sobald du z. B. `1` drückst, wird ab sofort jedes
ankommende Sample als `meldung` gespeichert – so lange, bis du eine andere
Zahl drückst (dann wird auf die neue Klasse umgeschaltet) oder `q` drückst.
Jeder Tastendruck startet einen neuen Trial der gedrückten Klasse. Jede
Position mindestens ~1,5 s halten, sonst ist der Trial zu kurz fürs Training.
Die Daten landen in `meldedaten/uhr_positionen.csv`.

Danach CNN neu trainieren und in die Uhr einbetten:

```bash
python3 trainieren_cnn.py meldedaten/uhr_positionen.csv
python3 konvertiere_cnn_tflite.py meldedaten/uhr_positionen.csv  # auch CNN_MEAN/CNN_STD
./build.sh firmware
```

### Aufnahme direkt auf der Uhr (ganz ohne PC)

Die Uhr kann Trainingsdaten auch **allein** auf die SD-Karte aufnehmen:

1. Auf der Uhr: Apps → **Aufnahme**.
2. Klasse antippen: **Meldung** oder **Nicht melden**.
3. Es wird sofort auf die SD-Karte (`/aufnahme.csv`) geschrieben – so lange,
   bis du **Stoppen** (oder Zurück/Power) drückst.
4. SD-Karte in den PC stecken, `/aufnahme.csv` nach `meldedaten/` kopieren
   und trainieren:

```bash
python3 trainieren_cnn.py meldedaten/aufnahme.csv
```

Die Trial-Nummern der Uhr-Aufnahmen beginnen bei `1000`, damit sie nicht mit
den PC-Aufnahmen kollidieren. Alternativ per seriellem Befehl:
`RECSD meldung` … `STOPSD` (Kontrolle mit `SDCHECK`).

> **Wichtig:** Die Uhr muss vor dem Sammeln kalibriert sein (beim ersten
> Start einmal Arm UNTEN / Arm HOCH halten). Die Kalibrierung liefert die
> Rotation, ohne die die Daten nicht ins Trainings-Koordinatensystem passen.

Die Koeffizienten des finalen Modells sind als C-Arrays in
`UhrMeldezaehler_core.h` eingebettet (`SCALER_*`, `COEF_*`, `INTERCEPT_*`).
Nach einem Neutraining mit `verify_model.py` prüfen, ob die C-Arrays aktuell sind.

#### CNN auf der Uhr

Das CNN wird als int8-TFLite-Modell direkt in die Firmware eingebettet
(`UhrMeldezaehler/modell_cnn_data.h`, aus `modell_cnn_int8.tflite` erzeugt).
Die Firmware nutzt TensorFlow Lite Micro und legt das Modell zur Laufzeit in
einer 32-KB-Arena ab (`CNN_ARENA_SIZE` in `UhrMeldezaehler_core.h`).

> **Speicherort:** Das Modell (~19,6 KB) steckt standardmäßig als `const`-Array
> im **16-MB-Flash** (im App-Image).
>
> **Optional von der SD-Karte:** Die Uhr hat einen microSD-Slot. Liegt im
> Wurzelverzeichnis der SD-Karte eine Datei **`model.tflite`** (FAT32, int8-
> quantisiert), lädt die Uhr dieses Modell beim Start – sonst automatisch das
> eingebaute Flash-Modell. So kann man ein neues Modell ausprobieren, ohne die
> Firmware neu zu flashen. Dafür `modell_cnn_int8.tflite` in `model.tflite`
> umbenennen und auf die SD-Karte kopieren.

Modell + C-Header neu erzeugen:

```bash
python3 trainieren_cnn.py [datei.csv]
python3 konvertiere_cnn_tflite.py [datei.csv]   # erzeugt .tflite + modell_cnn_data.h + aktualisiert CNN_MEAN/CNN_STD
```

Ohne Argument wird die historische Datei `meldedaten/positionen.csv` (rohe
LSB-Werte) verwendet; mit Argument z. B. die mit der Uhr gesammelten Daten
`meldedaten/uhr_positionen.csv` (physikalische Einheiten). Die Skripte erkennen
die Einheiten automatisch.

Das CNN ist per `#define USE_CNN 1` (in `UhrMeldezaehler_core.h`) aktiv;
mit `USE_CNN 0` fällt die Uhr auf die bisherige LogisticRegression zurück.

## Werks-Firmware wiederherstellen

```bash
esptool.py --port /dev/ttyACM0 write_flash 0x0 \
  firmware_backup/ESP32-S3-Touch-AMOLED-2.06-xiaozhi-251104.bin
```
