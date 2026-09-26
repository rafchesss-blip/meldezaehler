#include "UhrMeldezaehler_core.h"

void setup() {
  USBSerial.begin(115200);
  delay(300);
  USBSerial.println("\n=== MELDEZAEHLER Uhr-App ===");

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

  // Display
  if (!canvas->begin()) {
    USBSerial.println("Display init fehlgeschlagen!");
  }
  canvas->fillScreen(BLACK);
  canvas->flush();

  // IMU
  bool imuOk = qmiInit();
  USBSerial.printf("QMI8658: %s\n", imuOk ? "OK" : "FEHLER");
  if (!imuOk) {
    canvas->fillScreen(BLACK);
    canvas->setTextSize(2);
    canvas->setTextColor(RED);
    canvas->setCursor(60, 220);
    canvas->print("QMI8658 nicht gefunden!");
    canvas->flush();
    while (1) delay(1000);
  }

  // Touch
  touchInit();

  // Tasten
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  // NVS laden
  prefs.begin("melde", false);
  brightness = prefs.getInt("bright", 208);
  watchface = prefs.getInt("wf", 0);
  if (watchface < 0 || watchface > 4) watchface = 0;
  sensorOn = prefs.getInt("sensorOn", 1) == 1;
  gfx->setBrightness(brightness);

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
    canvas->fillScreen(BLACK);
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE);
    canvas->setCursor(70, 200);
    canvas->print("Uhr anlegen, dann");
    canvas->setCursor(70, 240);
    canvas->print("auf das Display tippen");
    canvas->setTextColor(YELLOW);
    canvas->setCursor(90, 300);
    canvas->print("(Kalibrierung startet)");
    canvas->flush();
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

  // Bluetooth (BLE) automatisch aktivieren, damit die Handy-App die Uhr findet
  btEnable();

  // Audio (Rekorder): I2S + ES8311 + ES7210 initialisieren
  audioInit();

  USBSerial.println("Bereit! Meldebewegung machen.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  unsigned long nowUs = micros();
  unsigned long nowMs = millis();

  // 1) Sensor @100 Hz (nur wenn die Erkennung eingeschaltet ist)
  if (sensorOn) {
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

  // 3) Display @ ~15 Hz
  static unsigned long lastDrawMs = 0;
  if (nowMs - lastDrawMs >= 66) {
    lastDrawMs = nowMs;
    updateEnv();
    if (!standby) renderAndFlush();
  }

  // 4) Touch @ ~30 Hz
  static unsigned long lastTouchMs = 0;
  if (nowMs - lastTouchMs >= 30) {
    lastTouchMs = nowMs;
    if (!standby) handleTouch();
  }

  // 5) Tasten (Power + Boot)
  handleButtons();

  // 6) Zeit-App (Timer/Stoppuhr) aktualisieren
  updateZeitApp();

  // 7) Serielle Befehle
  handleSerial();
}
