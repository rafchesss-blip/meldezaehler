#pragma once

/*
 * ============================================================================
 *  MELDEZÄHLER – Uhr-App für Waveshare ESP32-S3-Touch-AMOLED-2.06
 * ============================================================================
 *
 *  Läuft komplett auf der Uhr (kein PC nötig):
 *    - QMI8658   (Accel + Gyro)      -> Handhebe-Erkennung
 *    - CO5300    (AMOLED 410x502)    -> UI / Statistik
 *    - FT3168    (Touch)             -> Ansicht wechseln / Zähler zurücksetzen
 *    - AXP2101   (PMU)               -> Akku-Anzeige
 *    - PCF85063  (RTC)               -> Uhrzeit
 *
 *  Das auf dem PC trainierte Positions-Modell (3 Klassen: meldung/kopf/tisch,
 *  logistische Regression) ist hier als C-Code eingebettet und läuft live mit.
 *
 *  Erkennung (robust, orientierungsunabhängig):
 *    Beim ersten Start kalibriert die Uhr "Arm UNTEN" und "Arm HOCH".
 *    Daraus wird zusätzlich eine Rotation berechnet, die die Uhr-Achsen in das
 *    Trainings-Koordinatensystem dreht, damit das Modell zuverlässig zwischen
 *    "MELDUNG" (Arm oben) und "KOPF" (Hand am Kopf) unterscheiden kann.
 *
 *  Eine Meldung zählt, wenn:
 *    Arm unten -> oben -> unten, oben >= 0.5 s gehalten, >= 2.5 s Abstand,
 *    und die Modell-Position war dabei NICHT "kopf" (Kopfkratzen zählt nicht).
 *
 *  Bedienung:
 *    - Tippen        : Ansicht wechseln (Zähler <-> Statistik)
 *    - Lang drücken  : Tageszähler zurücksetzen
 *    - Power-Taste   : eine Ebene zurück; auf dem Watchface -> Standby
 *                      (Display aus, Meldungen zählen weiter); im Standby -> aufwachen
 *    - BOOT-Taste    : Melde-Menü (Löschen / Hinzufügen / Bearbeiten)
 *
 *  Serielle Befehle (USB, 115200 Baud):
 *    RESET           : Tageszähler auf 0
 *    CAL             : Kalibrierung neu starten
 *    TIME HH:MM:SS   : Uhrzeit setzen
 *    DATE DD.MM.YY   : Datum setzen
 *    STATS           : Statistik ausgeben
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Preferences.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <BLEService.h>
#include <BLECharacteristic.h>
#include <BLEUtils.h>
#include "HWCDC.h"
#include "Arduino_GFX_Library.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

HWCDC USBSerial;
Preferences prefs;
XPowersPMU pmu;

#include "audio_codec.h"

// ---------------------------------------------------------------------------
// Pin-Konfiguration (Waveshare ESP32-S3-Touch-AMOLED-2.06)
// ---------------------------------------------------------------------------
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_RESET   8
#define LCD_WIDTH   410
#define LCD_HEIGHT  502

#define IIC_SDA     15
#define IIC_SCL     14
#define TP_RESET    9
#define TP_INT      38
#define BOOT_BTN_PIN 0     // BOOT-Taste (aktiv LOW)

// I2C-Adressen
#define QMI_ADDR    0x6B
#define RTC_ADDR    0x51
#define TOUCH_ADDR  0x38
#define PMU_ADDR    0x34

// ---------------------------------------------------------------------------
// Display-Objekte
// ---------------------------------------------------------------------------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */,
                                         LCD_WIDTH, LCD_HEIGHT,
                                         22 /* col_offset1 */, 0, 0, 0);

Arduino_Canvas *canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, gfx);

// ---------------------------------------------------------------------------
// QMI8658 Treiber (minimal, direkt über Wire)
// ---------------------------------------------------------------------------
static void qmiWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t qmiRead(uint8_t reg) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)QMI_ADDR, 1);
  if (Wire.available()) return (uint8_t)Wire.read();
  return 0;
}

// Liest 12 Bytes ab 0x35 (Accel 6 + Gyro 6) in einem Burst.
static bool qmiReadData(int16_t &ax, int16_t &ay, int16_t &az,
                        int16_t &gx, int16_t &gy, int16_t &gz) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(0x35);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t n = Wire.requestFrom((int)QMI_ADDR, 12);
  if (n < 12) return false;
  uint8_t b[12];
  for (int i = 0; i < 12; i++) b[i] = (uint8_t)Wire.read();
  ax = (int16_t)((b[1] << 8) | b[0]);
  ay = (int16_t)((b[3] << 8) | b[2]);
  az = (int16_t)((b[5] << 8) | b[4]);
  gx = (int16_t)((b[7] << 8) | b[6]);
  gy = (int16_t)((b[9] << 8) | b[8]);
  gz = (int16_t)((b[11] << 8) | b[10]);
  return true;
}

static bool qmiInit() {
  // Soft-Reset (Register 0x60 = 0xB0)
  qmiWrite(0x60, 0xB0);
  delay(20);

  // Adress-Auto-Increment aktivieren (CTRL1 Bit 6)
  qmiWrite(0x02, qmiRead(0x02) | 0x40);
  delay(5);

  if (qmiRead(0x00) != 0x05) return false;   // WHO_AM_I

  // CTRL8 = 0x80: STATUS_INT.bit7 als CTRL9-Handshake nutzen
  qmiWrite(0x09, 0x80);

  // CTRL2 (0x03): Accel ±2g (0), ODR 1000 Hz (3)
  qmiWrite(0x03, (0 << 4) | 3);

  // CTRL3 (0x04): Gyro ±1024 dps (6), ODR 896.8 Hz (3)
  qmiWrite(0x04, (6 << 4) | 3);

  // CTRL5 (0x06): Accel-LPF an (Bit0) + Gyro-LPF an (Bit4), beide Mode 0
  qmiWrite(0x06, 0x11);

  // CTRL7 (0x08): Accel + Gyro aktivieren
  qmiWrite(0x08, 0x03);
  return true;
}

// Skalen: Accel ±2g -> 2/32768 g/LSB, Gyro ±1024dps -> 1024/32768 dps/LSB
#define ACCEL_SCALE (2.0f / 32768.0f)
#define GYRO_SCALE  (1024.0f / 32768.0f)

// ---------------------------------------------------------------------------
// RTC PCF85063 (BCD)
// ---------------------------------------------------------------------------
static uint8_t bcd2dec(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

struct RTC_Time { int h, m, s, day, mon, yr; };

static bool rtcRead(RTC_Time &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RTC_ADDR, 7) < 7) return false;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = (uint8_t)Wire.read();
  t.s   = bcd2dec(b[0] & 0x7F);
  t.m   = bcd2dec(b[1] & 0x7F);
  t.h   = bcd2dec(b[2] & 0x3F);
  t.day = bcd2dec(b[3] & 0x3F);
  t.mon = bcd2dec(b[5] & 0x1F);
  t.yr  = bcd2dec(b[6] & 0xFF);
  return true;
}

static void rtcWrite(const RTC_Time &t) {
  uint8_t b[7];
  b[0] = dec2bcd(t.s & 0x3F);
  b[1] = dec2bcd(t.m & 0x3F);
  b[2] = dec2bcd(t.h & 0x3F);
  b[3] = dec2bcd(t.day & 0x3F);
  b[4] = 0x01; // Wochentag (egal)
  b[5] = dec2bcd(t.mon & 0x1F);
  b[6] = dec2bcd(t.yr & 0xFF);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  Wire.write(b, 7);
  Wire.endTransmission();
}

// ---------------------------------------------------------------------------
// Touch FT3168
// ---------------------------------------------------------------------------
static uint8_t touchReg(uint8_t reg) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)TOUCH_ADDR, 1);
  if (Wire.available()) return (uint8_t)Wire.read();
  return 0;
}

static void touchInit() {
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(20);
  digitalWrite(TP_RESET, HIGH);
  delay(60);
  // Power-Modus: Monitor/Trigger (0x01)
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0xA5);
  Wire.write(0x01);
  Wire.endTransmission();
}

static bool touchRead(uint16_t &x, uint16_t &y) {
  uint8_t n = touchReg(0x02);
  if (n == 0 || n > 2) return false;
  uint8_t xh = touchReg(0x03) & 0x0F;
  uint8_t xl = touchReg(0x04);
  uint8_t yh = touchReg(0x05) & 0x0F;
  uint8_t yl = touchReg(0x06);
  x = (xh << 8) | xl;
  y = (yh << 8) | yl;
  return true;
}

// Wartet auf einen Tipp auf den Bildschirm (für die Erst-Kalibrierung).
static void waitForTap() {
  bool wasDown = false;
  while (true) {
    uint16_t x, y;
    bool down = touchRead(x, y);
    if (down && !wasDown) wasDown = true;
    else if (!down && wasDown) return;
    delay(30);
    if (USBSerial.available()) {
      String l = USBSerial.readStringUntil('\n');
      l.trim();
      if (l == "CAL") return;
    }
  }
}

// ---------------------------------------------------------------------------
// Vibrationsmotor (GPIO18 steuert den Motor über einen Transistor)
// ---------------------------------------------------------------------------
static bool vibPinInit = false;
static void vibrate(unsigned long ms = 120) {
  if (!vibPinInit) {
    pinMode(18, OUTPUT);   // ohne OUTPUT-Modus wuerde digitalWrite nur den Pull-up schalten
    vibPinInit = true;
  }
  USBSerial.printf("[vib] GPIO18 HIGH fuer %lums\n", ms);
  digitalWrite(18, HIGH);
  delay(ms);
  digitalWrite(18, LOW);
}

// ---------------------------------------------------------------------------
// Portiertes Modell: StandardScaler + LogisticRegression (3 Klassen)
// Klassen-Reihenfolge: 0=kopf, 1=meldung, 2=tisch
// ---------------------------------------------------------------------------
static const float SCALER_MEAN[6] = {-0.13216681f, 0.06450418f, 0.58782174f,
                                      1.12247951f, 112.22219355f, 352.60333127f};
static const float SCALER_SCALE[6] = {0.47504765f, 0.55656242f, 0.31209735f,
                                      0.50190825f, 52.45411345f, 148.25679060f};
static const float COEF_KOPF[6]    = {-0.29373903f, 1.13010770f, 0.18891726f,
                                      -0.94081226f, 1.29465173f, 0.06444739f};
static const float COEF_MELDUNG[6] = {0.48128182f, 1.94770818f, 0.11779048f,
                                      1.86466373f, -2.94147028f, 0.71228661f};
static const float COEF_TISCH[6]   = {-0.18754279f, -3.07781587f, -0.30670775f,
                                      -0.92385147f, 1.64681855f, -0.77673399f};
static const float INTERCEPT_KOPF    = 1.22055428f;
static const float INTERCEPT_MELDUNG = -0.04804842f;
static const float INTERCEPT_TISCH   = -1.17250586f;

// Referenz-Schwerkraftrichtungen aus dem Training (für die Achsen-Rotation)
static const float REF_TISCH[3]   = {-0.21516448f, -0.62174984f, 0.75308126f};
static const float REF_MELDUNG[3] = {-0.19802275f, 0.68946092f, 0.69672852f};

// ---------------------------------------------------------------------------
// 3D-Helfer
// ---------------------------------------------------------------------------
struct Vec3 { float x, y, z; };

static float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 vscale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static Vec3 vcross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
static Vec3 vnorm(Vec3 a) {
  float n = sqrtf(vdot(a, a));
  if (n < 1e-6f) return {0, 0, 1};
  return {a.x / n, a.y / n, a.z / n};
}

// Rotation R, die den Quell-Basis (N=unten, H=hoch) auf den Ziel-Basis
// (T=Ref-Tisch, M=Ref-Meldung) dreht (2-Vektor-Rotation, geschlossene Form).
static void buildRotation(Vec3 N, Vec3 H, Vec3 T, Vec3 M, float R[3][3]) {
  Vec3 s1 = vnorm(N);
  Vec3 s2raw = vsub(H, vscale(s1, vdot(H, s1)));
  if (sqrtf(vdot(s2raw, s2raw)) < 0.1f) s2raw = vcross(s1, {0, 1, 0});
  Vec3 s2 = vnorm(s2raw);
  Vec3 s3 = vcross(s1, s2);

  Vec3 t1 = vnorm(T);
  Vec3 t2raw = vsub(M, vscale(t1, vdot(M, t1)));
  if (sqrtf(vdot(t2raw, t2raw)) < 0.1f) t2raw = vcross(t1, {0, 1, 0});
  Vec3 t2 = vnorm(t2raw);
  Vec3 t3 = vcross(t1, t2);

  // R = T_basis * S_basis^T  (Basisvektoren als Spalten)
  float S[3][3] = {{s1.x, s2.x, s3.x}, {s1.y, s2.y, s3.y}, {s1.z, s2.z, s3.z}};
  float Tb[3][3] = {{t1.x, t2.x, t3.x}, {t1.y, t2.y, t3.y}, {t1.z, t2.z, t3.z}};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      R[i][j] = Tb[i][0] * S[j][0] + Tb[i][1] * S[j][1] + Tb[i][2] * S[j][2];
}

static void rotVec(const float R[3][3], Vec3 a, Vec3 &out) {
  out.x = R[0][0] * a.x + R[0][1] * a.y + R[0][2] * a.z;
  out.y = R[1][0] * a.x + R[1][1] * a.y + R[1][2] * a.z;
  out.z = R[2][0] * a.x + R[2][1] * a.y + R[2][2] * a.z;
}

