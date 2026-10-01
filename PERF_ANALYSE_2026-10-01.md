# Performance-Analyse Serienkonsole – 01.10.2026

> Auswertung der `[perf]`-Ausgabe nach dem Flash des Canvas-only-Stands
> (Commit `b39309f`, Merge PR #1 „fix/ui-reaktion“).
> Hardware: Waveshare ESP32-S3-Touch-AMOLED-2.06, Port COM6, 115200 Baud.

## Gemessene Werte

Serielle Ausgabe `[perf] max loop` über ~20 s (4 Proben):

```
[perf] max loop=74999 us   (~75 ms)
[perf] max loop=74767 us   (~75 ms)
[perf] max loop=74749 us   (~75 ms)
[perf] max loop=74764 us   (~75 ms)
```

Die max. Loop-Dauer liegt **konstant bei ~75 ms** und ist damit extrem stabil.

## Ursache

Die ~75 ms stammen vom **kompletten Canvas-Neuzeichnen**:

```
canvas->fillScreen(BLACK);   // 410×502 = 411 KB in PSRAM
drawWatchface() / drawApp(); // Zeichnen
canvas->flush();             // 411 KB PSRAM -> Display (QSPI)
```

Effektive Übertragungsrate: `411.640 Bytes / 0,075 s ≈ 5,5 MB/s`.
Für QSPI @ 80 MHz ist das ungewöhnlich niedrig → im CO5300-Treiber steckt
noch deutlicher Overhead.

## Auswirkung auf das Touch-Gefühl

- Watchface (Screen 0): Neuzeichnen **1×/s** → 1×/s ein 75-ms-Block.
- Live-Screens (1, 7, 8, 9): Neuzeichnen **5 Hz** (200 ms) → alle 200 ms
  75 ms blockiert = **~37 % der Zeit**.
- **Jeder Tipp** setzt `redrawNow` → sofortiges 75-ms-Neuzeichnen.
- Touch wird nur alle ~33 ms gepollt (30 Hz).

Worst-Case pro Tipp: **~33 ms (Poll) + ~75 ms (Redraw) ≈ 100 ms**
Reaktionsverzögerung. Das ist die Ursache für das „zähe, sporadische“
Touch-Verhalten.

## Weitere Beobachtungen (Serienbefehle)

| Befehl  | Antwort |
|---------|---------|
| `SENSOR`| `Sensor AUS` |
| `BATT`  | `4,118 V, gauge=100 %, spannung=93 %, entlaedt, Akku verbunden` |
| `CALIB` | `N=(-0.110 0.014 -0.994) H=(0.029 -0.844 -0.535) enterHoch=0.169 seitCalib=6` |
| `STATS` | `total=2 session=0 drange=0 richtig=0 falsch=0 view=0 klasse=1 p(meldung)=0 %` |

Hinweise:

- **Sensor ist AUS** → deshalb keine `[dbg]`-Zeilen und kein
  `[perf] CNN avg` im Log. Der Erkennungspfad läuft aktuell nicht.
- Akku gesund (spannungsbasiert 93 %, 4,118 V).
- Kalibrierung vorhanden (`enterHoch=0.169`).
- 2 Meldungen heute.

## Fazit

Der Canvas-only-Stand läuft stabil, aber die **~75 ms Vollbild-Flush**
bleiben der Flaschenhals. Die PR-Verbesserungen
(5 Hz für Live-Screens, BLE-Befehle in `loop()`, `vibrate()` ohne `delay()`)
sind wirksam, lösen aber das Flush-Problem nicht.

## Empfehlungen (nächste Schritte)

1. **Teil-Redraw / Dirty-Rectangle** im Canvas-Modus (nur geänderte Bereiche
   flushen) – größter Effekt, etwas Umbau.
2. **Flush beschleunigen prüfen** (CO5300-Treiber / QSPI-Transfer).
3. **Sensor einschalten** (`SENSOR ON`) und anschließend `[perf] CNN avg` +
   `[dbg]` auswerten, um den Erkennungspfad zu beurteilen.
