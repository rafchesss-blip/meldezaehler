# Test 2 – Display

Ziel: den Verbrauch des AMOLED-Displays bestimmen. Wie in Test 1 wird die
Akkuspannung über die Zeit aufgezeichnet; der Vergleich mit Test 1
(Grundbedarf) ergibt den Anteil des Displays.

## Zustand während des Tests

- Display an, Helligkeit 255 (einstellbar), **jede halbe Sekunde ein neues
  Muster** im Wechsel: Schachbrett weiß/schwarz, Regenbogen-Verlauf,
  Schachbrett rot/cyan, Verlauf blau→gelb, Schachbrett blau/gelb, Verlauf
  magenta→grün, Schachbrett magenta/grün, Regenbogen versetzt
- In der Mitte steht immer die **aktuelle Akkuspannung** (eine Lesung je
  Bild); das Muster spart dieses Feld aus, damit nichts flackert
- ESP32-S3 zeichnet nur kurz (~60 ms je Bild, 80 MHz) und schläft dazwischen
  im Light-Sleep; das Display hält sein Bild im eigenen Speicher.
  Der Chipselect des Displays wird im Schlaf HIGH gehalten.
- Touch Hibernate, Bewegungssensor aus, Motorversorgung aus, kein Funk
- Kein Tastenwecken-Pull-up nötig: Die Taste wird mit jedem Bild über den
  AXP2101 (I2C) abgefragt, der sich den Druck merkt.

## Messung

- Alle 5 min (einstellbar), **immer bei Muster 0** (Schachbrett weiß/schwarz)
  und 200 ms Wartezeit, damit die Last beim Messen jedes Mal gleich ist –
  auch die erste Messung nach dem Abziehen
- Akkuspannung vom AXP2101 (1 mV), 16 Lesungen, RTC-Zeitstempel, RTC-RAM
  (700 Messungen)
- **Jeder Messwert wird zusätzlich im Flash gesichert** (LittleFS auf der
  sonst ungenutzten `spiffs`-Partition: `/test2.bin`, Einstellungen in
  `/test2.cfg`). Nach einem Stromausfall lädt die Uhr beides zurück; die
  Reihe geht mit einer Messung „start“ weiter.
- Unter **3,55 V** endet der Lauf: Messung „leer“, Display aus, Tiefschlaf.
  Die Daten bleiben erhalten; die Power-Taste zeigt sie an.

## Ablauf

1. Flashen (siehe `../README.md`), am Kabel voll laden (`STAT`: `laedt=0`).
2. Kabel abziehen → Messung „abgezogen“, Farbwechsel beginnt.
3. Laufen lassen, bis der Lauf von selbst endet (Akku bei 3,45 V) oder lange
   genug für eine klare Steigung (≥ 1–2 h).
4. Power-Taste kurz: 3 s Anzeige (Messung „taste“). **Nicht lange drücken** –
   nach ~6 s schaltet der AXP2101 ab und der RTC-RAM ist verloren.
5. Auswerten: Kabel anstecken (Lauf pausiert, Messung „angesteckt“),
   serielle Konsole 115200 Baud, `DUMP`. Abziehen setzt den Lauf fort.

## Serielle Befehle (nur am USB-Kabel)

| Befehl | Wirkung |
|---|---|
| `DUMP` | Kopfzeile (Helligkeit, Bilder) + CSV `nr,zeit,stunden,mv,streuung_mv,grund,usb,laedt` |
| `STAT` | Anzahl, Intervall, Helligkeit, Zustand, Bilder, Zeichendauer, aktuelle Spannung |
| `CLEAR` | Messungen in RTC-RAM und Flash löschen, neu starten (beendet auch `LAUF`) |
| `INTERVALL n` | Messabstand in Minuten (1–240, Standard 5) |
| `HELL n` | Helligkeit 0–255 (Standard 255) |
| `LAUF` | Musterwechsel am Kabel starten, ohne Light-Sleep (nur zum Prüfen) |
| `STROMAUSFALL` | RTC-RAM verwerfen und neu starten (prüft die Flash-Sicherung) |

`grund`: `start`, `timer`, `taste`, `abgezogen`, `angesteckt`, `leer`.

## Auswertung und Grenzen

- Der Verbrauch des Displays hängt stark von Farbe und Helligkeit ab (AMOLED:
  Weiß am teuersten, Schwarz fast gratis). Gemessen wird der Mittelwert über
  die acht Muster – im Alltag mit dunklem Design liegt er deutlich darunter.
- Enthalten sind auch das Zeichnen (~60 ms je halbe Sekunde) und das
  Aufwachen aus dem Light-Sleep; die Grundlast aus Test 1 muss abgezogen
  werden.
- Erster Lauf (03.10.2026, eine helle Vollfarbe je Sekunde, 54 min):
  ≈ 55 mV/h, unsicher ±30 %; damals fehlte die Last bei der ersten Messung.
- Die ersten 1–3 h nach dem Vollladen sind durch Entspannung des Akkus
  verfälscht; bei hoher Last fällt das weniger ins Gewicht als in Test 1.

## Geprüft (03.10.2026)

Vollfarben-Version: 3222 Bilder in 54 min ohne Kabel (Light-Sleep) und
Anzeige per Power-Taste ✅. Muster-Version: am Kabel mit `LAUF` 2 Bilder/s,
59–65 ms je Bild ✅; ohne Kabel noch nicht beobachtet.

Lauf in der Nacht 03./04.10.2026 verloren: Der Akku lief bis 2,9 V leer, der
AXP2101 schaltete ab, der RTC-RAM war gelöscht (Abschaltung bei 3,45 V hat
nicht gereicht oder nicht ausgelöst). Seitdem Flash-Sicherung und 3,55 V.
Am 04.10. geprüft: 3 Messungen, `STROMAUSFALL`, danach alle 3 Messungen und
die Einstellungen (Intervall, Helligkeit, Laufzustand) wiederhergestellt ✅.
