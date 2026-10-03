#pragma once
// ---------------------------------------------------------------------------
// Akku-Test: Die Uhr läuft eine eingestellte Zeit mit gewählten Verbrauchern
// (Sensor/Erkennung, Display, Vibration) und simuliert Meldungen. Gemessen
// wird die Akkuspannung (der AXP2101 kann keinen Strom messen); aus Start- und
// Endwert ergibt sich der Verbrauch in % pro Stunde. Vergleicht man Läufe, die
// sich nur in einem Verbraucher unterscheiden, sieht man dessen Anteil.
//
// Ablauf: Einstellen -> Start -> (USB abziehen) -> Lauf -> Ergebnis.
// Abbrechen: Power- oder BOOT-Taste. Der Touch ist während des Laufs gesperrt,
// damit nichts verstellt wird; mit Display an blättert die Uhr selbst durch
// Masken, die nichts verändern.
//
// Speicher: Ergebnisse in NVS (letzte AK_HIST), Minutenwerte auf der SD-Karte
// (/akkutest.csv). Ausgabe: seriell (AKKU, AKKULOG), BLE (JSON), Uhr-App.
// ---------------------------------------------------------------------------

#define AK_HIST 8

struct AkConfig {
  uint16_t minutes;
  uint16_t sims;            // simulierte Meldungen über die ganze Dauer
  bool sensor, display, motor;
};

struct AkResult {           // wird binär in NVS gespeichert – Layout nicht ändern
  uint32_t id;
  uint16_t plannedMin, minutes, sims;
  uint16_t mvStart, mvEnd;
  int8_t pctStart, pctEnd;
  uint8_t sensor, display, motor, bt, aborted, pad;
};

enum { AK_IDLE, AK_WAIT_USB, AK_RUN };

static AkConfig akCfg = {60, 10, true, true, true};
static int akState = AK_IDLE;
static AkResult akHist[AK_HIST];   // [0] = neuestes Ergebnis
static int akHistN = 0;
static AkResult akCur;

static unsigned long akStartMs, akLastSampleMs, akMinuteMs, akNextSimMs, akNextDemoMs;
static uint32_t akMvSum = 0;       // Mittelwert der laufenden Minute
static uint16_t akMvN = 0, akMvLast = 0, akSimsDone = 0, akDemoIdx = 0;
static bool akFirstMinute = true;
static bool akSavedSensor, akSavedMotor, akSavedMute;

static unsigned long akDurationMs() { return (unsigned long)akCfg.minutes * 60000UL; }
static bool akUsbIn() { return pmu.isVbusIn(); }

// --- Speicher ---------------------------------------------------------------
static void akLoad() {
  akHistN = 0;
  size_t n = prefs.getBytesLength("akHist");
  if (n % sizeof(AkResult) == 0 && n <= sizeof(akHist)) {
    prefs.getBytes("akHist", akHist, n);
    akHistN = n / sizeof(AkResult);
  }
  akCfg.minutes = prefs.getUShort("akMin", 60);
  akCfg.sims = prefs.getUShort("akSims", 10);
  uint8_t f = prefs.getUChar("akFlags", 7);
  akCfg.sensor = f & 1;
  akCfg.display = f & 2;
  akCfg.motor = f & 4;
  // Lief beim Ausschalten ein Test (z. B. Akku leer)? Zwischenstand übernehmen.
  if (prefs.getBytesLength("akRun") == sizeof(AkResult)) {
    AkResult r;
    prefs.getBytes("akRun", &r, sizeof(r));
    prefs.remove("akRun");
    r.aborted = 2;   // 2 = Uhr ging während des Tests aus
    memmove(&akHist[1], &akHist[0], sizeof(AkResult) * (AK_HIST - 1));
    akHist[0] = r;
    if (akHistN < AK_HIST) akHistN++;
    prefs.putBytes("akHist", akHist, sizeof(AkResult) * akHistN);
    USBSerial.println("[akku] Unterbrochenen Test als Ergebnis übernommen");
  }
}

static void akSaveConfig() {
  prefs.putUShort("akMin", akCfg.minutes);
  prefs.putUShort("akSims", akCfg.sims);
  prefs.putUChar("akFlags", (akCfg.sensor ? 1 : 0) | (akCfg.display ? 2 : 0) | (akCfg.motor ? 4 : 0));
}

