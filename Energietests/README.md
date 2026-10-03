# Energietests

Eigenständige Test-Programme, die den Energiebedarf der Uhr (Waveshare
ESP32-S3-Touch-AMOLED-2.06) pro Komponente bestimmen. Jedes Programm ersetzt
für die Dauer des Tests die Meldezähler-Firmware; danach wird diese wieder
geflasht. NVS (Einstellungen, Kalibrierung) bleibt dabei erhalten, solange
dasselbe Partitionsschema (`huge_app`) verwendet wird.

| Test | Ordner | misst |
|---|---|---|
| 1 | `Test1_Grundbedarf/` | Grundbedarf des Boards: alles aus, ESP32 im Deep-Sleep |

Bauen und flashen (Port anpassen):

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=disabled Energietests/Test1_Grundbedarf
arduino-cli upload  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=disabled -p COM6 Energietests/Test1_Grundbedarf
```

Zurück zur Meldezähler-Firmware: `./build.sh firmware` (bzw. `arduino-cli`
mit `PSRAM=opi`, siehe `build.sh`).