// ---------------------------------------------------------------------------
// Kalibrierung + Modell
// ---------------------------------------------------------------------------
Vec3 N_dir = {0, 0, 1};   // "Arm UNTEN" Richtung (Uhr-Koordinaten)
Vec3 H_dir = {0, 0, 1};   // "Arm HOCH" Richtung (Uhr-Koordinaten)
Vec3 T_dir = {0, 0, 1};   // "TISCH" Richtung (Uhr liegt auf dem Tisch)
bool tischCalibrated = false;
float R_model[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
bool calibrated = false;

// ---------------------------------------------------------------------------
// Erkennungs-Parameter
// ---------------------------------------------------------------------------
#define BUF_N        300   // 3 s Ringpuffer
#define FENSTER      150   // 1,5 s Fenster für das Modell
#define FENSTER_KURZ 50    // 0,5 s Fenster für den Zustands-Score
#define AUSWERT_N    50    // alle 50 Samples (0,5 s) auswerten

#define CAL_REPS      10    // Wiederholungen pro Kalibrier-Haltung
#define LEAVE_HOCH   0.00f
#define MIN_HALTEN_MS  500
#define SPERRE_MS      2500

float enterHoch = 0.10f;     // "Arm oben"-Schwelle (wird aus der Kalibrierung gelernt)
int   meldungenSeitCalib = 0;

float axBuf[BUF_N], ayBuf[BUF_N], azBuf[BUF_N];
float gxBuf[BUF_N], gyBuf[BUF_N], gzBuf[BUF_N];
int   bufHead = 0, bufCount = 0;

unsigned long nextSampleUs = 0;
int sampleCounter = 0;

// Zustand
bool imHoch = false;
unsigned long hochSeitMs = 0;
unsigned long letzteMeldungMs = 0;
bool hochKopf = false;         // finale Entscheidung: "oben" war überwiegend KOPF
int  hochKopfCount = 0;        // wie oft KOPF während "oben" gesehen wurde
int  hochMeldungCount = 0;     // wie oft MELDUNG während "oben" gesehen wurde
int  aktuellKlasse = 2;        // 0=kopf,1=meldung,2=tisch
float aktuellProb[3] = {0, 0, 1};
float aktuellScore = -1.0f;

// Statistik
int totalHeute = 0;
int sessionCount = 0;
int minHist[60] = {0};
int minHistIdx = 0;
int minuteCount = 0;
unsigned long minuteStartMs = 0;
unsigned long meldeZeitMs = 0;   // gesamte Zeit, die der Arm oben war (Meldzeit heute)

// Zusatz-Statistik (wird am Tagesende zurückgesetzt)
int drange = 0;    // wie oft drangenommen
int richtig = 0;   // Antwort richtig
int falsch = 0;    // Antwort falsch

// Letzte Meldungen (nur für "letzte Meldung löschen/bearbeiten")
#define MAX_MELD_LOG 16
struct MeldLogEntry {
  bool valid;
  uint32_t absMinute;   // minuteStartMs/60000 zum Zeitpunkt der Meldung
  uint32_t dauerMs;     // wie lange der Arm oben war
  int8_t minSlot;       // minHistIdx zum Zeitpunkt der Meldung
  int8_t lessonDay;     // -1 = keine Stunde
  int8_t lessonIdx;     // -1 = keine Stunde
};
static MeldLogEntry meldLog[MAX_MELD_LOG];
static int meldLogWrite = 0;
static int meldLogCount = 0;

// UI
int view = 0;                  // 0 = Zähler, 1 = Statistik
int watchface = 0;             // aktuelles Watchface (0..4)
int watchfaceSel = 0;          // Auswahl im Zifferblatt-Picker
bool touchWasDown = false;
unsigned long touchDownMs = 0;
uint16_t touchX = 0, touchY = 0;
uint16_t touchStartX = 0, touchStartY = 0;
bool showResetHint = false;
unsigned long resetHintMs = 0;

// Test-App
char testInfo[40] = "";
unsigned long testInfoMs = 0;

// Standby + Tasten
bool standby = false;
bool bootBtnWasDown = false;
unsigned long bootDownMs = 0;

// App-Struktur (Launcher)
int screen = 0;                // 0 = Watchface, 1 = Meldezähler, 2 = Einstellungen, 3 = Zifferblatt, 4 = Apps, 5 = Rekorder, 6 = Melde-Bearbeiten, 7 = Zeit, 8 = Test
int settingsItem = 0;          // 0 = Menü, 1 = Helligkeit, 2 = WLAN, 3 = Bluetooth
int meldeEditMode = 0;         // 0 = Menü (Löschen/Hinzufügen/Bearbeiten), 1 = Bearbeiten, 2 = Richtig/Falsch
bool calibSelectOpen = false;  // Auswahl "Was kalibrieren?" im Kalibrier-Screen

// Zeit-App (Timer + Stoppuhr)
int zeitTab = 0;               // 0 = Timer, 1 = Stoppuhr
unsigned long timerSetMs = 5UL * 60UL * 1000UL;
unsigned long timerRemainingMs = 5UL * 60UL * 1000UL;
bool timerRunning = false;
unsigned long timerLastMs = 0;
unsigned long stopwatchBaseMs = 0;
unsigned long stopwatchStartMs = 0;
bool stopwatchRunning = false;
int brightness = 208;          // 0..255 (CO5300 Normal-Mode-Helligkeit)

// WLAN
String wifiPass = "";
int wifiOffset = 0;            // Scroll-Offset der Netzliste
int wifiPasswordMode = 0;      // 0 = aus, 1 = Passworteingabe
String wifiTargetSSID = "";
int wifiCursor = 0;

// Bluetooth (BLE)
static bool bleInited = false;
bool btOn = false;

// Sensor-Erkennung (IMU) an/aus (Energie sparen / Fehlmeldungen vermeiden)
bool sensorOn = true;

// Umgebungsdaten (1 Hz aktualisiert, damit das Rendern flüssig bleibt)
static int cachedH = -1, cachedM = -1, cachedS = -1;
static int cachedDay = -1, cachedMon = -1, cachedYr = -1, cachedPct = -1;
static unsigned long lastEnvMs = 0;

// ---------------------------------------------------------------------------
// Stundenplan (Timetable) – wird im SPIFFS gespeichert
// ---------------------------------------------------------------------------
#define MAX_DAYS     7    // 0=So .. 6=Sa
#define MAX_PERIODS 12

struct Period {
  char name[12];
  uint8_t sh, sm, eh, em;   // Start/Ende (Stunde, Minute)
  bool active;
};

static Period ttDays[MAX_DAYS][MAX_PERIODS];
static uint8_t ttCount[MAX_DAYS];                 // Anzahl Stunden pro Tag
static int16_t lessonCounts[MAX_DAYS][MAX_PERIODS];
static bool ttActive = false;

// BLE GATT-Objekte (Zeit-Sync + Statistik)
static BLEServer *pServer = nullptr;
static BLECharacteristic *pCharTime = nullptr;
static BLECharacteristic *pCharStats = nullptr;
static BLECharacteristic *pCharClear = nullptr;
static BLECharacteristic *pCharTT = nullptr;
static BLECharacteristic *pCharLesson = nullptr;

// WICHTIG: Eine BLE-Charakteristik ist auf ESP_GATT_MAX_ATTR_LEN (517 Bytes)
// begrenzt. Deshalb wird die Statistik auf zwei Charakteristiken aufgeteilt,
// damit bei vollem Stundenplan nichts abgeschnitten wird.

// Skalare + Minuten-Verlauf (immer klein genug)
static String buildStatsJson() {
  String s = "{";
  s += "\"total\":" + String(totalHeute) + ",";
  s += "\"session\":" + String(sessionCount) + ",";
  s += "\"seitCalib\":" + String(meldungenSeitCalib) + ",";
  s += "\"drange\":" + String(drange) + ",";
  s += "\"richtig\":" + String(richtig) + ",";
  s += "\"falsch\":" + String(falsch) + ",";
  s += "\"meldezeit\":" + String(meldeZeitMs / 1000UL) + ",";
  s += "\"batt\":" + String(cachedPct) + ",";
  s += "\"min\":[";
  // Rotiert wie auf dem Display: Index 0 = aelteste Minute, Index 59 = neueste.
  for (int i = 0; i < 60; i++) {
    if (i) s += ",";
    s += String(minHist[(minHistIdx + i) % 60]);
  }
  s += "]}";
  return s;
}

// Stunden-Statistik (separat, kann bei vollem Stundenplan gross werden)
static String buildLessonJson() {
  String s = "{\"lessonCounts\":[";
  for (int d = 0; d < MAX_DAYS; d++) {
    if (d) s += ",";
    s += "[";
    for (int p = 0; p < ttCount[d]; p++) {
      if (p) s += ",";
      s += String(lessonCounts[d][p]);
    }
    s += "]";
  }
  s += "]}";
  return s;
}

static const char *WD_DE[7] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

// Wochentag aus Datum (Sakamoto-Algorithmus), 0=So..6=Sa
static int weekdayOf(int d, int m, int y) {
  static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void updateEnv() {
  unsigned long now = millis();
  if (now - lastEnvMs < 1000) return;
  lastEnvMs = now;
  RTC_Time t;
  if (rtcRead(t)) {
    cachedH = t.h; cachedM = t.m; cachedS = t.s;
    cachedDay = t.day; cachedMon = t.mon; cachedYr = t.yr;
  } else {
    cachedH = cachedM = cachedS = -1;
    cachedDay = cachedMon = cachedYr = -1;
  }
  cachedPct = pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1;
  if (pCharStats) pCharStats->setValue(buildStatsJson().c_str());
  if (pCharLesson) pCharLesson->setValue(buildLessonJson().c_str());
}

// ---------------------------------------------------------------------------
// Stundenplan: Speicher (NVS/Flash) + Zuordnung + BLE-Befehle
// ---------------------------------------------------------------------------
static void splitCmd(const String &s, String out[], int maxOut, int &n) {
  n = 0;
  int start = 0;
  while (n < maxOut) {
    int pos = s.indexOf('|', start);
    if (pos < 0) { out[n++] = s.substring(start); break; }
    out[n++] = s.substring(start, pos);
    start = pos + 1;
  }
}

static void setPeriod(int day, int idx, const String &name, int sh, int sm, int eh, int em) {
  if (day < 0 || day >= MAX_DAYS || idx < 0 || idx >= MAX_PERIODS) return;
  ttDays[day][idx].active = true;
  ttDays[day][idx].sh = sh; ttDays[day][idx].sm = sm;
  ttDays[day][idx].eh = eh; ttDays[day][idx].em = em;
  strncpy(ttDays[day][idx].name, name.c_str(), 11);
  ttDays[day][idx].name[11] = 0;
  if (idx + 1 > ttCount[day]) ttCount[day] = idx + 1;
}

static void saveTimetable() {
  String s = "";
  for (int d = 0; d < MAX_DAYS; d++)
    for (int p = 0; p < ttCount[d]; p++)
      if (ttDays[d][p].active) {
        if (s.length()) s += "\n";
        s += String(d) + "|" + String(p) + "|" + String(ttDays[d][p].name) + "|" +
             String(ttDays[d][p].sh) + "|" + String(ttDays[d][p].sm) + "|" +
             String(ttDays[d][p].eh) + "|" + String(ttDays[d][p].em);
      }
  prefs.putString("tt", s);
  prefs.putInt("ttactive", ttActive ? 1 : 0);
}

static void loadTimetable() {
  ttActive = prefs.getInt("ttactive", 0) == 1;
  String s = prefs.getString("tt", "");
  if (s.length() == 0) return;
  int start = 0;
  while (start < (int)s.length()) {
    int nl = s.indexOf('\n', start);
    String line = (nl < 0) ? s.substring(start) : s.substring(start, nl);
    String t[8];
    int n = 0;
    splitCmd(line, t, 8, n);
    if (n >= 7)
      setPeriod(t[0].toInt(), t[1].toInt(), t[2], t[3].toInt(), t[4].toInt(), t[5].toInt(), t[6].toInt());
    if (nl < 0) break;
    start = nl + 1;
  }
  USBSerial.println(ttActive ? "Stundenplan geladen (aktiv)." : "Stundenplan geladen (inaktiv).");
}

static void saveLessonStats() {
  String s = "";
  for (int d = 0; d < MAX_DAYS; d++)
    for (int p = 0; p < MAX_PERIODS; p++) {
      if (s.length()) s += ",";
      s += String(lessonCounts[d][p]);
    }
  prefs.putString("ttstats", s);
}

static void loadLessonStats() {
  String s = prefs.getString("ttstats", "");
  if (s.length() == 0) return;
  int idx = 0;
  int start = 0;
  while (idx < MAX_DAYS * MAX_PERIODS) {
    int comma = s.indexOf(',', start);
    String num = (comma < 0) ? s.substring(start) : s.substring(start, comma);
    int d = idx / MAX_PERIODS;
    int p = idx % MAX_PERIODS;
    lessonCounts[d][p] = (int16_t)num.toInt();
    idx++;
    if (comma < 0) break;
    start = comma + 1;
  }
}

static int findPeriod(int day, int h, int m) {
  int cur = h * 60 + m;
  for (int p = 0; p < ttCount[day]; p++) {
    int s = ttDays[day][p].sh * 60 + ttDays[day][p].sm;
    int e = ttDays[day][p].eh * 60 + ttDays[day][p].em;
    if (cur >= s && cur < e) return p;
  }
  return -1;
}

static void saveMeldeExtras() {
  prefs.putInt("drange", drange);
  prefs.putInt("richtig", richtig);
  prefs.putInt("falsch", falsch);
}

static void logMeldung(int wd, int p, unsigned long dauerMs) {
  int idx = meldLogWrite % MAX_MELD_LOG;
  meldLog[idx].valid = true;
  meldLog[idx].absMinute = minuteStartMs / 60000UL;
  meldLog[idx].dauerMs = (uint32_t)dauerMs;
  meldLog[idx].minSlot = (int8_t)minHistIdx;
  meldLog[idx].lessonDay = (int8_t)wd;
  meldLog[idx].lessonIdx = (int8_t)p;
  meldLogWrite = (meldLogWrite + 1) % MAX_MELD_LOG;
  if (meldLogCount < MAX_MELD_LOG) meldLogCount++;
}

// Eine Meldung zählen (echte Handhebung oder manuell über "Hinzufügen").
// dauerMs = wie lange der Arm oben war (0 bei manuell hinzugefügten Meldungen).
static void registerMeldung(unsigned long dauerMs = 0) {
  totalHeute++;
  sessionCount++;
  minuteCount++;
  meldungenSeitCalib++;
  meldeZeitMs += dauerMs;

  int wd = -1, p = -1;
  if (ttActive && cachedDay >= 1 && cachedMon >= 1) {
    wd = weekdayOf(cachedDay, cachedMon, 2000 + cachedYr);  // 0=So..6=Sa
    p = findPeriod(wd, cachedH, cachedM);
    if (p >= 0) {
      lessonCounts[wd][p]++;
      saveLessonStats();
      USBSerial.printf("[stunde] Tag %d Stunde %d: %d Meldungen\n", wd, p, lessonCounts[wd][p]);
    }
  }

  logMeldung(wd, p, dauerMs);
  prefs.putInt("total", totalHeute);
  prefs.putInt("seitCalib", meldungenSeitCalib);
  prefs.putULong("meldezeit", meldeZeitMs);
}

// Die zuletzt hinzugefügte Meldung wieder zurücknehmen.
static void removeLastMeldung() {
  if (totalHeute <= 0) return;
  totalHeute--;
  if (sessionCount > 0) sessionCount--;
  if (meldungenSeitCalib > 0) meldungenSeitCalib--;

  if (meldLogCount > 0) {
    int idx = (meldLogWrite - 1 + MAX_MELD_LOG) % MAX_MELD_LOG;
    MeldLogEntry &e = meldLog[idx];
    if (e.valid) {
      if (e.absMinute == minuteStartMs / 60000UL) {
        // Meldung liegt noch in der laufenden Minute
        if (minuteCount > 0) minuteCount--;
      } else if (e.minSlot >= 0 && e.minSlot < 60 && minHist[e.minSlot] > 0) {
        minHist[e.minSlot]--;
      }
      if (e.lessonDay >= 0 && e.lessonIdx >= 0) {
        if (lessonCounts[e.lessonDay][e.lessonIdx] > 0) {
          lessonCounts[e.lessonDay][e.lessonIdx]--;
          saveLessonStats();
        }
      }
      if (meldeZeitMs >= e.dauerMs) meldeZeitMs -= e.dauerMs;
      else meldeZeitMs = 0;
      e.valid = false;
    }
    meldLogCount--;
    meldLogWrite = (meldLogWrite - 1 + MAX_MELD_LOG) % MAX_MELD_LOG;
  } else {
    // Nach einem Neustart ist kein Log mehr da -> nur aktuelle Minute schätzen.
    if (minuteCount > 0) minuteCount--;
  }

  prefs.putInt("total", totalHeute);
  prefs.putInt("seitCalib", meldungenSeitCalib);
  prefs.putULong("meldezeit", meldeZeitMs);
  USBSerial.printf("Letzte Meldung geloescht. Heute: %d\n", totalHeute);
}

static void clearTimetable() {
  for (int d = 0; d < MAX_DAYS; d++) {
    ttCount[d] = 0;
    for (int p = 0; p < MAX_PERIODS; p++) {
      ttDays[d][p].active = false;
      ttDays[d][p].name[0] = 0;
      lessonCounts[d][p] = 0;
    }
  }
  ttActive = false;
  prefs.putString("tt", "");
  prefs.putInt("ttactive", 0);
  saveLessonStats();
}

static void handleTTCommand(const String &cmd) {
  if (cmd == "CLEAR") {
    clearTimetable();
    USBSerial.println("Stundenplan geleert.");
  } else if (cmd == "SAVE") {
    ttActive = true;
    saveTimetable();
    USBSerial.println("Stundenplan aktiviert & gespeichert.");
  } else if (cmd.startsWith("P|")) {
    String t[8];
    int n = 0;
    splitCmd(cmd, t, 8, n);
    if (n >= 8 && t[0] == "P") {
      int day = t[1].toInt();
      int idx = t[2].toInt();
      int sh = t[4].toInt(), sm = t[5].toInt();
      int eh = t[6].toInt(), em = t[7].toInt();
      if (day >= 0 && day < MAX_DAYS && idx >= 0 && idx < MAX_PERIODS) {
        setPeriod(day, idx, t[3], sh, sm, eh, em);
        USBSerial.printf("[stunde] Tag %d Idx %d = %s %02d:%02d-%02d:%02d\n",
                         day, idx, ttDays[day][idx].name, sh, sm, eh, em);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Merkmale über die letzten n Samples berechnen
// ---------------------------------------------------------------------------
struct Stats {
  float mx, my, mz;    // mittlere Beschleunigung (Rohmittel, nicht normiert)
  float accStd;        // Summe der Achsen-Standardabweichungen
  float gmean, gmax;   // Drehraten-Betrag Mittel/Max
};

static Stats computeStats(int n) {
  Stats st;
  int start = (bufHead - n + BUF_N) % BUF_N;

  float sx = 0, sy = 0, sz = 0;
  for (int k = 0; k < n; k++) {
    int i = (start + k) % BUF_N;
    sx += axBuf[i]; sy += ayBuf[i]; sz += azBuf[i];
  }
  st.mx = sx / n; st.my = sy / n; st.mz = sz / n;

  float vx = 0, vy = 0, vz = 0;
  for (int k = 0; k < n; k++) {
    int i = (start + k) % BUF_N;
    float dx = axBuf[i] - st.mx;
    float dy = ayBuf[i] - st.my;
    float dz = azBuf[i] - st.mz;
    vx += dx * dx; vy += dy * dy; vz += dz * dz;
  }
  st.accStd = sqrtf(vx / n) + sqrtf(vy / n) + sqrtf(vz / n);

  float gs = 0; st.gmax = 0;
  for (int k = 0; k < n; k++) {
    int i = (start + k) % BUF_N;
    float g = sqrtf(gxBuf[i] * gxBuf[i] + gyBuf[i] * gyBuf[i] + gzBuf[i] * gzBuf[i]);
    gs += g;
    if (g > st.gmax) st.gmax = g;
  }
  st.gmean = gs / n;
  return st;
}

// Softmax über die 3 Klassen, gibt Klassen-Index zurück.
static int predictClass(Vec3 m, float accStd, float gmean, float gmax,
                        float prob[3]) {
  float f[6] = {m.x, m.y, m.z, accStd, gmean, gmax};
  float x[6];
  for (int i = 0; i < 6; i++) x[i] = (f[i] - SCALER_MEAN[i]) / SCALER_SCALE[i];

  float z0 = 0, z1 = 0, z2 = 0;
  for (int i = 0; i < 6; i++) {
    z0 += COEF_KOPF[i]    * x[i];
    z1 += COEF_MELDUNG[i] * x[i];
    z2 += COEF_TISCH[i]   * x[i];
  }
  z0 += INTERCEPT_KOPF;
  z1 += INTERCEPT_MELDUNG;
  z2 += INTERCEPT_TISCH;

  float mz = fmaxf(z0, fmaxf(z1, z2));
  float e0 = expf(z0 - mz), e1 = expf(z1 - mz), e2 = expf(z2 - mz);
  float s = e0 + e1 + e2;
  prob[0] = e0 / s; prob[1] = e1 / s; prob[2] = e2 / s;

  if (prob[0] >= prob[1] && prob[0] >= prob[2]) return 0;
  if (prob[1] >= prob[2]) return 1;
  return 2;
}

// ---------------------------------------------------------------------------
// Sample lesen + in Ringpuffer
// ---------------------------------------------------------------------------
static void readAndStoreSample() {
  int16_t ax, ay, az, gx, gy, gz;
  if (!qmiReadData(ax, ay, az, gx, gy, gz)) return;

  axBuf[bufHead] = ax * ACCEL_SCALE;
  ayBuf[bufHead] = ay * ACCEL_SCALE;
  azBuf[bufHead] = az * ACCEL_SCALE;
  gxBuf[bufHead] = gx * GYRO_SCALE;
  gyBuf[bufHead] = gy * GYRO_SCALE;
  gzBuf[bufHead] = gz * GYRO_SCALE;

  bufHead = (bufHead + 1) % BUF_N;
  if (bufCount < BUF_N) bufCount++;
}

// ---------------------------------------------------------------------------
// Auswertung (Merkmale + Modell + Zustandsmaschine)
// ---------------------------------------------------------------------------
static void evaluate() {
  // Meldungen werden jetzt IMMER gezählt (auch auf dem Watchface / in anderen Apps)
  if (bufCount < FENSTER) return;

  // Kurzes Fenster -> Score (reaktionsschnell)
  Stats kurz = computeStats(FENSTER_KURZ);
  Vec3 vk = vnorm({kurz.mx, kurz.my, kurz.mz});
  aktuellScore = vdot(vk, H_dir) - vdot(vk, N_dir);
  float dotH = vdot(vk, H_dir);
  float dotN = vdot(vk, N_dir);

  // Langes Fenster -> Modell (im Trainings-Koordinatensystem)
  Stats lang = computeStats(FENSTER);
  Vec3 mSensor = {lang.mx, lang.my, lang.mz};
  Vec3 mRot;
  rotVec(R_model, vnorm(mSensor), mRot);
  aktuellKlasse = predictClass(mRot, lang.accStd, lang.gmean, lang.gmax, aktuellProb);

  unsigned long now = millis();

  static unsigned long lastDbgMs = 0;
  if (now - lastDbgMs >= 2000) {
    lastDbgMs = now;
    USBSerial.printf("[dbg] score=%+.2f enter=%.2f dotH=%.2f dotN=%.2f klasse=%d p(m/k/t)=%d/%d/%d%% hoch=%d\n",
                     aktuellScore, enterHoch, dotH, dotN, aktuellKlasse,
                     (int)(aktuellProb[0] * 100), (int)(aktuellProb[1] * 100),
                     (int)(aktuellProb[2] * 100), imHoch ? 1 : 0);
  }

  if (!imHoch) {
    if (aktuellScore > enterHoch) {
      imHoch = true;
      hochSeitMs = now;
      hochKopf = false;
      hochKopfCount = 0;
      hochMeldungCount = 0;
      USBSerial.printf("[zustand] ARM OBEN (score=%.2f)\n", aktuellScore);
    }
  } else {
    // Modell beobachten: KOPF unterdrückt die Zählung nur, wenn er
    // während der "oben"-Phase überwiegt (robuster gegen kurze Ausreißer).
    if (aktuellKlasse == 0) hochKopfCount++;
    else if (aktuellKlasse == 1) hochMeldungCount++;

    if (aktuellScore < LEAVE_HOCH) {
      unsigned long dauer = now - hochSeitMs;
      hochKopf = (hochKopfCount > hochMeldungCount);
      bool zaehlt = (dauer >= MIN_HALTEN_MS) &&
                    (now - letzteMeldungMs > SPERRE_MS) &&
                    !hochKopf;
      USBSerial.printf("[zustand] arm unten (dauer=%lums klasse=%d kopfC=%d meldC=%d zaehlt=%d)\n",
                       dauer, aktuellKlasse, hochKopfCount, hochMeldungCount,
                       zaehlt ? 1 : 0);
      if (zaehlt) {
        registerMeldung(dauer);
        letzteMeldungMs = now;
        vibrate(120);   // haptisches Feedback am Handgelenk
        USBSerial.print(">>> MELDUNG!  Gesamt heute: ");
        USBSerial.println(totalHeute);
      }
      imHoch = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Kalibrierung
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Kalibrierung (10 Wiederholungen x 3 Haltungen)
// ---------------------------------------------------------------------------
static void centerText(int y, const char *s, uint16_t color, int scale);

// Eine ruhige Haltung messen -> mittlere "oben"-Richtung (Einheitsvektor)
static Vec3 collectStaticRep(unsigned long dauerMs) {
  float sx = 0, sy = 0, sz = 0;
  int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < dauerMs) {
    int16_t ax, ay, az, gx, gy, gz;
    if (qmiReadData(ax, ay, az, gx, gy, gz)) {
      sx += ax * ACCEL_SCALE;
      sy += ay * ACCEL_SCALE;
      sz += az * ACCEL_SCALE;
      n++;
    }
    delay(8);
  }
  if (n < 20) return {0, 0, 1};
  return vnorm({sx / n, sy / n, sz / n});
}

// Eine Wiederholung "NICHT MELDEN" (normale Bewegung) -> maximaler Score
static float collectNormalRep(unsigned long dauerMs) {
  float ax[200], ay[200], az[200];
  int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < dauerMs && n < 200) {
    int16_t a, b, c, gx, gy, gz;
    if (qmiReadData(a, b, c, gx, gy, gz)) {
      ax[n] = a * ACCEL_SCALE;
      ay[n] = b * ACCEL_SCALE;
      az[n] = c * ACCEL_SCALE;
      n++;
    }
    delay(7);
  }
  float maxScore = -10.0f;
  for (int i = 0; i + FENSTER_KURZ <= n; i += 5) {
    float sx = 0, sy = 0, sz = 0;
    for (int k = i; k < i + FENSTER_KURZ; k++) { sx += ax[k]; sy += ay[k]; sz += az[k]; }
    Vec3 v = vnorm({sx / FENSTER_KURZ, sy / FENSTER_KURZ, sz / FENSTER_KURZ});
    float sc = vdot(v, H_dir) - vdot(v, N_dir);
    if (sc > maxScore) maxScore = sc;
  }
  return maxScore;
}

// Schwelle geometrisch aus "Arm unten" und "Arm oben" berechnen.
// upScore = Score-Wert, wenn der Arm exakt in der kalibrierten "oben"-Haltung ist.
static float computeEnterHoch() {
  float upScore = 1.0f - vdot(N_dir, H_dir);
  float e = 0.35f * upScore;
  if (e < 0.10f) e = 0.10f;
  if (e > 0.60f) e = 0.60f;
  return e;
}

static void calibScreen(const char *title, const char *sub, int big) {
  canvas->fillScreen(BLACK);
  canvas->setTextSize(2);
  canvas->setTextColor(WHITE);
  canvas->setCursor(80, 150);
  canvas->print("Kalibrierung");
  centerText(225, title, YELLOW, 3);
  if (sub) centerText(295, sub, CYAN, 2);
  if (big > 0) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", big);
    canvas->setTextSize(6);
    canvas->setTextColor(CYAN);
    int w = strlen(buf) * 6 * 6;
    canvas->setCursor(LCD_WIDTH / 2 - w / 2, 340);
    canvas->print(buf);
  }
  canvas->flush();
}

static void calibRep(const char *title, int rep) {
  char buf[32];
  snprintf(buf, sizeof(buf), "Wiederholung %d/%d", rep, CAL_REPS);
  calibScreen(title, buf, 0);
}

static Vec3 collectArmUnten() {
  char rep[32];
  for (int i = 3; i >= 1; i--) { calibScreen("Arm UNTEN halten", "ruhig", i); delay(1000); }
  Vec3 Nsum = {0, 0, 0};
  for (int i = 1; i <= CAL_REPS; i++) {
    snprintf(rep, sizeof(rep), "Wiederholung %d/%d", i, CAL_REPS);
    calibScreen("Arm UNTEN halten", rep, 0);
    Vec3 v = collectStaticRep(1500);
    Nsum = {Nsum.x + v.x, Nsum.y + v.y, Nsum.z + v.z};
  }
  return vnorm(Nsum);
}

static Vec3 collectArmHoch() {
  char rep[32];
  for (int i = 3; i >= 1; i--) { calibScreen("Arm HOCH halten", "ruhig", i); delay(1000); }
  Vec3 Hsum = {0, 0, 0};
  for (int i = 1; i <= CAL_REPS; i++) {
    snprintf(rep, sizeof(rep), "Wiederholung %d/%d", i, CAL_REPS);
    calibScreen("Arm HOCH halten", rep, 0);
    Vec3 v = collectStaticRep(1500);
    Hsum = {Hsum.x + v.x, Hsum.y + v.y, Hsum.z + v.z};
  }
  return vnorm(Hsum);
}

static float collectNichtMelden() {
  char rep[32];
  for (int i = 3; i >= 1; i--) { calibScreen("NICHT MELDEN", "normal bewegen", i); delay(1000); }
  float maxScore = -10.0f;
  for (int i = 1; i <= CAL_REPS; i++) {
    snprintf(rep, sizeof(rep), "Wiederholung %d/%d", i, CAL_REPS);
    calibScreen("NICHT MELDEN", rep, 0);
    float s = collectNormalRep(1500);
    if (s > maxScore) maxScore = s;
  }
  return maxScore;
}

static Vec3 collectTisch() {
  char rep[32];
  for (int i = 3; i >= 1; i--) { calibScreen("TISCH kalibrieren", "Uhr auf den Tisch legen", i); delay(1000); }
  Vec3 Tsum = {0, 0, 0};
  for (int i = 1; i <= CAL_REPS; i++) {
    snprintf(rep, sizeof(rep), "Wiederholung %d/%d", i, CAL_REPS);
    calibScreen("TISCH halten", rep, 0);
    Vec3 v = collectStaticRep(1500);
    Tsum = {Tsum.x + v.x, Tsum.y + v.y, Tsum.z + v.z};
  }
  return vnorm(Tsum);
}

static void finishCalibration(bool resetSeit, float maxScore) {
  // "Arm oben"-Schwelle geometrisch aus UNTEN/HOCH ableiten (robust).
  // Die NICHT-MELDEN-Daten dienen nur als Hinweis, nicht als Schwelle.
  float upScore = 1.0f - vdot(N_dir, H_dir);
  enterHoch = computeEnterHoch();
  if (maxScore > -5.0f && maxScore > 0.75f * upScore) {
    USBSerial.printf("Hinweis: normale Bewegungen erreichten Score %.2f (Arm-oben-Score %.2f).\n", maxScore, upScore);
  }

  // Rotation für das Modell berechnen.
  // Wenn eine TISCH-Richtung kalibriert wurde, wird sie als "Tisch"-Referenz
  // verwendet; sonst dient weiterhin die Arm-UNTEN-Richtung als Referenz.
  Vec3 T = {REF_TISCH[0], REF_TISCH[1], REF_TISCH[2]};
  Vec3 M = {REF_MELDUNG[0], REF_MELDUNG[1], REF_MELDUNG[2]};
  Vec3 sourceTisch = tischCalibrated ? T_dir : N_dir;
  buildRotation(sourceTisch, H_dir, T, M, R_model);
  calibrated = true;
  if (resetSeit) {
    meldungenSeitCalib = 0;
    prefs.putInt("seitCalib", 0);
  }

  // Speichern
  prefs.putInt("calib", 1);
  prefs.putInt("calibT", tischCalibrated ? 1 : 0);
  prefs.putFloat("Nx", N_dir.x); prefs.putFloat("Ny", N_dir.y); prefs.putFloat("Nz", N_dir.z);
  prefs.putFloat("Hx", H_dir.x); prefs.putFloat("Hy", H_dir.y); prefs.putFloat("Hz", H_dir.z);
  prefs.putFloat("Tx", T_dir.x); prefs.putFloat("Ty", T_dir.y); prefs.putFloat("Tz", T_dir.z);
  prefs.putFloat("enterHoch", enterHoch);

  USBSerial.println("Kalibrierung abgeschlossen.");
  USBSerial.printf("N = (%.3f %.3f %.3f)\n", N_dir.x, N_dir.y, N_dir.z);
  USBSerial.printf("H = (%.3f %.3f %.3f)\n", H_dir.x, H_dir.y, H_dir.z);
  USBSerial.printf("enterHoch = %.3f (Arm-oben-Score = %.3f)\n", enterHoch, upScore);

  // Puffer leeren, damit keine alte Bewegung ausgewertet wird
  bufHead = 0; bufCount = 0;
}

// what: 0 = alles, 1 = nur Arm UNTEN, 2 = nur Arm HOCH,
//       3 = nur NICHT MELDEN, 4 = nur TISCH
static void runCalibrationPart(int what) {
  if (what == 4) {
    T_dir = collectTisch();
    tischCalibrated = true;
    finishCalibration(true, -10.0f);
    return;
  }

  float maxScore = -10.0f;
  if (what == 0 || what == 1) N_dir = collectArmUnten();
  if (what == 0 || what == 2) H_dir = collectArmHoch();
  if (what == 0 || what == 3) maxScore = collectNichtMelden();
  bool resetSeit = (what == 0 || what == 1 || what == 2);
  finishCalibration(resetSeit, maxScore);
}

static void runCalibration() {
  runCalibrationPart(0);
}

// Gespeicherte Kalibrierung löschen -> beim nächsten Start beginnt die
// Erst-Kalibrierung wieder von vorn.
static void clearCalibration() {
  calibrated = false;
  tischCalibrated = false;
  N_dir = {0, 0, 1};
  H_dir = {0, 0, 1};
  T_dir = {0, 0, 1};
  meldungenSeitCalib = 0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      R_model[i][j] = (i == j) ? 1.0f : 0.0f;

  prefs.putInt("calib", 0);
  prefs.putInt("calibT", 0);
  prefs.putFloat("Nx", 0); prefs.putFloat("Ny", 0); prefs.putFloat("Nz", 1);
  prefs.putFloat("Hx", 0); prefs.putFloat("Hy", 0); prefs.putFloat("Hz", 1);
  prefs.putFloat("Tx", 0); prefs.putFloat("Ty", 0); prefs.putFloat("Tz", 1);
  prefs.putFloat("enterHoch", 0.10f);
  prefs.putInt("seitCalib", 0);
  USBSerial.println("Kalibrierung geloescht.");
}

// ---------------------------------------------------------------------------
// Zeichnen
// ---------------------------------------------------------------------------
static void centerText(int y, const char *s, uint16_t color, int scale) {
  int cw = 6 * scale;
  int w = strlen(s) * cw;
  canvas->setTextSize(scale);
  canvas->setTextColor(color);
  canvas->setCursor(LCD_WIDTH / 2 - w / 2, y);
  canvas->print(s);
}

static void formatMMSS(char *buf, size_t n, unsigned long ms) {
  unsigned long sec = ms / 1000UL;
  snprintf(buf, n, "%02lu:%02lu", sec / 60UL, sec % 60UL);
}

static void drawViewCounter() {
  char buf[24];

  // Uhrzeit (gecacht)
  if (cachedH >= 0) snprintf(buf, sizeof(buf), "%02d:%02d", cachedH, cachedM);
  else snprintf(buf, sizeof(buf), "--:--");
  centerText(80, buf, WHITE, 3);

  // Akku
  if (cachedPct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", cachedPct);
    canvas->setTextSize(2);
    canvas->setTextColor(GREEN);
    canvas->setCursor(LCD_WIDTH / 2 - 30, 128);
    canvas->print(buf);
  }

  // Große Zahl
  snprintf(buf, sizeof(buf), "%d", totalHeute);
  int scale = 6;
  int len = strlen(buf);
  if (len > 3) scale = 4;
  if (len > 5) scale = 3;
  centerText(220, buf, YELLOW, scale);

  centerText(305, "Meldungen heute", WHITE, 2);

  // Arm-Zustand
  const char *zustand;
  uint16_t zc;
  if (!sensorOn) {
    zustand = "Sensor AUS";
    zc = RED;
  } else {
    zustand = imHoch ? "ARM OBEN" : "arm unten";
    zc = imHoch ? ORANGE : GREEN;
  }
  centerText(355, zustand, zc, 3);

  // Position aus Modell
  const char *posName[3] = {"KOPF", "MELDUNG", "TISCH"};
  snprintf(buf, sizeof(buf), "Pos: %s  p=%d%%",
           posName[aktuellKlasse], (int)(aktuellProb[aktuellKlasse] * 100));
  centerText(398, buf, CYAN, 2);

  if (meldungenSeitCalib >= 10) {
    centerText(440, "Bitte neu kalibrieren!", RED, 2);
  } else if (showResetHint && millis() - resetHintMs < 1200) {
    centerText(440, "Zurueckgesetzt!", RED, 2);
  } else {
    centerText(440, "Tippen: Statistik", 0x8410 /* grau */, 2);
  }
}

static void drawViewStats() {
  char buf[48];

  if (cachedH >= 0) snprintf(buf, sizeof(buf), "%02d:%02d", cachedH, cachedM);
  else snprintf(buf, sizeof(buf), "--:--");
  centerText(70, buf, WHITE, 3);

  snprintf(buf, sizeof(buf), "Heute: %d", totalHeute);
  centerText(118, buf, YELLOW, 3);
  snprintf(buf, sizeof(buf), "Session: %d", sessionCount);
  centerText(156, buf, CYAN, 2);
  snprintf(buf, sizeof(buf), "Drangenommen: %d", drange);
  centerText(176, buf, GREEN, 2);
  char zeitbuf[16];
  formatMMSS(zeitbuf, sizeof(zeitbuf), meldeZeitMs);
  snprintf(buf, sizeof(buf), "Meldzeit: %s", zeitbuf);
  centerText(196, buf, CYAN, 2);
  snprintf(buf, sizeof(buf), "Richtig: %d   Falsch: %d", richtig, falsch);
  centerText(216, buf, WHITE, 2);

  // Max für Skalierung
  int mx = 1;
  for (int i = 0; i < 60; i++) if (minHist[i] > mx) mx = minHist[i];

  snprintf(buf, sizeof(buf), "pro Minute (max %d)", mx);
  centerText(236, buf, WHITE, 2);

  // Balkendiagramm (60 Balken)
  int chartX = 45, chartW = 320, chartBottom = 400, chartTop = 250;
  canvas->drawFastHLine(chartX - 5, chartBottom + 2, chartW + 10, 0x8410);
  for (int i = 0; i < 60; i++) {
    int idx = (minHistIdx + i) % 60;   // älteste zuerst
    int h = (int)((long)minHist[idx] * (chartBottom - chartTop) / mx);
    int x = chartX + i * (chartW / 60);
    int w = (chartW / 60) - 1;
    if (w < 1) w = 1;
    uint16_t c = (minHist[idx] > 0) ? GREEN : 0x2104;
    canvas->fillRect(x, chartBottom - h, w, h, c);
  }

  centerText(440, "Tippen: Kalibrieren", 0x8410, 2);
}

#define CALIB_START_X 40
#define CALIB_START_Y 330
#define CALIB_START_W 330
#define CALIB_START_H 65

static void drawViewCalibration() {
  centerText(120, "KALIBRIERUNG", YELLOW, 3);
  centerText(200, "1) Arm UNTEN  10x", WHITE, 2);
  centerText(230, "2) Arm HOCH  10x", WHITE, 2);
  centerText(260, "3) NICHT MELDEN  10x", WHITE, 2);
  centerText(290, "4) TISCH  10x", WHITE, 2);

  canvas->fillRoundRect(CALIB_START_X, CALIB_START_Y, CALIB_START_W, CALIB_START_H, 12, 0x18E3);
  canvas->drawRoundRect(CALIB_START_X, CALIB_START_Y, CALIB_START_W, CALIB_START_H, 12, GREEN);
  centerText(CALIB_START_Y + 20, "AUSWAHL STARTEN", WHITE, 3);

  char buf[32];
  if (calibrated) snprintf(buf, sizeof(buf), "kalibriert (%d Meldungen)", meldungenSeitCalib);
  else snprintf(buf, sizeof(buf), "nicht kalibriert");
  uint16_t sc = (meldungenSeitCalib >= 10) ? RED : GREEN;
  centerText(412, buf, sc, 2);
  centerText(445, "Tippen = Zaehler", 0x8410, 2);
}

// ---------------------------------------------------------------------------
// Home-Button + Launcher
// ---------------------------------------------------------------------------
#define HOME_BTN_X (LCD_WIDTH - 95)
#define HOME_BTN_Y 22
#define HOME_BTN_W 75
#define HOME_BTN_H 38

#define BACK_BTN_X 40
#define BACK_BTN_Y 445
#define BACK_BTN_W 130
#define BACK_BTN_H 40

static bool inRect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh) {
  return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}
static bool inHomeButton(uint16_t x, uint16_t y) {
  return inRect(x, y, HOME_BTN_X, HOME_BTN_Y, HOME_BTN_W, HOME_BTN_H);
}
static bool inBackButton(uint16_t x, uint16_t y) {
  return inRect(x, y, BACK_BTN_X, BACK_BTN_Y, BACK_BTN_W, BACK_BTN_H);
}

static void textCenterX(int cx, int y, const char *s, uint16_t color, int scale) {
  int cw = 6 * scale;
  int w = strlen(s) * cw;
  canvas->setTextSize(scale);
  canvas->setTextColor(color);
  canvas->setCursor(cx - w / 2, y);
  canvas->print(s);
}

static void drawHomeButton() {
  canvas->fillRoundRect(HOME_BTN_X, HOME_BTN_Y, HOME_BTN_W, HOME_BTN_H, 8, 0x4228);
  canvas->drawRoundRect(HOME_BTN_X, HOME_BTN_Y, HOME_BTN_W, HOME_BTN_H, 8, WHITE);
  textCenterX(HOME_BTN_X + HOME_BTN_W / 2, HOME_BTN_Y + 10, "HOME", WHITE, 2);
}

static void drawBackButton() {
  canvas->fillRoundRect(BACK_BTN_X, BACK_BTN_Y, BACK_BTN_W, BACK_BTN_H, 8, 0x4228);
  canvas->drawRoundRect(BACK_BTN_X, BACK_BTN_Y, BACK_BTN_W, BACK_BTN_H, 8, WHITE);
  textCenterX(BACK_BTN_X + BACK_BTN_W / 2, BACK_BTN_Y + 10, "ZURUECK", WHITE, 2);
}

// ---------------------------------------------------------------------------
// Watchfaces (5 Stück)
// ---------------------------------------------------------------------------
static uint16_t battColor(int pct) {
  if (pct < 20) return RED;
  if (pct < 50) return ORANGE;
  return GREEN;
}

static void timeLine(char *buf, size_t n, bool seconds) {
  if (cachedH >= 0) {
    if (seconds) snprintf(buf, n, "%02d:%02d:%02d", cachedH, cachedM, cachedS);
    else snprintf(buf, n, "%02d:%02d", cachedH, cachedM);
  } else {
    if (seconds) snprintf(buf, n, "--:--:--");
    else snprintf(buf, n, "--:--");
  }
}

static void dateLine(char *buf, size_t n) {
  if (cachedDay >= 1 && cachedMon >= 1 && cachedYr >= 0) {
    int wd = weekdayOf(cachedDay, cachedMon, 2000 + cachedYr);
    snprintf(buf, n, "%s %02d.%02d.%02d", WD_DE[wd], cachedDay, cachedMon, cachedYr);
  } else {
    snprintf(buf, n, "-- --.--.--");
  }
}

static void drawBatteryIcon(int x, int y, int w, int h, int pct) {
  uint16_t c = battColor(pct);
  canvas->drawRoundRect(x, y, w, h, 4, 0x8410);
  canvas->fillRoundRect(x + 3, y + 3, (w - 6) * pct / 100, h - 6, 3, c);
  canvas->fillRect(x + w + 1, y + h / 2 - 5, 4, 10, 0x8410);  // Plus-Pol
}

// dicke Uhrzeiger-Linie
static void drawHand(int cx, int cy, float angleDeg, int len, int w, uint16_t color) {
  float a = angleDeg * PI / 180.0f;
  int x1 = cx + (int)(len * sinf(a));
  int y1 = cy - (int)(len * cosf(a));
  float dx = x1 - cx, dy = y1 - cy;
  float d = sqrtf(dx * dx + dy * dy);
  if (d < 1.0f) return;
  float px = -dy / d, py = dx / d;
  for (int i = -w / 2; i <= w / 2; i++) {
    canvas->drawLine(cx + (int)(px * i), cy + (int)(py * i),
                     x1 + (int)(px * i), y1 + (int)(py * i), color);
  }
}

// 0) Minimal
static void wfMinimal() {
  char buf[48];
  timeLine(buf, sizeof(buf), false);
  centerText(165, buf, WHITE, 5);
  dateLine(buf, sizeof(buf));
  centerText(252, buf, 0x8410, 2);
  canvas->drawFastHLine(110, 300, 190, 0x39C7);
  if (cachedPct >= 0)
    snprintf(buf, sizeof(buf), "%d%%  |  %d Meldungen", cachedPct, totalHeute);
  else
    snprintf(buf, sizeof(buf), "%d Meldungen", totalHeute);
  centerText(362, buf, 0x8410, 2);
}

// 1) Farbig
static void wfColorful() {
  char buf[48];
  const uint16_t cols[6] = {RED, ORANGE, YELLOW, GREEN, CYAN, MAGENTA};
  for (int i = 0; i < 6; i++) {
    float a0 = 180 + i * 30.0f;
    float a1 = (i == 5) ? 359.0f : 180 + (i + 1) * 30.0f;
    canvas->drawArc(205, 150, 62, 76, a0, a1, cols[i]);
  }
  timeLine(buf, sizeof(buf), false);
  centerText(245, buf, YELLOW, 5);
  dateLine(buf, sizeof(buf));
  centerText(310, buf, CYAN, 2);
  if (cachedPct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", cachedPct);
    centerText(365, buf, battColor(cachedPct), 3);
  }
  snprintf(buf, sizeof(buf), "%d", totalHeute);
  centerText(415, buf, GREEN, 3);
  centerText(452, "Meldungen", 0x8410, 2);
}

// 2) Analog
static void wfAnalog() {
  char buf[48];
  int cx = 205, cy = 208, R = 140, RN = R - 55;

  canvas->drawCircle(cx, cy, R, WHITE);
  canvas->drawCircle(cx, cy, R - 8, 0x3186);

  for (int i = 0; i < 60; i++) {
    float a = i * 6.0f * PI / 180.0f;
    float s = sinf(a), c = cosf(a);
    int major = (i % 5 == 0);
    int rOut = R - 8, rIn = major ? R - 30 : R - 16;
    canvas->drawLine(cx + (int)(rIn * s), cy - (int)(rIn * c),
                     cx + (int)(rOut * s), cy - (int)(rOut * c),
                     major ? WHITE : 0x39C7);
  }

  textCenterX(cx, cy - RN - 6, "12", 0x8410, 2);
  textCenterX(cx + RN, cy - 8, "3", 0x8410, 2);
  textCenterX(cx, cy + RN - 10, "6", 0x8410, 2);
  textCenterX(cx - RN, cy - 8, "9", 0x8410, 2);

  if (cachedH >= 0) {
    float ha = (cachedH % 12) * 30.0f + cachedM * 0.5f;
    float ma = cachedM * 6.0f + cachedS * 0.1f;
    float sa = cachedS * 6.0f;
    drawHand(cx, cy, ha, 82, 7, WHITE);
    drawHand(cx, cy, ma, 118, 5, WHITE);
    int sxp = cx + (int)(132 * sinf(sa * PI / 180.0f));
    int syp = cy - (int)(132 * cosf(sa * PI / 180.0f));
    int sxt = cx - (int)(34 * sinf(sa * PI / 180.0f));
    int syt = cy + (int)(34 * cosf(sa * PI / 180.0f));
    canvas->drawLine(sxt, syt, sxp, syp, RED);
  }
  canvas->fillCircle(cx, cy, 8, RED);
  canvas->fillCircle(cx, cy, 3, WHITE);

  dateLine(buf, sizeof(buf));
  centerText(375, buf, 0x8410, 2);
  drawBatteryIcon(60, 418, 120, 32, cachedPct >= 0 ? cachedPct : 0);
  if (cachedPct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", cachedPct);
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE);
    canvas->setCursor(190, 424);
    canvas->print(buf);
  }
  snprintf(buf, sizeof(buf), "%d Mel.", totalHeute);
  canvas->setTextSize(2);
  canvas->setTextColor(YELLOW);
  canvas->setCursor(250, 424);
  canvas->print(buf);
}

// 3) Digital (groß)
static void wfBig() {
  char buf[48];
  timeLine(buf, sizeof(buf), false);
  centerText(150, buf, WHITE, 8);

  if (cachedS >= 0) {
    snprintf(buf, sizeof(buf), ":%02d", cachedS);
    centerText(252, buf, 0x8410, 3);
  }

  dateLine(buf, sizeof(buf));
  centerText(320, buf, CYAN, 2);

  int bx = 70, by = 385, bw = 270, bh = 26;
  canvas->drawRoundRect(bx, by, bw, bh, 8, 0x8410);
  if (cachedPct >= 0) {
    canvas->fillRoundRect(bx + 3, by + 3, (bw - 6) * cachedPct / 100, bh - 6, 6, battColor(cachedPct));
    snprintf(buf, sizeof(buf), "%d%%", cachedPct);
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE);
    canvas->setCursor(bx + bw / 2 - 16, by + 2);
    canvas->print(buf);
  }

  snprintf(buf, sizeof(buf), "%d Meldungen heute", totalHeute);
  centerText(450, buf, YELLOW, 2);
}

// 4) Geometrisch
static void wfGeometric() {
  char buf[48];
  canvas->fillCircle(70, 110, 52, YELLOW);
  canvas->fillCircle(340, 110, 52, CYAN);
  canvas->fillRect(0, 185, 410, 130, MAGENTA);

  timeLine(buf, sizeof(buf), false);
  centerText(215, buf, WHITE, 6);
  dateLine(buf, sizeof(buf));
  centerText(292, buf, BLACK, 2);

  drawBatteryIcon(45, 400, 120, 36, cachedPct >= 0 ? cachedPct : 0);
  if (cachedPct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", cachedPct);
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE);
    canvas->setCursor(175, 408);
    canvas->print(buf);
  }
  canvas->fillRoundRect(240, 395, 130, 44, 10, GREEN);
  snprintf(buf, sizeof(buf), "%d Mel.", totalHeute);
  canvas->setTextSize(2);
  canvas->setTextColor(BLACK);
  canvas->setCursor(250, 407);
  canvas->print(buf);
}

static void drawWatchface() {
  switch (watchface) {
    case 0: wfMinimal(); break;
    case 1: wfColorful(); break;
    case 2: wfAnalog(); break;
    case 3: wfBig(); break;
    default: wfGeometric(); break;
  }
}

// ---------------------------------------------------------------------------
// Einstellungen
// ---------------------------------------------------------------------------
static void applyBrightness() {
  gfx->setBrightness(brightness);
  prefs.putInt("bright", brightness);
}

static void setSensorOn(bool on) {
  sensorOn = on;
  prefs.putInt("sensorOn", on ? 1 : 0);
  if (on) {
    // Puffer leeren, damit beim Einschalten keine alte Bewegung ausgewertet wird
    bufHead = 0;
    bufCount = 0;
    sampleCounter = 0;
    nextSampleUs = micros();
    imHoch = false;
    hochKopf = false;
    hochKopfCount = 0;
    hochMeldungCount = 0;
  }
  USBSerial.printf("Sensor %s\n", on ? "AN" : "AUS");
}

static void drawSettingsMenu() {
  centerText(70, "EINSTELLUNGEN", YELLOW, 3);
  const char *items[4] = {"Helligkeit", "WLAN", "Bluetooth", "Sensor"};
  for (int i = 0; i < 4; i++) {
    int ry = 145 + i * 72;
    canvas->fillRoundRect(40, ry, 330, 64, 12, 0x18E3);
    canvas->drawRoundRect(40, ry, 330, 64, 12, WHITE);
    char buf[48];
    if (i == 0) snprintf(buf, sizeof(buf), "Helligkeit   %d%%", brightness * 100 / 255);
    else if (i == 1) snprintf(buf, sizeof(buf), "WLAN   %s", WiFi.status() == WL_CONNECTED ? "verbunden" : "aus");
    else if (i == 2) snprintf(buf, sizeof(buf), "Bluetooth   %s", btOn ? "an" : "aus");
    else snprintf(buf, sizeof(buf), "Sensor   %s", sensorOn ? "an" : "aus");
    canvas->setTextSize(2);
    canvas->setTextColor(WHITE);
    canvas->setCursor(60, ry + 22);
    canvas->print(buf);
  }
  drawHomeButton();
  centerText(460, "Tippen = auswaehlen", 0x8410, 2);
}

static void drawBrightness() {
  centerText(55, "HELLIGKEIT", YELLOW, 3);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d%%", brightness * 100 / 255);
  centerText(170, buf, CYAN, 5);

  int bx = 40, by = 280, bw = 330, bh = 40;
  canvas->drawRoundRect(bx, by, bw, bh, 8, WHITE);
  int fill = (int)((long)brightness * (bw - 6) / 255);
  canvas->fillRect(bx + 3, by + 3, fill, bh - 6, YELLOW);

  canvas->fillRoundRect(60, 360, 100, 70, 12, RED);
  textCenterX(110, 382, "-", BLACK, 4);
  canvas->fillRoundRect(250, 360, 100, 70, 12, GREEN);
  textCenterX(300, 382, "+", BLACK, 4);
  drawBackButton();
  drawHomeButton();
}

static String wifiSSIDs[10];
static int wifiCount = 0;
static uint8_t wifiAuth[10];

static void wifiScanNow() {
  canvas->fillScreen(BLACK);
  centerText(220, "Scanne WLAN ...", YELLOW, 3);
  canvas->flush();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  wifiCount = n > 10 ? 10 : n;
  for (int i = 0; i < wifiCount; i++) {
    wifiSSIDs[i] = WiFi.SSID(i);
    wifiAuth[i] = (uint8_t)WiFi.encryptionType(i);
  }
  WiFi.scanDelete();
  wifiOffset = 0;
  USBSerial.printf("WLAN-Scan: %d Netze\n", wifiCount);
}

static void wifiConnect(const String &ssid) {
  canvas->fillScreen(BLACK);
  centerText(200, "Verbinde ...", YELLOW, 3);
  canvas->flush();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), wifiPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    USBSerial.printf("WLAN verbunden: %s IP=%s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
  } else {
    USBSerial.println("WLAN-Verbindung fehlgeschlagen.");
  }
  wifiPass = "";
}

static void drawWifi() {
  centerText(55, "WLAN", YELLOW, 3);
  if (WiFi.status() == WL_CONNECTED) {
    char buf[40];
    snprintf(buf, sizeof(buf), "Verbunden: %s", WiFi.SSID().c_str());
    centerText(98, buf, GREEN, 2);
    snprintf(buf, sizeof(buf), "IP: %s", WiFi.localIP().toString().c_str());
    centerText(126, buf, WHITE, 2);
  } else {
    centerText(112, "nicht verbunden", 0x8410, 2);
  }

  canvas->fillRoundRect(40, 145, 190, 42, 10, 0x18E3);
  canvas->drawRoundRect(40, 145, 190, 42, 10, WHITE);
  textCenterX(135, 155, "SCANNEN", WHITE, 2);
  canvas->fillRoundRect(240, 145, 130, 42, 10, 0x4228);
  canvas->drawRoundRect(240, 145, 130, 42, 10, WHITE);
  textCenterX(305, 155, "TRENNEN", WHITE, 2);

  if (wifiCount == 0) {
    centerText(230, "keine Netze - erst Scannen", 0x8410, 2);
  } else {
    for (int r = 0; r < 4; r++) {
      int idx = wifiOffset + r;
      if (idx >= wifiCount) break;
      int y = 205 + r * 52;
      uint16_t c = (wifiAuth[idx] == WIFI_AUTH_OPEN) ? GREEN : 0xE5A0;
      canvas->fillRoundRect(40, y, 330, 46, 8, 0x2104);
      canvas->drawRoundRect(40, y, 330, 46, 8, c);
      String label = wifiSSIDs[idx];
      if (label.length() > 18) label = label.substring(0, 18) + "..";
      if (wifiAuth[idx] != WIFI_AUTH_OPEN) label = "*" + label;
      canvas->setTextSize(2);
      canvas->setTextColor(c);
      canvas->setCursor(52, y + 14);
      canvas->print(label);
    }
  }

  canvas->fillRoundRect(190, 445, 60, 40, 8, 0x4228);
  textCenterX(220, 452, "^", WHITE, 2);
  canvas->fillRoundRect(260, 445, 60, 40, 8, 0x4228);
  textCenterX(290, 452, "v", WHITE, 2);
  drawBackButton();
  drawHomeButton();
}

static const char *PW_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-!@";

static char cycleChar(char c, int dir) {
  int n = strlen(PW_CHARS);
  for (int i = 0; i < n; i++) {
    if (PW_CHARS[i] == c) {
      int j = (i + dir) % n;
      if (j < 0) j += n;
      return PW_CHARS[j];
    }
  }
  return 'a';
}

static void drawPWButton(int col, int row, const char *label) {
  int x = 40 + col * 115;
  int y = 300 + row * 75;
  canvas->fillRoundRect(x, y, 100, 55, 10, 0x4228);
  canvas->drawRoundRect(x, y, 100, 55, 10, WHITE);
  textCenterX(x + 50, y + 18, label, WHITE, 2);
}

static void drawWifiPassword() {
  centerText(45, "PASSWORT", YELLOW, 3);
  centerText(95, wifiTargetSSID.c_str(), CYAN, 2);

  // Eingabe anzeigen
  int scale = 2, cw = 12;
  int len = wifiPass.length();
  int startX = LCD_WIDTH / 2 - len * cw / 2;
  canvas->setTextSize(scale);
  canvas->setTextColor(WHITE);
  canvas->setCursor(startX, 150);
  canvas->print(wifiPass);
  canvas->fillRect(startX + len * cw, 170, cw, 4, YELLOW);   // Cursor am Ende

  // Ziffernblock: 1..9, DEL, 0, OK
  const char *keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "OK"};
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      int idx = r * 3 + c;
      int x = 40 + c * 115;
      int y = 190 + r * 60;
      uint16_t bg = (idx >= 9) ? 0x4228 : 0x18E3;
      canvas->fillRoundRect(x, y, 100, 52, 10, bg);
      canvas->drawRoundRect(x, y, 100, 52, 10, WHITE);
      textCenterX(x + 50, y + 14, keys[idx], WHITE, 2);
    }
  }
  drawBackButton();
  drawHomeButton();
}

