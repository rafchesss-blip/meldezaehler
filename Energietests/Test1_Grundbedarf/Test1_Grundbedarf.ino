// ---------------------------------------------------------------------------
// Energietest 1 – Grundbedarf des Boards: Ruhestrom der Platine über die
// Akkuspannung bestimmen. Anleitung: README.md im selben Ordner.
//
// Die Uhr schläft (Deep-Sleep, nur RTC-Speicher + Weck-Timer laufen) und
// wacht alle INTERVALL Minuten kurz auf, um die Akkuspannung zu messen
// (AXP2101-ADC, 1 mV je Stufe, Mittel aus 16 Lesungen, gespeichert in 0,1 mV).
// Alle Verbraucher sind aus: Display und Controller im Sleep, Touch im
// Ruhezustand, Bewegungssensor abgeschaltet, Motor-Versorgung (DC4) aus,
// WLAN/BLE werden nie gestartet.
//
// Power-Taste: misst ebenfalls und zeigt 3 s lang Spannung + Anzahl Messungen.
// USB angesteckt: Uhr bleibt wach, Auslesen über die serielle Konsole:
//   DUMP          alle Messungen als CSV
//   STAT          Kurzfassung
//   CLEAR         Messungen löschen
//   INTERVALL n   Messabstand in Minuten (Standard 10)
//   SCHLAF        sofort schlafen (Test des Weckens auch am Kabel)
// Kabel abziehen startet die Messreihe (erste Messung sofort).
//
// Die Werte liegen im RTC-Speicher: Sie überstehen den Deep-Sleep, aber nicht
// einen Stromausfall (Akku leer) oder das Flashen einer anderen Firmware.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>
#include "HWCDC.h"
#include "Arduino_GFX_Library.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

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
#define PWR_KEY_PIN 10   // SYS_OUT des AXP2101, folgt der Power-Taste
#define PMU_ADDR 0x34
#define QMI_ADDR 0x6B
#define TOUCH_ADDR 0x38
#define RTC_ADDR 0x51

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

// --- Messspeicher im RTC-RAM (bleibt im Deep-Sleep erhalten) ---------------
enum : uint8_t { W_START = 0, W_TIMER = 1, W_KEY = 2, W_UNPLUG = 3 };
#define F_VBUS 0x10       // USB angesteckt (Messung wertlos, Akku lädt)
#define F_CHARGE 0x20     // AXP meldet Laden
#define F_NORTC 0x40      // Uhrzeit nicht lesbar

struct Rec {
  uint32_t t;       // Sekunden seit 1.1.2000 (externe RTC)
  uint16_t mv10;    // Akkuspannung in 0,1 mV (Mittel aus 16 Lesungen)
  uint8_t flags;    // Bits 0-3: Weckgrund, dazu F_*
  uint8_t spread;   // max - min der 16 Lesungen in mV (Rauschen)
};
#define REC_MAX 700   // 700 x 10 min = 116 h
#define MAGIC 0x4B47534CUL

// NOINIT: übersteht auch einen Reset (z. B. durch Öffnen des seriellen Ports),
// RTC_DATA_ATTR würde dabei neu belegt. Nur Stromausfall löscht – dann stimmt MAGIC nicht.
RTC_NOINIT_ATTR static uint32_t magic;
RTC_NOINIT_ATTR static uint16_t recN;
RTC_NOINIT_ATTR static uint16_t intervalMin;
RTC_NOINIT_ATTR static uint32_t wakeCount;
RTC_NOINIT_ATTR static uint8_t keyWake;   // 1 = Power-Taste weckt (kostet Pull-up-Strom, s. goSleep)
RTC_NOINIT_ATTR static Rec recs[REC_MAX];
static esp_sleep_wakeup_cause_t wakeCause;
static esp_reset_reason_t resetReason;

// --- Hilfen ------------------------------------------------------------------
static void i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t bcd(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }

// Uhrzeit der PCF85063 als Sekunden seit 1.1.2000 (0 = nicht lesbar)
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

// Akkuspannung: 16 Lesungen im Abstand von 2 ms mitteln
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
  return (uint16_t)((sum * 10 + 8) / 16);   // 0,1 mV
}

static void record(uint8_t why) {
  if (recN >= REC_MAX) return;
  Rec &r = recs[recN];
  r.t = rtcSeconds();
  r.mv10 = measure(r.spread);
  r.flags = why | (pmu.isVbusIn() ? F_VBUS : 0) | (pmu.isCharging() ? F_CHARGE : 0) | (r.t ? 0 : F_NORTC);
  recN++;
}

// Alle Verbraucher außer der Uhrzeit abschalten (nur beim Kaltstart nötig,
// die Zustände bleiben im Deep-Sleep erhalten)
static void allOff() {
  // Bewegungssensor QMI8658: Accel+Gyro aus (CTRL7), interner Oszillator aus (CTRL1 Bit 0)
  i2cWrite(QMI_ADDR, 0x08, 0x00);
  i2cWrite(QMI_ADDR, 0x02, 0x01);
  // Touch FT3168: Ruhezustand (Power-Modus 0x03 = Hibernate)
  i2cWrite(TOUCH_ADDR, 0xA5, 0x03);
  // Motor-Versorgung aus
  pmu.disableDC4();
}

