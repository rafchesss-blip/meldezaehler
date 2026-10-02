#pragma once
// ---------------------------------------------------------------------------
// Stub der Kernlogik für den Host-Simulator: dieselben Namen und Typen wie in
// UhrMeldezaehler_core.h, damit ui_ctl.h / ui*.h unverändert übersetzen.
// Aufrufe werden in simLog protokolliert, damit Abläufe prüfbar sind.
// ---------------------------------------------------------------------------
#include <lvgl.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<std::string> simLog;
static void simCall(const char *fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  simLog.push_back(b);
}

// --- Arduino-Ersatz ---------------------------------------------------------
static uint32_t simMs = 0;
static uint32_t millis() { return simMs; }
static void delay(uint32_t ms) { simMs += ms; }

struct String {
  std::string s;
  String(const char *c = "") : s(c) {}
  const char *c_str() const { return s.c_str(); }
};

struct SimSerial {
  void println(const char *t) { simCall("serial: %s", t); }
  void printf(const char *fmt, ...) {}
} USBSerial;

struct SimPrefs {
  void putInt(const char *k, int v) { simCall("prefs.putInt(%s,%d)", k, v); }
} prefs;

struct SimEsp {
  void restart() { simCall("ESP.restart"); }
} ESP;

// --- Zustand (Werte für die Screenshots) ------------------------------------
int screen = 0, view = 0, meldeEditMode = 0, zeitTab = 0, watchface = 0;
int brightness = 208;
bool btOn = true, sensorOn = true, motorOn = true, muteInLessons = false;
bool standby = false, alarmActive = false;
int totalHeute = 7, sessionCount = 2, drange = 3, richtig = 2, falsch = 1;
int minHist[60] = {0};
int minHistIdx = 0;
unsigned long meldeZeitMs = 83000;
int meldungenSeitCalib = 4;
bool imHoch = false, calibrated = true, tischCalibrated = true;
int aktuellKlasse = 1;
float aktuellProb[2] = {0.03f, 0.97f};
unsigned long timerSetMs = 5UL * 60000UL, timerRemainingMs = 5UL * 60000UL, timerLastMs = 0;
bool timerRunning = false;
unsigned long stopwatchBaseMs = 0, stopwatchStartMs = 0;
bool stopwatchRunning = false;
bool sensorRec = false;
String sensorRecLabel("meldung");
unsigned sensorRecTrial = 1004;
unsigned long sensorRecCount = 0;
int bufHead = 0, bufCount = 0;

static int cachedH = 10, cachedM = 42, cachedS = 17;
static int cachedDay = 2, cachedMon = 10, cachedYr = 26, cachedPct = 76;
static int battMV = 3940;
static bool battCharging = false;

#define MAX_DAYS 7
#define MAX_PERIODS 12
struct Period {
  char name[12];
  uint8_t sh, sm, eh, em;
  bool active;
};
static Period ttDays[MAX_DAYS][MAX_PERIODS];
static uint8_t ttCount[MAX_DAYS];
static bool ttActive = true;

static int weekdayOf(int d, int m, int y) {
  static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static int findPeriod(int day, int h, int m) {
  int now = h * 60 + m;
  for (int p = 0; p < ttCount[day]; p++) {
    const Period &q = ttDays[day][p];
    if (q.active && now >= q.sh * 60 + q.sm && now < q.eh * 60 + q.em) return p;
  }
  return -1;
}
static int nextLessonIdx(int day, int h, int m) {
  int now = h * 60 + m;
  for (int p = 0; p < ttCount[day]; p++)
    if (ttDays[day][p].active && ttDays[day][p].sh * 60 + ttDays[day][p].sm > now) return p;
  return -1;
}

// --- Logik-Stubs ------------------------------------------------------------
static void resetAllStats() { simCall("resetAllStats"); totalHeute = 0; }
static void removeLastMeldung() { simCall("removeLastMeldung"); if (totalHeute > 0) totalHeute--; }
static void registerMeldung(unsigned long d = 0) { simCall("registerMeldung"); totalHeute++; }
static void saveMeldeExtras() { simCall("saveMeldeExtras"); }
static void setSensorOn(bool on) { simCall("setSensorOn(%d)", on); sensorOn = on; }
static void setMotorOn(bool on) { simCall("setMotorOn(%d)", on); motorOn = on; }
static void setMuteInLessons(bool on) { simCall("setMuteInLessons(%d)", on); muteInLessons = on; }
static void btEnable() { simCall("btEnable"); btOn = true; }
static void btDisable() { simCall("btDisable"); btOn = false; }
static void vibrate(unsigned long ms = 120, int pulses = 1, unsigned long gap = 100) { simCall("vibrate(%lu)", ms); }
static void startSensorRec(const char *l) { simCall("startSensorRec(%s)", l); sensorRec = true; sensorRecLabel = String(l); }
static void stopSensorRec() { simCall("stopSensorRec"); sensorRec = false; }
static void alarmStop() { simCall("alarmStop"); alarmActive = false; }
static void enterStandby() { simCall("enterStandby"); standby = true; }
static void clearCalibration() { simCall("clearCalibration"); }
static void runCalibrationPart(int p) { simCall("runCalibrationPart(%d)", p); }
static bool halOk = true;
static void halSetBrightness(uint8_t b) { simCall("halSetBrightness(%d)", b); }

static bool simTouchDown = false;
static int simTouchX = 0, simTouchY = 0;
static bool touchRead(uint16_t &x, uint16_t &y) {
  if (!simTouchDown) return false;
  x = simTouchX;
  y = simTouchY;
  return true;
}
