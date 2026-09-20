/*
 * MPU6050 Beschleunigungs-Test fuer ESP32-C3
 *
 * Pins (automatisch ermittelt):
 *   SDA = GPIO6
 *   SCL = GPIO7
 *   MPU6050 Adresse = 0x68
 *
 * Liest die Beschleunigung (AX, AY, AZ) direkt aus den
 * Registern aus und zeigt sie im Seriellen Monitor an.
 *
 * Werte sind in Roh-Einheiten (LSB). Bei +/-2g gilt:
 *   16384 LSB = 1g (Erdanziehung)
 * Sensor flach auf dem Tisch: AZ ~ +16384, AX/AY ~ 0
 *
 * Serieller Monitor: 115200 Baud
 */

#include <Wire.h>

#define MPU_ADDR 0x68
#define SDA_PIN  6
#define SCL_PIN  7

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // MPU6050 aufwecken (PWR_MGMT_1 = 0)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();

  delay(100);
  Serial.println("MPU6050 Test - Beschleunigung (AX, AY, AZ)");
  Serial.println("Flach hinlegen: AZ sollte ~+16384 sein");
  Serial.println("------------------------");
}

void loop() {
  // 6 Bytes ab Register 0x3B lesen (AX_H, AX_L, AY_H, AY_L, AZ_H, AZ_L)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MPU_ADDR, 6);

  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();

  Serial.print("AX=");
  Serial.print(ax);
  Serial.print("\tAY=");
  Serial.print(ay);
  Serial.print("\tAZ=");
  Serial.println(az);

  delay(200);
}