static bool gfxReady = false;

static void showStatus(const char *line1, const char *line2, const char *line3) {
  if (!gfxReady) {
    if (!gfx->begin()) return;   // Bus nur einmal pro Wachphase einrichten
    gfxReady = true;
  } else {
    gfx->displayOn();
  }
  gfx->setBrightness(0);
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(5);
  gfx->setCursor(30, 150);
  gfx->print(line1);
  gfx->setTextSize(3);
  gfx->setTextColor(0x8410);
  gfx->setCursor(30, 240);
  gfx->print(line2);
  gfx->setCursor(30, 290);
  gfx->print(line3);
  gfx->setBrightness(60);   // gedimmt: Anzeige soll die Messung kaum stören
}

static void displayOff() {
  gfx->setBrightness(0);
  gfx->displayOff();   // Display Off + Sleep In
}

static void showLast() {
  if (!recN) { showStatus("keine Daten", "", ""); return; }
  const Rec &r = recs[recN - 1];
  char l1[24], l2[40], l3[40];
  snprintf(l1, sizeof(l1), "%u.%u mV", r.mv10 / 10, r.mv10 % 10);
  float h = (recN > 1 && recs[0].t && r.t) ? (r.t - recs[0].t) / 3600.0f : 0;
  int d = (int)r.mv10 - (int)recs[0].mv10;   // 0,1 mV, negativ = gesunken
  snprintf(l2, sizeof(l2), "%u Messungen, %.1f h", recN, h);
  snprintf(l3, sizeof(l3), "seit Start %c%d.%d mV", d < 0 ? '-' : '+', abs(d) / 10, abs(d) % 10);
  showStatus(l1, l2, l3);
}

static void goSleep() {
  pmu.clearIrqStatus();
  // Power-Taste: Die Platine zieht GPIO10 in Ruhe aktiv auf LOW und lässt beim
  // Drücken los – HIGH entsteht nur mit Pull-up (gemessen 03.10.2026; ohne
  // Pull-up bleibt der Pin immer 0). Der interne Pull-up (~45 kOhm) zieht
  // deshalb in Ruhe dauerhaft ~70 µA – für reine Grundlast-Messungen abschaltbar.
  if (keyWake) {
    esp_sleep_enable_ext1_wakeup(1ULL << PWR_KEY_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // Pull-ups brauchen RTC-Peripherie
    rtc_gpio_pullup_en((gpio_num_t)PWR_KEY_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)PWR_KEY_PIN);
  }
  esp_sleep_enable_timer_wakeup((uint64_t)intervalMin * 60ULL * 1000000ULL);
  // Reset-Leitungen von Display und Touch festhalten, sonst wachen sie beim
  // Neustart des ESP32 aus dem Ruhezustand auf
  digitalWrite(LCD_RESET, HIGH);
  digitalWrite(TP_RESET, HIGH);
  gpio_hold_en((gpio_num_t)LCD_RESET);
  gpio_hold_en((gpio_num_t)TP_RESET);
  gpio_deep_sleep_hold_en();
  USBSerial.flush();
  esp_deep_sleep_start();
}

// --- USB-Betrieb: wach bleiben, Befehle bedienen ----------------------------
static void dump() {
  USBSerial.println("nr,zeit,stunden,mv,streuung_mv,grund,usb,laedt");
  static const char *WHY[4] = {"start", "timer", "taste", "abgezogen"};
  for (int i = 0; i < recN; i++) {
    const Rec &r = recs[i];
    char tb[24];
    fmtTime(r.t, tb, sizeof(tb));
    float h = (recs[0].t && r.t) ? (r.t - recs[0].t) / 3600.0f : -1;
    while (USBSerial.availableForWrite() < 80) delay(2);
    USBSerial.printf("%d,%s,%.3f,%u.%u,%u,%s,%d,%d\n", i, tb, h, r.mv10 / 10, r.mv10 % 10, r.spread,
                     WHY[r.flags & 3], (r.flags & F_VBUS) ? 1 : 0, (r.flags & F_CHARGE) ? 1 : 0);
  }
  USBSerial.println("--- ende ---");
}

static void stat() {
  USBSerial.printf("Messungen: %u / %u, Intervall %u min, Tastenwecken %s, Weckvorgaenge %lu, Weckgrund %d, Reset %d\n",
                   recN, REC_MAX, intervalMin, keyWake ? "an" : "aus", (unsigned long)wakeCount, (int)wakeCause,
                   (int)resetReason);
  uint8_t sp;
  uint16_t now = measure(sp);
  USBSerial.printf("Jetzt: %u.%u mV (Streuung %u mV), USB=%d, laedt=%d\n", now / 10, now % 10, sp, pmu.isVbusIn(),
                   pmu.isCharging());
}

