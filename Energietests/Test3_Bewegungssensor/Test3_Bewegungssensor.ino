// ---------------------------------------------------------------------------
// Energietest 3 – Bewegungssensor: Verbrauch des QMI8658 im Betrieb wie in der
// Meldezähler-Firmware über die Akkuspannung bestimmen. Anleitung: README.md
// im selben Ordner.
//
// Der Sensor läuft wie in der Firmware (Accel ±2 g / 1000 Hz, Gyro ±1024 dps /
// 896,8 Hz, beide Tiefpässe an). Der ESP32 liest wie die Firmware alle 10 ms
// (100 Hz) einen Satz Accel+Gyro per I2C, verwirft ihn aber (nur Zähler) und
// schläft dazwischen im Light-Sleep (80 MHz). Alles andere ist aus wie in
// Test 1: Display Sleep In, Touch Hibernate, Motorversorgung aus, kein Funk.
//
// Alle INTERVALL Minuten wird die Akkuspannung gemessen und mit RTC-Zeit im
// RTC-RAM und zusätzlich im Flash (LittleFS) gespeichert. Power-Taste: 3 s
// Anzeige der Werte. USB angesteckt: Lauf pausiert, serielle Konsole:
//   DUMP, STAT, CLEAR, INTERVALL n, RATE n (Lesungen/s), LAUF, STROMAUSFALL
// Kabel abziehen startet bzw. setzt den Lauf fort. Unter 3,55 V endet der Lauf
// (Daten bleiben erhalten, Uhr schläft, Power-Taste weckt).
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>
#include "HWCDC.h"
#include "Arduino_GFX_Library.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include <LittleFS.h>

HWCDC USBSerial;
XPowersPMU pmu;

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_RESET 9
#define PWR_KEY_PIN 10   // in Ruhe aktiv LOW, gedrückt nur mit Pull-up HIGH (s. Test 1)
#define PMU_ADDR 0x34
#define QMI_ADDR 0x6B
#define TOUCH_ADDR 0x38
#define RTC_ADDR 0x51

#define EMPTY_MV 3550    // darunter endet der Lauf (Akku schonen)
#define POLL_MS 500      // Taste und USB alle 0,5 s prüfen (je eine I2C-Abfrage am AXP2101)

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

// --- Messspeicher im RTC-RAM -------------------------------------------------
enum : uint8_t { W_START = 0, W_TIMER = 1, W_KEY = 2, W_UNPLUG = 3, W_PLUG = 4, W_EMPTY = 5 };
#define F_VBUS 0x10
#define F_CHARGE 0x20
#define F_NORTC 0x40

struct Rec {
  uint32_t t;       // Sekunden seit 1.1.2000 (externe RTC)
  uint16_t mv10;    // Akkuspannung in 0,1 mV (Mittel aus 16 Lesungen)
  uint8_t flags;    // Bits 0-3: Grund, dazu F_*
  uint8_t spread;   // max - min der 16 Lesungen in mV
};
#define REC_MAX 700   // 700 x 10 min = 116 h
#define MAGIC 0x494D5533UL   // "IMU3"

// NOINIT: übersteht Resets (z. B. Öffnen des seriellen Ports), nicht aber Stromausfall
RTC_NOINIT_ATTR static uint32_t magic;
RTC_NOINIT_ATTR static uint16_t recN;
RTC_NOINIT_ATTR static uint16_t intervalMin;
RTC_NOINIT_ATTR static uint16_t rateHz;      // Lesungen pro Sekunde (Firmware: 100)
RTC_NOINIT_ATTR static uint8_t runState;     // 0 = wartet auf Abziehen, 1 = läuft, 2 = beendet (Akku leer)
RTC_NOINIT_ATTR static uint32_t samples;     // gelesene Datensätze seit Start
RTC_NOINIT_ATTR static uint32_t readErrors;  // fehlgeschlagene I2C-Lesungen
RTC_NOINIT_ATTR static Rec recs[REC_MAX];

