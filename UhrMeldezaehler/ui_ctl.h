#pragma once
// ---------------------------------------------------------------------------
// Bedien-Logik zwischen Oberfläche und Kernlogik: Navigation (Zurück/Power),
// Timer, Stoppuhr, Meldungs-Nachbearbeitung, Einstellungen.
// Die Masken (ui_*.h) ändern Zustand nur über diese Funktionen bzw. die
// Kernfunktionen (registerMeldung, setSensorOn, …).
// ---------------------------------------------------------------------------

// Vorabdeklarationen aus ui.h (modale Masken)
static void uiMessage(const char *icon, lv_color_t c, const char *title, const char *sub);
static void spOeffnen();   // ui_spiel.h

// Eltern-Hierarchie: Zifferblatt <- Apps <- {Melden, Einstellungen, Zeit,
// Aufnahme, Test}; Zifferblatt <- Auswahl / Meldungen bearbeiten.
// Zurück-Knopf und Power-Taste verhalten sich gleich.
static void powerBack() {
  if (alarmActive) {
    alarmStop();
    zeitTab = 0;
    timerRemainingMs = timerSetMs;
    screen = 4;
    return;
  }
  switch (screen) {
    case 0:
      enterStandby();
      break;
    case 6:
      if (meldeEditMode > 0) meldeEditMode--;
      else screen = 0;
      break;
    case 9:
      if (sensorRec) stopSensorRec();
      screen = 4;
      break;
    case 1: case 2: case 7: case 8: case 11: case SCREEN_SPIEL: case SCREEN_AUSLOESER:
      screen = 4;
      break;
    default:   // 3 Auswahl, 4 Apps
      screen = 0;
      break;
  }
}

// App aus dem Apps-Menü öffnen
static void ctlOpen(int s) {
  if (s == 1) {
    bufHead = bufCount = 0;   // Erkennung frisch beginnen (wie bisher beim Öffnen)
  } else if (s == 7) {
    zeitTab = 0;
  }
  screen = s;
  if (s == SCREEN_SPIEL) spOeffnen();   // „gerade“ neu = jetzige Haltung
}

static void ctlSelectWatchface(int i) {
  if (i < 0 || i > 5) return;
  watchface = i;
  prefs.putInt("wf", i);
  screen = 0;
}

// Letzte Meldung wurde drangenommen -> weiter zu „Antwort richtig/falsch“
static void ctlMarkDrange() {
  drange++;
  saveMeldeExtras();
  meldeEditMode = 2;
  USBSerial.println("Drangenommen +1");
}

static void ctlMarkAnswer(bool ok) {
  if (ok) richtig++;
  else falsch++;
  saveMeldeExtras();
  meldeEditMode = 0;
  USBSerial.printf("%s +1\n", ok ? "Richtig" : "Falsch");
}

// Helligkeit sofort setzen; NVS-Schreiben nur, wenn save (Slider losgelassen)
static void ctlSetBrightness(int v, bool save) {
  if (v < 10) v = 10;
  if (v > 255) v = 255;
  brightness = v;
  halSetBrightness((uint8_t)v);
  if (save) prefs.putInt("bright", brightness);
}

// --- Timer -----------------------------------------------------------------
static void ctlTimerSet(unsigned long ms) {
  if (timerRunning) return;
  if (ms > 23UL * 3600000UL + 59UL * 60000UL + 59000UL) ms = 23UL * 3600000UL + 59UL * 60000UL + 59000UL;
  timerSetMs = ms;
  timerRemainingMs = ms;
}

static void ctlTimerToggle() {
  if (timerRunning) {
    timerRunning = false;
    return;
  }
  if (timerRemainingMs == 0) timerRemainingMs = timerSetMs;
  if (timerRemainingMs == 0) return;   // 00:00 eingestellt -> nichts zu tun
  timerRunning = true;
  timerLastMs = millis();
}

static void ctlTimerReset() {
  timerRunning = false;
  timerRemainingMs = timerSetMs;
}

static void ctlAlarmStop() {
  alarmStop();
  zeitTab = 0;
  timerRemainingMs = timerSetMs;
}

// --- Stoppuhr --------------------------------------------------------------
static unsigned long ctlStopwatchMs() {
  return stopwatchBaseMs + (stopwatchRunning ? millis() - stopwatchStartMs : 0);
}

static void ctlStopwatchToggle() {
  if (stopwatchRunning) {
    stopwatchBaseMs += millis() - stopwatchStartMs;
    stopwatchRunning = false;
  } else {
    stopwatchStartMs = millis();
    stopwatchRunning = true;
  }
}

static void ctlStopwatchReset() {
  stopwatchRunning = false;
  stopwatchBaseMs = 0;
}

// --- Kalibrierung ----------------------------------------------------------
// Die Kalibrierung blockiert ~20–60 s. Aus einem Touch-Ereignis heraus (also
// mitten in lv_timer_handler) wird sie deshalb nur vorgemerkt und in loop()
// über ctlProcessPending() gestartet.
static int ctlPendingCalib = -1;
static bool ctlPendingClear = false;

static void ctlRequestCalibration(int part) { ctlPendingCalib = part; }
static void ctlRequestCalibClear() { ctlPendingClear = true; }

static void ctlProcessPending() {
  if (ctlPendingClear) {
    ctlPendingClear = false;
    clearCalibration();
    uiMessage(LV_SYMBOL_REFRESH, lv_color_hex(0xFF453A), "Neustart", "Kalibrierung gelöscht.");
    delay(800);
    ESP.restart();
  }
  if (goalCelebratePending) {
    goalCelebratePending = false;
    if (halOk && goalReachedLesson >= 0) {
      // Die Melde-Vibration erst zu Ende laufen lassen: während der Anzeige
      // läuft loop() nicht und könnte den Motor nicht abschalten
      while (vibPhaseOn || vibPulsesLeft > 0) {
        updateVibration();
        delay(5);
      }
      const Period &pr = ttDays[goalReachedLesson / MAX_PERIODS][goalReachedLesson % MAX_PERIODS];
      bool wasStandby = standby;
      if (wasStandby) wakeFromStandby();   // auch bei ausgeschaltetem Display kurz zeigen
      char sub[48];
      snprintf(sub, sizeof(sub), "%s  %d/%d", pr.name, sessionCount, pr.goal);
      uiMessage(LV_SYMBOL_OK, lv_color_hex(0x30D158), "Ziel erreicht!", sub);
      vibrateBlocking(150);                // Doppelpuls zur Bestätigung
      delay(120);
      vibrateBlocking(150);
      delay(2000 - 420);                   // zusammen ≈ 2 s
      if (wasStandby) enterStandby();
    }
  }
  if (ctlPendingCalib >= 0) {
    int part = ctlPendingCalib;
    ctlPendingCalib = -1;
    if (runCalibrationPart(part)) {   // false = per Power-Taste abgebrochen
      uiMessage(LV_SYMBOL_OK, lv_color_hex(0x30D158), "Gespeichert", "Kalibrierung abgeschlossen.");
      delay(1500);
    }
    screen = 1;
    view = 2;
  }
}
