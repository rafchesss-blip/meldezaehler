// ---------------------------------------------------------------------------
// Energietest 2 – Display: Verbrauch des AMOLED-Displays über die
// Akkuspannung bestimmen. Anleitung: README.md im selben Ordner.
//
// Das Display wechselt jede halbe Sekunde zwischen kontrastreichen
// Schachbrettmustern und Farbverläufen; in der Mitte steht immer die aktuelle
// Akkuspannung (das Muster spart ihr Feld aus, damit nichts flackert). Damit
// der ESP32 selbst möglichst wenig beiträgt, zeichnet er nur kurz und geht
// dazwischen in den Light-Sleep (Display behält sein Bild im eigenen RAM).
// Alle anderen Verbraucher sind aus wie in Test 1.
//
// Alle INTERVALL Minuten wird die Akkuspannung gemessen – immer beim ersten
// Muster (Schachbrett weiß/schwarz), damit die Last beim Messen gleich ist –
// und mit RTC-Zeit im RTC-RAM gespeichert. Power-Taste: 3 s Anzeige der Werte.
// USB angesteckt: Lauf pausiert, Auslesen über die serielle Konsole:
//   DUMP, STAT, CLEAR, INTERVALL n, HELL n (Helligkeit 0-255), LAUF
// Kabel abziehen startet bzw. setzt den Lauf fort. Unter 3,45 V endet der Lauf
// (Daten bleiben im RTC-RAM, Uhr schläft, Power-Taste weckt).
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

#define EMPTY_MV 3550    // darunter endet der Lauf (Akku schonen; 3450 reichte nicht, Akku lief leer)

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

#define FRAME_MS 500     // Musterwechsel jede halbe Sekunde
#define N_PATTERNS 8     // Muster 0 (Schachbrett weiß/schwarz) = Messmuster

// Feld für die Spannungsanzeige in der Bildmitte – das Muster spart es aus
#define BOX_X 70
#define BOX_Y 216
#define BOX_W 270
#define BOX_H 70

// --- Messspeicher im RTC-RAM -------------------------------------------------
enum : uint8_t { W_START = 0, W_TIMER = 1, W_KEY = 2, W_UNPLUG = 3, W_PLUG = 4, W_EMPTY = 5 };
#define F_VBUS 0x10
#define F_CHARGE 0x20
#define F_NORTC 0x40

struct Rec {
  uint32_t t;       // Sekunden seit 1.1.2000 (externe RTC)
  uint16_t mv10;    // Akkuspannung in 0,1 mV (Mittel aus 16 Lesungen, bei weißem Bild)
  uint8_t flags;    // Bits 0-3: Grund, dazu F_*
  uint8_t spread;   // max - min der 16 Lesungen in mV
};
#define REC_MAX 700
#define MAGIC 0x44495350UL   // "DISP"

// NOINIT: übersteht Resets (z. B. Öffnen des seriellen Ports), nicht aber Stromausfall
RTC_NOINIT_ATTR static uint32_t magic;
RTC_NOINIT_ATTR static uint16_t recN;
RTC_NOINIT_ATTR static uint16_t intervalMin;
RTC_NOINIT_ATTR static uint8_t brightness;
RTC_NOINIT_ATTR static uint8_t runState;     // 0 = wartet auf Abziehen, 1 = läuft, 2 = beendet (Akku leer)
RTC_NOINIT_ATTR static uint32_t frames;      // gezeichnete Bilder seit Start
RTC_NOINIT_ATTR static Rec recs[REC_MAX];

// --- Sicherung im Flash (LittleFS auf der freien spiffs-Partition) ----------
// Jeder Messwert wird zusätzlich an /test2.bin angehängt, die Einstellungen
// stehen in /test2.cfg. Nach einem Stromausfall (RTC-RAM gelöscht) lädt
// setup() beides zurück. LittleFS übersteht Stromausfälle beim Schreiben.
struct Cfg {
  uint32_t magic;
  uint16_t intervalMin;
  uint8_t brightness, runState;
  uint32_t frames;
};

static bool fsOk = false;

static bool fsBegin() {
  if (!fsOk) fsOk = LittleFS.begin(true);   // true = beim ersten Mal formatieren
  return fsOk;
}

