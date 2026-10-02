#include "UhrMeldezaehler_core.h"
#include "ui_ctl.h"
#include "ui.h"

void setup() {
  // Deep-Sleep-Timer-Wakeup: nur prüfen, ob die Power-Taste gedrückt wurde.
  // Wenn nicht, sofort weiterschlafen (spart Strom, nur RTC läuft weiter).
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    Wire.begin(IIC_SDA, IIC_SCL);
    if (!peekPowerKeyIrq()) {
      esp_sleep_enable_timer_wakeup(3000000ULL);
      esp_deep_sleep_start();   // kehrt nicht zurück
    }
  }

  USBSerial.begin(115200);
  delay(300);
  USBSerial.println("\n=== MELDEZAEHLER Uhr-App ===");

  // Akku sparen: WLAN aus, CPU-Takt senken (BLE/Display/Sensor laufen damit problemlos)
  WiFi.mode(WIFI_OFF);
  setCpuFrequencyMhz(160);

  Wire.begin(IIC_SDA, IIC_SCL);

  // PMU (Akku) – begin() setzt Wire ggf. neu auf, daher Clock erst danach
  bool pmuOk = pmu.begin(Wire, PMU_ADDR, IIC_SDA, IIC_SCL);
  if (pmuOk) {
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);  // Power-Taste: kurzer Druck
    pmu.setChargeTargetVoltage(3);
    pmu.clearIrqStatus();
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.enableSystemVoltageMeasure();
    // Vibrationsmotor-Versorgung (DCDC4/LX4) einschalten, sonst hat der
    // Motor trotz gesetztem GPIO18 keine Spannung.
    pmu.enableDC4();
    pmu.setDC4Voltage(1800);
  }
  USBSerial.printf("PMU AXP2101: %s\n", pmuOk ? "OK" : "FEHLER");
  Wire.setClock(400000);

  // Display (LVGL) + Touch
  if (!halDisplayInit()) {
    USBSerial.println("Display init fehlgeschlagen!");
  }
  touchInit();
  if (halOk) uiInit();

  // IMU
  bool imuOk = qmiInit();
  USBSerial.printf("QMI8658: %s\n", imuOk ? "OK" : "FEHLER");
  if (!imuOk) {
    halSetBrightness(200);
    if (halOk) uiFatal("Sensorfehler", "Bewegungssensor QMI8658 nicht gefunden.");
    while (1) delay(1000);
  }

  // Tasten
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  // NVS laden
  prefs.begin("melde", false);
  brightness = prefs.getInt("bright", 208);
  watchface = prefs.getInt("wf", 0);
  if (watchface < 0 || watchface > 5) watchface = 0;
  sensorOn = prefs.getInt("sensorOn", 1) == 1;
  recTrialCounter = prefs.getInt("recTrial", 1000);
  motorOn = prefs.getInt("motorOn", 1) == 1;
  muteInLessons = prefs.getInt("muteLessons", 0) == 1;
  lastSession = prefs.getInt("lastSession", 0);
  // Erst ein Bild ins Panel, dann Licht an – sonst ist kurz der alte Panel-Speicher zu sehen
  if (halOk) {
    uiSync();
    lv_refr_now(nullptr);
  }
  halSetBrightness(brightness);

  // Stundenplan + Stunden-Statistik aus NVS laden
  loadTimetable();
  loadLessonStats();

  drange = prefs.getInt("drange", 0);
  richtig = prefs.getInt("richtig", 0);
  falsch = prefs.getInt("falsch", 0);
  meldeZeitMs = prefs.getULong("meldezeit", 0);

  meldungenSeitCalib = prefs.getInt("seitCalib", 0);
  tischCalibrated = prefs.getInt("calibT", 0) == 1;
  T_dir = {prefs.getFloat("Tx", 0), prefs.getFloat("Ty", 0), prefs.getFloat("Tz", 1)};
  T_dir = vnorm(T_dir);
  if (prefs.getInt("calib", 0) == 1) {
    N_dir = {prefs.getFloat("Nx", 0), prefs.getFloat("Ny", 0), prefs.getFloat("Nz", 1)};
    H_dir = {prefs.getFloat("Hx", 0), prefs.getFloat("Hy", 0), prefs.getFloat("Hz", 1)};
    N_dir = vnorm(N_dir);
    H_dir = vnorm(H_dir);
    Vec3 T = {REF_TISCH[0], REF_TISCH[1], REF_TISCH[2]};
    Vec3 M = {REF_MELDUNG[0], REF_MELDUNG[1], REF_MELDUNG[2]};
    Vec3 sourceTisch = tischCalibrated ? T_dir : N_dir;
    buildRotation(sourceTisch, H_dir, T, M, R_model);
    calibrated = true;
    // Schwelle immer aus der gespeicherten Geometrie neu berechnen (sichert gegen alte Fehlwerte)
    enterHoch = computeEnterHoch();
    prefs.putFloat("enterHoch", enterHoch);
    USBSerial.printf("Kalibrierung aus NVS geladen. enterHoch=%.3f\n", enterHoch);
  } else {
    // Erst-Kalibrierung: erst tragen lassen, dann auf Tipp warten
    if (halOk) uiFirstBoot();
    USBSerial.println("Warte auf Tipp zum Kalibrieren (oder Befehl CAL) ...");
    waitForTap();
    runCalibration();
  }

  // Tageszähler laden + ggf. Tageswechsel
  RTC_Time t;
  int today = -1;
  if (rtcRead(t)) today = t.day;
  int storedDay = prefs.getInt("day", -1);
  totalHeute = prefs.getInt("total", 0);
  if (today != -1 && today != storedDay) {
    // neuer Tag -> zurücksetzen
    totalHeute = 0;
    meldeZeitMs = 0;
    drange = 0;
    richtig = 0;
    falsch = 0;
    meldLogCount = 0;
    meldLogWrite = 0;
    prefs.putInt("total", 0);
    prefs.putULong("meldezeit", 0);
    prefs.putInt("drange", 0);
    prefs.putInt("richtig", 0);
    prefs.putInt("falsch", 0);
    prefs.putInt("day", today);
    USBSerial.println("Neuer Tag -> Tageszaehler zurueckgesetzt.");
  } else {
    prefs.putInt("day", today);
  }

  minuteStartMs = millis();
  nextSampleUs = micros();

  // Bluetooth (BLE): gespeicherten Zustand wiederherstellen (Standard: an)
  if (prefs.getInt("btOn", 1) == 1) {
    btEnable();
  } else {
    btOn = false;
  }

  USBSerial.println("Bereit! Meldebewegung machen.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  unsigned long loopStartUs = micros();
  unsigned long nowUs = micros();
  unsigned long nowMs = millis();

  // 1) Sensor: Streaming / SD-Aufnahme / normale Erkennung @100 Hz
  //    (läuft auch im Standby weiter, damit Meldungen weiter gezählt werden)
  if (streamMode) {
    int catches = 0;
    while ((long)(nowUs - nextStreamUs) >= 0 && catches < 5) {
      nextStreamUs += 10000;
      streamSample();
      catches++;
    }
    if (catches >= 5) nextStreamUs = nowUs;  // zu weit hinten -> neu aufsetzen
  } else if (sensorRec) {
    int catches = 0;
    while ((long)(nowUs - sensorRecNextUs) >= 0 && catches < 5) {
      sensorRecNextUs += 10000;
      sensorRecSample();
      catches++;
    }
    if (catches >= 5) sensorRecNextUs = nowUs;  // zu weit hinten -> neu aufsetzen
  } else if (sensorOn) {
    int catches = 0;
    while ((long)(nowUs - nextSampleUs) >= 0 && catches < 5) {
      nextSampleUs += 10000;
      readAndStoreSample();
      if (++sampleCounter >= AUSWERT_N) {
        sampleCounter = 0;
        evaluate();
      }
      catches++;
    }
    if (catches >= 5) nextSampleUs = nowUs;  // zu weit hinten -> neu aufsetzen
  }

  // 2) Minuten-Statistik rollieren
  unsigned long minuteElapsed = (nowMs - minuteStartMs) / 60000UL;
  if (minuteElapsed >= 1) {
    for (unsigned long i = 0; i < minuteElapsed; i++) {
      minHist[minHistIdx] = minuteCount;
      minHistIdx = (minHistIdx + 1) % 60;
      minuteCount = 0;
    }
    minuteStartMs += minuteElapsed * 60000UL;
  }

  // 3) Uhrzeit, Akku, Stunde, BLE-Werte
  updateEnv();

  // 4) Oberfläche: Navigation + LVGL (Touch lesen, geänderte Flächen zeichnen).
  //    Im Standby/Streaming ruht die Oberfläche; Touch wird dann nicht gelesen.
  ctlProcessPending();
  static unsigned long uiMaxUs = 0;
  if (halOk && !standby && !streamMode) {
    unsigned long u0 = micros();
    uiSync();
    lv_timer_handler();
    unsigned long du = micros() - u0;
    if (du > uiMaxUs) uiMaxUs = du;
  }

  // 4b) BLE-Befehle aus der App abarbeiten (im Bluetooth-Task nur eingereiht)
  processBleCommands();

  // 5) Tasten (Power + Boot)
  handleButtons();

  // 6) Zeit-App (Timer/Stoppuhr) aktualisieren
  updateZeitApp();

  // 6b) Wecker-Vibrationsmuster (läuft, bis der Alarm gestoppt wird)
  updateAlarm();

  // 6c) Vibrations-Pulse von vibrate() abschalten/fortsetzen
  updateVibration();

  // 7) Serielle Befehle
  handleSerial();

  // Performance-Diagnose: max. Loop-Dauer alle 5 s ausgeben
  {
    static unsigned long loopMaxUs = 0;
    static unsigned long loopReportMs = 0;
    unsigned long d = micros() - loopStartUs;
    if (d > loopMaxUs) loopMaxUs = d;
    if (nowMs - loopReportMs > 5000) {
      loopReportMs = nowMs;
      USBSerial.printf("[perf] max loop=%lu us  max ui=%lu us\n", loopMaxUs, uiMaxUs);
      loopMaxUs = 0;
      uiMaxUs = 0;
    }
  }
}
