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
 *    - BOOT 2x schnell: TISCH-Position neu lernen (Uhr liegt auf dem Tisch)
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
#include <esp_sleep.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <BLEService.h>
#include <BLECharacteristic.h>
#include <BLEUtils.h>
#include "HWCDC.h"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

HWCDC USBSerial;
Preferences prefs;
XPowersPMU pmu;

#include <SD_MMC.h>

// ---------------------------------------------------------------------------
// CNN-Erkennung (TensorFlow Lite Micro, int8-quantisiert)
// USE_CNN = 1: CNN (TFLite-Micro) | 0: bisherige LogisticRegression
// ---------------------------------------------------------------------------
#define USE_CNN 1

#if USE_CNN
#include <TensorFlowLite_ESP32.h>
#include <SD_MMC.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "modell_cnn_data.h"
#endif

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
#define PWR_KEY_PIN  10    // Power-Taste (SYS_OUT, gepuffert; Ruhe LOW, Drücken -> HIGH)

// SD-Karte (microSD, 1-Bit-SDMMC) – für CNN-Modell + Sensor-Aufnahme
#define SDMMC_CLK   2
#define SDMMC_CMD   1
#define SDMMC_DATA  3

// Gemeinsame SD-Mount-Hilfe (einmal mounten, CNN + Recorder teilen sich das).
static bool sdMounted = false;

static bool ensureSd() {
  if (sdMounted) return true;
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (SD_MMC.begin("/sdcard", true)) {   // true = 1-Bit-Modus (nur D0 verdrahtet)
    sdMounted = true;
    return true;
  }
  return false;
}

// I2C-Adressen
#define QMI_ADDR    0x6B
#define RTC_ADDR    0x51
#define TOUCH_ADDR  0x38
#define PMU_ADDR    0x34

#include "hal_display.h"

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

// Ein I2C-Burst über Reg 0x02..0x06 (Anzahl, X, Y) statt fünf Einzelzugriffen
static bool touchRead(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) < 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = (uint8_t)Wire.read();
  if (b[0] == 0 || b[0] > 2) return false;
  x = ((b[1] & 0x0F) << 8) | b[2];
  y = ((b[3] & 0x0F) << 8) | b[4];
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
bool motorOn = true;          // Vibrationsmotor global an/aus (Einstellungen)
bool muteInLessons = false;   // Stummschaltung während Unterrichtsstunden
bool inLesson = false;        // wird in updateEnv() aktualisiert

static void motorWrite(bool on) {
  if (!vibPinInit) {
    pinMode(18, OUTPUT);   // ohne OUTPUT-Modus wuerde digitalWrite nur den Pull-up schalten
    vibPinInit = true;
  }
  digitalWrite(18, on ? HIGH : LOW);
}

static bool vibrationAllowed() {
  if (!motorOn) return false;
  if (muteInLessons && inLesson) return false;
  return true;
}

// Wecker-Vibrationsmuster: 3x kurz (kurze Pausen), laengere Pause,
// 3x kurz, laengere Pause, 2x lang (kurze Pause dazwischen), dann von vorn.
// Gerade Indizes = Motor AN, ungerade = Motor AUS.
static const uint16_t ALARM_PATTERN[] = {
  150, 150, 150, 150, 150, 500,
  150, 150, 150, 150, 150, 500,
  400, 150, 400, 150
};
static const int ALARM_PATTERN_LEN = sizeof(ALARM_PATTERN) / sizeof(ALARM_PATTERN[0]);

static bool alarmActive = false;
static int alarmPatternIdx = 0;
static unsigned long alarmNextMs = 0;

static void alarmStart() {
  alarmActive = true;
  alarmPatternIdx = 0;
  alarmNextMs = millis() + ALARM_PATTERN[0];
  if (motorOn) motorWrite(true);
}

static void alarmStop() {
  alarmActive = false;
  motorWrite(false);
}

static void updateAlarm() {
  if (!alarmActive) return;
  unsigned long now = millis();
  if ((long)(now - alarmNextMs) >= 0) {
    alarmPatternIdx = (alarmPatternIdx + 1) % ALARM_PATTERN_LEN;
    alarmNextMs += ALARM_PATTERN[alarmPatternIdx];
    if (motorOn) motorWrite((alarmPatternIdx % 2) == 0);
    else motorWrite(false);
  }
}

// Vibration ohne delay(): vibrate() schaltet den Motor nur ein, updateVibration()
// in loop() schaltet ihn ab bzw. setzt weitere Pulse fort. Vorher blockierte
// jede Meldung loop() für 120 ms – in der Zeit gingen Touch-Gesten verloren.
static bool vibPhaseOn = false;
static int vibPulsesLeft = 0;            // Pulse nach dem laufenden
static unsigned long vibOnMs = 0, vibGapMs = 0, vibNextMs = 0;

static void vibrate(unsigned long ms = 120, int pulses = 1, unsigned long gapMs = 100) {
  if (alarmActive) return;   // Wecker-Muster hat Vorrang
  if (!vibrationAllowed()) {
    USBSerial.printf("[vib] Vibration unterdrueckt\n");
    return;
  }
  vibOnMs = ms;
  vibGapMs = gapMs;
  vibPulsesLeft = pulses - 1;
  vibPhaseOn = true;
  vibNextMs = millis() + ms;
  motorWrite(true);
}

static void updateVibration() {
  if (!vibPhaseOn && vibPulsesLeft <= 0) return;
  if (alarmActive) {         // Wecker hat den Motor übernommen
    vibPhaseOn = false;
    vibPulsesLeft = 0;
    return;
  }
  unsigned long now = millis();
  if ((long)(now - vibNextMs) < 0) return;
  if (vibPhaseOn) {
    motorWrite(false);
    vibPhaseOn = false;
    vibNextMs = now + vibGapMs;
  } else {
    vibPulsesLeft--;
    vibPhaseOn = true;
    motorWrite(true);
    vibNextMs = now + vibOnMs;
  }
}