static String akJson() {
  String s = "{\"tests\":[";
  for (int i = 0; i < akHistN; i++) {
    const AkResult &r = akHist[i];
    if (i) s += ",";
    s += "{\"id\":" + String(r.id) + ",\"min\":" + String(r.minutes) + ",\"plan\":" + String(r.plannedMin) +
         ",\"s\":" + String(r.sensor) + ",\"d\":" + String(r.display) + ",\"m\":" + String(r.motor) +
         ",\"bt\":" + String(r.bt) + ",\"sims\":" + String(r.sims) + ",\"mv0\":" + String(r.mvStart) +
         ",\"mv1\":" + String(r.mvEnd) + ",\"p0\":" + String(r.pctStart) + ",\"p1\":" + String(r.pctEnd) +
         ",\"ab\":" + String(r.aborted) + "}";
  }
  s += "]}";
  return s;
}

// Aussagekräftig erst ab 5 min (Spannung schwankt, Prozent sind ganzzahlig)
static bool akValid(const AkResult &r) { return r.minutes >= 5 && r.pctStart >= 0 && r.pctEnd >= 0; }

// Verbrauch in Prozentpunkten pro Stunde (negativ = Akku stieg, also geladen)
static float akPctPerHour(const AkResult &r) {
  return (r.pctStart - r.pctEnd) * 60.0f / (r.minutes ? r.minutes : 1);
}

static void akLogLine(const char *fmt, ...) {
  if (!ensureSd()) return;
  File f = SD_MMC.open("/akkutest.csv", FILE_APPEND);
  if (!f) return;
  if (f.size() == 0) f.println("test,minute,mv,pct,sensor,display,motor,bt,sims,usb");
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  f.println(buf);
  f.close();
}

// --- Ablauf -----------------------------------------------------------------
static void akBegin() {
  akState = AK_RUN;
  akBusy = true;
  akSavedSensor = sensorOn;
  akSavedMotor = motorOn;
  akSavedMute = muteInLessons;
  sensorApply(akCfg.sensor);
  motorOn = akCfg.motor;          // nur im RAM – wird am Ende wiederhergestellt
  muteInLessons = false;

  memset(&akCur, 0, sizeof(akCur));
  akCur.id = prefs.getUInt("akId", 0) + 1;
  prefs.putUInt("akId", akCur.id);
  akCur.plannedMin = akCfg.minutes;
  akCur.sims = 0;
  akCur.sensor = akCfg.sensor;
  akCur.display = akCfg.display;
  akCur.motor = akCfg.motor;
  akCur.bt = btOn;
  akCur.pctStart = akCur.pctEnd = -1;

  unsigned long now = millis();
  akStartMs = akMinuteMs = akLastSampleMs = now;
  akMvSum = akMvN = 0;
  akFirstMinute = true;
  akSimsDone = 0;
  akNextSimMs = akCfg.sims ? now + akDurationMs() / (akCfg.sims + 1) : 0;
  akNextDemoMs = now + 8000;
  akDemoIdx = 0;

  if (!akCfg.display) {
    standby = true;               // Oberfläche ruht wie im Standby
    halSetBrightness(0);
    halDisplayPower(false);
  }
  USBSerial.printf("[akku] Test %lu gestartet: %u min, Sensor=%d Display=%d Motor=%d BT=%d, %u Meldungen\n",
                   (unsigned long)akCur.id, akCfg.minutes, akCfg.sensor, akCfg.display, akCfg.motor, btOn,
                   akCfg.sims);
}

