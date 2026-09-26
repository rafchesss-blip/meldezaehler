# Firmware-Backup (Werks-Firmware der Uhr)

Diese Datei ist die **originale Werks-Firmware** (ESP-Brookesia „Phone"-Demo)
der Waveshare ESP32-S3-Touch-AMOLED-2.06. Sie wurde vor dem Flashen der
Meldezähler-App gesichert.

## Zurücksetzen auf die Werks-Firmware

```bash
esptool.py --port /dev/ttyACM0 write_flash 0x0 ESP32-S3-Touch-AMOLED-2.06-xiaozhi-251104.bin
```

Das überschreibt die Meldezähler-App wieder mit der ursprünglichen
„Phone"-Demo (inkl. aller dort enthaltenen Apps).

Quelle: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06 (FirmWare/)