static void drawBluetooth() {
  centerText(55, "BLUETOOTH", YELLOW, 3);
  if (btOn) {
    centerText(140, "Aktiviert", GREEN, 3);
    centerText(190, "Name: Meldezaehler", WHITE, 2);
    centerText(220, "Geraet ist sichtbar / pairbar", 0x8410, 2);
  } else {
    centerText(160, "Deaktiviert", 0x8410, 3);
  }
  canvas->fillRoundRect(60, 300, 120, 70, 12, GREEN);
  textCenterX(120, 322, "AN", BLACK, 3);
  canvas->fillRoundRect(230, 300, 120, 70, 12, RED);
  textCenterX(290, 322, "AUS", BLACK, 3);
  drawBackButton();
  drawHomeButton();
}

static void drawSensor() {
  centerText(55, "SENSOR", YELLOW, 3);
  if (sensorOn) {
    centerText(130, "Erkennung aktiv", GREEN, 3);
    centerText(180, "IMU misst Meldungen", WHITE, 2);
    centerText(210, "normaler Energieverbrauch", 0x8410, 2);
  } else {
    centerText(130, "Deaktiviert", 0x8410, 3);
    centerText(180, "spart Energie", WHITE, 2);
    centerText(210, "vermeidet Fehlmeldungen", WHITE, 2);
  }
  canvas->fillRoundRect(60, 300, 120, 70, 12, GREEN);
  textCenterX(120, 322, "AN", BLACK, 3);
  canvas->fillRoundRect(230, 300, 120, 70, 12, RED);
  textCenterX(290, 322, "AUS", BLACK, 3);
  drawBackButton();
  drawHomeButton();
}