static void akFinish(int aborted) {
  akCur.minutes = (millis() - akStartMs) / 60000UL;
  akCur.sims = akSimsDone;
  akCur.aborted = aborted;
  // angefangene letzte Minute nur, wenn genug Messwerte; sonst Mittel der letzten vollen Minute
  if (akMvN >= 10) akCur.mvEnd = akMvSum / akMvN;
  else if (!akCur.mvEnd) akCur.mvEnd = akMvLast;
  akCur.pctEnd = battPctFromVoltage(akCur.mvEnd);
  if (!akCur.mvStart) {                        // kürzer als eine Minute
    akCur.mvStart = akCur.mvEnd;
    akCur.pctStart = akCur.pctEnd;
  }

  memmove(&akHist[1], &akHist[0], sizeof(AkResult) * (AK_HIST - 1));
  akHist[0] = akCur;
  if (akHistN < AK_HIST) akHistN++;
  prefs.putBytes("akHist", akHist, sizeof(AkResult) * akHistN);
  prefs.remove("akRun");
  if (pCharAkku) pCharAkku->setValue(akJson().c_str());

  sensorApply(akSavedSensor);
  motorOn = akSavedMotor;
  muteInLessons = akSavedMute;
  akState = AK_IDLE;
  akBusy = false;
  if (standby) wakeFromStandby();
  screen = 11;
  USBSerial.printf("[akku] Test %lu %s: %u min, %u -> %u mV, %d -> %d %%, %.1f %%/h\n", (unsigned long)akCur.id,
                   aborted ? "abgebrochen" : "fertig", akCur.minutes, akCur.mvStart, akCur.mvEnd, akCur.pctStart,
                   akCur.pctEnd, akPctPerHour(akCur));
}

static void akStart() {
  if (akState != AK_IDLE || akCfg.minutes == 0) return;
  akSaveConfig();
  if (akUsbIn()) {
    akState = AK_WAIT_USB;        // beim Laden wäre die Messung wertlos
    akBusy = true;
    USBSerial.println("[akku] Warte, bis USB abgezogen ist ...");
  } else {
    akBegin();
  }
}

static void akCancelWait() {
  if (akState != AK_WAIT_USB) return;
  akState = AK_IDLE;
  akBusy = false;
}

// Power-/BOOT-Taste während des Tests: abbrechen (true = Taste verbraucht)
static bool akButton() {
  if (akState == AK_RUN) {
    akFinish(1);
    return true;
  }
  if (akState == AK_WAIT_USB) {
    akCancelWait();
    return true;
  }
  return false;
}

// Simulierte Meldung: wie eine echte speichern (NVS) und vibrieren, aber nicht
// in die Tagesstatistik zählen
static void akSimMeldung() {
  akSimsDone++;
  prefs.putUShort("akSimN", akSimsDone);
  if (akCfg.motor) vibrate(120);
}

// Display an: alle 8 s eine Maske weiter – nur Anzeigen, nichts Verstellbares
static void akDemoStep() {
  static const int8_t SEQ[][2] = {{0, 0}, {1, 0}, {1, 1}, {4, 0}, {7, 0}, {11, 0}};
  akDemoIdx = (akDemoIdx + 1) % (sizeof(SEQ) / sizeof(SEQ[0]));
  screen = SEQ[akDemoIdx][0];
  view = SEQ[akDemoIdx][1];
  zeitTab = 0;
}

// In loop() aufrufen
static void akUpdate() {
  unsigned long now = millis();
  if (akState == AK_WAIT_USB) {
    if (!akUsbIn()) akBegin();
    return;
  }
  if (akState != AK_RUN) return;

  // Spannung 1x/s, gemittelt pro Minute (unter Last schwankt sie)
  if (now - akLastSampleMs >= 1000) {
    akLastSampleMs = now;
    uint16_t mv = pmu.getBattVoltage();
    if (mv > 2500) {
      akMvLast = mv;
      akMvSum += mv;
      akMvN++;
    }
  }
  if (now - akMinuteMs >= 60000UL) {
    akMinuteMs += 60000UL;
    uint16_t avg = akMvN ? akMvSum / akMvN : akMvLast;
    akMvSum = akMvN = 0;
    if (akFirstMinute) {
      akFirstMinute = false;
      akCur.mvStart = avg;
      akCur.pctStart = battPctFromVoltage(avg);
    }
    akCur.mvEnd = avg;
    akCur.pctEnd = battPctFromVoltage(avg);
    akCur.minutes = (now - akStartMs) / 60000UL;
    akCur.sims = akSimsDone;
    bool usb = akUsbIn();
    akLogLine("%lu,%u,%u,%d,%d,%d,%d,%d,%u,%d", (unsigned long)akCur.id, akCur.minutes, avg, akCur.pctEnd,
              akCfg.sensor, akCfg.display, akCfg.motor, btOn, akSimsDone, usb);
    if (akCur.minutes % 5 == 0) prefs.putBytes("akRun", &akCur, sizeof(akCur));   // falls der Akku leer wird
    USBSerial.printf("[akku] Minute %u: %u mV (%d %%)%s\n", akCur.minutes, avg, akCur.pctEnd,
                     usb ? "  USB angesteckt!" : "");
  }

  if (akNextSimMs && akSimsDone < akCfg.sims && (long)(now - akNextSimMs) >= 0) {
    akSimMeldung();
    akNextSimMs = akStartMs + (unsigned long)((uint64_t)akDurationMs() * (akSimsDone + 1) / (akCfg.sims + 1));
  }
  if (akCfg.display && (long)(now - akNextDemoMs) >= 0) {
    akNextDemoMs = now + 8000;
    akDemoStep();
  }
  if (now - akStartMs >= akDurationMs()) akFinish(0);
}

