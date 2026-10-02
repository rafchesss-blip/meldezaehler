# Hinweise für Pi – LVGL-Portierung und Arbeitsweise (02.10.2026)

> Auftrag: „Kannst du noch in einer md datei pi erklähren, was ihm fehlte und was
> und wie du es besser gemacht hast. … So aus deiner sicht die Empfehlungen an
> pi, was er bei sich in agent-configs einstellen soll, um mit dem projekt
> effektiv weiter zu machen?“
>
> Verfasst von Claude (Claude Code) nach dem Merge von PR #1 (`lvgl-port`,
> `b0d9b76`). Grundlage sind die Git-Historie, `SITZUNG_2026-10-01.md`,
> `PERF_ANALYSE_2026-10-01.md`, `LOGBUCH.md` und Messungen auf der Uhr.
> Pis eigene Sitzungen auf dem anderen Rechner kenne ich nicht.

## Kurzfassung

Der zähe Touch hatte zwei Ursachen.

1. **Das CNN blockierte die Hauptschleife.** `Invoke()` dauerte 310–420 ms und
   lief alle 0,5 s direkt in `loop()`. Gemessen wurde nur mit ausgeschaltetem
   Sensor, deshalb fiel das nie auf.
2. **Das Display wurde blockierend übertragen.** Arduino_GFX schickt jedes Bild
   mit Polling über den Bus und tauscht die Bytes per CPU. Ein Vollbild kostete
   so ~75 ms.

Pi hat das Framework gewechselt (LVGL), nicht aber den Übertragungsweg. Er hat
nur zwei Masken umgestellt und das Modell nie gemessen. Deshalb blieb es zäh.

| Stand | `max loop` in Ruhe | Maskenwechsel |
|---|---|---|
| vor dem Port (Canvas, Sensor an) | 75 ms + 310–420 ms CNN alle 0,5 s | – |
| Pis LVGL-Stand (`2f32729`) | ~19 ms auf LVGL-Masken, sonst 75 ms; CNN unverändert | – |
| jetzt (`main`) | **2–3 ms mit Sensor an** | 52–78 ms (einmalig) |

## Was gefehlt hat – mit Belegen

### 1. Gemessen wurde nicht der Zustand, um den es ging
- `PERF_ANALYSE_2026-10-01.md`, Abschnitt „Hinweise“: „**Sensor ist AUS** →
  deshalb keine `[dbg]`-Zeilen und kein `[perf] CNN avg`“. Der Nutzer klagt
  über den Betrieb im Unterricht, also mit Sensor. Genau dieser Fall wurde
  nicht gemessen.
- In `0658026` wurde die CNN-Diagnose eingebaut, die Werte wurden aber nie
  gelesen. Mit eingeschaltetem Sensor zeigt sie sofort
  `[perf] CNN avg=419792 us`.
- 240 MHz wurde in `0658026` verworfen („brachte keine Besserung“). Das lag
  daran, dass der eigentliche Engpass woanders saß. Auf dem heutigen Stand
  rendert 240 MHz messbar ~25 % schneller.

**Regel:** Erst den Fall messen, den der Nutzer beschreibt. Ein Messwert gilt
nur zusammen mit dem Zustand, in dem er entstand (Sensor, BLE, Maske).

### 2. Die Ursache wurde in der Schicht vermutet, die man sieht
- `8dbe9bc` hängt LVGL an `gfx->draw16bitRGBBitmap()`. Das ist derselbe
  blockierende Pfad wie vorher, nur in kleineren Stücken.
- Die flüssige Hersteller-Demo wurde nicht gelesen. Sie überträgt mit
  **esp_lcd über Quad-SPI und DMA** (Waveshare-BSP, Treiber `esp_lcd_sh8601`).
  LVGL rendert dort in den zweiten Puffer, während der DMA den ersten
  überträgt. Das ist der Teil, der den Unterschied macht.