#define BLE_SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_TIME_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_CHAR_STATS_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define BLE_CHAR_CLEAR_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define BLE_CHAR_TT_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define BLE_CHAR_LESSON_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"

class MeldeBleCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    if (pChar == pCharTime) {
      String v = pChar->getValue();
      if (v.length() >= 6) {
        RTC_Time t;
        rtcRead(t);
        t.yr = (uint8_t)v[0];
        t.mon = (uint8_t)v[1];
        t.day = (uint8_t)v[2];
        t.h = (uint8_t)v[3];
        t.m = (uint8_t)v[4];
        t.s = (uint8_t)v[5];
        rtcWrite(t);
        updateEnv();
        USBSerial.println("Zeit/Datum per BLE gesetzt");
      }
    } else if (pChar == pCharClear) {
      totalHeute = 0;
      sessionCount = 0;
      minuteCount = 0;
      meldeZeitMs = 0;
      drange = 0;
      richtig = 0;
      falsch = 0;
      for (int i = 0; i < 60; i++) minHist[i] = 0;
      meldLogCount = 0;
      meldLogWrite = 0;
      prefs.putInt("total", 0);
      prefs.putULong("meldezeit", 0);
      prefs.putInt("drange", 0);
      prefs.putInt("richtig", 0);
      prefs.putInt("falsch", 0);
      USBSerial.println("Statistik per BLE geloescht");
    } else if (pChar == pCharTT) {
      handleTTCommand(pChar->getValue());
    }
  }
};