// --- Serielle Befehle: AKKU (Ergebnisse als CSV), AKKULOG (Minutenwerte) ----
static void akSerial(const String &line) {
  // AKKUSTART min meldungen sensor display motor [usb] – "usb" startet auch am
  // Ladekabel (nur zum Prüfen des Ablaufs, die Messwerte sind dann wertlos)
  if (line.startsWith("AKKUSTART")) {
    unsigned m, n, s, d, v;
    if (sscanf(line.c_str(), "AKKUSTART %u %u %u %u %u", &m, &n, &s, &d, &v) != 5 || m == 0) {
      USBSerial.println("Syntax: AKKUSTART min meldungen sensor(0/1) display(0/1) motor(0/1) [usb]");
      return;
    }
    if (akState != AK_IDLE) { USBSerial.println("Test laeuft bereits"); return; }
    akCfg = {(uint16_t)m, (uint16_t)n, s != 0, d != 0, v != 0};
    if (line.endsWith(" usb")) {
      akSaveConfig();
      akBegin();
    } else {
      akStart();
    }
    screen = 11;
    return;
  }
  if (line == "AKKUCLEAR") {   // Ergebnisse und Minutenwerte löschen
    akHistN = 0;
    prefs.remove("akHist");
    if (ensureSd()) SD_MMC.remove("/akkutest.csv");
    if (pCharAkku) pCharAkku->setValue(akJson().c_str());
    USBSerial.println("OK Akku-Ergebnisse geloescht");
    return;
  }
  if (line == "AKKUSTOP") {
    if (!akButton()) USBSerial.println("Kein Akku-Test aktiv");
    return;
  }
  if (line == "AKKULOG") {
    if (!ensureSd()) { USBSerial.println("SD nicht bereit"); return; }
    File f = SD_MMC.open("/akkutest.csv", FILE_READ);
    if (!f) { USBSerial.println("kein /akkutest.csv"); return; }
    USBSerial.println("--- akkutest.csv ---");
    while (f.available()) {
      String l = f.readStringUntil('\n');
      while (USBSerial.availableForWrite() < (int)l.length() + 2) delay(2);   // Puffer nicht überlaufen lassen
      USBSerial.println(l);
    }
    f.close();
    USBSerial.println("--- ende ---");
    return;
  }
  USBSerial.println("--- akku ---");
  USBSerial.println("id,minuten,geplant,sensor,display,motor,bt,meldungen,mv_start,mv_ende,pct_start,pct_ende,pct_pro_h,abbruch");
  for (int i = 0; i < akHistN; i++) {
    const AkResult &r = akHist[i];
    char ph[12] = "";   // leer = zu kurz für eine Aussage
    if (akValid(r)) snprintf(ph, sizeof(ph), "%.1f", akPctPerHour(r));
    USBSerial.printf("%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%s,%u\n", (unsigned long)r.id, r.minutes, r.plannedMin,
                     r.sensor, r.display, r.motor, r.bt, r.sims, r.mvStart, r.mvEnd, r.pctStart, r.pctEnd, ph,
                     r.aborted);
  }
  if (akState == AK_RUN)
    USBSerial.printf("laeuft: Test %lu, %lu von %u min\n", (unsigned long)akCur.id,
                     (millis() - akStartMs) / 60000UL, akCfg.minutes);
  USBSerial.println("--- ende ---");
}
