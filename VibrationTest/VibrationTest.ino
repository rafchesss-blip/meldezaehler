/*
 * Vibrationstest mit Start-Button (Waveshare ESP32-S3-Touch-AMOLED-2.06)
 *
 * - Zeigt einen "START"-Button auf dem Display
 * - Beim Antippen vibriert der Motor 10 x 0,5 s (0,5 s Pause)
 * - Danach wieder zurück zum Start-Button
 *
 * Vibrationsmotor: GPIO18 (über Transistor), Versorgung AXP2101 DCDC4/LX4 (1,8 V).
 */

#include <Arduino.h>
#include <Wire.h>
#include "HWCDC.h"
#include "Arduino_GFX_Library.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

HWCDC USBSerial;
XPowersPMU pmu;

// ---- Pin-Konfiguration ----
#define LCD_SDIO0  4
#define LCD_SDIO1  5
#define LCD_SDIO2  6
#define LCD_SDIO3  7
#define LCD_SCLK   11
#define LCD_CS     12
#define LCD_RESET  8
#define LCD_WIDTH  410
#define LCD_HEIGHT 502

#define IIC_SDA    15
#define IIC_SCL    14
#define TP_RESET   9
#define MOTOR_PIN  18
#define PMU_ADDR   0x34
#define TOUCH_ADDR 0x38

#define ANZAHL    10
#define VIBR_MS   500
#define PAUSE_MS  500

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT,
                                      22, 0, 0, 0);

// ---- Touch FT3168 ----
static uint8_t touchReg(uint8_t reg) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)TOUCH_ADDR, 1);
  if (Wire.available()) return (uint8_t)Wire.read();
  return 0;
}

static bool touchRead(uint16_t &x, uint16_t &y) {
  uint8_t n = touchReg(0x02);
  if (n == 0 || n > 2) return false;
  x = ((touchReg(0x03) & 0x0F) << 8) | touchReg(0x04);
  y = ((touchReg(0x05) & 0x0F) << 8) | touchReg(0x06);
  return true;
}

static void touchInit() {
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(20);
  digitalWrite(TP_RESET, HIGH);
  delay(60);
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0xA5);
  Wire.write(0x01);
  Wire.endTransmission();
}

// ---- Motor ----
static void vibrateOnce(unsigned long ms) {
  digitalWrite(MOTOR_PIN, HIGH);
  delay(ms);
  digitalWrite(MOTOR_PIN, LOW);
}

// ---- Display-Helfer ----
static void centerText(int y, const char *s, uint16_t color, int scale) {
  int cw = 6 * scale;
  int w = strlen(s) * cw;
  gfx->setTextSize(scale);
  gfx->setTextColor(color);
  gfx->setCursor(LCD_WIDTH / 2 - w / 2, y);
  gfx->print(s);
}

static void drawStartScreen() {
  gfx->fillScreen(BLACK);
  centerText(120, "VIBRATIONSTEST", YELLOW, 3);
  centerText(200, "10 x 0,5 Sekunden", WHITE, 2);

  // Start-Button
  int bx = 105, by = 300, bw = 200, bh = 90;
  gfx->fillRoundRect(bx, by, bw, bh, 20, GREEN);
  gfx->drawRoundRect(bx, by, bw, bh, 20, WHITE);
  centerText(by + 25, "START", BLACK, 4);

  centerText(440, "Display antippen", 0x8410, 2);
}

static void drawProgress(int nr) {
  gfx->fillScreen(BLACK);
  centerText(120, "VIBRIERE ...", YELLOW, 3);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d / %d", nr, ANZAHL);
  centerText(240, buf, CYAN, 5);
  centerText(360, "Motor laeuft 0,5 s", WHITE, 2);
}

static void runVibration() {
  USBSerial.println("Start Vibration (10 x 0,5 s) ...");
  for (int i = 1; i <= ANZAHL; i++) {
    drawProgress(i);
    USBSerial.printf("Vibration %d/%d\n", i, ANZAHL);
    vibrateOnce(VIBR_MS);
    delay(PAUSE_MS);
  }
  gfx->fillScreen(BLACK);
  centerText(220, "FERTIG!", GREEN, 4);
  delay(1200);
  USBSerial.println("Fertig.");
}

void setup() {
  USBSerial.begin(115200);
  delay(300);
  USBSerial.println("\n=== VIBRATIONSTEST (Start-Button) ===");

  Wire.begin(IIC_SDA, IIC_SCL);
  bool pmuOk = pmu.begin(Wire, PMU_ADDR, IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  if (pmuOk) {
    pmu.enableDC4();
    pmu.setDC4Voltage(1800);
    USBSerial.println("PMU OK, DCDC4/LX4 auf 1,8 V.");
  } else {
    USBSerial.println("WARNUNG: PMU nicht gefunden!");
  }

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  if (!gfx->begin()) {
    USBSerial.println("Display init fehlgeschlagen!");
  }
  gfx->fillScreen(BLACK);

  touchInit();
  drawStartScreen();
  USBSerial.println("Bereit - Display antippen zum Starten (oder seriell START).");
}

void loop() {
  uint16_t x, y;
  static bool wasDown = false;
  bool down = touchRead(x, y);

  if (down && !wasDown) {
    wasDown = true;
  } else if (!down && wasDown) {
    wasDown = false;
    runVibration();
    drawStartScreen();
  }

  // Serieller Start als Fallback
  if (USBSerial.available()) {
    String l = USBSerial.readStringUntil('\n');
    l.trim();
    if (l == "START") {
      runVibration();
      drawStartScreen();
    }
  }

  delay(30);
}