static void btEnable() {
  if (!bleInited) {
    BLEDevice::init("Meldezaehler");
    pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(BLE_SERVICE_UUID);
    pCharTime = pService->createCharacteristic(BLE_CHAR_TIME_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCharStats = pService->createCharacteristic(BLE_CHAR_STATS_UUID, BLECharacteristic::PROPERTY_READ);
    pCharClear = pService->createCharacteristic(BLE_CHAR_CLEAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCharTT = pService->createCharacteristic(BLE_CHAR_TT_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCharLesson = pService->createCharacteristic(BLE_CHAR_LESSON_UUID, BLECharacteristic::PROPERTY_READ);
    static MeldeBleCallbacks cbs;
    pCharTime->setCallbacks(&cbs);
    pCharClear->setCallbacks(&cbs);
    pCharTT->setCallbacks(&cbs);
    pCharStats->setValue(buildStatsJson().c_str());
    pCharLesson->setValue(buildLessonJson().c_str());
    pService->start();
    bleInited = true;
  }
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  if (pAdv) {
    pAdv->addServiceUUID(BLE_SERVICE_UUID);
    pAdv->start();
  }
  btOn = true;
  USBSerial.println("BLE aktiviert (Name: Meldezaehler, Zeit/Statistik-Dienst)");
}

static void btDisable() {
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  if (pAdv) pAdv->stop();
  btOn = false;
  USBSerial.println("BLE deaktiviert.");
}

// ---------------------------------------------------------------------------
// App-Tray + Zifferblatt-Auswahl
// ---------------------------------------------------------------------------
static void drawAppTray() {
  centerText(60, "APPS", YELLOW, 3);
  char buf[32];

  // Meldezähler
  canvas->fillRoundRect(40, 100, 160, 120, 16, 0x18C3);
  canvas->drawRoundRect(40, 100, 160, 120, 16, CYAN);
  textCenterX(120, 135, "MELDE-", BLACK, 2);
  textCenterX(120, 160, "ZAEHLER", BLACK, 2);
  snprintf(buf, sizeof(buf), "Heute: %d", totalHeute);
  textCenterX(120, 190, buf, BLACK, 2);

  // Rekorder
  canvas->fillRoundRect(210, 100, 160, 120, 16, 0x9FE0);
  canvas->drawRoundRect(210, 100, 160, 120, 16, GREEN);
  textCenterX(290, 135, "REKORDER", BLACK, 2);
  textCenterX(290, 165, "Sprach-", BLACK, 2);
  textCenterX(290, 190, "notizen", BLACK, 2);

  // Einstellungen
  canvas->fillRoundRect(40, 230, 160, 120, 16, 0xE5A0);
  canvas->drawRoundRect(40, 230, 160, 120, 16, YELLOW);
  textCenterX(120, 265, "EINSTEL-", BLACK, 2);
  textCenterX(120, 290, "LUNGEN", BLACK, 2);
  textCenterX(120, 320, "WiFi/BT", BLACK, 2);

  // Zeit
  canvas->fillRoundRect(210, 230, 160, 120, 16, 0xD69A);
  canvas->drawRoundRect(210, 230, 160, 120, 16, MAGENTA);
  textCenterX(290, 265, "ZEIT", BLACK, 2);
  textCenterX(290, 290, "Timer +", BLACK, 2);
  textCenterX(290, 320, "Stoppuhr", BLACK, 2);

  // Test (breite Kachel)
  canvas->fillRoundRect(40, 360, 330, 72, 14, 0xBDF7);
  canvas->drawRoundRect(40, 360, 330, 72, 14, WHITE);
  textCenterX(205, 380, "TEST", BLACK, 3);
  textCenterX(205, 412, "Motor/Ton/Sensor/Akku", BLACK, 2);

  drawBackButton();
}

static void drawTestApp() {
  centerText(55, "TEST", YELLOW, 3);
  char buf[48];

  // Motor
  canvas->fillRoundRect(40, 100, 330, 64, 12, 0x18E3);
  canvas->drawRoundRect(40, 100, 330, 64, 12, WHITE);
  canvas->setTextSize(2);
  canvas->setTextColor(WHITE);
  canvas->setCursor(60, 112);
  canvas->print("MOTOR");
  canvas->setTextColor(CYAN);
  canvas->setCursor(60, 138);
  canvas->print("kurz vibrieren");

  // Ton
  canvas->fillRoundRect(40, 175, 330, 64, 12, 0x18E3);
  canvas->drawRoundRect(40, 175, 330, 64, 12, WHITE);
  canvas->setTextColor(WHITE);
  canvas->setCursor(60, 187);
  canvas->print("TON");
  canvas->setTextColor(CYAN);
  canvas->setCursor(60, 213);
  canvas->print("440 Hz abspielen");

  // Sensor (live)
  canvas->fillRoundRect(40, 250, 330, 64, 12, 0x18E3);
  canvas->drawRoundRect(40, 250, 330, 64, 12, WHITE);
  canvas->setTextColor(WHITE);
  canvas->setCursor(60, 262);
  canvas->print("SENSOR");
  if (!sensorOn) {
    snprintf(buf, sizeof(buf), "Sensor AUS");
  } else {
    const char *posName[3] = {"KOPF", "MELDUNG", "TISCH"};
    snprintf(buf, sizeof(buf), "Pos: %s  p=%d%%", posName[aktuellKlasse],
             (int)(aktuellProb[aktuellKlasse] * 100));
  }
  canvas->setTextColor(CYAN);
  canvas->setCursor(60, 288);
  canvas->print(buf);

  // Akku (live)
  canvas->fillRoundRect(40, 325, 330, 64, 12, 0x18E3);
  canvas->drawRoundRect(40, 325, 330, 64, 12, WHITE);
  canvas->setTextColor(WHITE);
  canvas->setCursor(60, 337);
  canvas->print("AKKU");
  uint16_t battMV = pmu.getBattVoltage();
  if (battMV > 0) {
    snprintf(buf, sizeof(buf), "%d%%  %.2fV  %s", cachedPct, battMV / 1000.0f,
             pmu.isCharging() ? "laedt" : "Akku");
  } else {
    snprintf(buf, sizeof(buf), "kein Akku");
  }
  canvas->setTextColor(CYAN);
  canvas->setCursor(60, 363);
  canvas->print(buf);

  // Status
  if (testInfo[0] && millis() - testInfoMs < 2500) {
    centerText(415, testInfo, GREEN, 2);
  } else {
    centerText(415, "Tippen = testen", 0x8410, 2);
  }

  drawBackButton();
}

static void drawRecorder() {
  centerText(70, "REKORDER", YELLOW, 3);

  char buf[48];
  if (recording) {
    snprintf(buf, sizeof(buf), "Aufnahme... %.1f s", recLen / 64000.0f);
    centerText(120, buf, RED, 2);
  } else if (playing) {
    centerText(120, "Wiedergabe...", GREEN, 2);
  } else if (recLen > 0) {
    snprintf(buf, sizeof(buf), "Aufnahme: %.1f s", recLen / 64000.0f);
    centerText(120, buf, CYAN, 2);
  } else {
    centerText(120, "Keine Aufnahme", 0x8410, 2);
  }

  canvas->fillRoundRect(40, 150, 330, 70, 14, recording ? RED : 0x18E3);
  canvas->drawRoundRect(40, 150, 330, 70, 14, WHITE);
  textCenterX(205, 172, recording ? "STOPP" : "AUFNEHMEN", BLACK, 3);

  canvas->fillRoundRect(40, 240, 330, 70, 14, playing ? RED : 0x18E3);
  canvas->drawRoundRect(40, 240, 330, 70, 14, WHITE);
  textCenterX(205, 262, playing ? "STOPP" : "ABSPIELEN", BLACK, 3);

  canvas->fillRoundRect(40, 330, 150, 60, 12, 0x4228);
  canvas->drawRoundRect(40, 330, 150, 60, 12, WHITE);
  textCenterX(115, 350, "LOESCHEN", WHITE, 2);

  canvas->fillRoundRect(205, 330, 165, 60, 12, 0x18E3);
  canvas->drawRoundRect(205, 330, 165, 60, 12, WHITE);
  textCenterX(287, 350, "TESTTON", BLACK, 2);

  drawBackButton();
}

static const char *WF_NAMES[5] = {"Minimal", "Farbig", "Analog", "Digital", "Geometrisch"};

static void wfIcon(int i, int cx, int cy) {
  switch (i) {
    case 0: canvas->drawCircle(cx, cy, 14, WHITE); break;
    case 1:
      canvas->fillCircle(cx - 12, cy, 6, RED);
      canvas->fillCircle(cx, cy, 6, GREEN);
      canvas->fillCircle(cx + 12, cy, 6, CYAN);
      break;
    case 2:
      canvas->drawCircle(cx, cy, 14, WHITE);
      canvas->drawLine(cx, cy, cx, cy - 10, WHITE);
      canvas->drawLine(cx, cy, cx + 7, cy + 6, WHITE);
      break;
    case 3: textCenterX(cx, cy - 8, "88", WHITE, 1); break;
    default: canvas->fillRect(cx - 12, cy - 12, 24, 24, MAGENTA); break;
  }
}

static void drawWfPicker() {
  centerText(62, "ZIFERBLATT", YELLOW, 3);
  for (int i = 0; i < 5; i++) {
    int y = 108 + i * 66;
    bool sel = (i == watchfaceSel);
    canvas->fillRoundRect(40, y, 330, 54, 12, sel ? 0x4228 : 0x18E3);
    canvas->drawRoundRect(40, y, 330, 54, 12, sel ? YELLOW : 0x8410);
    canvas->setTextSize(2);
    canvas->setTextColor(sel ? YELLOW : WHITE);
    canvas->setCursor(62, y + 17);
    canvas->print(WF_NAMES[i]);
    wfIcon(i, 320, y + 27);
  }
  drawBackButton();
}

static void drawCalibSelect() {
  centerText(60, "WAS KALIBRIEREN?", YELLOW, 3);
  const char *items[5] = {"KOMPLETT (alles)", "Arm UNTEN", "Arm HOCH", "NICHT MELDEN", "TISCH (Uhr auf Tisch)"};
  for (int i = 0; i < 5; i++) {
    int y = 110 + i * 64;
    canvas->fillRoundRect(40, y, 330, 56, 10, 0x18E3);
    canvas->drawRoundRect(40, y, 330, 56, 10, i == 0 ? YELLOW : 0x8410);
    canvas->setTextSize(2);
    canvas->setTextColor(i == 0 ? YELLOW : WHITE);
    canvas->setCursor(60, y + 17);
    canvas->print(items[i]);
  }
  drawBackButton();
}

static void drawZeitApp() {
  char buf[32];

  // Tabs
  canvas->fillRoundRect(40, 40, 160, 45, 10, zeitTab == 0 ? 0x4228 : 0x18E3);
  canvas->drawRoundRect(40, 40, 160, 45, 10, zeitTab == 0 ? YELLOW : 0x8410);
  textCenterX(120, 52, "TIMER", zeitTab == 0 ? YELLOW : WHITE, 2);

  canvas->fillRoundRect(210, 40, 160, 45, 10, zeitTab == 1 ? 0x4228 : 0x18E3);
  canvas->drawRoundRect(210, 40, 160, 45, 10, zeitTab == 1 ? YELLOW : 0x8410);
  textCenterX(290, 52, "STOPPUHR", zeitTab == 1 ? YELLOW : WHITE, 2);

  if (zeitTab == 0) {
    formatMMSS(buf, sizeof(buf), timerRemainingMs);
    centerText(130, buf, WHITE, 5);

    if (timerRunning) {
      centerText(195, "laeuft ...", GREEN, 2);
      canvas->fillRoundRect(40, 260, 200, 80, 14, 0xE5A0);
      canvas->drawRoundRect(40, 260, 200, 80, 14, WHITE);
      textCenterX(140, 288, "PAUSE", BLACK, 3);
    } else {
      const char *labels[4] = {"MIN -", "MIN +", "SEK -", "SEK +"};
      int xs[4] = {40, 120, 220, 300};
      for (int i = 0; i < 4; i++) {
        canvas->fillRoundRect(xs[i], 195, 70, 50, 10, 0x18E3);
        canvas->drawRoundRect(xs[i], 195, 70, 50, 10, WHITE);
        canvas->setTextSize(2);
        canvas->setTextColor(WHITE);
        canvas->setCursor(xs[i] + 8, 210);
        canvas->print(labels[i]);
      }
      canvas->fillRoundRect(40, 260, 200, 80, 14, GREEN);
      canvas->drawRoundRect(40, 260, 200, 80, 14, WHITE);
      textCenterX(140, 288, "START", BLACK, 3);
    }

    canvas->fillRoundRect(250, 260, 120, 80, 14, RED);
    canvas->drawRoundRect(250, 260, 120, 80, 14, WHITE);
    textCenterX(310, 288, "RESET", BLACK, 3);
  } else {
    unsigned long sw = stopwatchBaseMs;
    if (stopwatchRunning) sw += millis() - stopwatchStartMs;
    unsigned long cs = (sw / 10UL) % 100UL;
    unsigned long sec = sw / 1000UL;
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", sec / 60UL, sec % 60UL, cs);
    centerText(130, buf, WHITE, 5);

    canvas->fillRoundRect(40, 280, 200, 80, 14, stopwatchRunning ? 0xE5A0 : GREEN);
    canvas->drawRoundRect(40, 280, 200, 80, 14, WHITE);
    textCenterX(140, 308, stopwatchRunning ? "STOP" : "START", BLACK, 3);

    canvas->fillRoundRect(250, 280, 120, 80, 14, RED);
    canvas->drawRoundRect(250, 280, 120, 80, 14, WHITE);
    textCenterX(310, 308, "RESET", BLACK, 3);
  }

  drawBackButton();
}

static void drawMeldeEdit() {
  char buf[48];

  if (meldeEditMode == 0) {
    centerText(70, "MELDUNGEN", YELLOW, 3);
    snprintf(buf, sizeof(buf), "Heute: %d", totalHeute);
    centerText(112, buf, WHITE, 2);

    canvas->fillRoundRect(40, 140, 330, 70, 14, 0x4228);
    canvas->drawRoundRect(40, 140, 330, 70, 14, RED);
    textCenterX(205, 162, "LOESCHEN", WHITE, 3);

    canvas->fillRoundRect(40, 225, 330, 70, 14, 0x18E3);
    canvas->drawRoundRect(40, 225, 330, 70, 14, GREEN);
    textCenterX(205, 247, "HINZUFUEGEN", WHITE, 3);

    canvas->fillRoundRect(40, 310, 330, 70, 14, 0x18E3);
    canvas->drawRoundRect(40, 310, 330, 70, 14, CYAN);
    textCenterX(205, 332, "BEARBEITEN", WHITE, 3);

    drawBackButton();
    if (totalHeute <= 0) centerText(452, "Keine Meldung vorhanden", RED, 2);
    else centerText(452, "Power = zurueck", 0x8410, 2);
  } else if (meldeEditMode == 1) {
    centerText(70, "LETZTE MELDUNG", YELLOW, 3);
    snprintf(buf, sizeof(buf), "Heute: %d", totalHeute);
    centerText(112, buf, WHITE, 2);

    canvas->fillRoundRect(40, 180, 330, 110, 16, 0x18E3);
    canvas->drawRoundRect(40, 180, 330, 110, 16, GREEN);
    textCenterX(205, 215, "DRANGENOMMEN", WHITE, 3);
    centerText(258, "als aufgerufen markieren", 0x8410, 2);

    drawBackButton();
  } else {
    centerText(70, "DRANGENOMMEN", GREEN, 3);
    centerText(120, "Antwort war ...", WHITE, 2);

    canvas->fillRoundRect(40, 180, 330, 90, 16, GREEN);
    canvas->drawRoundRect(40, 180, 330, 90, 16, WHITE);
    textCenterX(205, 210, "RICHTIG", BLACK, 3);

    canvas->fillRoundRect(40, 290, 330, 90, 16, RED);
    canvas->drawRoundRect(40, 290, 330, 90, 16, WHITE);
    textCenterX(205, 320, "FALSCH", BLACK, 3);

    drawBackButton();
  }
}

static void renderAndFlush() {
  canvas->fillScreen(BLACK);
  if (screen == 0) {
    drawWatchface();
  } else if (screen == 1) {
    if (view == 0) drawViewCounter();
    else if (view == 1) drawViewStats();
    else {
      if (calibSelectOpen) drawCalibSelect();
      else drawViewCalibration();
    }
    drawHomeButton();
  } else if (screen == 2) { // Einstellungen
    if (settingsItem == 0) drawSettingsMenu();
    else if (settingsItem == 1) drawBrightness();
    else if (settingsItem == 2) {
      if (wifiPasswordMode) drawWifiPassword();
      else drawWifi();
    }
    else if (settingsItem == 3) drawBluetooth();
    else drawSensor();
  } else if (screen == 3) {
    drawWfPicker();
  } else if (screen == 5) {
    drawRecorder();
  } else if (screen == 6) {
    drawMeldeEdit();
  } else if (screen == 7) {
    drawZeitApp();
  } else if (screen == 8) {
    drawTestApp();
  } else { // screen == 4
    drawAppTray();
  }
  canvas->flush();
}

// ---------------------------------------------------------------------------
// Touch-Verarbeitung
// ---------------------------------------------------------------------------
static void meldeTap(uint16_t x, uint16_t y);
static void settingsTap(uint16_t x, uint16_t y);
static void wfPickerTap(uint16_t x, uint16_t y);
static void appTrayTap(uint16_t x, uint16_t y);
static void recorderTap(uint16_t x, uint16_t y);
static void meldeEditTap(uint16_t x, uint16_t y);
static void zeitAppTap(uint16_t x, uint16_t y);
static void testAppTap(uint16_t x, uint16_t y);

static void onTap(uint16_t x, uint16_t y) {
  USBSerial.printf("[touch] screen=%d x=%d y=%d\n", screen, x, y);
  if (screen == 1) meldeTap(x, y);
  else if (screen == 2) settingsTap(x, y);
  else if (screen == 3) wfPickerTap(x, y);
  else if (screen == 4) appTrayTap(x, y);
  else if (screen == 5) recorderTap(x, y);
  else if (screen == 6) meldeEditTap(x, y);
  else if (screen == 7) zeitAppTap(x, y);
  else if (screen == 8) testAppTap(x, y);
  // screen 0 (Watchface): Tap ohne Funktion
}

static void onLongPress(uint16_t x, uint16_t y) {
  if (screen == 1 && view == 0 && !inHomeButton(x, y)) {
    totalHeute = 0;
    sessionCount = 0;
    minuteCount = 0;
    meldeZeitMs = 0;
    drange = 0;
    richtig = 0;
    falsch = 0;
    for (int i = 0; i < 60; i++) minHist[i] = 0;
    meldLogCount = 0;
    meldLogWrite = 0;
    prefs.putInt("total", 0);
    prefs.putULong("meldezeit", 0);
    prefs.putInt("drange", 0);
    prefs.putInt("richtig", 0);
    prefs.putInt("falsch", 0);
    showResetHint = true;
    resetHintMs = millis();
    USBSerial.println("Tageszaehler zurueckgesetzt.");
  }
}

static void onVeryLongPress(uint16_t x, uint16_t y) {
  if (screen == 0) {
    watchfaceSel = watchface;
    screen = 3;
    USBSerial.println("Zifferblatt-Auswahl geoeffnet.");
  } else {
    onLongPress(x, y);
  }
}

static void onSwipeUp(uint16_t x, uint16_t y) {
  if (screen == 0) screen = 4;
}

static void onSwipeDown(uint16_t x, uint16_t y) {
  if (screen == 3 || screen == 4) screen = 0;
}

#define SWIPE_DIST        70
#define LONG_PRESS_MS     700
#define WATCHFACE_HOLD_MS 2000

static void handleTouch() {
  uint16_t x, y;
  bool down = touchRead(x, y);
  if (down && !touchWasDown) {
    touchWasDown = true;
    touchDownMs = millis();
    touchStartX = x; touchStartY = y;
    touchX = x; touchY = y;
  } else if (down && touchWasDown) {
    touchX = x; touchY = y;
  } else if (!down && touchWasDown) {
    touchWasDown = false;
    unsigned long dauer = millis() - touchDownMs;
    int16_t dx = (int16_t)touchX - (int16_t)touchStartX;
    int16_t dy = (int16_t)touchY - (int16_t)touchStartY;
    bool still = (abs(dx) < 40 && abs(dy) < 40);

    if (dauer > WATCHFACE_HOLD_MS && still) {
      onVeryLongPress(touchX, touchY);
    } else if (abs(dy) > SWIPE_DIST && abs(dy) > abs(dx) && dauer < WATCHFACE_HOLD_MS) {
      if (dy < 0) onSwipeUp(touchX, touchY);
      else onSwipeDown(touchX, touchY);
    } else if (dauer > LONG_PRESS_MS && still) {
      onLongPress(touchX, touchY);
    } else if (dauer > 60 && still) {
      onTap(touchX, touchY);
    }
  }
}

// ---------------------------------------------------------------------------
// Physische Tasten (Power + Boot)
// ---------------------------------------------------------------------------
static void enterStandby() {
  standby = true;
  screen = 0;                 // beim Aufwachen auf dem Watchface landen
  gfx->setBrightness(0);
  USBSerial.println("[power] Standby (Display aus, Zaehlung laeuft weiter)");
}

static void wakeFromStandby() {
  standby = false;
  gfx->setBrightness(brightness);
  USBSerial.println("[power] Aufgewacht");
}

static void powerBack() {
  if (screen == 0) {
    enterStandby();
  } else if (screen == 1) {
    if (view == 2 && calibSelectOpen) {
      calibSelectOpen = false;
      return;
    }
    screen = 0;
  } else if (screen == 2) {
    if (wifiPasswordMode) {
      wifiPasswordMode = 0;
      return;
    }
    if (settingsItem == 0) screen = 0;
    else settingsItem = 0;
  } else if (screen == 3) {
    screen = 0;
  } else if (screen == 4) {
    screen = 0;
  } else if (screen == 5) {
    screen = 4;
  } else if (screen == 6) {
    if (meldeEditMode == 2) meldeEditMode = 1;
    else if (meldeEditMode == 1) meldeEditMode = 0;
    else screen = 0;
  } else if (screen == 7) {
    screen = 4;
  } else if (screen == 8) {
    screen = 4;
  }
}

static void powerShortPress() {
  if (standby) {
    wakeFromStandby();
    return;
  }
  powerBack();
}

static void bootPress() {
  USBSerial.println("[boot] Melde-Menue geoeffnet");
  if (standby) wakeFromStandby();
  screen = 6;
  meldeEditMode = 0;
}

static void handleButtons() {
  unsigned long now = millis();

  // Power-Taste: AXP2101 PEK kurzer Druck (Interrupt-Status pollen)
  static unsigned long lastPowerCheckMs = 0;
  if (now - lastPowerCheckMs >= 30) {
    lastPowerCheckMs = now;
    pmu.getIrqStatus();
    if (pmu.isPekeyShortPressIrq()) {
      pmu.clearIrqStatus();
      powerShortPress();
    }
  }

  // Boot-Taste GPIO0 (aktiv LOW) – Auslösen beim Loslassen
  bool b = digitalRead(BOOT_BTN_PIN) == LOW;
  if (b && !bootBtnWasDown) {
    bootBtnWasDown = true;
    bootDownMs = now;
  } else if (!b && bootBtnWasDown) {
    bootBtnWasDown = false;
    unsigned long d = now - bootDownMs;
    if (d >= 30 && d < 1500) bootPress();
  }
}

// Timer-Countdown aktualisieren (läuft auch, wenn die Zeit-App nicht sichtbar ist).
static void updateZeitApp() {
  if (!timerRunning) return;
  unsigned long now = millis();
  unsigned long d = now - timerLastMs;
  timerLastMs = now;
  if (d >= timerRemainingMs) {
    timerRemainingMs = 0;
    timerRunning = false;
    vibrate(250);
    USBSerial.println("[zeit] Timer abgelaufen");
  } else {
    timerRemainingMs -= d;
  }
}

static void wfPickerTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) { screen = 0; return; }
  for (int i = 0; i < 5; i++) {
    if (inRect(x, y, 40, 108 + i * 66, 330, 54)) {
      watchface = i;
      prefs.putInt("wf", i);
      screen = 0;
      USBSerial.printf("Watchface gewechselt: %s\n", WF_NAMES[i]);
      return;
    }
  }
}

static void appTrayTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) { screen = 0; return; }
  if (inRect(x, y, 40, 100, 160, 120)) {
    screen = 1;
    bufHead = bufCount = 0;
  } else if (inRect(x, y, 210, 100, 160, 120)) {
    screen = 5;
  } else if (inRect(x, y, 40, 230, 160, 120)) {
    screen = 2;
    settingsItem = 0;
  } else if (inRect(x, y, 210, 230, 160, 120)) {
    screen = 7;
    zeitTab = 0;
  } else if (inRect(x, y, 40, 360, 330, 72)) {
    screen = 8;
  }
}

