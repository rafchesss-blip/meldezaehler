# Logbuch – Meldezähler

> Chronologisches Projekt-Logbuch. **Neueste Einträge stehen unten.**
> Wird laufend fortgeschrieben.

---

## 20.09.2026 – Projektstart (ältere Hardware: ESP32-C3 + MPU6050)

Beginn des Meldezählers: Ein Schulsystem, das Handheben („Meldungen")
automatisch zählt. Erste Version lief mit einem ESP32-C3 + MPU6050
(Accel/Gyro) am Band, die Erkennung lief am PC.

- **Datenerfassung:** `anlernen.py`, `anlernen2.py`, `zustaende_sammeln.py`
  → Aufnahme gelabelter Sensordaten als CSV (`meldedaten/`)
- **Training Meldebewegung:** `trainieren.py`
  → RandomForest + LogisticRegression, Merkmale pro 3-s-Fenster
- **Live-Test am PC:** `live_test.py`, `live_zustand.py`
- **Personen-Kalibrierung:** Arm UNTEN / Arm HOCH beim Start lernen,
  damit jede Person das Band tragen kann
- **3-Zonen-Klassifikator:** `trainieren_positionen.py`
  → kopf / meldung / tisch (LogisticRegression, 6 Merkmale)
- **Animation:** `live_animation.py`
  → Schüler-Animation, vorzeichenbehaftete Höhe (Hoch/Tief getrennt),
    Boden-Position zählt nicht

---

## 26.09.2026 – Uhr-Firmware + Flutter-App (Waveshare ESP32-S3)

Umzug auf die **Waveshare ESP32-S3-Touch-AMOLED-2.06** Uhr. Die Erkennung
läuft jetzt komplett auf der Uhr (eingebettete LogisticRegression als C-Arrays).

- **Uhr-Firmware:** `UhrMeldezaehler/` (QMI8658-Sensor, AMOLED-Display,
  Touch, RTC, Vibrationsmotor an GPIO18)
- **BLE-Anbindung:** Flutter-App `MeldeApp/` synchronisiert Zeit/Datum,
  zeigt Statistik + Stundenplan
- **BLE-Statistik** auf zwei Charakteristiken aufgeteilt (517-Byte-Limit)
- **Vibrationsmotor:** PMU-Versorgung (DCDC4) eingeschaltet, haptisches
  Feedback bei jeder Meldung
- **Build:** korrekte Board-Konfiguration (16 M Flash, `huge_app`,
  OPI-PSRAM), `build.sh`
- **Modell-Prüfung:** `verify_model.py` vergleicht eingebettetes C-Modell
  mit dem trainierten Modell (OK)
- **UI/Features:** Test-App, Kalibrierung per UI löschen, Einstellungen
  (Motor an/aus), Meldungen löschen, Timer/Alarm mit Vibrationsmuster,
  Schul-Watchfaces, Akku-Warnung, Session-Auto-Reset bei Stundenwechsel,
  Stummschaltung im Unterricht, Minimal-Watchface

---

## 27.09.2026 – Umstellung der Erkennung auf CNN (1D-ConvNet)

Die bisherige LogisticRegression wurde durch ein **1D-CNN** ersetzt und als
**TensorFlow Lite Micro (int8)** in die Uhr integriert.

- **Analyse:** Klärung der bisherigen Methode (RandomForest/LogReg, kein SVM/CNN)
- **CNN-Training:** `trainieren_cnn.py`
  → 1D-ConvNet (3 Conv1D-Schichten) auf 1,5-s-Fenstern (150 Samples × 6 Kanäle)
  → Ergebnis (fairer GroupKFold-Vergleich nach Trial):
    - **CNN: 0.929 ± 0.063**
    - LogisticRegression: 0.855 ± 0.084
- **Konvertierung:** `konvertiere_cnn_tflite.py`
  → `modell_cnn.keras` → `modell_cnn_int8.tflite` (19,6 KB, Full-Integer int8)
  → Genauigkeit nach Quantisierung: 100 % (kein Verlust)
  → erzeugt automatisch `UhrMeldezaehler/modell_cnn_data.h` (C-Array)
- **Firmware-Integration** (`UhrMeldezaehler_core.h`):
  - `#define USE_CNN 1` (0 = Fallback auf LogReg)
  - `cnnInit()` + `predictClassCNN()`: Fenster rotieren → standardisieren →
    int8-quantisieren → Inferenz (Scale/Zero-Point aus dem Tensor)
  - 32-KB-Arena (`CNN_ARENA_SIZE`)
- **Bibliotheks-Patch:** `patch_tflite_lib.sh`
  → `TensorFlowLite_ESP32` (v1.0.0) für ESP32-Core 3.x / GCC 14 repariert
    (flatbuffers-`span` const-Member, `TF_LITE_REMOVE_VIRTUAL_DELETE`,
    unbenutzte LCD-Treiber entfernt)
- **Build-Ergebnis:**
  - Flash: 1.543.927 Bytes (49 %) von 3 MB
  - RAM: 97.356 Bytes (29 %) global
- **Doku:** README um CNN-Abschnitt + Build-Hinweise ergänzt
- **Klarstellung:** Training läuft am PC, die Uhr führt nur aus (Inferenz);
  die Kalibrierung auf der Uhr ist eine reine Orientierungs-Anpassung, kein Training
- **Kalibrierungs-Feedback:** kurze Vibration (100 ms) bei jeder
  Kalibrier-Wiederholung, längere Vibration (500 ms) nach jedem abgeschlossenen
  Kalibrier-Schritt bzw. am Ende der Kalibrierung
  (`VIB_REP_MS` / `VIB_SCHRITT_MS` in `UhrMeldezaehler_core.h`)
- **Auto-Upload:** `./build.sh firmware` kompiliert und flasht jetzt automatisch
  auf die Uhr (Port wird unter /dev/ttyACM* bzw. /dev/ttyUSB* erkannt,
  Überschreiben mit `PORT=...`). Erfolgreicher Upload auf `/dev/ttyACM0`.
- **BOOT-Doppelklick:** Zwei schnelle Drücke auf BOOT (innerhalb 800 ms)
  lernen die TISCH-Position neu (Uhr liegt gerade auf dem Tisch). Kurze Vibration
  beim Start, längere bei Erfolg. Ein einfacher Klick öffnet weiterhin das
  Melde-Menü (jetzt mit ~800 ms Verzögerung für die Doppelklick-Erkennung).
- **SD-Karten-Unterstützung:** Die Uhr hat einen microSD-Slot (Pins CLK=2,
  CMD=1, D0=3, 1-Bit-SDMMC). Liegt `model.tflite` auf der SD-Karte, wird dieses
  CNN-Modell beim Start geladen; sonst das eingebaute Flash-Modell.
  Erfolgreich kompiliert + geflasht.
- **SD-Karte eingerichtet:** Karte „AC Family" (15 GB, FAT32) erkannt und
  `model.tflite` (19,6 KB) ins Wurzelverzeichnis kopiert. Bug im TFL3-Header-Check
  behoben (Kennung liegt bei Byte 4, nicht 0). Umbenenn-Skript `sd_benennen.sh`
  angelegt (benötigt sudo). Auf der Karte liegen noch 6,1 GB alter Müll
  (.Trash-1000).
- **SD-Modell erfolgreich getestet:** Karte umbenannt + in die Uhr gesteckt →
  serielle Ausgabe `[cnn] Modell von SD-Karte geladen (19592 Bytes)` ✅
  Der komplette Ablauf SD → Modell → CNN-Inferenz funktioniert.

### Stand der Dateien (CNN)

| Datei | Zweck |
|-------|-------|
| `trainieren_cnn.py` | CNN trainieren → `modell_cnn.keras` |
| `konvertiere_cnn_tflite.py` | int8-TFLite + C-Header erzeugen |
| `modell_cnn.keras` / `modell_cnn_int8.tflite` | Modelle |
| `UhrMeldezaehler/modell_cnn_data.h` | eingebettetes Modell |
| `patch_tflite_lib.sh` | reproduzierbare Library-Patches |

### Nächste Schritte (offen)

- [ ] Firmware auf die Uhr flashen (`./build.sh upload`) und live testen
- [ ] Seriell prüfen: `[cnn] bereit ...` + `[dbg] p(m/k/t)=...`
- [ ] Ggf. Arena vergrößern, falls `AllocateTensors()` fehlschlägt
- [ ] Ggf. Aufnahme-Modus auf der Uhr (neue Trainingsdaten sammeln)

---

## 27.09.2026 (abends) – Trainingsdaten mit der Uhr sammeln (so viel man will)

Neuer Workflow, um mit der Uhr beliebig viele CNN-Trainingsdaten auf dem PC
aufzunehmen und das Modell danach neu zu trainieren.

- **Uhr-Firmware** (`UhrMeldezaehler_core.h`, `UhrMeldezaehler.ino`):
  - Neue serielle Befehle `STREAM` / `STOPSTREAM`.
  - `STREAM` versetzt die Uhr in einen Streaming-Modus: ~100 Hz werden die
    6 Kanäle (ax, ay, az, gx, gy, gz) **rotiert ins Trainings-Koordinatensystem**
    (mit `R_model`, exakt wie bei der CNN-Inferenz) und in physikalischen
    Einheiten (g / dps) gesendet.
  - Während des Streamings pausieren Display-Rendering und Touch, damit die
    100 Hz stabil bleiben.
  - `STOPSTREAM` beendet den Modus und leert den Ringpuffer.
  - Erfolgreich kompiliert (51 % Flash, 39 % RAM).
- **PC-Collector** `uhr_daten_sammeln.py`:
  - Auto-Port (`/dev/ttyACM*`/`/dev/ttyUSB*`), startet den Stream per `STREAM`.
  - Tastatur-Labels (ein Tastendruck, kein ENTER):
    `1` = meldung, `2` = kopf, `3` = tisch, `q` = beenden.
  - Retroaktive Beschriftung: jeder Tastendruck speichert die **letzten
    1,5 s (150 Samples)** als Trial → sehr schnelles Sammeln.
  - Ausgabe: `meldedaten/uhr_positionen.csv` (physikalische Einheiten).
- **Training/Konvertierung**:
  - `trainieren_cnn.py` akzeptiert jetzt einen Dateipfad und erkennt automatisch,
    ob rohe LSB-Werte (alte MPU6050-Daten) oder physikalische Einheiten (Uhr)
    vorliegen.
  - `konvertiere_cnn_tflite.py` ebenso; zusätzlich schreibt es nach dem
    Konvertieren die `CNN_MEAN`/`CNN_STD`-Konstanten aus
    `modell_cnn_meta.joblib` automatisch in `UhrMeldezaehler_core.h` zurück.
  - Damit passt ein neu trainiertes Modell inkl. Standardisierung ohne
    manuelle C-Array-Pflege zur Uhr.
- **README** um den Sammel-Workflow + die neuen Befehle ergänzt.

### Neuer Sammel-/Trainings-Ablauf

```bash
python3 uhr_daten_sammeln.py
python3 trainieren_cnn.py meldedaten/uhr_positionen.csv
python3 konvertiere_cnn_tflite.py meldedaten/uhr_positionen.csv
./build.sh firmware
```

> Hinweis: `REF_TISCH`/`REF_MELDUNG` in der Firmware bleiben unverändert, weil
> der Stream bereits ins alte Trainings-Koordinatensystem rotiert – nur das
> CNN-Modell und die Standardisierung werden neu gelernt.

## 28.09.2026 – Collector auf Live-Aufnahme umgestellt

Der Collector `uhr_daten_sammeln.py` arbeitet jetzt nicht mehr retrograd
(letzte 1,5 s je Tastendruck), sondern als Live-Aufnahme:

- Taste `1`/`2`/`3` drücken → ab sofort wird jedes Sample mit dieser Klasse
  gespeichert (neuer Trial pro Tastendruck).
- Andere Zahl drücken → Umschalten auf die neue Klasse.
- `q` → Aufnahme beenden, Stream stoppen.
- Reader-Thread schreibt kontinuierlich in `meldedaten/uhr_positionen.csv`;
  zu kurze Trials (< 150 Samples) werden beim Beenden markiert.

## 28.09.2026 – 4. Klasse „boden" für das CNN

Neue Klasse `boden` (bücken / etwas vom Boden aufheben) als weiteres
„nicht melden" ergänzt. Das CNN hat jetzt 4 Klassen:

- `0` = kopf, `1` = meldung, `2` = tisch, `3` = boden

Geändert:
- **Firmware** (`UhrMeldezaehler_core.h`): `aktuellProb[4]`, `predictClassCNN`
  liest jetzt bis zu 4 Ausgänge (mit `dims`-Guard, damit ein altes 3-Klassen-
  SD-Modell nicht crasht), `posName[4]` inkl. `BODEN`, Debug-Ausgabe `p(m/k/t/b)`.
- **Collector** (`uhr_daten_sammeln.py`): Taste `4` = boden (nicht melden).
- **Training** (`trainieren_cnn.py`): `KLASSEN = ['kopf','meldung','tisch','boden']`,
  Konfusionsmatrix 4×4.
- **Konvertierung** (`konvertiere_cnn_tflite.py`): Klassenliste zentral, Header-
  Kommentar aktualisiert.
- Firmware kompiliert + auf die Uhr geflasht ✅.

## 28.09.2026 – Sensor-Aufnahme direkt auf der Uhr (ohne PC)

Neue Funktion „SENSOR-AUFNAHME": Die Uhr kann Trainingsdaten jetzt komplett
allein auf die SD-Karte aufnehmen.

- **UI:** `Apps` → Kachel `SENSOR-AUFNAHME` (neuer Screen 9).
  - Klassen antippen: `MELDUNG` / `KOPF` / `TISCH` / `BODEN`.
  - Aufnahme läuft sofort auf `/aufnahme.csv`, stoppen per `STOPPEN`
    oder Zurück-Taste.
- **Firmware** (`UhrMeldezaehler_core.h`, `UhrMeldezaehler.ino`):
  - `ensureSd()` als gemeinsame 1-Bit-SDMMC-Mount-Hilfe (CNN + Recorder).
    Bug behoben: vorher `begin("/sdcard", false)` = 4-Bit-Modus → Mount schlug
    fehl; jetzt `true` = 1-Bit.
  - `startSensorRec()` / `stopSensorRec()` / `sensorRecSample()` schreiben
    rotierte Werte (g, dps) als CSV auf die SD-Karte; Trial-Nummern ab 1000
    (NVS-Zähler `recTrial`).
  - Loop: eigener 100-Hz-Zweig `else if (sensorRec)`.
- **Serielle Befehle:** `RECSD <klasse>`, `STOPSD`, `SDCHECK`.
- Getestet: `RECSD meldung` → 271 Samples → `STOPSD`; `SDCHECK` zeigt
  Header + Zeilen `meldung,1000,0,-0.57,...` ✅
- README/LOGBUCH aktualisiert.

## 28.09.2026 – Umstellung auf 2 Klassen (meldung / nicht_meldung)

Der 4-Klassen-Ansatz (kopf/meldung/tisch/boden) war für den Nutzer zu
umständlich. Neues Ziel: **einfach 2 Klassen**, „nicht melden" = normale
Bewegungen (herumlaufen, kleine Bewegungen).

- **Firmware** (`UhrMeldezaehler_core.h`):
  - `aktuellProb[2]`, Klassen `0=meldung`, `1=nicht_meldung`.
  - `predictClassCNN` liest 2 Ausgänge; LogReg-Fallback mappt die alten
    3 Klassen auf 2 (meldung vs. kopf+tisch).
  - Zustandsmaschine: während „oben" zählt MELDUNG vs. NICHT-MELDEN;
    überwiegt NICHT-MELDEN, wird nicht gezählt.
  - `posName[2] = {"MELDUNG", "NICHT MELDEN"}`, Debug `p(m/n)=...`.
  - **SD-Modell-Guard:** Lädt die SD-Karte ein Modell mit falscher
    Klassenanzahl (z. B. altes 4-Klassen-Modell), wird es verworfen und das
    eingebaute 2-Klassen-Modell benutzt.
  - Sensor-Aufnahme-UI jetzt mit 2 Knöpfen (`MELDUNG` / `NICHT MELDEN`).
- **Collector** (`uhr_daten_sammeln.py`): `1`=meldung, `2`=nicht_meldung.
- **Training/Konvertierung:** `KLASSEN = ['meldung','nicht_meldung']`,
  alte Labels `kopf/tisch/boden` werden automatisch zu `nicht_meldung`.
- 2-Klassen-Modell trainiert (681 Fenster) + konvertiert (Ausgabe `[1,2]`)
  + geflasht ✅.
- Getestet: SD-4-Klassen-Modell wird erkannt + verworfen, Flash-2-Klassen-
  Modell aktiv, `[dbg] p(m/n)=99/0%` ✅.

## 28.09.2026 – 2-Klassen-Modell mit Herumlauf-Daten (99 % Genauigkeit)

- Neue „nicht_meldung"-Daten direkt auf der Uhr gesammelt (19.809 Samples
  Herumlaufen/kleine Bewegungen) → `meldedaten/aufnahme.csv`.
