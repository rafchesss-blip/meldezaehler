/*
 * Erweiterter I2C-Pin-Scanner fuer ESP32-C3
 *
 * Probiert ALLE GPIO-Pin-Kombinationen durch und sucht
 * den MPU6050 (Adresse 0x68 oder 0x69).
 *
 * Damit finden wir den Sensor, egal an welchen Pins er
 * wirklich haengt (z.B. falls D4/D5 auf deinem Board
 * andere GPIOs sind).
 *
 * Serieller Monitor: 115200 Baud
 */

#include <Wire.h>

// Alle nutzbaren GPIOs des ESP32-C3 (18/19 sind USB, daher weggelassen)
const int pins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21};
const int nPins = sizeof(pins) / sizeof(pins[0]);

// Testet ein Pin-Paar auf MPU6050 (0x68/0x69)
// Gibt die gefundene Adresse zurueck, oder 0 wenn nichts da ist.
int probePair(int sda, int scl) {
  Wire.begin(sda, scl);
  for (byte addr = 0x68; addr <= 0x69; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Wire.end();
      return addr;
    }
  }
  Wire.end();
  return 0;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Erweiterter I2C-Pin-Scanner (ESP32-C3) ===");
  Serial.println("Suche MPU6050 auf allen Pin-Kombinationen...");
  Serial.println();

  int gefunden = 0;

  for (int i = 0; i < nPins; i++) {
    for (int j = 0; j < nPins; j++) {
      if (i == j) continue;
      int addr = probePair(pins[i], pins[j]);
      if (addr != 0) {
        Serial.print(">>> GEFUNDEN!  SDA = GPIO");
        Serial.print(pins[i]);
        Serial.print("   SCL = GPIO");
        Serial.print(pins[j]);
        Serial.print("   Adresse = 0x");
        Serial.println(addr, HEX);
        gefunden++;
      }
    }
  }

  Serial.println();
  if (gefunden == 0) {
    Serial.println("Auf KEINEM Pin-Paar gefunden.");
    Serial.println("-> Sensor bekommt vermutlich keinen Strom (VCC/GND)");
    Serial.println("   oder ein Kabel/Sensor ist defekt.");
  } else {
    Serial.println("Siehe oben: So musst du SDA und SCL anschliessen.");
  }
}

void loop() {
  // einmalig gescannt, hier nichts mehr tun
  delay(10000);
}