static void testAppTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) { screen = 4; return; }
  if (inRect(x, y, 40, 100, 330, 64)) {
    vibrate(300);
    snprintf(testInfo, sizeof(testInfo), "Motor: 300ms");
    testInfoMs = millis();
  } else if (inRect(x, y, 40, 175, 330, 64)) {
    playTestTone();
    snprintf(testInfo, sizeof(testInfo), "Ton: 440 Hz");
    testInfoMs = millis();
  } else if (inRect(x, y, 40, 250, 330, 64)) {
    snprintf(testInfo, sizeof(testInfo), "Sensor-Daten oben");
    testInfoMs = millis();
  } else if (inRect(x, y, 40, 325, 330, 64)) {
    snprintf(testInfo, sizeof(testInfo), "Akku-Daten oben");
    testInfoMs = millis();
  }
}

static void recorderTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) { screen = 4; return; }
  if (inRect(x, y, 40, 150, 330, 70)) {
    if (recording) recStopRecording();
    else recStart();
  } else if (inRect(x, y, 40, 240, 330, 70)) {
    if (playing) playStopPlayback();
    else playStart();
  } else if (inRect(x, y, 40, 330, 150, 60)) {
    recClear();
  } else if (inRect(x, y, 205, 330, 165, 60)) {
    playTestTone();
  }
}