static void fsAppend(const Rec &r) {
  if (!fsBegin()) return;
  File f = LittleFS.open("/test2.bin", "a");
  if (!f) return;
  f.write((const uint8_t *)&r, sizeof(r));
  f.close();
}

static void saveCfg() {
  if (!fsBegin()) return;
  Cfg c;
  c.magic = MAGIC;
  c.intervalMin = intervalMin;
  c.brightness = brightness;
  c.runState = runState;
  c.frames = frames;
  File f = LittleFS.open("/test2.cfg.tmp", "w");
  if (!f) return;
  f.write((const uint8_t *)&c, sizeof(c));
  f.close();
  LittleFS.rename("/test2.cfg.tmp", "/test2.cfg");   // Umbenennen ist atomar
}

// true = Sicherung gefunden und übernommen
static bool fsLoad() {
  if (!fsBegin()) return false;
  File f = LittleFS.open("/test2.cfg", "r");
  if (!f) return false;
  Cfg c;
  bool ok = f.read((uint8_t *)&c, sizeof(c)) == sizeof(c) && c.magic == MAGIC;
  f.close();
  if (!ok) return false;
  intervalMin = c.intervalMin;
  brightness = c.brightness;
  runState = c.runState;
  frames = c.frames;
  recN = 0;
  File l = LittleFS.open("/test2.bin", "r");
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
  LittleFS.remove("/test2.bin");
  LittleFS.remove("/test2.cfg");
}

static bool forceRun = false;   // LAUF: auch am Kabel laufen (nur zum Prüfen)

// --- Hilfen (wie Test 1) -------------------------------------------------------
static void i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
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

static void allOff() {
  i2cWrite(QMI_ADDR, 0x08, 0x00);     // QMI8658: Accel+Gyro aus
  i2cWrite(QMI_ADDR, 0x02, 0x01);     // interner Oszillator aus
  i2cWrite(TOUCH_ADDR, 0xA5, 0x03);   // Touch: Hibernate
  pmu.disableDC4();                   // Motor-Versorgung aus
}

// --- Display -----------------------------------------------------------------
static bool gfxReady = false;

static bool displayOn() {
  if (!gfxReady) {
    if (!gfx->begin()) return false;
    gfxReady = true;
  } else {
    gfx->displayOn();
  }
  return true;
}

static void displayOff() {
  if (!gfxReady) return;
  gfx->setBrightness(0);
  gfx->displayOff();
}

// --- Muster -------------------------------------------------------------------
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

// Regenbogen: Farbton 0..1535 -> volle Sättigung und Helligkeit
static uint16_t hue(int h) {
  h %= 1536;
  int x = h % 256, s = h / 256;
  switch (s) {
    case 0: return rgb(255, x, 0);
    case 1: return rgb(255 - x, 255, 0);
    case 2: return rgb(0, 255, x);
    case 3: return rgb(0, 255 - x, 255);
    case 4: return rgb(x, 0, 255);
    default: return rgb(255, 0, 255 - x);
  }
}