// --- Sicherung im Flash (LittleFS auf der freien spiffs-Partition) ----------
// Jeder Messwert wird zusätzlich an /test3.bin angehängt, die Einstellungen
// und Zähler stehen in /test3.cfg. Nach einem Stromausfall (RTC-RAM gelöscht)
// lädt setup() beides zurück. LittleFS übersteht Stromausfälle beim Schreiben.
struct Cfg {
  uint32_t magic;
  uint16_t intervalMin, rateHz;
  uint8_t runState;
  uint32_t samples, readErrors;
};

static bool fsOk = false;

static bool fsBegin() {
  if (!fsOk) fsOk = LittleFS.begin(true);   // true = beim ersten Mal formatieren
  return fsOk;
}

static void fsAppend(const Rec &r) {
  if (!fsBegin()) return;
  File f = LittleFS.open("/test3.bin", "a");
  if (!f) return;
  f.write((const uint8_t *)&r, sizeof(r));
  f.close();
}

static void saveCfg() {
  if (!fsBegin()) return;
  Cfg c;
  c.magic = MAGIC;
  c.intervalMin = intervalMin;
  c.rateHz = rateHz;
  c.runState = runState;
  c.samples = samples;
  c.readErrors = readErrors;
  File f = LittleFS.open("/test3.cfg.tmp", "w");
  if (!f) return;
  f.write((const uint8_t *)&c, sizeof(c));
  f.close();
  LittleFS.rename("/test3.cfg.tmp", "/test3.cfg");   // Umbenennen ist atomar
}

// true = Sicherung gefunden und übernommen
static bool fsLoad() {
  if (!fsBegin()) return false;
  File f = LittleFS.open("/test3.cfg", "r");
  if (!f) return false;
  Cfg c;
  bool ok = f.read((uint8_t *)&c, sizeof(c)) == sizeof(c) && c.magic == MAGIC;
  f.close();
  if (!ok) return false;
  intervalMin = c.intervalMin;
  rateHz = c.rateHz;
  runState = c.runState;
  samples = c.samples;
  readErrors = c.readErrors;
  recN = 0;
  File l = LittleFS.open("/test3.bin", "r");
  if (l) {
    size_t n = l.size() / sizeof(Rec);
    if (n > REC_MAX) n = REC_MAX;
    recN = l.read((uint8_t *)recs, n * sizeof(Rec)) / sizeof(Rec);
    l.close();
  }
  return true;
}

static void fsClear() {
  if (!fsBegin()) return;
  LittleFS.remove("/test3.bin");
  LittleFS.remove("/test3.cfg");
}

static bool forceRun = false;   // LAUF: auch am Kabel laufen (nur zum Prüfen)

// --- Hilfen (wie Test 1/2) -----------------------------------------------------
static void i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t i2cRead(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)addr, 1);
  return Wire.available() ? (uint8_t)Wire.read() : 0;
}

static uint8_t bcd(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }

static uint32_t rtcSeconds() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)RTC_ADDR, 7) < 7) return 0;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = Wire.read();
  int s = bcd(b[0] & 0x7F), m = bcd(b[1] & 0x7F), h = bcd(b[2] & 0x3F);
  int d = bcd(b[3] & 0x3F), mo = bcd(b[5] & 0x1F), y = bcd(b[6]);
  static const uint16_t cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  if (mo < 1 || mo > 12 || d < 1) return 0;
  uint32_t days = y * 365UL + (y + 3) / 4 + cum[mo - 1] + (d - 1) + ((y % 4 == 0 && mo > 2) ? 1 : 0);
  return ((days * 24 + h) * 60 + m) * 60 + s;
}