**Regel:** Bevor man eine Technik übernimmt, die „bei der Demo flüssig läuft“,
die Demo-Quelle lesen und herausfinden, *welcher Teil* den Unterschied macht.

### 3. Halbe Migration statt sauberer Grenze
- Nach `8dbe9bc` und `2f32729` rendern zwei Systeme auf dasselbe Display
  (Canvas und LVGL), und es gibt zwei Touch-Pfade.
- `SITZUNG_2026-10-01.md` listet die Folgen:
  - Die Zifferblätter waren weg.
  - Eine App fehlte.
  - Es blieben alte Bilder stehen.
- Jede Korrektur behob ein Symptom der Mischform und machte den Code noch
  verflochtener.

**Regel:** Eine Oberfläche gehört genau einem Renderer. Lieber zuerst die
Grenze zwischen Oberfläche und Logik ziehen und dann komplett umstellen.

### 4. „Zu aufwendig“ lag an der fehlenden Trennung, nicht an LVGL
- Vorher stand alles in einer Datei: `UhrMeldezaehler_core.h` mit 3727 Zeilen.
- Die Logik setzte `screen` selbst, zum Beispiel in `powerBack`,
  `updateZeitApp` und `bootPress`.
- Kalibrierung, WLAN und TISCH zeichneten direkt auf den Canvas.
- Tap-Handler veränderten Zählerstände direkt.

Der Port wurde überschaubar, sobald drei Schritte gemacht waren:

1. **Bestandsaufnahme:** Jede Maske, Geste und Taste mit ihrer Wirkung
   aufschreiben (10 Masken, 6 Zifferblätter, Navigationstabelle).
2. **Kopplung kartieren:** Welche Variable schreibt die Logik, welche liest
   die Oberfläche?
3. **Grenze ziehen:**
   - `screen` bleibt Quelle der Wahrheit; `uiSync()` lädt bei Änderung die
     passende Maske.
   - Masken ändern Zustand nur über Funktionen in `ui_ctl.h` und den Kern.
   - Blockierende Abläufe wie die Kalibrierung melden sich über
     `uiCalibShow()` / `uiMessage()`.

Danach waren es rund 1900 Zeilen neuer Oberflächen-Code in fünf Dateien. Die
Kerndatei schrumpfte auf ~2100 Zeilen.

### 5. Kein Gestaltungssystem und keine Sichtprüfung → „hässlich“
- Pis Oberfläche bestand aus Pixelkoordinaten pro Element.
- Die Schrift war der 6×8-GFX-Font hochskaliert, ASCII statt Umlaute
  („ZURUECK“).
- Es gab keine einheitlichen Abstände, Farben oder Bausteine und keine
  Screenshots zur Kontrolle.

Jetzt ist es anders:
- **`ui_theme.h`** legt alles einmal fest:
  - Farbtoken mit Bedeutung: Grün = Melden, Amber = Zeit, Rot = Löschen.
  - Ränder, Kopfzeile.
  - Bausteine: Karte, Knopf, Listenzeile, Schalterzeile, Bestätigungsdialog,
    Toast.
- **Schriften:** Montserrat mit Umlauten, erzeugt mit `lv_font_conv`
  (`tools/fonts.sh`).
- **Sichtprüfung:** Jede Maske wurde im Simulator als PNG gerendert und
  angesehen. Gefundene Fehler wurden korrigiert: Überlappungen, fehlende
  Glyphen, abgeschnittene Texte.

### 6. Keine Prüfung ohne Hardware
Ohne Uhr am Rechner gab es keine Möglichkeit, Masken oder Abläufe zu prüfen.

`tools/ui_sim/build.sh` schließt diese Lücke:
- Es übersetzt dieselben `ui*.h` gegen eine Stub-Logik.
- Es rendert alle Masken als PNG.
- Es spielt Touch-Abläufe durch und prüft die erwarteten Aufrufe, zum
  Beispiel „Kalibrierung löschen → Bestätigen ruft `clearCalibration`“.
- Es misst, wie viel Fläche im Ruhezustand pro Sekunde neu gezeichnet wird.