// Diagnose: 20 s lang GPIO10 und die PEK-Interrupts des AXP2101 beobachten
static void keyDiag() {
  USBSerial.println("Jetzt die Power-Taste kurz druecken (20 s) ...");
  pinMode(PWR_KEY_PIN, INPUT_PULLUP);   // ohne Pull-up bleibt GPIO10 immer 0
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ |
                XPOWERS_AXP2101_PKEY_POSITIVE_IRQ | XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ);
  pmu.clearIrqStatus();
  int last = -1;
  unsigned long t0 = millis();
  while (millis() - t0 < 20000) {
    int v = digitalRead(PWR_KEY_PIN);
    if (v != last) {
      USBSerial.printf("%5lu ms  GPIO10=%d\n", millis() - t0, v);
      last = v;
    }
    pmu.getIrqStatus();
    if (pmu.isPekeyShortPressIrq() || pmu.isPekeyLongPressIrq() || pmu.isPekeyPositiveIrq() ||
        pmu.isPekeyNegativeIrq()) {
      USBSerial.printf("%5lu ms  AXP: kurz=%d lang=%d steigend=%d fallend=%d\n", millis() - t0,
                       pmu.isPekeyShortPressIrq(), pmu.isPekeyLongPressIrq(), pmu.isPekeyPositiveIrq(),
                       pmu.isPekeyNegativeIrq());
      pmu.clearIrqStatus();
    }
    delay(5);
  }
  USBSerial.println("--- ende ---");
}

static void usbMode() {
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);
  showStatus("USB", "Auslesen: DUMP", "Abziehen = Start");
  delay(1500);
  USBSerial.println("\n=== AKKU-GRUNDLAST ===  Befehle: DUMP, STAT, CLEAR, INTERVALL n, TASTENWECKEN 0|1, TASTE, SCHLAF");
  stat();
  String line;
  unsigned long offSince = 0;
  for (;;) {
    while (USBSerial.available()) {
      char c = USBSerial.read();
      if (c != '\n' && c != '\r') { line += c; continue; }
      line.trim();
      if (line == "DUMP") dump();
      else if (line == "STAT") stat();
      else if (line == "TASTE") keyDiag();
      else if (line.startsWith("TASTENWECKEN ")) {
        keyWake = line.substring(13).toInt() ? 1 : 0;
        USBSerial.printf("Tastenwecken %s\n", keyWake ? "AN (~70 uA Pull-up)" : "AUS (nur Timer)");
      } else if (line == "CLEAR") { recN = 0; USBSerial.println("OK geloescht"); }
      else if (line.startsWith("INTERVALL ")) {
        int n = line.substring(10).toInt();
        if (n >= 1 && n <= 240) intervalMin = n;
        USBSerial.printf("Intervall %u min\n", intervalMin);
      } else if (line == "SCHLAF") {
        displayOff();
        goSleep();
      } else if (line.length()) USBSerial.println("Befehle: DUMP, STAT, CLEAR, INTERVALL n, TASTENWECKEN 0|1, TASTE, SCHLAF");
      line = "";
    }
    // Kabel ab (2 s stabil) -> Messreihe beginnt
    if (!pmu.isVbusIn()) {
      if (!offSince) offSince = millis();
      else if (millis() - offSince > 2000) {
        displayOff();
        record(W_UNPLUG);
        goSleep();
      }
    } else {
      offSince = 0;
    }
    delay(20);
  }
}

void setup() {
  setCpuFrequencyMhz(80);
  esp_sleep_wakeup_cause_t cause = wakeCause = esp_sleep_get_wakeup_cause();
  resetReason = esp_reset_reason();
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)LCD_RESET);
  gpio_hold_dis((gpio_num_t)TP_RESET);
  pinMode(LCD_RESET, OUTPUT);
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(LCD_RESET, HIGH);
  digitalWrite(TP_RESET, HIGH);

  Wire.begin(IIC_SDA, IIC_SCL);
  pmu.begin(Wire, PMU_ADDR, IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  wakeCount++;

  bool valid = magic == MAGIC && recN <= REC_MAX && intervalMin >= 1 && intervalMin <= 240 && keyWake <= 1;
  bool cold = !valid || (cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_EXT1);
  if (cold) {
    if (!valid) {   // Speicher nach Stromausfall ungültig
      magic = MAGIC;
      recN = 0;
      intervalMin = 10;
      wakeCount = 1;
      keyWake = 1;
    }
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    allOff();
    showStatus("Grundlast", "Messung alle 10 min", "Taste = Anzeige");
    delay(1500);
    displayOff();
    record(W_START);
  } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    record(W_KEY);
    showLast();
    delay(3000);
    displayOff();
  } else {
    record(W_TIMER);
  }

  if (pmu.isVbusIn()) usbMode();   // kehrt nur über goSleep() zurück
  goSleep();
}

void loop() {}
