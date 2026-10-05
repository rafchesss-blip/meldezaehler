# Energietest 4 – Motor

Misst den Vibrationsmotor samt seiner Versorgung in **einem** Lauf. Alles aus
wie in Test 1 (Display Sleep In, Touch Hibernate, **Bewegungssensor aus**,
kein Funk), aber:

- Motorversorgung **DC4 dauerhaft an (1,8 V)**, wie die Firmware sie in
  `setup()` einschaltet
- Motor (GPIO18) pulst **120 ms an, alle 1000 ms** (= `vibrate(120)` einer
  Meldung, ≈ 3600 Pulse/h); die Pulse werden gezählt
- ESP32 im Light-Sleep (80 MHz), wacht nur zum Schalten des Motors und alle
  0,5 s für Taste/USB auf (Deep-Sleep ginge nicht: jedes Wecken wäre ein
  Neustart)

**Auswertung:** Abfall minus Grundlast = Motor gesamt (Leerlauf DC4 + Pulse).
Grundlast = Test 1 (≈ 5 mV/h, Deep-Sleep) plus Light-Sleep statt Deep-Sleep
(Datenblatt ≈ 0,24 mA statt ≈ 8 µA, nicht gemessen). Geteilt durch Pulse/h
ergibt das eine Obergrenze für die Kosten einer Meldung (der DC4-Leerlauf
steckt mit drin; er fällt in der Firmware ohnehin dauernd an).

Die Akkuspannung wird immer kurz vor einem Puls gemessen (Motor steht seit
≈ 880 ms), damit der Spannungseinbruch unter Last die Werte nicht verfälscht.
Beim Start gibt es einen Probepuls von 200 ms.

## Bedienung

Wie Test 3: Kabel abziehen startet bzw. setzt den Lauf fort, Power-Taste
zeigt 3 s den letzten Wert, unter 3,55 V endet der Lauf (DC4 wird dann
abgeschaltet). Werte im RTC-RAM und im Flash (`/test4.bin`, `/test4.cfg`).

| Befehl (115200 Baud, am Kabel) | |
|---|---|
| `DUMP` | Kopfzeile (Puls, Takt, Pulse) + CSV `nr,zeit,stunden,mv,streuung_mv,grund,usb,laedt` |
| `STAT` | Kurzfassung inkl. Pulse, DC4-Zustand/-Spannung, QMI8658-Register (CTRL7 00 = aus) |
| `PULS n` | Pulsdauer in ms (Standard 120) |
| `TAKT n` | Pulsabstand in ms (Standard 1000) |
| `CLEAR`, `INTERVALL n`, `LAUF`, `STROMAUSFALL` | wie Test 3 |