// Für modale Abläufe (Kalibrierung, TISCH-Messung), die loop() nicht
// durchlaufen: dort muss der Motor vor dem Weitermachen wieder aus sein.
static void vibrateBlocking(unsigned long ms) {
  vibrate(ms);
  if (!vibPhaseOn) return;   // unterdrückt (Motor aus / Stumm / Wecker)
  delay(ms);
  updateVibration();
}

// ---------------------------------------------------------------------------
// Portiertes Modell: StandardScaler + LogisticRegression (3 Klassen)
// Klassen-Reihenfolge: 0=meldung, 1=nicht_meldung
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

#if USE_CNN
// Per-Kanal-Standardisierung des CNN (aus modell_cnn_meta.joblib).
// Kanäle: 0..2 = Beschleunigung (g), 3..5 = Drehrate (dps)
static const float CNN_MEAN[6] = {-0.517373979f, 0.144679025f, 0.636336744f, 7.82536364f, 0.632597327f, 7.15408325f};
static const float CNN_STD[6]  = {0.412273735f, 0.468833745f, 0.34916544f, 74.5159683f, 86.1606064f, 78.6526871f};

// Tensor-Arena + Interpreter (int8-CNN, 1,5-s-Fenster x 6 Kanäle)
static constexpr int CNN_ARENA_SIZE = 32 * 1024;
static uint8_t cnnArena[CNN_ARENA_SIZE];
static bool cnnTried = false;
static bool cnnOk = false;
static tflite::MicroErrorReporter cnnReporter;
static tflite::MicroInterpreter *cnnInterp = nullptr;
static TfLiteTensor *cnnInput = nullptr;
static TfLiteTensor *cnnOutput = nullptr;

// Optional: CNN-Modell von der SD-Karte (/model.tflite) statt aus dem Flash
#define CNN_MODEL_MAX_BYTES (32 * 1024)
alignas(8) static uint8_t cnnModelSdBuf[CNN_MODEL_MAX_BYTES];
static const unsigned char *cnnModelData = modell_cnn_int8_tflite;  // Standard: Flash
#endif

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
#define VIB_REP_MS     100   // kurze Vibration pro Kalibrier-Wiederholung
#define VIB_SCHRITT_MS 500   // längere Vibration nach jedem Kalibrier-Schritt
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

// Daten-Streaming für das PC-Training: STREAM startet, STOPSTREAM beendet.
bool streamMode = false;
unsigned long nextStreamUs = 0;

// Sensor-Aufnahme direkt auf die SD-Karte (Training, ganz ohne PC)
bool sensorRec = false;
File sensorRecFile;
String sensorRecLabel;
uint32_t sensorRecTrial = 0;
int sensorRecCount = 0;
unsigned long sensorRecNextUs = 0;
int recTrialCounter = 1000;   // Startwert, damit Trials nicht mit PC-Daten kollidieren

// Zustand
bool imHoch = false;
unsigned long hochSeitMs = 0;
unsigned long letzteMeldungMs = 0;
bool hochNicht = false;        // finale Entscheidung: "oben" war überwiegend NICHT-MELDEN
int  hochNichtCount = 0;       // wie oft NICHT-MELDEN während "oben" gesehen wurde
int  hochMeldungCount = 0;     // wie oft MELDUNG während "oben" gesehen wurde
int  aktuellKlasse = 1;        // 0=meldung, 1=nicht_meldung
float aktuellProb[2] = {0, 1};
float aktuellScore = -1.0f;

// Statistik
int totalHeute = 0;
int sessionCount = 0;
int lastSession = 0;   // letzte abgeschlossene Session (wird an die App uebertragen)
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

// Oberfläche (Navigation; Masken in ui*.h)
int view = 0;                  // Melden-Seite: 0 = Zähler, 1 = Statistik, 2 = Kalibrierung
int watchface = 0;             // aktuelles Watchface (0..5)

// Standby + Tasten
#define BOOT_DOUBLE_MS 800   // Zeitfenster (ms) für Doppel-Klick auf BOOT
bool standby = false;
bool bootBtnWasDown = false;
unsigned long bootDownMs = 0;
// Doppel-Klick-Erkennung (zwei schnelle BOOT-Drücke)
bool bootDoublePending = false;
unsigned long bootFirstPressMs = 0;

// App-Struktur (Launcher)
int screen = 0;                // 0 = Watchface, 1 = Melden, 2 = Einstellungen, 3 = Zifferblatt, 4 = Apps, 6 = Meldungen bearbeiten, 7 = Zeit, 8 = Test, 9 = Sensor-Aufnahme
int meldeEditMode = 0;         // 0 = Menü (Löschen/Hinzufügen/Bearbeiten), 1 = Bearbeiten, 2 = Richtig/Falsch

// Zeit-App (Timer + Stoppuhr)
int zeitTab = 0;               // 0 = Timer, 1 = Stoppuhr, 2 = Alarm
unsigned long timerSetMs = 5UL * 60UL * 1000UL;
unsigned long timerRemainingMs = 5UL * 60UL * 1000UL;
bool timerRunning = false;
unsigned long timerLastMs = 0;
unsigned long stopwatchBaseMs = 0;
unsigned long stopwatchStartMs = 0;
bool stopwatchRunning = false;
int brightness = 208;          // 0..255 (CO5300 Normal-Mode-Helligkeit)

