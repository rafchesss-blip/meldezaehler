# Test 3 – Bewegungssensor

Ziel: den Verbrauch des Bewegungssensors QMI8658 so bestimmen, wie die
Meldezähler-Firmware ihn nutzt. Wie in Test 1 und 2 wird die Akkuspannung über
die Zeit aufgezeichnet; der Vergleich mit Test 1 (Grundbedarf) ergibt den
Anteil von Sensor und Auslesen.

## Zustand während des Tests

- QMI8658 eingestellt wie `qmiInit()` der Firmware: Accel ±2 g / 1000 Hz,
  Gyro ±1024 dps / 896,8 Hz, beide Tiefpässe an. Die Einstellung wird
  zurückgelesen (bis zu 3 Versuche, s. u.)
- ESP32-S3 liest wie die Firmware **alle 10 ms einen Datensatz** (12 Bytes
  Accel+Gyro per I2C, 400 kHz) und **verwirft ihn** – nur ein Zähler läuft mit.
  Dazwischen Light-Sleep, 80 MHz
- Display aus (Sleep In), Touch Hibernate, Motorversorgung aus, kein Funk;
  die RTC läuft
- Power-Taste und USB werden alle 0,5 s über den AXP2101 (I2C) abgefragt

## Messung

- Alle 10 min (einstellbar) Akkuspannung vom AXP2101 (1 mV, 16 Lesungen),
  RTC-Zeitstempel, RTC-RAM (700 Messungen ≈ 116 h)
- **Jeder Messwert wird zusätzlich im Flash gesichert** (LittleFS: `/test3.bin`,
  Einstellungen und Zähler in `/test3.cfg`) und nach einem Stromausfall
  zurückgeladen
- Unter **3,55 V** endet der Lauf: Messung „leer“, Sensor aus, Tiefschlaf;
  die Power-Taste zeigt dann die Daten

## Ablauf

1. Flashen (siehe `../README.md`), am Kabel voll laden (`STAT`: `laedt=0`).
   `STAT` zeigt einen Datensatz – Accel muss ~16000 auf der Achse nach unten
   zeigen (1 g), die sechs Werte dürfen nicht alle gleich sein.
2. Kabel abziehen → Messung „abgezogen“, der Sensor wird ab jetzt gelesen.
3. Ruhig liegen lassen, mindestens 12–24 h.
4. Power-Taste kurz: 3 s gedimmte Anzeige (Messung „taste“). **Nicht lange
   drücken** – nach ~6 s schaltet der AXP2101 ab und der RTC-RAM ist verloren
   (die Flash-Sicherung bleibt).
5. Auswerten: Kabel anstecken (Lauf pausiert, Messung „angesteckt“),
   serielle Konsole 115200 Baud, `DUMP`. Abziehen setzt den Lauf fort.

## Serielle Befehle (nur am USB-Kabel)

| Befehl | Wirkung |
|---|---|
| `DUMP` | Kopfzeile (Rate, Lesungen, Fehler) + CSV `nr,zeit,stunden,mv,streuung_mv,grund,usb,laedt` |
| `STAT` | Anzahl, Intervall, Rate, Zustand, Lesungen/Fehler, letzter Datensatz, CTRL1–CTRL8, Spannung |
| `CLEAR` | Messungen in RTC-RAM und Flash löschen, neu starten (beendet auch `LAUF`) |
| `INTERVALL n` | Messabstand in Minuten (1–240, Standard 10) |
| `RATE n` | Lesungen pro Sekunde (1–200, Standard 100 wie die Firmware) |
| `LAUF` | Lesen am Kabel starten, ohne Light-Sleep (nur zum Prüfen) |
| `STROMAUSFALL` | RTC-RAM verwerfen und neu starten (prüft die Flash-Sicherung) |

`grund`: `start`, `timer`, `taste`, `abgezogen`, `angesteckt`, `leer`.

## Auswertung und Grenzen

- Gemessen wird Sensor **plus** 100-maliges Aufwachen und I2C-Lesen pro
  Sekunde – genau das, was die Firmware im Standby für den Sensor tut. Die
  Grundlast aus Test 1 muss abgezogen werden.
- Die Auswertung der Daten (Ringpuffer, Erkennung, CNN) fehlt; sie kostet in
  der Firmware zusätzlich Rechenzeit.
- Die ersten 1–3 h nach dem Vollladen sind durch Entspannung des Akkus
  verfälscht.

## Geprüft (04.10.2026)

- Am Kabel mit `LAUF`: ~100 Lesungen/s, 0 Fehler ✅; Datensatz plausibel
  (a ≈ 15500/1000/−3500, also ~1 g; Gyro nahe 0) ✅
- Beim ersten Start nach dem Flashen kamen erst nur Nullen (CTRL1 Bit 0 =
  Oszillator aus, von Test 2 gesetzt), dann sechs gleiche Werte (Auto-Increment
  fehlte). Seitdem CTRL1 ausdrücklich gesetzt und die Einstellung zurückgelesen
- `STROMAUSFALL`: Messungen und Einstellungen aus dem Flash zurück ✅
- Ohne Kabel (Light-Sleep-Pfad) noch nicht beobachtet