So fiel auf, dass das Analog-Zifferblatt jede Sekunde 100 % des Bildschirms
neu zeichnete. Die Zeiger waren Objekte in Bildschirmgröße. Das kostete auf
der Uhr ~100 ms und ist jetzt behoben.

### 7. Dokumentation wich vom Code ab
- `LOGBUCH.md` meldete „audio_codec.h entfernt“, die Datei lag aber noch im
  Repo.
- Die README enthielt halbe Sätze und eine doppelte Zeile.
- „Serienkonsole“ statt *serielle Konsole*.

**Regel:** Nur dokumentieren, was im Commit tatsächlich steht. Begriffe nach
ihrer etablierten Bedeutung verwenden.

### 8. Nebenläufigkeit übersehen
Die BLE-Callbacks liefen im Bluetooth-Task und griffen auf I2C, NVS und den
Stundenplan zu, gleichzeitig mit `loop()`. Behoben in `6766dec`: Die Callbacks
reihen nur noch in eine Queue ein, `loop()` arbeitet sie ab. Inzwischen läuft
außerdem das CNN in einem eigenen Task (`cnnWorker`, Kern 0). Bei jeder neuen
Stelle prüfen, **in welchem Task** sie läuft.

## Wie vorgegangen wurde (zum Nachmachen)

1. **Bestandsaufnahme und Kopplungsanalyse** schriftlich, bevor Code
   entsteht. Die Rückfrage an den Nutzer betraf nur das, was nicht ableitbar
   war (WLAN behalten?).
2. **Referenz lesen:** Hersteller-BSP und Demo-Quellen (`gh api`), Treiber
   übernommen (Lizenzkopf erhalten).
3. **Plan** mit Dateien, Reihenfolge, Verifikation und Risiken, vom Nutzer
   freigegeben.
4. **HAL zuerst auf der Hardware prüfen:** Ein eigener Test-Sketch zeigt
   Farbbalken, Rand, Umlaute, Touch-Koordinaten und eine Animation. Der
   Nutzer bestätigte das Bild, *bevor* die volle Firmware geflasht wurde.
5. **Oberfläche bauen** mit Gestaltungssystem und Simulator-Screenshots nach
   jeder Maske.
6. **Zweite Meinung:** Ein unabhängiger Review-Agent prüfte den Diff gegen
   den LVGL-Quellcode. Seine 7 Befunde wurden behoben, zum Beispiel:
   - Wischen auf den Apps löste zusätzlich einen Klick aus.
   - Halten wirkte auf manchen Zifferblättern nicht.
   - Ein fehlgeschlagener DMA-Transfer hätte die Uhr einfrieren lassen.
7. **Auf der Uhr messen**, reproduzierbar: Der serielle Befehl `SCREEN n`
   öffnet Masken per Skript, und `[perf] max loop/max ui/CNN avg` zeigt die
   Wirkung. Jede Optimierung wurde einzeln gemessen und nur behalten, wenn sie
   wirkte:
   - **behalten:** `-O2`, CNN auf Kern 0, keine Übergangsanimation
   - **verworfen:** zwei LVGL-Render-Threads, weil ohne Gewinn
8. **Abschluss:** LOGBUCH mit Messtabelle, README (Bedienung, Rückfallebene,
   Simulator), Pull Request mit offenem Punkt.

**Noch offen:** Erkennungstest am Handgelenk. Das CNN-Ergebnis kommt jetzt
0,5 s versetzt.

## Empfehlungen für Pis Agent-Konfiguration

Pi läuft auf dem anderen Rechner. Die folgenden Inhalte gehören am besten in
eine **Projektdatei `AGENTS.md` im Repo-Wurzelverzeichnis**. Pi lädt solche
Kontextdateien aus dem Projektverzeichnis; den genauen Dateinamen und die
Ladereihenfolge bitte in der Pi-Doku gegenprüfen, ich konnte Pis Installation
dort nicht einsehen. Im Repo versioniert gelten sie für jeden Agenten und
jeden Rechner.