// Bluetooth (BLE)
static bool bleInited = false;
bool btOn = false;

// Sensor-Erkennung (IMU) an/aus (Energie sparen / Fehlmeldungen vermeiden)
bool sensorOn = true;

// Umgebungsdaten (1 Hz aktualisiert, damit das Rendern flüssig bleibt)
static int cachedH = -1, cachedM = -1, cachedS = -1;
static int cachedDay = -1, cachedMon = -1, cachedYr = -1, cachedPct = -1;
static int battMV = 0;            // Akkuspannung in mV (für Test-Maske)
static bool battCharging = false;
static unsigned long lastEnvMs = 0;
static unsigned long lastRtcMs = 0;

// Akku-Warnung + Stunden-Auto-Reset
static bool battWarned = false;
static int lastLessonDay = -1;
static int lastLessonIdx = -1;

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
  s += "\"lastSession\":" + String(lastSession) + ",";
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


// Wochentag aus Datum (Sakamoto-Algorithmus), 0=So..6=Sa
static int weekdayOf(int d, int m, int y) {
  static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int findPeriod(int day, int h, int m);  // Vorabdeklaration (Definition weiter unten)

// Spannungsbasierte Akku-Prozent (LiPo-Entladekurve). Der AXP2101-Fuel-Gauge
// ist ohne Kalibrierdaten unzuverlässig; die Batteriespannung ist robuster.
static int battPctFromVoltage(uint16_t mv) {
  if (mv <= 0) return -1;
  if (mv >= 4200) return 100;
  if (mv <= 3300) return 0;
  static const uint16_t v[] = {4200, 4100, 3980, 3870, 3780, 3720, 3670, 3620, 3560, 3470, 3380, 3300};
  static const int    p[] = {100,   92,   82,   72,   60,   50,   40,   30,   20,   12,    5,    0};
  for (int i = 0; i < 11; i++) {
    if (mv <= v[i] && mv >= v[i + 1]) {
      float t = (float)(mv - v[i + 1]) / (v[i] - v[i + 1]);
      return p[i + 1] + (int)(t * (p[i] - p[i + 1]) + 0.5f);
    }
  }
  return 0;
}

// In jedem loop()-Durchlauf aufrufen. Die Uhrzeit wird alle 200 ms gelesen –
// bei nur 1x/s würde die Sekundenanzeige durch Phasenversatz gelegentlich
// springen. Akku, Stunde und BLE-Werte reichen 1x/s.
static void updateEnv() {
  unsigned long now = millis();
  if (now - lastRtcMs >= 200) {
    lastRtcMs = now;
    RTC_Time t;
    if (rtcRead(t)) {
      cachedH = t.h; cachedM = t.m; cachedS = t.s;
      cachedDay = t.day; cachedMon = t.mon; cachedYr = t.yr;
    } else {
      cachedH = cachedM = cachedS = -1;
      cachedDay = cachedMon = cachedYr = -1;
    }
  }
  if (now - lastEnvMs < 1000) return;
  lastEnvMs = now;
  // Akku: spannungsbasiert + gleitend gemittelt (keine Sprünge unter Last)
  static int smoothPct = -1;
  bool battOk = pmu.isBatteryConnect();
  battMV = battOk ? pmu.getBattVoltage() : 0;
  battCharging = battOk && pmu.isCharging();
  int pct = battPctFromVoltage(battMV);
  if (pct >= 0) {
    if (smoothPct < 0) smoothPct = pct;
    else smoothPct = (smoothPct * 3 + pct) / 4;
    cachedPct = smoothPct;
  } else {
    cachedPct = -1;
  }

  // Akku-Warnung: einmalig vibrieren, wenn unter 20 %
  if (cachedPct >= 0 && cachedPct < 20 && !battWarned) {
    battWarned = true;
    vibrate(150, 2, 120);
    USBSerial.printf("[akku] WARNUNG: nur noch %d%%\n", cachedPct);
  }
  if (cachedPct >= 25) battWarned = false;

  // Aktuelle Stunde bestimmen (für Mute + Session-Auto-Reset)
  inLesson = false;
  int curWd = -1, curP = -1;
  if (ttActive && cachedDay >= 1 && cachedMon >= 1 && cachedYr >= 0) {
    curWd = weekdayOf(cachedDay, cachedMon, 2000 + cachedYr);
    curP = findPeriod(curWd, cachedH, cachedM);
    inLesson = (curP >= 0);
  }

  // Session bei Stundenwechsel automatisch zurücksetzen (alte Session sichern)
  if (curWd != lastLessonDay || curP != lastLessonIdx) {
    if (lastLessonDay != -1) {
      lastSession = sessionCount;
      prefs.putInt("lastSession", lastSession);
      sessionCount = 0;
      USBSerial.println("[stunde] Neue Stunde -> Session zurueckgesetzt");
    }
    lastLessonDay = curWd;
    lastLessonIdx = curP;
  }

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

// Nächste Stunde nach der aktuellen Zeit (Index, -1 = keine weitere heute)
static int nextLessonIdx(int day, int h, int m) {
  int cur = h * 60 + m;
  for (int p = 0; p < ttCount[day]; p++) {
    int s = ttDays[day][p].sh * 60 + ttDays[day][p].sm;
    if (s > cur) return p;
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

// Alle Meldungen/Statistiken des Tages zuruecksetzen (Kalibrierung + Stundenplan bleiben)
static void resetAllStats() {
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
  USBSerial.println("Alle Meldungen geloescht.");
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

// Softmax über die alten 3 LogReg-Klassen, gemappt auf 2 Klassen
// (0 = meldung, 1 = nicht_meldung = kopf + tisch).
static int predictClass(Vec3 m, float accStd, float gmean, float gmax,
                        float prob[2]) {
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
  prob[0] = e1 / s;              // meldung
  prob[1] = (e0 + e2) / s;       // nicht_meldung (kopf + tisch)

  return prob[0] >= prob[1] ? 0 : 1;
}

#if USE_CNN
// ---------------------------------------------------------------------------
// CNN (TFLite-Micro): Einmalige Initialisierung (lazy, beim ersten Aufruf)
// ---------------------------------------------------------------------------
// Lädt das Modell bevorzugt von der SD-Karte (/model.tflite), sonst aus dem Flash.
static bool loadCnnModelFromSd() {
  if (!ensureSd()) {
    USBSerial.println("[cnn] SD-Karte nicht lesbar - nutze eingebautes Modell");
    return false;
  }
  File f = SD_MMC.open("/model.tflite", FILE_READ);
  if (!f) {
    USBSerial.println("[cnn] /model.tflite nicht gefunden - nutze eingebautes Modell");
    return false;
  }
  size_t n = f.size();
  if (n < 8 || n > CNN_MODEL_MAX_BYTES) {
    USBSerial.printf("[cnn] /model.tflite ungueltige Groesse (%u Bytes)\n", (unsigned)n);
    f.close();
    return false;
  }
  size_t r = f.read(cnnModelSdBuf, n);
  f.close();
  if (r != n) {
    USBSerial.println("[cnn] /model.tflite Lesefehler - nutze eingebautes Modell");
    return false;
  }
  if (memcmp(cnnModelSdBuf + 4, "TFL3", 4) != 0) {
    USBSerial.println("[cnn] /model.tflite ist keine gueltige TFLite-Datei");
    return false;
  }
  cnnModelData = cnnModelSdBuf;
  USBSerial.printf("[cnn] Modell von SD-Karte geladen (%u Bytes)\n", (unsigned)n);
  return true;
}

static void cnnInit() {
  if (cnnTried) return;
  cnnTried = true;

  // Modellquelle: SD-Karte bevorzugt, sonst eingebautes Flash-Modell
  if (!loadCnnModelFromSd()) {
    cnnModelData = modell_cnn_int8_tflite;
  }

  const tflite::Model *model = tflite::GetModel(cnnModelData);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    if (cnnModelData != modell_cnn_int8_tflite) {
      USBSerial.println("[cnn] SD-Modell ungueltig - nutze eingebautes Modell");
      cnnModelData = modell_cnn_int8_tflite;
      model = tflite::GetModel(cnnModelData);
    }
  }
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    USBSerial.printf("[cnn] Schema-Version %d != %d\n",
                     model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  // Modell muss genau 2 Ausgänge (Klassen) haben; sonst (z. B. altes
  // 4-Klassen-Modell auf der SD-Karte) das eingebaute Modell verwenden.
  if (cnnModelData != modell_cnn_int8_tflite) {
    int nOut = 0;
    const auto *sub = model->subgraphs()->Get(0);
    if (sub && sub->outputs()->size() > 0) {
      int oi = sub->outputs()->Get(0);
      const auto *shape = sub->tensors()->Get(oi)->shape();
      if (shape && shape->size() >= 2) nOut = shape->Get(1);
    }
    if (nOut != 2) {
      USBSerial.printf("[cnn] SD-Modell hat %d Klassen statt 2 - nutze eingebautes Modell\n", nOut);
      cnnModelData = modell_cnn_int8_tflite;
      model = tflite::GetModel(cnnModelData);
    }
  }

  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter interp(model, resolver, cnnArena,
                                         CNN_ARENA_SIZE, &cnnReporter);
  cnnInterp = &interp;
  if (cnnInterp->AllocateTensors() != kTfLiteOk) {
    USBSerial.println("[cnn] AllocateTensors() fehlgeschlagen");
    return;
  }
  cnnInput = cnnInterp->input(0);
  cnnOutput = cnnInterp->output(0);
  cnnOk = true;
  USBSerial.printf("[cnn] bereit (in scale=%.6f zero=%d, out scale=%.6f zero=%d)\n",
                   (double)cnnInput->params.scale, cnnInput->params.zero_point,
                   (double)cnnOutput->params.scale, cnnOutput->params.zero_point);
}

// CNN-Inferenz: 1,5-s-Fenster (150 Samples x 6 Kanäle) aus dem Ringpuffer,
// rotiert ins Trainings-Koordinatensystem, standardisiert und int8-quantisiert.
static int predictClassCNN(float prob[2]) {
  cnnInit();
  if (!cnnOk) return 1;   // sicherer Fallback: NICHT-MELDEN

  const int start = (bufHead - FENSTER + BUF_N) % BUF_N;
  const float inScale = cnnInput->params.scale;
  const int inZero = cnnInput->params.zero_point;

  for (int t = 0; t < FENSTER; t++) {
    int i = (start + t) % BUF_N;
    Vec3 a = {axBuf[i], ayBuf[i], azBuf[i]};
    Vec3 g = {gxBuf[i], gyBuf[i], gzBuf[i]};
    Vec3 ar, gr;
    rotVec(R_model, a, ar);
    rotVec(R_model, g, gr);
    const float f[6] = {ar.x, ar.y, ar.z, gr.x, gr.y, gr.z};
    for (int c = 0; c < 6; c++) {
      float x = (f[c] - CNN_MEAN[c]) / CNN_STD[c];
      int q = (int)lroundf(x / inScale) + inZero;
      if (q < -128) q = -128;
      else if (q > 127) q = 127;
      cnnInput->data.int8[t * 6 + c] = (int8_t)q;
    }
  }

  unsigned long inv0 = micros();
  if (cnnInterp->Invoke() != kTfLiteOk) return 1;

  {
    static unsigned long cnnAcc = 0; static int cnnN = 0;
    cnnAcc += micros() - inv0; cnnN++;
    if (cnnN >= 20) {
      USBSerial.printf("[perf] CNN avg=%lu us\n", cnnAcc / cnnN);
      cnnAcc = 0; cnnN = 0;
    }
  }

  const float outScale = cnnOutput->params.scale;
  const int outZero = cnnOutput->params.zero_point;
  const int nOut = (cnnOutput->dims->size >= 2) ? cnnOutput->dims->data[1] : 1;
  for (int i = 0; i < 2; i++) prob[i] = 0.0f;
  for (int i = 0; i < nOut && i < 2; i++)
    prob[i] = (cnnOutput->data.int8[i] - outZero) * outScale;

  return prob[0] >= prob[1] ? 0 : 1;
}
#endif

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

// Liefert ein bereits rotiertes Sensor-Sample (g, dps) für den PC-Trainings-
// Stream. Die Rotation in das Trainings-Koordinatensystem entspricht exakt
// dem Pfad der CNN-Inferenz, damit neu gesammelte Daten zum Modell passen.
static void streamSample() {
  int16_t ax, ay, az, gx, gy, gz;
  if (!qmiReadData(ax, ay, az, gx, gy, gz)) return;
  Vec3 a = {ax * ACCEL_SCALE, ay * ACCEL_SCALE, az * ACCEL_SCALE};
  Vec3 g = {gx * GYRO_SCALE, gy * GYRO_SCALE, gz * GYRO_SCALE};
  Vec3 ar, gr;
  rotVec(R_model, a, ar);
  rotVec(R_model, g, gr);
  USBSerial.printf("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                   ar.x, ar.y, ar.z, gr.x, gr.y, gr.z);
}

// Sensor-Aufnahme: rotierte Werte (g, dps) als CSV-Zeile auf die SD-Karte.
static void sensorRecSample() {
  int16_t ax, ay, az, gx, gy, gz;
  if (!qmiReadData(ax, ay, az, gx, gy, gz)) return;
  Vec3 a = {ax * ACCEL_SCALE, ay * ACCEL_SCALE, az * ACCEL_SCALE};
  Vec3 g = {gx * GYRO_SCALE, gy * GYRO_SCALE, gz * GYRO_SCALE};
  Vec3 ar, gr;
  rotVec(R_model, a, ar);
  rotVec(R_model, g, gr);
  sensorRecFile.printf("%s,%u,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                       sensorRecLabel.c_str(), (unsigned)sensorRecTrial,
                       sensorRecCount, ar.x, ar.y, ar.z, gr.x, gr.y, gr.z);
  sensorRecCount++;
}

static void startSensorRec(const char *label) {
  if (!ensureSd()) {
    USBSerial.println("[sdrec] SD-Karte nicht bereit!");
    return;
  }
  sensorRecFile = SD_MMC.open("/aufnahme.csv", FILE_APPEND);
  if (!sensorRecFile) {
    USBSerial.println("[sdrec] /aufnahme.csv nicht oeffenbar!");
    return;
  }
  if (sensorRecFile.size() == 0) {
    sensorRecFile.println("label,trial,t,ax,ay,az,gx,gy,gz");
  }
  sensorRecLabel = label;
  sensorRecTrial = recTrialCounter++;
  prefs.putInt("recTrial", recTrialCounter);
  sensorRecCount = 0;
  sensorRec = true;
  sensorRecNextUs = micros();
  USBSerial.printf("[sdrec] Aufnahme %s #%u gestartet\n", label, (unsigned)sensorRecTrial);
}

static void stopSensorRec() {
  if (sensorRecFile) {
    sensorRecFile.flush();
    sensorRecFile.close();
  }
  sensorRec = false;
  USBSerial.printf("[sdrec] Aufnahme beendet (%d Samples)\n", sensorRecCount);
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
#if USE_CNN
  aktuellKlasse = predictClassCNN(aktuellProb);
#else
  Stats lang = computeStats(FENSTER);
  Vec3 mSensor = {lang.mx, lang.my, lang.mz};
  Vec3 mRot;
  rotVec(R_model, vnorm(mSensor), mRot);
  aktuellKlasse = predictClass(mRot, lang.accStd, lang.gmean, lang.gmax, aktuellProb);
#endif

  unsigned long now = millis();

  static unsigned long lastDbgMs = 0;
  if (now - lastDbgMs >= 2000) {
    lastDbgMs = now;
    USBSerial.printf("[dbg] score=%+.2f enter=%.2f dotH=%.2f dotN=%.2f klasse=%d p(m/n)=%d/%d%% hoch=%d\n",
                     aktuellScore, enterHoch, dotH, dotN, aktuellKlasse,
                     (int)(aktuellProb[0] * 100), (int)(aktuellProb[1] * 100),
                     imHoch ? 1 : 0);
  }

  if (!imHoch) {
    if (aktuellScore > enterHoch) {
      imHoch = true;
      hochSeitMs = now;
      hochNicht = false;
      hochNichtCount = 0;
      hochMeldungCount = 0;
      USBSerial.printf("[zustand] ARM OBEN (score=%.2f)\n", aktuellScore);
    }
  } else {
    // Modell beobachten: wenn während "oben" überwiegend NICHT-MELDEN
    // erkannt wird, zählt es nicht (z. B. Kopfkratzen, Winken, kleine Bewegungen).
    if (aktuellKlasse == 0) hochMeldungCount++;
    else hochNichtCount++;

    if (aktuellScore < LEAVE_HOCH) {
      unsigned long dauer = now - hochSeitMs;
      hochNicht = (hochNichtCount > hochMeldungCount);
      bool zaehlt = (dauer >= MIN_HALTEN_MS) &&
                    (now - letzteMeldungMs > SPERRE_MS) &&
                    !hochNicht;
      USBSerial.printf("[zustand] arm unten (dauer=%lums klasse=%d meldC=%d nichtC=%d zaehlt=%d)\n",
                       dauer, aktuellKlasse, hochMeldungCount, hochNichtCount,
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
// Anzeige der Kalibrier-Schritte (ui.h); hier nur vorab deklariert
static void uiCalibShow(const char *title, const char *sub, int countdown, int rep, int reps);
static void uiMessage(const char *icon, lv_color_t c, const char *title, const char *sub);
static void uiInvalidateAll();
static void powerBack();

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

// Ein Kalibrier-Schritt: 3-s-Countdown, dann CAL_REPS Wiederholungen mit
// Vibration als Startsignal; measure() misst eine Wiederholung.
template <typename F>
static void calibSteps(const char *title, const char *sub, F measure) {
  for (int i = 3; i >= 1; i--) {
    uiCalibShow(title, sub, i, 0, 0);
    delay(1000);
  }
  for (int i = 1; i <= CAL_REPS; i++) {
    uiCalibShow(title, sub, 0, i, CAL_REPS);
    vibrateBlocking(VIB_REP_MS);
    measure();
  }
  vibrateBlocking(VIB_SCHRITT_MS);
}

static Vec3 collectArmUnten() {
  Vec3 sum = {0, 0, 0};
  calibSteps("Arm unten", "Arm locker hängen lassen", [&]() {
    Vec3 v = collectStaticRep(1500);
    sum = {sum.x + v.x, sum.y + v.y, sum.z + v.z};
  });
  return vnorm(sum);
}

static Vec3 collectArmHoch() {
  Vec3 sum = {0, 0, 0};
  calibSteps("Arm hoch", "wie beim Melden halten", [&]() {
    Vec3 v = collectStaticRep(1500);
    sum = {sum.x + v.x, sum.y + v.y, sum.z + v.z};
  });
  return vnorm(sum);
}

static float collectNichtMelden() {
  float maxScore = -10.0f;
  calibSteps("Nicht melden", "normal bewegen", [&]() {
    float sc = collectNormalRep(1500);
    if (sc > maxScore) maxScore = sc;
  });
  return maxScore;
}

static Vec3 collectTisch() {
  Vec3 sum = {0, 0, 0};
  calibSteps("Tisch", "Uhr flach hinlegen", [&]() {
    Vec3 v = collectStaticRep(1500);
    sum = {sum.x + v.x, sum.y + v.y, sum.z + v.z};
  });
  return vnorm(sum);
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
// Einstellungen
// ---------------------------------------------------------------------------
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
    hochNicht = false;
    hochNichtCount = 0;
    hochMeldungCount = 0;
  }
  USBSerial.printf("Sensor %s\n", on ? "AN" : "AUS");
}

static void setMotorOn(bool on) {
  motorOn = on;
  prefs.putInt("motorOn", on ? 1 : 0);
  USBSerial.printf("Motor %s\n", on ? "AN" : "AUS");
}

static void setMuteInLessons(bool on) {
  muteInLessons = on;
  prefs.putInt("muteLessons", on ? 1 : 0);
  USBSerial.printf("Stumm in Stunden %s\n", on ? "AN" : "AUS");
}

#define BLE_SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_TIME_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_CHAR_STATS_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define BLE_CHAR_CLEAR_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define BLE_CHAR_TT_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define BLE_CHAR_LESSON_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"

// onWrite() läuft im Bluetooth-Task, nicht in loop(). I2C (RTC, PMU, Touch,
// IMU), NVS und die Stundenplan-Arrays sind nicht für gleichzeitigen Zugriff
// aus zwei Tasks ausgelegt – verschränkte I2C-Transaktionen machen u. a. den
// Touch unzuverlässig. Deshalb reiht onWrite() nur ein, loop() führt aus
// (processBleCommands).
enum BleCmdKind : uint8_t { BLE_CMD_TIME, BLE_CMD_CLEAR, BLE_CMD_TT };
struct BleCmd {
  BleCmdKind kind;
  uint8_t len;
  char data[64];             // Stundenplan-Befehl max. ~40 Zeichen, Zeit 6 Bytes
};
static QueueHandle_t bleCmdQueue = nullptr;
static volatile uint32_t bleCmdDropped = 0;

class MeldeBleCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    BleCmd c;
    if (pChar == pCharTime) c.kind = BLE_CMD_TIME;
    else if (pChar == pCharClear) c.kind = BLE_CMD_CLEAR;
    else if (pChar == pCharTT) c.kind = BLE_CMD_TT;
    else return;
    String v = pChar->getValue();
    c.len = (uint8_t)min((size_t)v.length(), sizeof(c.data) - 1);
    memcpy(c.data, v.c_str(), c.len);
    c.data[c.len] = 0;
    // Kurz warten statt verwerfen: die App schreibt den Stundenplan Befehl für Befehl
    if (!bleCmdQueue || xQueueSend(bleCmdQueue, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
      bleCmdDropped = bleCmdDropped + 1;
    }
  }
};

static void processBleCommands() {
  if (bleCmdDropped) {
    USBSerial.printf("[ble] %lu Befehl(e) verworfen (Warteschlange voll)\n",
                     (unsigned long)bleCmdDropped);
    bleCmdDropped = 0;
  }
  if (!bleCmdQueue) return;
  BleCmd c;
  while (xQueueReceive(bleCmdQueue, &c, 0) == pdTRUE) {
    if (c.kind == BLE_CMD_TIME) {
      if (c.len >= 6) {
        RTC_Time t;
        rtcRead(t);
        t.yr = (uint8_t)c.data[0];
        t.mon = (uint8_t)c.data[1];
        t.day = (uint8_t)c.data[2];
        t.h = (uint8_t)c.data[3];
        t.m = (uint8_t)c.data[4];
        t.s = (uint8_t)c.data[5];
        rtcWrite(t);
        lastEnvMs = lastRtcMs = 0;   // Drosselung umgehen -> neue Zeit sofort übernehmen
        updateEnv();
        USBSerial.println("Zeit/Datum per BLE gesetzt");
      }
    } else if (c.kind == BLE_CMD_CLEAR) {
      resetAllStats();
      USBSerial.println("Statistik per BLE geloescht");
    } else {
      handleTTCommand(String(c.data));
    }
  }
}

static void btEnable() {
  if (!bleInited) {
    if (!bleCmdQueue) bleCmdQueue = xQueueCreate(32, sizeof(BleCmd));
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
    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    if (pAdv) {
      pAdv->addServiceUUID(BLE_SERVICE_UUID);
    }
  }
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  if (pAdv) {
    // Langes Advertising-Intervall (1-2 s statt ~100 ms) spart deutlich Akku,
    // solange keine Verbindung zur App besteht.
    pAdv->setMinInterval(1600);   // 1600 * 0,625 ms = 1 s
    pAdv->setMaxInterval(3200);   // 3200 * 0,625 ms = 2 s
    pAdv->start();
  }
  btOn = true;
  prefs.putInt("btOn", 1);
  USBSerial.println("BLE aktiviert (Name: Meldezaehler, Zeit/Statistik-Dienst)");
}

static void btDisable() {
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  if (pAdv) pAdv->stop();
  btOn = false;
  prefs.putInt("btOn", 0);
  USBSerial.println("BLE deaktiviert.");
}

// ---------------------------------------------------------------------------
// Physische Tasten (Power + Boot)
// ---------------------------------------------------------------------------
// Power-Taste (GPIO10 = SYS_OUT) liest das AXP2101-IRQ-Statusregister ohne
// volles pmu.begin() – für den schnellen Deep-Sleep-Aufwach-Check.
static bool peekPowerKeyIrq() {
  Wire.beginTransmission(PMU_ADDR);
  Wire.write(0x49);  // AXP2101 INTSTS2
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)PMU_ADDR, 1) < 1) return false;
  uint8_t sts = (uint8_t)Wire.read();
  return (sts & 0x08) != 0;   // PKEY_SHORT_IRQ = Bit 3 in INTSTS2
}

// Alles aus -> ESP32 komplett schlafen legen, nur die externe RTC läuft weiter.
static void enterDeepSleep() {
  USBSerial.println("[power] Alles aus -> Deep-Sleep (nur RTC laeuft, Power-Taste weckt)");
  USBSerial.flush();
  delay(100);

  halDisplayPower(false);

  // BLE-Werbung stoppen
  if (btOn) {
    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    if (pAdv) pAdv->stop();
    btOn = false;
    prefs.putInt("btOn", 0);
  }

  // Power-Taste (GPIO10 = SYS_OUT) weckt sofort; Polarität automatisch erkennen.
  pinMode(PWR_KEY_PIN, INPUT);
  delay(20);
  bool idleHigh = digitalRead(PWR_KEY_PIN);
  esp_sleep_enable_ext1_wakeup(1ULL << PWR_KEY_PIN,
                               idleHigh ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH);

  // Fallback: alle 3 s kurz aufwachen und Power-Taste per I2C pollen.
  esp_sleep_enable_timer_wakeup(3000000ULL);
  esp_deep_sleep_start();   // kehrt nie zurück
}

static void enterStandby() {
  // Sensor aus + BLE aus -> nur noch RTC nötig -> ESP32 komplett schlafen legen
  // (laufender Timer: wach bleiben, sonst käme der Alarm nie)
  if (!sensorOn && !btOn && !alarmActive && !timerRunning && !streamMode && !sensorRec) {
    enterDeepSleep();
    return;
  }
  standby = true;
  screen = 0;                 // beim Aufwachen auf dem Watchface landen
  halSetBrightness(0);        // nur Display aus – Sensor/Motor zählen weiter
  halDisplayPower(false);     // Display-Controller in Sleep (spart mehr als nur Helligkeit 0)
  USBSerial.println("[power] Standby (Display aus, Zaehlung laeuft weiter)");
}

static void wakeFromStandby() {
  standby = false;
  halDisplayPower(true);
  halSetBrightness(brightness);
  uiInvalidateAll();          // Panel-RAM nach dem Sleep neu beschreiben
  USBSerial.println("[power] Aufgewacht");
}

// powerBack(): Navigation, siehe ui_ctl.h

static void powerShortPress() {
  if (standby) {
    wakeFromStandby();
    return;
  }
  powerBack();
}

static void bootPress() {
  if (alarmActive) {
    alarmStop();
    zeitTab = 0;
  }
  USBSerial.println("[boot] Melde-Menue geoeffnet");
  if (standby) wakeFromStandby();
  screen = 6;
  meldeEditMode = 0;
}

// Doppel-Klick auf BOOT: TISCH-Richtung neu lernen (Uhr liegt gerade auf dem Tisch).
static void bootDoublePress() {
  USBSerial.println("[boot] Doppel-Klick -> TISCH neu kalibrieren");
  if (!calibrated) {
    USBSerial.println("[tisch] Noch nicht kalibriert - TISCH-Update ignoriert.");
    vibrate(120, 2, 100);
    return;
  }
  if (standby) wakeFromStandby();

  uiMessage(LV_SYMBOL_HOME, lv_color_hex(0x64D2FF), "Tisch lernen", "Uhr liegt flach auf dem Tisch …");

  vibrateBlocking(100);
  delay(500);                        // kurz ruhen lassen
  T_dir = collectStaticRep(2000);    // 2 s ruhig messen
  tischCalibrated = true;

  // Rotation des Modells mit der neuen TISCH-Richtung neu aufbauen
  Vec3 T = {REF_TISCH[0], REF_TISCH[1], REF_TISCH[2]};
  Vec3 M = {REF_MELDUNG[0], REF_MELDUNG[1], REF_MELDUNG[2]};
  buildRotation(T_dir, H_dir, T, M, R_model);
  calibrated = true;

  prefs.putInt("calibT", 1);
  prefs.putFloat("Tx", T_dir.x);
  prefs.putFloat("Ty", T_dir.y);
  prefs.putFloat("Tz", T_dir.z);

  bufHead = 0;
  bufCount = 0;

  vibrateBlocking(VIB_SCHRITT_MS);   // längere Bestätigung

  uiMessage(LV_SYMBOL_OK, lv_color_hex(0x30D158), "Gespeichert", "Tisch-Lage gelernt.");
  delay(1200);

  screen = 0;                        // zurück zum Watchface
  USBSerial.printf("[tisch] Neue TISCH-Richtung: (%.3f %.3f %.3f)\n",
                   (double)T_dir.x, (double)T_dir.y, (double)T_dir.z);
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
    if (d >= 30 && d < 1500) {
      if (bootDoublePending && (now - bootFirstPressMs) <= BOOT_DOUBLE_MS) {
        bootDoublePending = false;
        bootDoublePress();
      } else {
        bootDoublePending = true;
        bootFirstPressMs = now;
      }
    }
  }

  // Einzel-Klick ausführen, wenn kein zweiter Klick folgt
  if (bootDoublePending && (now - bootFirstPressMs) > BOOT_DOUBLE_MS) {
    bootDoublePending = false;
    bootPress();
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
    // Wecker ausloesen: aufwachen, Alarm-Screen zeigen, Vibrationsmuster starten
    if (standby) wakeFromStandby();
    screen = 7;
    zeitTab = 2;
    alarmStart();
    USBSerial.println("[zeit] Timer abgelaufen");
  } else {
    timerRemainingMs -= d;
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
          resetAllStats();
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
                           (int)(aktuellProb[0] * 100));
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
        } else if (line == "VIB") {
          USBSerial.println("Vibrationstest ...");
          vibrate(300);
        } else if (line == "BATT") {
          uint16_t mv = pmu.getBattVoltage();
          int gauge = pmu.getBatteryPercent();
          int voltPct = battPctFromVoltage(mv);
          USBSerial.printf("Akku: %.3fV  gauge=%d%%  spannung=%d%%  %s  %s\n",
                           mv / 1000.0f, gauge, voltPct,
                           pmu.isCharging() ? "laedt" : "entlaedt",
                           pmu.isBatteryConnect() ? "Akku verbunden" : "kein Akku");
        } else if (line == "BTN") {
          pinMode(PWR_KEY_PIN, INPUT);
          USBSerial.printf("Power-Taste (GPIO%d): %s\n", PWR_KEY_PIN,
                           digitalRead(PWR_KEY_PIN) ? "HIGH (gedrueckt)" : "LOW (losgelassen)");
        } else if (line == "SLEEP") {
          enterDeepSleep();
        } else if (line == "SENSOR") {
          USBSerial.printf("Sensor %s\n", sensorOn ? "AN" : "AUS");
        } else if (line == "SENSOR ON") {
          setSensorOn(true);
        } else if (line == "SENSOR OFF") {
          setSensorOn(false);
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
        } else if (line == "STREAM") {
          streamMode = true;
          nextStreamUs = micros();
          USBSerial.println("STREAMING");
        } else if (line == "STOPSTREAM") {
          streamMode = false;
          bufHead = 0; bufCount = 0;
          nextSampleUs = micros();
          USBSerial.println("OK stream stop");
        } else if (line.startsWith("RECSD ")) {
          String lbl = line.substring(6);
          lbl.trim();
          if (lbl == "meldung" || lbl == "nicht_meldung") {
            startSensorRec(lbl.c_str());
          } else {
            USBSerial.println("Unbekannte Klasse (meldung/nicht_meldung)");
          }
        } else if (line == "RECSD") {
          USBSerial.println("Syntax: RECSD meldung|nicht_meldung");
        } else if (line == "STOPSD") {
          if (sensorRec) stopSensorRec();
          else USBSerial.println("Keine Sensor-Aufnahme aktiv");
        } else if (line == "SDCHECK") {
          if (!ensureSd()) {
            USBSerial.println("SD nicht bereit");
          } else {
            File f = SD_MMC.open("/aufnahme.csv", FILE_READ);
            if (!f) {
              USBSerial.println("kein /aufnahme.csv");
            } else {
              USBSerial.printf("aufnahme.csv: %u Bytes\n", (unsigned)f.size());
              for (int i = 0; i < 4 && f.available(); i++) {
                String l = f.readStringUntil('\n');
                USBSerial.println(l);
              }
              f.close();
            }
          }
        } else {
          USBSerial.println("Unbekannt. Befehle: RESET, CAL, CALIB, STATS, BATT, BTN, SLEEP, VIB, SENSOR, STREAM, STOPSTREAM, RECSD, STOPSD, SDCHECK, TIME HH:MM:SS, DATE DD.MM.YY");
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
