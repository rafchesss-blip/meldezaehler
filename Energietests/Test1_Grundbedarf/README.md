# Test 1 – Grundbedarf des Boards

Ziel: den Ruhestrom der Platine bestimmen, wenn alle Verbraucher aus sind.
Da der AXP2101 keinen Strom misst, wird über lange Zeit die Akkuspannung
aufgezeichnet; aus ihrem Abfall ergibt sich der Grundbedarf.

## Zustand während des Tests

- ESP32-S3 im Deep-Sleep, weckt alle 10 min (Timer) oder per Power-Taste
- Display: Display Off + Sleep In; Touch FT3168: Hibernate
- Bewegungssensor QMI8658: Accel/Gyro aus, Oszillator aus
- Motorversorgung (AXP2101 DC4) aus; WLAN/BLE werden nie gestartet
- Reset-Leitungen von Display und Touch werden im Schlaf festgehalten

## Messung

- Akkuspannung vom AXP2101-ADC, 1 mV je Stufe (feinste verfügbare Auflösung)
- 16 Lesungen pro Messung, gespeichert als Mittel in 0,1 mV plus Streuung
  (in der Praxis sind alle 16 gleich – effektiv also 1 mV)
- Zeitstempel aus der externen RTC (PCF85063)
- Speicher: RTC-RAM (`RTC_NOINIT_ATTR`), 700 Messungen ≈ 116 h bei 10 min.
  Übersteht Deep-Sleep und Resets, **nicht** aber einen Stromausfall.

## Ablauf

1. Flashen (siehe `../README.md`), Uhr am Kabel voll laden, bis `STAT`
   `laedt=0` meldet. Die Uhr bleibt am Kabel wach.
2. Ggf. SD-Karte entnehmen (zieht sonst Leerlaufstrom).
3. Kabel abziehen → erste Messung sofort, danach alle 10 min.
4. Uhr an einem Ort mit gleichbleibender Temperatur liegen lassen,
   mindestens 24–48 h.
5. Power-Taste zeigt 3 s lang: Spannung, Anzahl Messungen/Stunden,
   Änderung seit Start (Messung wird als `taste` markiert).
6. Auswerten: Kabel anstecken, Power-Taste drücken (Uhr bleibt wach),
   serielle Konsole 115200 Baud, `DUMP`.

## Serielle Befehle (nur am USB-Kabel)

| Befehl | Wirkung |
|---|---|
| `DUMP` | alle Messungen als CSV: `nr,zeit,stunden,mv,streuung_mv,grund,usb,laedt` |
| `STAT` | Anzahl, Intervall, Weck-/Resetgrund, aktuelle Spannung |
| `CLEAR` | Messungen löschen |
| `INTERVALL n` | Messabstand in Minuten (1–240, Standard 10) |
| `TASTENWECKEN 0/1` | Wecken per Power-Taste aus/an (Standard an, kostet ~70 µA, s. u.) |
| `TASTE` | 20 s lang GPIO10 und die Tasten-Interrupts des AXP2101 anzeigen |
| `SCHLAF` | sofort schlafen (zum Prüfen des Weckens am Kabel) |

`grund`: `start` (Kaltstart), `timer`, `taste`, `abgezogen` (Beginn der Reihe).
Messungen mit `usb=1` sind wertlos (Akku lädt).

## Auswertung und Grenzen

- Die ersten 1–3 h nach dem Vollladen nicht werten: Die Spannung entspannt
  sich, ohne dass Strom fließt.
- Erwartet werden nur einige 10 mV pro Tag; Temperaturschwankungen wirken in
  derselben Größenordnung.
- Jedes Aufwachen (~0,1–0,2 s) und jede Tastenanzeige (3 s Display) gehören
  mit zum gemessenen Bedarf.
- **Power-Taste und GPIO10:** Die Platine zieht GPIO10 in Ruhe aktiv auf LOW
  und lässt beim Drücken nur los – HIGH entsteht erst durch einen Pull-up.
  Ohne Pull-up bleibt der Pin immer 0 (die Taste weckte deshalb anfangs
  nicht). Im Schlaf ist deshalb der interne RTC-Pull-up an; er zieht in Ruhe
  dauerhaft grob 70 µA (3,3 V / ~45 kΩ) und hält die RTC-Peripherie wach.
  Für eine reine Grundlast-Messung `TASTENWECKEN 0` setzen (dann nur Timer)
  oder den Anteil abziehen.
- Ein IRQ-Ausgang des AXP2101 an einem freien ESP-Pin wurde nicht gefunden
  (geprüft: GPIO 13, 16, 17, 21, 38–48); Tastendrücke sieht der ESP sonst nur
  per I2C.
- Für µA-genaue Werte: Multimeter in Reihe mit dem Akku.

## Geprüft (03.10.2026)

Am Kabel: Schlaf (USB-Port nach 1 s weg), Timer-Wecken nach 60 s
(Weckgrund 4, Reset 8), Messwerte bleiben erhalten. Wecken per Power-Taste
nach 212 s Schlaf (Weckgrund 3 = EXT1, Messung „taste“) ✅.