static void calibSelectTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) {
    calibSelectOpen = false;
    return;
  }
  for (int i = 0; i < 5; i++) {
    int ry = 110 + i * 64;
    if (inRect(x, y, 40, ry, 330, 56)) {
      calibSelectOpen = false;
      if (i == 0) runCalibration();          // alles
      else runCalibrationPart(i);            // 1=N, 2=H, 3=NICHT MELDEN, 4=TISCH
      view = 2;
      calibSelectOpen = true;                // zurück zur Auswahl
      return;
    }
  }
}

static void meldeTap(uint16_t x, uint16_t y) {
  if (inHomeButton(x, y)) { screen = 0; return; }
  if (view == 2) {
    if (calibSelectOpen) {
      calibSelectTap(x, y);
    } else if (inRect(x, y, CALIB_START_X, CALIB_START_Y, CALIB_START_W, CALIB_START_H)) {
      calibSelectOpen = true;
      USBSerial.println("Kalibrier-Auswahl geoeffnet.");
    } else {
      view = (view + 1) % 3;  // zurück zum Zähler (Tippen wechselt die Ansicht)
    }
  } else {
    view = (view + 1) % 3;
  }
}

static void meldeEditTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) {
    if (meldeEditMode == 2) meldeEditMode = 1;
    else if (meldeEditMode == 1) meldeEditMode = 0;
    else screen = 0;
    return;
  }

  if (meldeEditMode == 0) {
    if (inRect(x, y, 40, 140, 330, 70)) {
      removeLastMeldung();
    } else if (inRect(x, y, 40, 225, 330, 70)) {
      registerMeldung();
      meldeEditMode = 1;
      USBSerial.println("Meldung hinzugefuegt -> bearbeiten.");
    } else if (inRect(x, y, 40, 310, 330, 70)) {
      if (totalHeute > 0) {
        meldeEditMode = 1;
      } else {
        showResetHint = true;
        resetHintMs = millis();
        USBSerial.println("Keine Meldung zum Bearbeiten.");
      }
    }
  } else if (meldeEditMode == 1) {
    if (inRect(x, y, 40, 180, 330, 110)) {
      drange++;
      saveMeldeExtras();
      meldeEditMode = 2;
      USBSerial.println("Drangenommen +1");
    }
  } else if (meldeEditMode == 2) {
    if (inRect(x, y, 40, 180, 330, 90)) {
      richtig++;
      saveMeldeExtras();
      meldeEditMode = 0;
      USBSerial.println("Richtig +1");
    } else if (inRect(x, y, 40, 290, 330, 90)) {
      falsch++;
      saveMeldeExtras();
      meldeEditMode = 0;
      USBSerial.println("Falsch +1");
    }
  }
}