static void fmtTime(uint32_t t, char *buf, size_t n) {
  if (!t) { snprintf(buf, n, "--"); return; }
  uint32_t days = t / 86400, sec = t % 86400;
  int y = 0;
  while (days >= (uint32_t)(365 + (y % 4 == 0))) { days -= 365 + (y % 4 == 0); y++; }
  static const uint8_t ml[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int mo = 0;
  while (days >= (uint32_t)(ml[mo] + (mo == 1 && y % 4 == 0))) { days -= ml[mo] + (mo == 1 && y % 4 == 0); mo++; }
  snprintf(buf, n, "%02d.%02d.%04d %02lu:%02lu:%02lu", (int)days + 1, mo + 1, 2000 + y,
           (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
}

static uint16_t measure(uint8_t &spread) {
  uint32_t sum = 0;
  uint16_t lo = 0xFFFF, hi = 0;
  for (int i = 0; i < 16; i++) {
    uint16_t v = pmu.getBattVoltage();
    sum += v;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    delay(2);
  }
  spread = (hi - lo) > 255 ? 255 : (hi - lo);
  return (uint16_t)((sum * 10 + 8) / 16);
}

static void record(uint8_t why) {
  if (recN >= REC_MAX) return;
  Rec &r = recs[recN];
  r.t = rtcSeconds();
  r.mv10 = measure(r.spread);
  r.flags = why | (pmu.isVbusIn() ? F_VBUS : 0) | (pmu.isCharging() ? F_CHARGE : 0) | (r.t ? 0 : F_NORTC);
  recN++;
  fsAppend(r);
}

// Alles aus außer Bewegungssensor und RTC
static void othersOff() {
  i2cWrite(TOUCH_ADDR, 0xA5, 0x03);   // Touch: Hibernate
  pmu.disableDC4();                   // Motor-Versorgung aus
}

// --- Bewegungssensor: Einstellung wie in der Firmware (qmiInit) ---------------
static bool imuOk = false;
static int16_t lastData[6];   // letzter Datensatz (nur für STAT)

// Nach dem Flashen kam die Einstellung einmal nicht an (Auto-Increment fehlte,
// alle 12 Bytes gleich) – deshalb zurücklesen und bis zu 3 Versuche
static bool imuInitOnce() {
  i2cWrite(QMI_ADDR, 0x60, 0xB0);                          // Soft-Reset
  delay(30);
  // CTRL1: Auto-Increment, Bit 0 (Oszillator aus, von Test 1/2 gesetzt) löschen
  i2cWrite(QMI_ADDR, 0x02, (i2cRead(QMI_ADDR, 0x02) | 0x40) & ~0x01);
  delay(5);
  if (i2cRead(QMI_ADDR, 0x00) != 0x05) return false;      // WHO_AM_I
  i2cWrite(QMI_ADDR, 0x09, 0x80);                          // CTRL8
  i2cWrite(QMI_ADDR, 0x03, (0 << 4) | 3);                  // CTRL2: Accel ±2 g, ODR 1000 Hz
  i2cWrite(QMI_ADDR, 0x04, (6 << 4) | 3);                  // CTRL3: Gyro ±1024 dps, ODR 896,8 Hz
  i2cWrite(QMI_ADDR, 0x06, 0x11);                          // CTRL5: beide Tiefpässe an
  i2cWrite(QMI_ADDR, 0x08, 0x03);                          // CTRL7: Accel + Gyro an
  delay(5);
  return (i2cRead(QMI_ADDR, 0x02) & 0x41) == 0x40 && i2cRead(QMI_ADDR, 0x03) == 0x03 &&
         i2cRead(QMI_ADDR, 0x04) == 0x63 && i2cRead(QMI_ADDR, 0x08) == 0x03;
}

static bool imuInit() {
  for (int i = 0; i < 3; i++)
    if (imuInitOnce()) return true;
  return false;
}

static void imuOff() {
  i2cWrite(QMI_ADDR, 0x08, 0x00);   // Accel+Gyro aus
  i2cWrite(QMI_ADDR, 0x02, 0x01);   // interner Oszillator aus
}

// Ein Datensatz wie qmiReadData() der Firmware (12 Bytes ab 0x35) – wird verworfen
static void imuRead() {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(0x35);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)QMI_ADDR, 12) < 12) {
    readErrors++;
    return;
  }
  uint8_t b[12];
  for (int i = 0; i < 12; i++) b[i] = Wire.read();
  for (int i = 0; i < 6; i++) lastData[i] = (int16_t)((b[2 * i + 1] << 8) | b[2 * i]);
  samples++;
}

// --- Display (nur für die Tastenanzeige) ---------------------------------------
static bool gfxReady = false;

static void displayOff() {
  if (!gfxReady) return;
  gfx->setBrightness(0);
  gfx->displayOff();   // Display Off + Sleep In
}