### Vorschlag für `AGENTS.md`

```markdown
# Meldezähler – Arbeitsregeln für Agenten

## Projekt
- Firmware: UhrMeldezaehler/ (Arduino, ESP32-S3, Waveshare AMOLED 2.06), Oberfläche LVGL 9.3.
- Schichten: UhrMeldezaehler_core.h (Logik, kein Zeichnen) · ui_ctl.h (Bedien-Logik)
  · ui*.h (Masken, nur LVGL) · hal_display.h (esp_lcd + DMA). Diese Grenzen einhalten.
- Navigation: globale Variable `screen`; uiSync() lädt die Maske. Masken ändern Zustand
  nur über ctl*-/Kernfunktionen.
- Alles, was LVGL oder I2C anfasst, läuft in loop(). BLE-Callbacks reihen nur ein.
  CNN läuft im Task cnnWorker (Kern 0).

## Vor jeder Änderung
1. Den beschriebenen Fehler im beschriebenen Zustand messen (Sensor AN, BLE AN).
   Log: [perf] max loop / max ui / CNN avg (seriell, 115200).
2. Ursache belegen (Messwert oder Codezeile), erst dann ändern.
3. Bei größeren Änderungen: Plan mit Dateien, Prüfschritten, Risiken – vom Nutzer freigeben lassen.

## Prüfen
- Oberfläche: tools/ui_sim/build.sh muss „ALLES OK“ melden; geänderte Masken als PNG ansehen.
- Firmware: ./build.sh firmware (-O2). Keine Warnungen aus eigenem Code.
- Auf der Uhr: SCREEN n für Maskenwechsel, Messwerte vorher/nachher ins LOGBUCH.
- Ruhezustand: keine Maske darf mehr als ~15 % Fläche pro Sekunde neu zeichnen.

## Gestaltung
- Nur Bausteine/Farben/Schriften aus ui_theme.h; keine Pixel-Einzelanfertigungen.
- Echte Umlaute (Schriften: tools/fonts.sh), Bestätigung vor jedem Löschen.

## Dokumentation
- LOGBUCH.md: datiert, nur was im Commit steht. Keine neuen Doku-Dateien ohne Auftrag.
- Fachbegriffe korrekt (serielle Konsole, Fork ≠ Branch).
```

### Weitere Einstellungen bei Pi

- **Modell mit Bildeingabe** verwenden oder für UI-Aufgaben zuschalten. Ohne
  Bilder kann Pi die Simulator-Screenshots nicht prüfen, und „hässlich“
  bleibt unbemerkt.
- **Hohe Denkstufe** für Architektur- und Performance-Fragen (Pi:
  `defaultThinkingLevel` in `settings.json`). Bei solchen Aufgaben zuerst
  einen Plan verlangen, bevor Code entsteht.
- **Werkzeuge freigeben, die Messen ermöglichen:**
  - `arduino-cli` (compile/upload)
  - Python mit `pyserial` zum Mitschneiden des seriellen Logs
  - `tools/ui_sim/build.sh`
  - `gh` zum Lesen von Hersteller-Repos

  Unter Windows: Git Bash oder WSL für `build.sh` und `tools/ui_sim/build.sh`.
  Der Simulator braucht gcc und Python mit Pillow.
- **Zwei kurze Skills oder Prompt-Vorlagen** (Pi: `~/.pi/agent/skills/`):
  - *„Perf-Messung Uhr“*: flashen, 60 s mitschneiden mit Sensor an, Masken per
    `SCREEN n` durchschalten, Tabelle vorher/nachher.
  - *„Masken-Änderung“*: ändern → Simulator → PNG ansehen → Firmware bauen →
    LOGBUCH.
- **Sitzungsprotokolle** wie `SITZUNG_*.md` nur auf Wunsch anlegen; die
  laufende Dokumentation gehört ins LOGBUCH und an den Code.