static void zeitAppTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) { screen = 4; return; }

  // Tabs
  if (inRect(x, y, 40, 40, 160, 45)) { zeitTab = 0; return; }
  if (inRect(x, y, 210, 40, 160, 45)) { zeitTab = 1; return; }

  if (zeitTab == 0) {
    // RESET (immer sichtbar)
    if (inRect(x, y, 250, 260, 120, 80)) {
      timerRemainingMs = timerSetMs;
      timerRunning = false;
      return;
    }
    if (timerRunning) {
      if (inRect(x, y, 40, 260, 200, 80)) {   // PAUSE
        timerRunning = false;
        return;
      }
    } else {
      // Einstell-Buttons
      if (inRect(x, y, 40, 195, 70, 50)) {
        if (timerSetMs >= 60000UL) timerSetMs -= 60000UL;
        timerRemainingMs = timerSetMs;
        return;
      }
      if (inRect(x, y, 120, 195, 70, 50)) {
        if (timerSetMs <= (99UL * 60000UL)) timerSetMs += 60000UL;
        timerRemainingMs = timerSetMs;
        return;
      }
      if (inRect(x, y, 220, 195, 70, 50)) {
        if (timerSetMs >= 10000UL) timerSetMs -= 10000UL;
        timerRemainingMs = timerSetMs;
        return;
      }
      if (inRect(x, y, 300, 195, 70, 50)) {
        if (timerSetMs <= (99UL * 60000UL + 59000UL)) timerSetMs += 10000UL;
        timerRemainingMs = timerSetMs;
        return;
      }
      if (inRect(x, y, 40, 260, 200, 80)) {   // START
        if (timerRemainingMs == 0) timerRemainingMs = timerSetMs;
        timerRunning = true;
        timerLastMs = millis();
        return;
      }
    }
  } else {
    // Stoppuhr
    if (inRect(x, y, 250, 280, 120, 80)) {     // RESET
      stopwatchRunning = false;
      stopwatchBaseMs = 0;
      return;
    }
    if (inRect(x, y, 40, 280, 200, 80)) {      // START / STOP
      if (stopwatchRunning) {
        stopwatchBaseMs += millis() - stopwatchStartMs;
        stopwatchRunning = false;
      } else {
        stopwatchStartMs = millis();
        stopwatchRunning = true;
      }
      return;
    }
  }
}

static void wifiPasswordTap(uint16_t x, uint16_t y) {
  if (inBackButton(x, y)) { wifiPasswordMode = 0; return; }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      int idx = r * 3 + c;
      int bx = 40 + c * 115, by = 190 + r * 60;
      if (!inRect(x, y, bx, by, 100, 52)) continue;

      if (idx < 9) {                    // Ziffer 1..9
        if (wifiPass.length() < 20) wifiPass += (char)('1' + idx);
      } else if (idx == 9) {            // DEL
        if (wifiPass.length() > 0) wifiPass.remove(wifiPass.length() - 1, 1);
      } else if (idx == 10) {           // 0
        if (wifiPass.length() < 20) wifiPass += '0';
      } else {                          // OK
        String ssid = wifiTargetSSID;
        wifiPasswordMode = 0;
        wifiConnect(ssid);
      }
      return;
    }
  }
}

static void settingsTap(uint16_t x, uint16_t y) {
  if (inHomeButton(x, y)) { screen = 0; return; }

  if (settingsItem == 0) {
    if (inRect(x, y, 40, 145, 330, 64)) settingsItem = 1;
    else if (inRect(x, y, 40, 217, 330, 64)) settingsItem = 2;
    else if (inRect(x, y, 40, 289, 330, 64)) settingsItem = 3;
    else if (inRect(x, y, 40, 361, 330, 64)) settingsItem = 4;
  } else if (settingsItem == 1) {
    if (inBackButton(x, y)) settingsItem = 0;
    else if (inRect(x, y, 60, 360, 100, 70)) { brightness -= 20; if (brightness < 10) brightness = 10; applyBrightness(); }
    else if (inRect(x, y, 250, 360, 100, 70)) { brightness += 20; if (brightness > 255) brightness = 255; applyBrightness(); }
  } else if (settingsItem == 2) {
    if (wifiPasswordMode) { wifiPasswordTap(x, y); return; }
    if (inBackButton(x, y)) settingsItem = 0;
    else if (inRect(x, y, 40, 145, 190, 42)) wifiScanNow();
    else if (inRect(x, y, 240, 145, 130, 42)) WiFi.disconnect();
    else if (inRect(x, y, 190, 445, 60, 40)) { if (wifiOffset > 0) wifiOffset--; }
    else if (inRect(x, y, 260, 445, 60, 40)) { if (wifiOffset < wifiCount - 4) wifiOffset++; }
    else {
      for (int r = 0; r < 4; r++) {
        int idx = wifiOffset + r;
        if (idx >= wifiCount) break;
        if (inRect(x, y, 40, 205 + r * 52, 330, 46)) {
          if (wifiAuth[idx] == WIFI_AUTH_OPEN) {
            wifiPass = "";
            wifiConnect(wifiSSIDs[idx]);
          } else {
            wifiTargetSSID = wifiSSIDs[idx];
            wifiPass = "";
            wifiCursor = 0;
            wifiPasswordMode = 1;
          }
          break;
        }
      }
    }
  } else if (settingsItem == 3) {
    if (inBackButton(x, y)) settingsItem = 0;
    else if (inRect(x, y, 60, 300, 120, 70)) btEnable();
    else if (inRect(x, y, 230, 300, 120, 70)) btDisable();
  } else if (settingsItem == 4) {
    if (inBackButton(x, y)) settingsItem = 0;
    else if (inRect(x, y, 60, 300, 120, 70)) setSensorOn(true);
    else if (inRect(x, y, 230, 300, 120, 70)) setSensorOn(false);
  }
}

// ---------------------------------------------------------------------------
// Serielle Befehle
// ---------------------------------------------------------------------------
static void handleSerial() {
  static String line;
  while (USBSerial.available()) {
    char c = (char)USBSerial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) {
        if (line == "RESET") {
          totalHeute = 0;
          sessionCount = 0;
          minuteCount = 0;
          meldeZeitMs = 0;
          drange = 0;
          richtig = 0;
          falsch = 0;
          for (int i = 0; i < 60; i++) minHist[i] = 0;
          meldLogCount = 0;
          meldLogWrite = 0;
          prefs.putInt("total", 0);
          prefs.putULong("meldezeit", 0);
          prefs.putInt("drange", 0);
          prefs.putInt("richtig", 0);
          prefs.putInt("falsch", 0);
          USBSerial.println("OK total=0");
        } else if (line == "CALCLEAR") {
          clearCalibration();
          USBSerial.println("OK Kalibrierung geloescht, Neustart ...");
          USBSerial.flush();
          delay(200);
          ESP.restart();
        } else if (line == "CAL") {
          USBSerial.println("Neue Kalibrierung ...");
          runCalibration();
        } else if (line == "STATS") {
          USBSerial.printf("total=%d session=%d drange=%d richtig=%d falsch=%d view=%d klasse=%d p(meldung)=%d%%\n",
                           totalHeute, sessionCount, drange, richtig, falsch, view, aktuellKlasse,
                           (int)(aktuellProb[1] * 100));
        } else if (line == "CALIB") {
          USBSerial.printf("N=(%.3f %.3f %.3f) H=(%.3f %.3f %.3f) enterHoch=%.3f seitCalib=%d\n",
                           N_dir.x, N_dir.y, N_dir.z, H_dir.x, H_dir.y, H_dir.z,
                           enterHoch, meldungenSeitCalib);
        } else if (line.startsWith("RESTORE ")) {
          int total; float nx, ny, nz, hx, hy, hz, eh; int seit;
          if (sscanf(line.c_str(), "RESTORE %d %f %f %f %f %f %f %f %d",
                     &total, &nx, &ny, &nz, &hx, &hy, &hz, &eh, &seit) == 9) {
            totalHeute = total;
            N_dir = vnorm({nx, ny, nz});
            H_dir = vnorm({hx, hy, hz});
            enterHoch = eh;
            meldungenSeitCalib = seit;
            Vec3 T = {REF_TISCH[0], REF_TISCH[1], REF_TISCH[2]};
            Vec3 M = {REF_MELDUNG[0], REF_MELDUNG[1], REF_MELDUNG[2]};
            buildRotation(N_dir, H_dir, T, M, R_model);
            calibrated = true;
            prefs.putInt("calib", 1);
            prefs.putFloat("Nx", N_dir.x); prefs.putFloat("Ny", N_dir.y); prefs.putFloat("Nz", N_dir.z);
            prefs.putFloat("Hx", H_dir.x); prefs.putFloat("Hy", H_dir.y); prefs.putFloat("Hz", H_dir.z);
            prefs.putFloat("enterHoch", enterHoch);
            prefs.putInt("seitCalib", meldungenSeitCalib);
            prefs.putInt("total", totalHeute);
            bufHead = bufCount = 0;
            USBSerial.println("OK Kalibrierung wiederhergestellt");
          } else {
            USBSerial.println("Syntax: RESTORE total Nx Ny Nz Hx Hy Hz enterHoch seitCalib");
          }
        } else if (line == "TT") {
          for (int d = 0; d < MAX_DAYS; d++) {
            for (int p = 0; p < ttCount[d]; p++) {
              USBSerial.printf("Tag %d [%d] %s %02d:%02d-%02d:%02d = %d\n",
                               d, p, ttDays[d][p].name, ttDays[d][p].sh, ttDays[d][p].sm,
                               ttDays[d][p].eh, ttDays[d][p].em, lessonCounts[d][p]);
            }
          }
        } else if (line == "TON") {
          playTestTone();
        } else if (line == "PEAK") {
          int16_t pk = recPeak();
          USBSerial.printf("Aufnahme: %lu Bytes, Peak=%d (%.1f%%)\n",
                           (unsigned long)recLen, pk, pk * 100.0f / 32767.0f);
        } else if (line == "REC") {
          recStart();
          USBSerial.println("Aufnahme gestartet.");
        } else if (line == "STOP") {
          recStopRecording();
          USBSerial.println("Aufnahme gestoppt.");
        } else if (line == "VIB") {
          USBSerial.println("Vibrationstest ...");
          vibrate(300);
        } else if (line.startsWith("TIME ")) {
          RTC_Time t; rtcRead(t);
          int hh, mm, ss;
          if (sscanf(line.c_str(), "TIME %d:%d:%d", &hh, &mm, &ss) == 3) {
            t.h = hh; t.m = mm; t.s = ss;
            rtcWrite(t);
            USBSerial.println("OK Zeit gesetzt");
          }
        } else if (line.startsWith("DATE ")) {
          RTC_Time t; rtcRead(t);
          int dd, mo, yy;
          if (sscanf(line.c_str(), "DATE %d.%d.%d", &dd, &mo, &yy) == 3) {
            t.day = dd; t.mon = mo; t.yr = yy;
            rtcWrite(t);
            USBSerial.println("OK Datum gesetzt");
          }
        } else {
          USBSerial.println("Unbekannt. Befehle: RESET, CAL, CALIB, STATS, VIB, TIME HH:MM:SS, DATE DD.MM.YY");
        }
      }
      line = "";
    } else {
      line += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
