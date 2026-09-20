/*
 * Handhebe-Erkennung mit Auto-Kalibrierung (ESP32-C3 + MPU6050)
 *
 * - Liest Chip-ID und Konfiguration aus
 * - Misst beim Start 1 Sekunde lang den Ruhewert (Baseline)
 * - Erkennt Handheben als deutliche Abweichung von der Baseline
 * - Sperrzeit verhindert Doppelzaehlung
 *
 * Pins: SDA = GPIO6, SCL = GPIO7, Adresse 0x68
 */

#include <Wire.h>
#include <math.h>

#define MPU_ADDR   0x68
#define SDA_PIN    6
#define SCL_PIN    7

// ---------- Einstellungen ----------
const float SCHWELLE_G      = 0.35;    // Abweichung ueber Baseline in g
const unsigned long MIN_IMPULS_MS = 80;      // Impuls muss min. so lang sein
const unsigned long SPERRE_MS      = 3000;   // Sperrzeit zwischen zwei Meldungen
// -----------------------------------

float baselineG = 1.0;
int meldungen = 0;
unsigned long letzteMeldung = 0;
bool impulsAktiv = false;
unsigned long impulsStart = 0;
float impulsMaxG = 0;

void leseBeschleunigung(int16_t &ax, int16_t &ay, int16_t &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MPU_ADDR, 6);
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
}

byte liesRegister(byte reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MPU_ADDR, 1);
  return Wire.read();
}

float magnitudeG(int16_t ax, int16_t ay, int16_t az) {
  return sqrt((float)ax * ax + (float)ay * ay + (float)az * az) / 16384.0;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // MPU6050 aufwecken
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(100);

  Serial.println("=== Handhebe-Erkennung (Auto-Kalibrierung) ===");

  // Chip identifizieren
  byte who = liesRegister(0x75);
  byte accelCfg = liesRegister(0x1C);
  Serial.print("WHO_AM_I = 0x");
  Serial.println(who, HEX);
  Serial.print("ACCEL_CONFIG = 0x");
  Serial.println(accelCfg, HEX);

  // Baseline einmessen (1 Sekunde ruhig halten!)
  Serial.println("Kalibriere... Sensor 1 Sekunde RUHIG halten!");
  float sum = 0;
  int n = 100;
  for (int i = 0; i < n; i++) {
    int16_t ax, ay, az;
    leseBeschleunigung(ax, ay, az);
    sum += magnitudeG(ax, ay, az);
    delay(10);
  }
  baselineG = sum / n;
  Serial.print("Baseline = ");
  Serial.print(baselineG, 3);
  Serial.println(" g");
  Serial.println("Fertig! Jetzt Hand heben zum Testen.");
  Serial.println("------------------------");
}

void loop() {
  int16_t ax, ay, az;
  leseBeschleunigung(ax, ay, az);
  float g = magnitudeG(ax, ay, az);
  unsigned long jetzt = millis();

  bool ueberSchwelle = (g - baselineG) > SCHWELLE_G;

  if (ueberSchwelle && !impulsAktiv) {
    impulsAktiv = true;
    impulsStart = jetzt;
    impulsMaxG = g;
  } else if (ueberSchwelle && impulsAktiv) {
    if (g > impulsMaxG) impulsMaxG = g;
  } else if (!ueberSchwelle && impulsAktiv) {
    if (jetzt - impulsStart >= MIN_IMPULS_MS && jetzt - letzteMeldung > SPERRE_MS) {
      meldungen++;
      letzteMeldung = jetzt;
      Serial.print(">>> MELDUNG!  (Spitze +");
      Serial.print(impulsMaxG - baselineG, 2);
      Serial.print(" g)  Gesamt: ");
      Serial.println(meldungen);
    }
    impulsAktiv = false;
  }

  static unsigned long letzterPrint = 0;
  if (jetzt - letzterPrint > 200) {
    letzterPrint = jetzt;
    Serial.print("g=");
    Serial.print(g, 2);
    Serial.print(" (Abw. ");
    Serial.print(g - baselineG, 2);
    Serial.print(")  Meldungen=");
    Serial.println(meldungen);
  }

  delay(10);
}