static void showText(const char *l1, const char *l2, const char *l3) {
  if (!gfxReady) {
    if (!gfx->begin()) return;
    gfxReady = true;
  } else {
    gfx->displayOn();
  }
  gfx->setBrightness(0);
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(5);
  gfx->setCursor(30, 150);
  gfx->print(l1);
  gfx->setTextSize(3);
  gfx->setTextColor(0x8410);
  gfx->setCursor(30, 240);
  gfx->print(l2);
  gfx->setCursor(30, 290);
  gfx->print(l3);
  gfx->setBrightness(60);   // gedimmt: Anzeige soll die Messung kaum stören
}

static void showLast() {
  if (!recN) { showText("keine Daten", "", ""); return; }
  const Rec &r = recs[recN - 1];
  char l1[24], l2[40], l3[40];
  snprintf(l1, sizeof(l1), "%u.%u mV", r.mv10 / 10, r.mv10 % 10);
  float h = (recN > 1 && recs[0].t && r.t) ? (r.t - recs[0].t) / 3600.0f : 0;
  int d = (int)r.mv10 - (int)recs[0].mv10;
  snprintf(l2, sizeof(l2), "%u Messungen, %.1f h", recN, h);
  snprintf(l3, sizeof(l3), "seit Start %c%d.%d mV", d < 0 ? '-' : '+', abs(d) / 10, abs(d) % 10);
  showText(l1, l2, l3);
}

// Power-Taste: der AXP2101 merkt sich den Druck (IRQ-Status), auch im Light-Sleep
static bool keyPressed() {
  pmu.getIrqStatus();
  bool p = pmu.isPekeyShortPressIrq();
  pmu.clearIrqStatus();
  return p;
}