static uint16_t mix(uint32_t a, uint32_t b, int i, int n) {   // a, b als 0xRRGGBB
  int r = ((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * i) / n;
  int g = ((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * i) / n;
  int bl = (a & 255) + (((int)(b & 255) - (int)(a & 255)) * i) / n;
  return rgb(r, g, bl);
}

// Rechteck zeichnen, aber das Spannungsfeld aussparen (bis zu 4 Teilstücke)
static void fillClip(int x, int y, int w, int h, uint16_t c) {
  int x2 = x + w, y2 = y + h;
  if (x2 <= BOX_X || x >= BOX_X + BOX_W || y2 <= BOX_Y || y >= BOX_Y + BOX_H) {
    gfx->fillRect(x, y, w, h, c);
    return;
  }
  if (y < BOX_Y) gfx->fillRect(x, y, w, BOX_Y - y, c);
  if (y2 > BOX_Y + BOX_H) gfx->fillRect(x, BOX_Y + BOX_H, w, y2 - BOX_Y - BOX_H, c);
  int ty = max(y, BOX_Y), th = min(y2, BOX_Y + BOX_H) - ty;
  if (x < BOX_X) gfx->fillRect(x, ty, BOX_X - x, th, c);
  if (x2 > BOX_X + BOX_W) gfx->fillRect(BOX_X + BOX_W, ty, x2 - BOX_X - BOX_W, th, c);
}

static void checker(uint16_t a, uint16_t b, int sz) {
  for (int y = 0, r = 0; y < LCD_HEIGHT; y += sz, r++)
    for (int x = 0, k = 0; x < LCD_WIDTH; x += sz, k++)
      fillClip(x, y, min(sz, LCD_WIDTH - x), min(sz, LCD_HEIGHT - y), ((r + k) & 1) ? b : a);
}

static void gradientRows(uint32_t a, uint32_t b) {   // von oben nach unten
  const int band = 6, n = LCD_HEIGHT / band;
  for (int i = 0; i <= n; i++) fillClip(0, i * band, LCD_WIDTH, band, mix(a, b, i, n));
}

static void rainbowCols(int phase) {                 // von links nach rechts
  const int band = 6, n = LCD_WIDTH / band;
  for (int i = 0; i <= n; i++) fillClip(i * band, 0, band, LCD_HEIGHT, hue(phase + i * 1536 / n));
}

static void drawPattern(int p) {
  switch (p % N_PATTERNS) {
    case 0: checker(0xFFFF, 0x0000, 41); break;                     // weiß/schwarz
    case 1: rainbowCols(0); break;                                   // Regenbogen
    case 2: checker(rgb(255, 0, 0), rgb(0, 255, 255), 41); break;    // rot/cyan
    case 3: gradientRows(0x0000FF, 0xFFFF00); break;                 // blau -> gelb
    case 4: checker(rgb(0, 0, 255), rgb(255, 255, 0), 51); break;    // blau/gelb
    case 5: gradientRows(0xFF00FF, 0x00FF00); break;                 // magenta -> grün
    case 6: checker(rgb(255, 0, 255), rgb(0, 255, 0), 51); break;    // magenta/grün
    default: rainbowCols(768); break;                                // Regenbogen versetzt
  }
  frames++;
}

// Aktuelle Akkuspannung im ausgesparten Feld (eine Lesung, nur zur Anzeige)
static void drawVoltage() {
  uint16_t mv = pmu.getBattVoltage();
  char b[16];
  snprintf(b, sizeof(b), "%u mV", mv);
  gfx->fillRect(BOX_X, BOX_Y, BOX_W, BOX_H, RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(5);
  int w = strlen(b) * 30;
  gfx->setCursor(BOX_X + (BOX_W - w) / 2, BOX_Y + 15);
  gfx->print(b);
}

static void drawFrame(int p) {
  drawPattern(p);
  drawVoltage();
}

static void showText(const char *l1, const char *l2, const char *l3) {
  if (!displayOn()) return;
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
  gfx->setBrightness(brightness);
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

// Tiefschlaf nach dem Lauf: nur die Taste weckt (Pull-up nötig, s. Test 1)
static void deepSleepUntilKey() {
  displayOff();
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
  USBSerial.printf("# Test 2 Display: Helligkeit %u, Musterwechsel alle %u ms (Schachbrett/Verlauf + Spannung), "
                   "Messung bei Muster 0, %lu Bilder\n", brightness, FRAME_MS, (unsigned long)frames);
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

static uint16_t drawMs = 0;   // Dauer des letzten Bildes (STAT)

static void stat() {
  static const char *ST[3] = {"wartet", "laeuft", "beendet"};
  USBSerial.printf("Messungen: %u / %u, Intervall %u min, Helligkeit %u, Lauf %s, Bilder %lu, letztes Bild %u ms\n",
                   recN, REC_MAX, intervalMin, brightness, runState < 3 ? ST[runState] : "?", (unsigned long)frames,
                   drawMs);
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
      frames = 0;
      runState = 0;
      fsClear();
      saveCfg();
      USBSerial.println("OK geloescht, Neustart");
      USBSerial.flush();
      delay(100);
      ESP.restart();
    }
    else if (line.startsWith("INTERVALL ")) {
      int n = line.substring(10).toInt();
      if (n >= 1 && n <= 240) intervalMin = n;
      saveCfg();
      USBSerial.printf("Intervall %u min\n", intervalMin);
    } else if (line.startsWith("HELL ")) {
      int n = line.substring(5).toInt();
      if (n >= 0 && n <= 255) brightness = n;
      saveCfg();
      USBSerial.printf("Helligkeit %u\n", brightness);
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
      USBSerial.println("Befehle: DUMP, STAT, CLEAR, INTERVALL n, HELL n, LAUF");
  }
  return false;
}

// Am Kabel: pausieren, Befehle bedienen, bis abgezogen wird (oder LAUF)
static void usbMode() {
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);
  showText("USB", "Auslesen: DUMP", "Abziehen = Start");
  delay(1500);
  USBSerial.println("\n=== TEST 2 DISPLAY ===  Befehle: DUMP, STAT, CLEAR, INTERVALL n, HELL n, LAUF");
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

// --- Lauf: jede halbe Sekunde ein Muster, dazwischen Light-Sleep -------------
// Messmuster zeigen, kurz warten, dann messen – gleiche Last bei jeder Messung
static void recordUnderLoad(uint8_t why) {
  drawFrame(0);
  delay(200);
  record(why);
}

static void runLoop() {
  displayOn();
  gfx->setBrightness(brightness);
  if (runState != 1) {
    runState = 1;
    saveCfg();
    recordUnderLoad(W_UNPLUG);
  }
  unsigned long nextMeasure = millis() + (unsigned long)intervalMin * 60000UL;
  unsigned long nextFrame = millis();
  uint8_t ci = 1;
  for (;;) {
    if ((long)(millis() - nextMeasure) >= 0) {
      nextMeasure += (unsigned long)intervalMin * 60000UL;
      recordUnderLoad(W_TIMER);
      saveCfg();   // Bildzähler sichern
      uint16_t mv = recs[recN - 1].mv10 / 10;
      if (mv < EMPTY_MV) {
        record(W_EMPTY);
        runState = 2;
        saveCfg();
        deepSleepUntilKey();
      }
    }
    unsigned long t0 = millis();
    drawFrame(ci);
    drawMs = millis() - t0;
    ci = (ci + 1) % N_PATTERNS;

    if (keyPressed()) {
      record(W_KEY);
      showLast();
      delay(3000);
      nextFrame = millis();
    }
    if (pmu.isVbusIn() && !forceRun) {
      record(W_PLUG);
      usbMode();
      displayOn();
      gfx->setBrightness(brightness);
      if (!forceRun) recordUnderLoad(W_UNPLUG);
      nextFrame = millis();
      nextMeasure = millis() + (unsigned long)intervalMin * 60000UL;
    }
    if (forceRun && USBSerial) handleSerial();

    // bis zum nächsten Bild schlafen; das Display hält sein Bild selbst
    nextFrame += FRAME_MS;
    long wait = (long)(nextFrame - millis());
    if (wait > 20) {
      if (forceRun) {
        delay(wait);   // am Kabel wach bleiben, damit die Konsole erreichbar ist
      } else {
        gpio_hold_en((gpio_num_t)LCD_CS);   // Display-Chipselect im Schlaf HIGH halten
        esp_sleep_enable_timer_wakeup((uint64_t)wait * 1000ULL);
        esp_light_sleep_start();
        gpio_hold_dis((gpio_num_t)LCD_CS);
      }
    } else {
      nextFrame = millis();   // zu spät dran – Takt neu aufsetzen
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
  digitalWrite(LCD_RESET, HIGH);
  digitalWrite(TP_RESET, HIGH);

  Wire.begin(IIC_SDA, IIC_SCL);
  pmu.begin(Wire, PMU_ADDR, IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  allOff();

  bool valid = magic == MAGIC && recN <= REC_MAX && intervalMin >= 1 && intervalMin <= 240 && runState <= 2;
  if (!valid) {   // nach Stromausfall: Sicherung aus dem Flash, sonst Standardwerte
    magic = MAGIC;
    if (!fsLoad()) {
      recN = 0;
      intervalMin = 5;
      brightness = 255;
      runState = 0;
      frames = 0;
      saveCfg();
    }
    record(W_START);
  }
  pmu.clearIrqStatus();

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
