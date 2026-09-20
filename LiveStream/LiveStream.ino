/*
 * Dauer-Streamer fuer Live-Erkennung (ESP32-C3 + MPU6050)
 *
 * Sendet kontinuierlich mit ~100 Hz die Rohwerte:
 *   ax, ay, az (Beschleunigung, +/-2g, 16384 LSB/g)
 *   gx, gy, gz (Drehrate, +/-1000 dps, 32.8 LSB/dps)
 *
 * Der PC liest diesen Stream und fuehrt die Erkennung aus.
 * Pins: SDA = GPIO6, SCL = GPIO7, Adresse 0x68
 */

#include <Wire.h>

#define MPU_ADDR   0x68
#define SDA_PIN    6
#define SCL_PIN    7
#define INTERVAL_US 10000   // 100 Hz

void readIMU(int16_t &ax, int16_t &ay, int16_t &az,
             int16_t &gx, int16_t &gy, int16_t &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MPU_ADDR, 14);

  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();  // Temperatur ueberspringen
  gx = (Wire.read() << 8) | Wire.read();
  gy = (Wire.read() << 8) | Wire.read();
  gz = (Wire.read() << 8) | Wire.read();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // MPU6050 zuruecksetzen
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0x80);
  Wire.endTransmission();
  delay(100);

  // Aufwecken, PLL mit X-Gyro als Taktquelle
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0x01);
  Wire.endTransmission();
  delay(50);

  // Gyro: +/-1000 dps
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1B); Wire.write(0x10);
  Wire.endTransmission();

  // Accel: +/-2g
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C); Wire.write(0x00);
  Wire.endTransmission();
  delay(100);
}

void loop() {
  unsigned long start = micros();

  int16_t ax, ay, az, gx, gy, gz;
  readIMU(ax, ay, az, gx, gy, gz);

  Serial.print(ax); Serial.print(',');
  Serial.print(ay); Serial.print(',');
  Serial.print(az); Serial.print(',');
  Serial.print(gx); Serial.print(',');
  Serial.print(gy); Serial.print(',');
  Serial.println(gz);

  unsigned long elapsed = micros() - start;
  if (elapsed < INTERVAL_US) {
    delayMicroseconds(INTERVAL_US - elapsed);
  }
}