// Tiefschlaf nach dem Lauf: Sensor aus, nur die Taste weckt (Pull-up nötig, s. Test 1)
static void deepSleepUntilKey() {
  displayOff();
  imuOff();
  pmu.clearIrqStatus();
  esp_sleep_enable_ext1_wakeup(1ULL << PWR_KEY_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_pullup_en((gpio_num_t)PWR_KEY_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PWR_KEY_PIN);
  digitalWrite(LCD_RESET, HIGH);
  digitalWrite(TP_RESET, HIGH);
  gpio_hold_en((gpio_num_t)LCD_RESET);
  gpio_hold_en((gpio_num_t)TP_RESET);
  gpio_deep_sleep_hold_en();
  USBSerial.flush();
  esp_deep_sleep_start();
}

// --- Serielle Befehle --------------------------------------------------------
static void dump() {
  USBSerial.printf("# Test 3 Bewegungssensor: QMI8658 Accel 1000 Hz + Gyro 896,8 Hz, Lesen mit %u Hz "
                   "(verworfen), Display aus, %lu Lesungen, %lu Fehler\n",
                   rateHz, (unsigned long)samples, (unsigned long)readErrors);
  USBSerial.println("nr,zeit,stunden,mv,streuung_mv,grund,usb,laedt");
  static const char *WHY[6] = {"start", "timer", "taste", "abgezogen", "angesteckt", "leer"};
  for (int i = 0; i < recN; i++) {
    const Rec &r = recs[i];
    char tb[24];
    fmtTime(r.t, tb, sizeof(tb));
    float h = (recs[0].t && r.t) ? (r.t - recs[0].t) / 3600.0f : -1;
    uint8_t w = r.flags & 0x0F;
    while (USBSerial.availableForWrite() < 80) delay(2);
    USBSerial.printf("%d,%s,%.3f,%u.%u,%u,%s,%d,%d\n", i, tb, h, r.mv10 / 10, r.mv10 % 10, r.spread,
                     w < 6 ? WHY[w] : "?", (r.flags & F_VBUS) ? 1 : 0, (r.flags & F_CHARGE) ? 1 : 0);
  }
  USBSerial.println("--- ende ---");
}

static void stat() {
  static const char *ST[3] = {"wartet", "laeuft", "beendet"};
  USBSerial.printf("Messungen: %u / %u, Intervall %u min, Rate %u Hz, Lauf %s, Sensor %s, Lesungen %lu, Fehler %lu\n",
                   recN, REC_MAX, intervalMin, rateHz, runState < 3 ? ST[runState] : "?", imuOk ? "OK" : "FEHLT",
                   (unsigned long)samples, (unsigned long)readErrors);
  imuRead();
  USBSerial.printf("Letzter Datensatz: a=%d,%d,%d g=%d,%d,%d (roh)\n", lastData[0], lastData[1], lastData[2],
                   lastData[3], lastData[4], lastData[5]);
  USBSerial.printf("QMI8658 CTRL1..CTRL8: %02X %02X %02X %02X %02X %02X %02X %02X\n", i2cRead(QMI_ADDR, 0x02),
                   i2cRead(QMI_ADDR, 0x03), i2cRead(QMI_ADDR, 0x04), i2cRead(QMI_ADDR, 0x05), i2cRead(QMI_ADDR, 0x06),
                   i2cRead(QMI_ADDR, 0x07), i2cRead(QMI_ADDR, 0x08), i2cRead(QMI_ADDR, 0x09));
  uint8_t sp;
  uint16_t now = measure(sp);
  USBSerial.printf("Jetzt: %u.%u mV (Streuung %u mV), USB=%d, laedt=%d\n", now / 10, now % 10, sp, pmu.isVbusIn(),
                   pmu.isCharging());
}

static String serialLine;

// true = LAUF angefordert
static bool handleSerial() {
  while (USBSerial.available()) {
    char c = USBSerial.read();
    if (c != '\n' && c != '\r') { serialLine += c; continue; }
    String line = serialLine;
    serialLine = "";
    line.trim();
    if (line == "DUMP") dump();
    else if (line == "STAT") stat();
    else if (line == "CLEAR") {   // löschen + Neustart: beendet auch einen LAUF am Kabel
      recN = 0;
      samples = 0;
      readErrors = 0;
      runState = 0;
      fsClear();
      saveCfg();
      USBSerial.println("OK geloescht, Neustart");
      USBSerial.flush();
      delay(100);
      ESP.restart();
    } else if (line.startsWith("INTERVALL ")) {
      int n = line.substring(10).toInt();
      if (n >= 1 && n <= 240) intervalMin = n;
      saveCfg();
      USBSerial.printf("Intervall %u min\n", intervalMin);
    } else if (line.startsWith("RATE ")) {
      int n = line.substring(5).toInt();
      if (n >= 1 && n <= 200) rateHz = n;
      saveCfg();
      USBSerial.printf("Rate %u Hz\n", rateHz);
    } else if (line == "STROMAUSFALL") {   // Prüfung der Flash-Sicherung: RTC-RAM verwerfen, neu starten
      magic = 0;
      USBSerial.println("RTC-RAM verworfen, Neustart");
      USBSerial.flush();
      delay(100);
      ESP.restart();
    } else if (line == "LAUF") {
      USBSerial.println("Lauf startet am Kabel (Messwerte mit usb=1 sind wertlos)");
      return true;
    } else if (line.length())
      USBSerial.println("Befehle: DUMP, STAT, CLEAR, INTERVALL n, RATE n, LAUF, STROMAUSFALL");
  }
  return false;
}

// Am Kabel: pausieren, Befehle bedienen, bis abgezogen wird (oder LAUF).
// Der Sensor läuft weiter, wird aber nicht gelesen.
static void usbMode() {
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);
  showText("USB", "Auslesen: DUMP", "Abziehen = Start");
  delay(1500);
  USBSerial.println("\n=== TEST 3 BEWEGUNGSSENSOR ===  Befehle: DUMP, STAT, CLEAR, INTERVALL n, RATE n, LAUF");
  stat();
  unsigned long offSince = 0;
  for (;;) {
    if (handleSerial()) { forceRun = true; return; }
    if (!pmu.isVbusIn()) {
      if (!offSince) offSince = millis();
      else if (millis() - offSince > 2000) return;
    } else {
      offSince = 0;
    }
    delay(20);
  }
}