- Alte Melde-Daten + neue Aufnahme zu `meldedaten/kombiniert.csv` gemergt
  (37.979 Samples). Alte Labels `kopf/tisch/boden` → `nicht_meldung`.
- **Training-Skripte verbessert:** `lade_df()` vergibt eindeutige Trial-IDs,
  damit nach dem Label-Mapping keine Trial-Kollisionen entstehen (vorher
  verschmolzen kopf#1 + tisch#1 + boden#1 fälschlich zu einem Trial).
- **Ergebnis (5-fold GroupKFold): CNN 0.990 ± 0.009** (LogReg 0.732).
  Konfusionsmatrix: meldung 297/300, nicht_meldung 1148/1157 richtig.
- Konvertiert (int8, Ausgabe `[1,2]`, 19.504 Bytes) + geflasht.
- SD-Karte: neues 2-Klassen-`model.tflite` aufgespielt (Ausgabe `[1,2]`).
- Live-Test: Uhr meldet jetzt korrekt `p(m/n)=0/99%` (nicht_meldung) statt
  vorher fälschlich „meldung" ✅.

## 28.09.2026 – Akku-Optimierung (für einen ganzen Schultag)

Ziel: Entladung deutlich verlangsamen. Größte Verbraucher abgestellt:

- **Audio lazy initialisiert:** `audioInit()` wird nicht mehr beim Booten
  aufgerufen. Mikrofon (ES7210, 4 Mic-Vorverstärker + Bias), DAC (ES8311),
  I2S und Verstärker (PA_CTRL) waren dauerhaft an. Jetzt erst beim ersten
  Öffnen des Rekorders / TON / REC per `ensureAudio()`.
- **Display-Refresh 15 Hz → 5 Hz** (66 ms → 200 ms).
- **Auto-Standby:** Nach 30 s ohne Bedienung schaltet sich das Display auf
  dem Watchface aus (Zählung läuft weiter). Aufwachen per Tippen oder
  Power-Taste (`AUTO_STANDBY_MS`, `lastActivityMs`).
- **BLE-Advertising-Intervall 1–2 s** statt ~100 ms (spart Strom, solange
  keine App verbunden ist).
- **WLAN beim Boot explizit aus** (`WiFi.mode(WIFI_OFF)`).
- **CPU-Takt 240 MHz → 160 MHz** (`setCpuFrequencyMhz(160)`).
- Kompiliert + geflasht ✅; Audio-Test per `TON` funktioniert weiterhin
  (lazy init OK); CNN läuft normal.

## 28.09.2026 – Audio komplett entfernt + Standby = alles aus

- **Audio komplett gelöscht:** `audio_codec.h` (ES8311/ES7210/I2S/Verstärker)
  entfernt, Rekorder-App (Screen 5) entfernt, serielle Befehle
  `TON`/`REC`/`STOP`/`PEAK` entfernt. Flash-Bedarf um ~31 KB gesunken.
  App-Tray neu angeordnet (ohne Rekorder).
- **Standby (Power-Taste) = volle Stromspar-Stufe:**
  - Display aus, **Sensor aus** (keine Abtastung mehr),
  - **Vibrationsmotor aus**, **BLE aus**,
  - nur die externe **RTC (PCF85063) läuft unabhängig vom ESP32 weiter**
    und hält die Uhrzeit.
  - Aufwachen per Power-Taste oder Tippen.
- **Auto-Standby (30 s) entfernt**, damit der Meldezähler im Unterricht nicht
  versehentlich stoppt – Standby jetzt nur noch manuell per Power-Taste.
- Kompiliert + geflasht ✅; Boot ohne Audio-Init, CNN läuft normal.

## 28.09.2026 – Standby korrigiert: nur Display aus

Klarstellung: Im Standby wird **nur das Display** ausgeschaltet (größter
Stromfresser). Sensor und Vibrationsmotor laufen weiter, damit die Uhr
Meldungen auch bei ausgeschaltetem Bildschirm zählt und haptisch bestätigt.
BLE bleibt ebenfalls an. `enterStandby()`/`wakeFromStandby()` entsprechend
vereinfacht; die `!standby`-Sperren am Sensor-Block entfernt.

## 28.09.2026 – Standby: kein Aufwachen per Touch

Touch-Pollen im Standby entfernt. Aufwachen jetzt **nur noch per Power-Taste**
(weniger Stromverbrauch, weil im Standby kein Touch-Sensor mehr abgefragt wird).

## 01.10.2026 – Echter Deep-Sleep (ESP32 aus, nur RTC) + spannungsbasierte Akku-Anzeige

- **Deep-Sleep:** Wenn Sensor UND BLE aus sind, schaltet die Power-Taste den
  ESP32 komplett ab (nur die externe RTC läuft weiter, ~µA statt ~mA).
  - Aufwachen: Power-Taste (GPIO10 = SYS_OUT) sofort per EXT1-Wakeup
  - 3-s-Timer als Fallback (pollt die Power-Taste über AXP2101-I2C-Interrupt)
  - `enterDeepSleep()`, `peekPowerKeyIrq()`, Timer-Wakeup-Check in `setup()`
- **Akku-% spannungsbasiert:** Der AXP2101-Fuel-Gauge ist ohne Kalibrierdaten
  unzuverlässig → `battPctFromVoltage()` (LiPo-Kurve) + gleitende Glättung.
- **BLE-Zustand persistent:** `btOn` wird in NVS gespeichert und beim Boot
  wiederhergestellt (vorher immer an).
- **Display-Controller-Sleep:** `gfx->displayOff()` (SLPIN) im Standby statt
  nur Helligkeit 0.
- **Neue serielle Befehle:** `BATT` (Akku-Diagnose), `BTN` (Power-Taste GPIO10),
  `SLEEP` (sofort in Deep-Sleep).
- **Farbmakros:** `BLACK`/`WHITE`/… wieder bereitgestellt (GFX-Library ≥ 1.4
  heißt `RGB565_*`).
- **Build:** TensorFlowLite_ESP32 installiert, `patch_tflite_lib.sh` auf
  `python` umgestellt (Windows), kompiliert + geflasht ✅.

## 01.10.2026 – LVGL-App-Menü korrigiert (Watchfaces zurück, Sensor-App fehlte, Übergänge)

Die LVGL-Migration aus „Schritt 1" hatte das Watchface mit nur einem festen
Layout ersetzt und damit alle 6 Zifferblätter unerreichbar gemacht. Korrigiert:

- **Watchface wieder Canvas:** Screen 0 nutzt wieder `drawWatchface()` mit allen
  6 Zifferblättern (Minimal/Farbig/Analog/Digital/Geometrisch/Schule). Die
  Zifferblatt-Auswahl (Screen 3, langes Drücken auf das Watchface) funktioniert
  damit wieder wie vorher.
- **LVGL nur noch für das Apps-Menü (Screen 4).** Dort liegen die großen Buttons,
  dort bringt Partial-Redraw den größten Vorteil.
- **Sensor-Aufnahme-App wieder im Tray:** Das LVGL-Menü hatte nur 4 statt 5 Apps
  – `Sensor-Aufnahme` (Screen 9) fehlte und wurde ergänzt (gleiche Anordnung wie
  der Canvas-Tray).
- **ZURUECK-Button + Wisch-nach-unten** im LVGL-Menü führen zuverlässig zum
  Watchface zurück.
- **Übergang LVGL→Canvas sofort:** Beim Antippen einer App wird `redrawNow`
  gesetzt, damit die Canvas-App sofort gezeichnet wird statt bis zu 1 s das alte
  Bild stehen zu lassen (das war der „beim Rausgehen wird nur die alte App
  angezeigt"-Fehler).
- Kompiliert ✅ (Flash 59 %, RAM 60 %).

## 01.10.2026 – Branch `fix/ui-reaktion`: LVGL entfernt, Touch-Blockaden beseitigt

Auftrag: „mach schritte 1-3 auf eigenem branch, touch ist sehr zäe, reagiert
sporadisch". Ungetestet auf der Uhr – nur kompiliert (Flash 50 %, RAM 41 %).

- **LVGL entfernt** (`lvgl_ui.h`): deckte nur das Apps-Menü ab, brachte einen
  zweiten Renderer und eine zweite Touch-Verarbeitung auf demselben Display.
  Apps-Menü wieder Canvas (`drawAppTray`/`appTrayTap`).
- **Neuzeichnen pro Screen** (`redrawIntervalMs()`): Live-Screens (Melde,
  Zeit, Test, Sensor-Aufnahme) 5 Hz, übrige 1 Hz, sofort bei `redrawNow`.
- **BLE-Schreibbefehle in `loop()`** (`processBleCommands()`): `onWrite()` lief
  im Bluetooth-Task parallel zu `loop()` und griff auf I2C/NVS/Stundenplan zu.
- **`vibrate()` ohne `delay()`** (`updateVibration()` in `loop()`); modale
  Abläufe (Kalibrierung, TISCH-Messung) nutzen `vibrateBlocking()`.
- Unverändert und weiter zu prüfen: QSPI 80 MHz, Dauer von `Invoke()`
  (`[perf] CNN avg`), blockierende Kalibrier-/WLAN-Abläufe.

## 02.10.2026 – Branch `lvgl-port`: komplette Oberfläche auf LVGL 9.3

Auftrag: „Portiere einfach alles nach gui framework, kannst auch flow der
Masken ändern oder die Masken anders gestalten … Hauptsache die
Funktionalität bleibt." WLAN auf Rückfrage: „Entfernen".

- **Ursache des Ruckelns:** Arduino_GFX überträgt blockierend und tauscht
  Pixel per CPU (~75 ms je Vollbild, `PERF_ANALYSE_2026-10-01.md`). Die
  flüssige Hersteller-Demo nutzt esp_lcd mit DMA – entscheidend ist der
  Treiber, nicht LVGL allein (Pis erster Versuch setzte LVGL auf Arduino_GFX).
- **Display:** `hal_display.h` – esp_lcd Quad-SPI + DMA, Treiber
  `esp_lcd_sh8601` aus dem Waveshare-BSP, zwei Teilpuffer à 40 Zeilen,
  Rückfall `USE_ESP_LCD 0` (Arduino_GFX).
- **Oberfläche:** alle Masken neu (`ui*.h`), dunkles Design, Umlaute,
  Bestätigung vor jedem Löschen, Melden mit Seiten Zähler/Statistik/
  Kalibrierung, Einstellungen auf einer Seite, Timer mit Rollen.
  Zurück/Power führt aus Apps zu den Apps (vorher teils direkt zum Zifferblatt).
- **Logik:** Zeichen-, Touch- und WLAN-Code aus `UhrMeldezaehler_core.h`
  entfernt (3727 → ~2100 Zeilen); Touch als ein I2C-Burst; Kalibrierung
  zeigt ihre Schritte über `uiCalibShow()`; Uhrzeit alle 200 ms gelesen.
- **Prüfung ohne Uhr:** `tools/ui_sim` rendert alle Masken und spielt
  Abläufe durch (0 Fehler). Firmware kompiliert (Flash 61 %, RAM 41 %).
- **Offen:** Test auf der Uhr – Bild/Farben/Versatz mit esp_lcd, Touch-Gefühl,
  `[perf] max loop/max ui`, CNN-Laufzeit mit `SENSOR ON`.

### 02.10.2026 (Fortsetzung) – Messungen auf der Uhr

Alle Werte `[perf]` auf der Uhr, Maskenwechsel per seriellem `SCREEN n`:

| Stand | max loop in Ruhe | Maskenwechsel |
|---|---|---|
| main (Canvas, Sensor an) | 75 ms + CNN 420 ms alle 0,5 s | – |
| LVGL + esp_lcd, Analog-Zeiger bildschirmgroß | ~100 ms je Sekunde | – |
| Zeiger als kleine Objekte | 1,4 ms | 75–100 ms (mit Animation) |
| + CNN im Task auf Kern 0, -O2, ohne Animation | **2,4 ms (Sensor an)** | **52–78 ms** |

- **Hauptursache des zähen Touchs:** `Invoke()` des CNN dauerte 310–420 ms
  und lief alle 0,5 s in `loop()`. Jetzt Task `cnnWorker` auf Kern 0;
  Ergebnis 0,5 s versetzt. Die Perf-Analyse vom 01.10. lief mit Sensor aus.
- 2 LVGL-Render-Einheiten (FreeRTOS) brachten keinen Gewinn – verworfen.
- 240 MHz statt 160 MHz: Rendern ~25 % schneller; nicht übernommen (Akku).

## 02.10.2026 – Feinschliff auf der Uhr, USB-Hänger behoben, Sensor-Automatik

Erste Sitzung auf diesem Rechner (Windows): Git per winget installiert,
`setup_libs.py` fand ohne arduino-cli-Konfigurationsdatei den
Bibliotheksordner nicht (`config dump` liefert `{}`) – fragt jetzt
`config get directories.user` ab.

- **Seitenrand:** `UI_PAD` 20 → 32 px (~1 mm; Display hat ~12 px/mm) für alle
  Masken; die Zifferblätter behalten 20 px (`UI_PAD_WF`).
- **Timer mit Stunden:** drei Rollen (0–23 h, 0–59 min, 0–59 s), Höchstwert
  23:59:59. Ab einer Stunde Restzeit Anzeige `H:MM:SS` in `font_d64`.
- **Zifferblatt „Geometrisch“ neu:** Bauhaus-Stil – Stunden und Minuten
  übereinander, gelber Kreis, blaues Quadrat, roter Balken; darunter Datum,
  Meldungen (✓) und Akku. Lila Band und grüne Pille entfernt.
- **Hänger bis 2 s behoben:** Steckte die Uhr am PC, ohne dass ein Programm
  den seriellen Port las, blockierte jede Log-Zeile bei vollem Puffer.
  Gemessen bei 20 s geschlossenem Port: `max loop` 2 005 481 µs → 2–6 ms nach
  `USBSerial.setTxTimeoutMs(0)` (Zeilen werden dann verworfen). Beim Messen
  hatte bisher immer ein Programm den Port offen, daher fiel es nie auf.
- **Einstellung „Nur im Unterricht“** (`autoSensor`, NVS `autoSensor`):
  Sensor laut Stundenplan bei Stundenbeginn an, bei Stundenende aus.
  Geschaltet wird nur beim Wechsel, manuelles Umschalten gilt bis zum
  nächsten Wechsel. Ohne Stundenplan passiert nichts (Hinweis in Amber).
  Auf der Uhr noch nicht über einen echten Stundenwechsel geprüft.
- Kompiliert (Flash 67 %) und geflasht ✅.

## 02.10.2026 (Fortsetzung) – App „Akku-Test“

Ziel: Verbrauch je Komponente bestimmen. Der AXP2101 misst nur die Spannung,
keinen Strom – der Verbrauch ergibt sich aus Läufen, die sich nur in einer
Komponente unterscheiden.

- **Neue Datei `akkutest.h`**, Maske `UI_AKKU` (Screen 11), sechste Kachel
  unter Apps (Kachelraster jetzt 3 × 2).
- **Einstellen:** Dauer (Stunden/Minuten), Sensor, Display, Vibration,
  Anzahl simulierter Meldungen. Steckt USB, wartet der Test, bis das Kabel
  ab ist (beim Laden wäre die Messung wertlos).
- **Lauf:** Spannung 1×/s, Mittel pro Minute; Minutenwerte auf SD
  (`/akkutest.csv`). Display an: alle 8 s eine Maske weiter (nur Anzeigen).
  Touch gesperrt, Abbruch per Power-/BOOT-Taste. Simulierte Meldungen werden
  gespeichert und vibrieren, zählen aber nicht in die Tagesstatistik.
  Einstellungen werden danach wiederhergestellt (`sensorApply()` schaltet den
  Sensor ohne NVS-Schreiben).
- **Ergebnis:** % pro Stunde und hochgerechnete Laufzeit, letzte 8 in NVS;
  Zwischenstand alle 5 min, damit ein leerer Akku den Test nicht verliert.
  Gültig erst ab 5 min.
- **Ausgabe:** seriell `AKKU`, `AKKULOG`, `AKKUSTART`, `AKKUSTOP`,
  `AKKUCLEAR`; BLE-Charakteristik `…26ad` (JSON); PC-Skript
  `tools/akkutest.py` mit Anteil je Verbraucher. Handy-App noch nicht
  angepasst (Flutter auf diesem Rechner nicht installiert).
- Auf der Uhr per seriellem Befehl geprüft: Warten auf USB, Abbruch,
  2-min-Lauf mit Maskenwechsel und 2 Meldungen, SD-Log, Auslesen ✅.
- Grenzen: Prozent aus allgemeiner LiPo-Kurve (1 % ≈ 5–12 mV); nach einem
  Lastwechsel sackt die Spannung ab, die erste Minute als Startwert
  überschätzt den Verbrauch etwas.

## 03.10.2026 – Energietest 1: Grundbedarf des Boards

Eigenes Programm `Energietests/Test1_Grundbedarf/` (ersetzt während des Tests
die Firmware; Anleitung im README dort).

- ESP32 im Deep-Sleep, alles andere aus (Display Sleep In, Touch Hibernate,
  QMI8658 aus, DC4 aus, kein Funk). Weckt alle 10 min, misst die
  Akkuspannung (AXP2101, 1 mV, 16 Lesungen) und speichert sie mit
  RTC-Zeit im RTC-RAM (700 Werte ≈ 116 h).
- Power-Taste: misst und zeigt 3 s gedimmt Spannung, Messungen, Änderung.
  Am USB-Kabel bleibt die Uhr wach: `DUMP`, `STAT`, `CLEAR`, `INTERVALL n`,
  `SCHLAF`. Abziehen startet die Messreihe.
- Gefunden: Öffnen/Schließen des seriellen Ports setzt den Chip zurück;
  `RTC_DATA_ATTR` wird dabei neu belegt → Werte jetzt `RTC_NOINIT_ATTR`.
- Auf der Uhr geprüft: Schlaf (USB-Port nach 1 s weg), Timer-Wecken nach
  60 s, Werte bleiben erhalten ✅. Die 16 Lesungen sind immer gleich – die
  effektive Auflösung bleibt 1 mV. Wecken per Power-Taste noch offen.

### 03.10.2026 (Fortsetzung) – Power-Taste weckte nicht

- Befund: Im Schlaf reagierte die Power-Taste nicht. Langes Drücken schaltete
  den AXP2101 ab, der RTC-RAM ging verloren; beim Anstecken startete die Uhr
  neu.
- Diagnose (`TASTE`, Pin-Suche): Der AXP2101 meldet jeden Druck per I2C
  (Interrupts fallend/steigend/kurz). GPIO10 zeigt den Druck **nur mit
  Pull-up**: In Ruhe zieht die Platine den Pin aktiv auf LOW, beim Drücken
  lässt sie los. Ohne Pull-up bleibt er immer 0 – deshalb weckte EXT1 nie.
  Das betrifft wahrscheinlich auch den Deep-Sleep der Meldezähler-Firmware
  (dort weckte vermutlich nur der 3-s-Abfrage-Timer).
- Kein IRQ-Ausgang des AXP2101 an einem freien ESP-Pin gefunden.
- Fix im Energietest: RTC-Pull-up an GPIO10 im Schlaf, EXT1 auf HIGH.
  Geprüft: Wecken per Taste nach 212 s Schlaf ✅. Kosten: dauerhaft grob
  70 µA durch den Pull-up – abschaltbar mit `TASTENWECKEN 0`.

## 03./04.10.2026 – Energietest 2: Display, Datenverlust, Flash-Sicherung

- **Test 2** (`Energietests/Test2_Display/`): Display an, ESP32 zeichnet kurz
  und schläft dazwischen im Light-Sleep (80 MHz). Erster Lauf mit einer hellen
  Vollfarbe je Sekunde (54 min, 3222 Bilder): ≈ 55 mV/h gegenüber ≈ 5 mV/h
  Grundlast, unsicher ±30 %. Danach umgebaut: alle 0,5 s Schachbrett oder
  Farbverlauf, in der Mitte immer die aktuelle Akkuspannung (Feld wird vom
  Muster ausgespart), Messung immer beim selben Muster – auch die erste nach
  dem Abziehen (vorher ohne Last gemessen).
- **Datenverlust:** Der Lauf in der Nacht lief bis 2,9 V; der AXP2101 schaltete
  ab, der RTC-RAM war leer. Ob die Abschaltung bei 3,45 V ausgelöst hat, ist
  nicht mehr feststellbar.
- **Flash-Sicherung in Test 1 und 2:** jeder Messwert zusätzlich per LittleFS
  auf der ungenutzten `spiffs`-Partition (896 KB; die Firmware nutzt nur NVS,
  das mit 20 KB zu knapp wäre). Einstellungen atomar per Umbenennen.
  Abschaltschwelle Test 2 jetzt 3,55 V. Geprüft mit `STROMAUSFALL`:
  Messungen und Einstellungen kommen zurück ✅.
- Datenblatt ESP32-S3 (Tab. 5-9/5-10): 80/160/240 MHz im Leerlauf 22,0/27,6/
  32,9 mA, Light-Sleep 240 µA (+ PSRAM), Deep-Sleep 7–8 µA. Arduino-Core:
  `loop()` auf Kern 1, BLE/esp_timer auf Kern 0, FreeRTOS 10.5.1, kein
  automatisches Stromsparen (`CONFIG_PM_ENABLE` aus).

## 04.10.2026 – Energietest 2 ausgewertet, Energietest 3: Bewegungssensor

- **Test 2, Musterlauf** (`messungen/2026-10-04_hell255_muster.csv`): 4,08 h
  ohne Kabel, 29 457 Bilder, 4101 → 3791 mV, gleichmäßig ≈ 76 mV/h (Grundlast
  ≈ 5 mV/h). Grob hochgerechnet über eine allgemeine LiPo-Kurve: ≈ 9 %/h,
  also ≈ 9–11 h Laufzeit bei Helligkeit 255 und hellen Mustern.
- **Test 3** (`Energietests/Test3_Bewegungssensor/`): QMI8658 wie `qmiInit()`
  der Firmware, ESP32 liest alle 10 ms 12 Bytes und verwirft sie, dazwischen
  Light-Sleep; Display aus. Flash-Sicherung (`/test3.*`) und Power-Taste wie
  in Test 2.
- Gefunden: Nach Test 2 stand im QMI8658 noch CTRL1 Bit 0 (Oszillator aus) –
  der Soft-Reset löschte es nicht, alle Werte 0. Danach einmal fehlendes
  Auto-Increment (sechs gleiche Werte). Jetzt CTRL1 ausdrücklich setzen und
  die Einstellung zurücklesen. Betrifft möglicherweise auch `qmiInit()` der
  Firmware nach einem Energietest.
- Geprüft am Kabel: 100 Lesungen/s, 0 Fehler, Werte plausibel;
  `STROMAUSFALL` ✅. Lauf ohne Kabel steht aus.