// --- Lauf: Sensor mit rateHz lesen, dazwischen Light-Sleep ---------------------
static void runLoop() {
  displayOff();
  if (runState != 1) {
    runState = 1;
    saveCfg();
    record(W_UNPLUG);
  }
  const unsigned long periodUs = 1000000UL / rateHz;
  unsigned long nextMeasure = millis() + (unsigned long)intervalMin * 60000UL;
  unsigned long nextPoll = millis() + POLL_MS;
  unsigned long nextSample = micros();
  for (;;) {
    imuRead();

    unsigned long now = millis();
    if ((long)(now - nextMeasure) >= 0) {
      nextMeasure += (unsigned long)intervalMin * 60000UL;
      record(W_TIMER);
      saveCfg();   // Zähler sichern
      if (recs[recN - 1].mv10 / 10 < EMPTY_MV) {
        record(W_EMPTY);
        runState = 2;
        saveCfg();
        deepSleepUntilKey();
      }
    }
    if ((long)(now - nextPoll) >= 0) {
      nextPoll = now + POLL_MS;
      if (keyPressed()) {
        record(W_KEY);
        showLast();
        delay(3000);
        displayOff();
        nextSample = micros();
      }
      if (pmu.isVbusIn() && !forceRun) {
        record(W_PLUG);
        saveCfg();
        usbMode();
        displayOff();
        if (!forceRun) record(W_UNPLUG);
        nextMeasure = millis() + (unsigned long)intervalMin * 60000UL;
        nextSample = micros();
      }
      if (forceRun && USBSerial) handleSerial();
    }

    // bis zur nächsten Lesung schlafen
    nextSample += periodUs;
    long wait = (long)(nextSample - micros());
    if (wait > 1000) {
      if (forceRun) {
        delayMicroseconds(wait);   // am Kabel wach bleiben, damit die Konsole erreichbar ist
      } else {
        gpio_hold_en((gpio_num_t)LCD_CS);   // Display-Chipselect im Schlaf HIGH halten
        esp_sleep_enable_timer_wakeup((uint64_t)wait);
        esp_light_sleep_start();
        gpio_hold_dis((gpio_num_t)LCD_CS);
      }
    } else if (wait < -(long)(5 * periodUs)) {
      nextSample = micros();   // zu weit hinten – Takt neu aufsetzen
    }
  }
}

void setup() {
  setCpuFrequencyMhz(80);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)LCD_RESET);
  gpio_hold_dis((gpio_num_t)TP_RESET);
  if (cause == ESP_SLEEP_WAKEUP_EXT1) rtc_gpio_deinit((gpio_num_t)PWR_KEY_PIN);
  pinMode(LCD_RESET, OUTPUT);
  pinMode(TP_RESET, OUTPUT);
  pinMode(LCD_CS, OUTPUT);
  digitalWrite(LCD_RESET, HIGH);
  digitalWrite(TP_RESET, HIGH);
  digitalWrite(LCD_CS, HIGH);

  Wire.begin(IIC_SDA, IIC_SCL);
  pmu.begin(Wire, PMU_ADDR, IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  othersOff();
  imuOk = imuInit();

  bool valid = magic == MAGIC && recN <= REC_MAX && intervalMin >= 1 && intervalMin <= 240 && rateHz >= 1 &&
               rateHz <= 200 && runState <= 2;
  if (!valid) {   // nach Stromausfall: Sicherung aus dem Flash, sonst Standardwerte
    magic = MAGIC;
    if (!fsLoad()) {
      recN = 0;
      intervalMin = 10;
      rateHz = 100;
      runState = 0;
      samples = 0;
      readErrors = 0;
      saveCfg();
    }
    record(W_START);
  }
  pmu.clearIrqStatus();

  // Display einmal einrichten, damit runLoop() es sicher abschalten kann
  // (nach dem Flashen kann es noch das Bild eines anderen Programms zeigen)
  if (imuOk) showText("Sensortest", "QMI8658 OK", "Taste = Anzeige");
  else showText("Sensorfehler", "QMI8658 fehlt", "");
  delay(imuOk ? 1500 : 3000);
  if (runState == 2) {   // Lauf ist beendet (Akku leer) – nur Werte zeigen
    showLast();
    delay(3000);
    if (!pmu.isVbusIn()) deepSleepUntilKey();
  }
  if (pmu.isVbusIn()) usbMode();
  if (runState == 2 && !forceRun) deepSleepUntilKey();   // nach CLEAR (runState 0) startet ein neuer Lauf
  runLoop();
}

void loop() {}
