#pragma once
/*
 * audio_codec.h
 * ----------------------------------------------------------------------------
 *  Audio-Treiber für die Waveshare ESP32-S3-Touch-AMOLED-2.06:
 *    - ES8311  (DAC / Lautsprecher-Wiedergabe)  @ I2C 0x18
 *    - ES7210  (ADC / Mikrofon-Aufnahme)        @ I2C 0x40
 *    - ESP_I2S (I2S-Bus, 16 kHz, Stereo, 16 Bit)
 *
 *  Portiert aus den Waveshare- / Espressif-Referenztreibern auf Arduino Wire.
 * ----------------------------------------------------------------------------
 */
#include <Arduino.h>
#include <Wire.h>
#include "ESP_I2S.h"
#include <esp_heap_caps.h>

#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x40

#define REC_SAMPLE_RATE   16000
#define REC_MAX_SECONDS   15
#define REC_BUF_BYTES     (REC_SAMPLE_RATE * 4 * REC_MAX_SECONDS) // Stereo 16 Bit = 4 Byte/Frame

static I2SClass i2s;

static uint8_t  *recBuf = nullptr;      // Aufnahme-Puffer (PSRAM)
static volatile uint32_t recLen = 0;    // aufgenommene Bytes
static volatile bool recording = false;
static volatile bool playing = false;
static volatile bool recStop = false;
static volatile bool playStop = false;
static TaskHandle_t recTaskHandle = nullptr;
static TaskHandle_t playTaskHandle = nullptr;

static void codecWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t codecRead(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)addr, 1);
  if (Wire.available()) return (uint8_t)Wire.read();
  return 0;
}

// ES8311 für 16 kHz Wiedergabe initialisieren
static bool es8311Init() {
  // Initialisierung gemäß esp_codec_dev (Original-Firmware/BSP)
  uint8_t r;

  r = codecRead(ES8311_ADDR, 0x0D);
  if (r != 0xFA) codecWrite(ES8311_ADDR, 0x0D, 0xFA);

  codecWrite(ES8311_ADDR, 0x44, 0x08);   // I2C-Rauschimmunität
  codecWrite(ES8311_ADDR, 0x44, 0x08);

  codecWrite(ES8311_ADDR, 0x01, 0x30);
  codecWrite(ES8311_ADDR, 0x02, 0x00);
  codecWrite(ES8311_ADDR, 0x03, 0x10);
  codecWrite(ES8311_ADDR, 0x16, 0x24);
  codecWrite(ES8311_ADDR, 0x04, 0x10);
  codecWrite(ES8311_ADDR, 0x05, 0x00);
  codecWrite(ES8311_ADDR, 0x0B, 0x00);
  codecWrite(ES8311_ADDR, 0x0C, 0x00);
  codecWrite(ES8311_ADDR, 0x10, 0x1F);
  codecWrite(ES8311_ADDR, 0x11, 0x7F);
  codecWrite(ES8311_ADDR, 0x00, 0x80);   // Power-on

  // Slave-Modus
  r = codecRead(ES8311_ADDR, 0x00);
  codecWrite(ES8311_ADDR, 0x00, r & 0xBF);

  // MCLK verwenden, nicht invertiert
  codecWrite(ES8311_ADDR, 0x01, 0x3F);

  // SCLK nicht invertiert
  r = codecRead(ES8311_ADDR, 0x06);
  codecWrite(ES8311_ADDR, 0x06, r & ~0x20);

  codecWrite(ES8311_ADDR, 0x13, 0x10);
  codecWrite(ES8311_ADDR, 0x1B, 0x0A);
  codecWrite(ES8311_ADDR, 0x1C, 0x6A);
  codecWrite(ES8311_ADDR, 0x44, 0x58);   // interne Referenz (ADCL+DACR) – WICHTIG für DAC-Ausgabe!

  // 16 Bit SDP-Format
  r = codecRead(ES8311_ADDR, 0x09);
  codecWrite(ES8311_ADDR, 0x09, r | 0x0C);
  r = codecRead(ES8311_ADDR, 0x0A);
  codecWrite(ES8311_ADDR, 0x0A, r | 0x0C);

  // Sample-Rate 16 kHz @ MCLK 4,096 MHz (coeff: pre_div=1, mult=1x, div=1, bclk=4, dac_osr=0x20)
  r = codecRead(ES8311_ADDR, 0x02);
  r &= 0x07;
  r |= (1 - 1) << 5;                    // pre_div=1
  codecWrite(ES8311_ADDR, 0x02, r);
  codecWrite(ES8311_ADDR, 0x05, ((1-1) << 4) | (1-1));  // adc_div=1, dac_div=1

  r = codecRead(ES8311_ADDR, 0x03);
  r &= 0x80;
  r |= (0 << 6) | 0x10;                 // fs_mode=0, adc_osr=0x10
  codecWrite(ES8311_ADDR, 0x03, r);

  r = codecRead(ES8311_ADDR, 0x04);
  r &= 0x80;
  r |= 0x20;                            // dac_osr=0x20
  codecWrite(ES8311_ADDR, 0x04, r);

  r = codecRead(ES8311_ADDR, 0x07);
  r &= 0xC0;
  codecWrite(ES8311_ADDR, 0x07, r);     // lrck_h=0
  codecWrite(ES8311_ADDR, 0x08, 0xFF);  // lrck_l=0xff

  r = codecRead(ES8311_ADDR, 0x06);
  r &= 0xE0;
  r |= (4 - 1);                         // bclk_div=4 -> 3
  codecWrite(ES8311_ADDR, 0x06, r);

  codecWrite(ES8311_ADDR, 0x32, 216);   // Lautstärke 85%
  return true;
}

// ES7210 für 16 kHz Aufnahme initialisieren
static bool es7210Init() {
  codecWrite(ES7210_ADDR, 0x00, 0xFF);   // Software-Reset
  codecWrite(ES7210_ADDR, 0x00, 0x32);
  codecWrite(ES7210_ADDR, 0x09, 0x30);   // Init-Zeit
  codecWrite(ES7210_ADDR, 0x0A, 0x30);
  codecWrite(ES7210_ADDR, 0x23, 0x2A);   // HPF ADC1/2
  codecWrite(ES7210_ADDR, 0x22, 0x0A);
  codecWrite(ES7210_ADDR, 0x21, 0x2A);   // HPF ADC3/4
  codecWrite(ES7210_ADDR, 0x20, 0x0A);

  codecWrite(ES7210_ADDR, 0x11, 0x60);   // 16 Bit, Standard-I2S
  codecWrite(ES7210_ADDR, 0x12, 0x00);   // kein TDM
  codecWrite(ES7210_ADDR, 0x40, 0xC3);   // Analog-Power
  codecWrite(ES7210_ADDR, 0x41, 0x70);   // MIC Bias 2,87 V
  codecWrite(ES7210_ADDR, 0x42, 0x70);
  codecWrite(ES7210_ADDR, 0x43, 0x1B);   // MIC1 Gain 33 dB
  codecWrite(ES7210_ADDR, 0x44, 0x1B);   // MIC2
  codecWrite(ES7210_ADDR, 0x45, 0x1B);   // MIC3
  codecWrite(ES7210_ADDR, 0x46, 0x1B);   // MIC4
  codecWrite(ES7210_ADDR, 0x47, 0x08);   // MIC1 Power
  codecWrite(ES7210_ADDR, 0x48, 0x08);
  codecWrite(ES7210_ADDR, 0x49, 0x08);
  codecWrite(ES7210_ADDR, 0x4A, 0x08);

  // 16 kHz @ MCLK 4,096 MHz: adc_div=1, dll=1, doubler=1, osr=0x20, lrck=0x0100
  codecWrite(ES7210_ADDR, 0x07, 0x20);   // OSR
  codecWrite(ES7210_ADDR, 0x02, 0xC1);   // adc_div | doubler<<6 | dll<<7
  codecWrite(ES7210_ADDR, 0x04, 0x01);   // LRCK_H
  codecWrite(ES7210_ADDR, 0x05, 0x00);   // LRCK_L
  codecWrite(ES7210_ADDR, 0x06, 0x04);   // DLL power down
  codecWrite(ES7210_ADDR, 0x4B, 0x0F);   // MIC1/2 Power
  codecWrite(ES7210_ADDR, 0x4C, 0x0F);   // MIC3/4 Power
  codecWrite(ES7210_ADDR, 0x00, 0x71);   // Enable
  codecWrite(ES7210_ADDR, 0x00, 0x41);

  codecWrite(ES7210_ADDR, 0x1B, 0xE7);   // ADC-Lautstärke +20 dB
  codecWrite(ES7210_ADDR, 0x1C, 0xE7);
  codecWrite(ES7210_ADDR, 0x1D, 0xE7);
  codecWrite(ES7210_ADDR, 0x1E, 0xE7);
  return true;
}

// Audio-System komplett initialisieren (I2S + beide Codecs + Verstärker)
static bool audioInit() {
  pinMode(46, OUTPUT);
  digitalWrite(46, HIGH);   // Verstärker (PA_CTRL) an

  i2s.setPins(41, 45, 40, 42, 16);   // bclk, ws, dout, din, mclk
  if (!i2s.begin(I2S_MODE_STD, REC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    USBSerial.println("[audio] I2S-Init fehlgeschlagen");
    return false;
  }

  bool ok = es8311Init();
  USBSerial.printf("[audio] ES8311 (Lautsprecher): %s\n", ok ? "OK" : "FEHLER");
  uint8_t id1 = codecRead(ES8311_ADDR, 0xFD);
  uint8_t id2 = codecRead(ES8311_ADDR, 0xFE);
  USBSerial.printf("[audio] ES8311 ChipID: 0x%02X 0x%02X\n", id1, id2);
  ok = es7210Init();
  USBSerial.printf("[audio] ES7210 (Mikrofon): %s\n", ok ? "OK" : "FEHLER");

  recBuf = (uint8_t *)heap_caps_malloc(REC_BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!recBuf) recBuf = (uint8_t *)malloc(REC_BUF_BYTES);
  USBSerial.printf("[audio] Puffer %d Bytes (%s)\n", REC_BUF_BYTES, recBuf ? "OK" : "FEHLER");
  return true;
}

static int16_t recPeak() {
  if (!recBuf || recLen < 4) return 0;
  int16_t *s = (int16_t *)recBuf;
  uint32_t n = recLen / 2;
  int16_t peak = 0;
  for (uint32_t i = 0; i < n; i++) {
    int16_t v = s[i];
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  return peak;
}

static void recTask(void *arg) {
  while (!recStop && recLen < REC_BUF_BYTES) {
    size_t chunk = (REC_BUF_BYTES - recLen);
    if (chunk > 2560) chunk = 2560;
    size_t n = i2s.readBytes((char *)(recBuf + recLen), chunk);
    recLen += n;
    if (n == 0) break;
  }
  recording = false;
  recTaskHandle = nullptr;
  int16_t pk = recPeak();
  USBSerial.printf("[audio] Aufnahme fertig: %lu Bytes, Peak=%d (%.1f%%)\n",
                   (unsigned long)recLen, pk, pk * 100.0f / 32767.0f);
  vTaskDelete(NULL);
}

static void playTask(void *arg) {
  uint32_t off = 0;
  while (off < recLen && !playStop) {
    size_t chunk = recLen - off;
    if (chunk > 2560) chunk = 2560;
    size_t n = i2s.write((const uint8_t *)(recBuf + off), chunk);
    off += n;
    if (n == 0) break;
  }
  playing = false;
  playTaskHandle = nullptr;
  vTaskDelete(NULL);
}

static void recStart() {
  if (!recBuf || recording) return;
  recStop = false;
  recLen = 0;
  recording = true;
  xTaskCreate(recTask, "rec", 4096, NULL, 1, &recTaskHandle);
}

static void recStopRecording() {
  if (!recording) return;
  recStop = true;
  recording = false;
}

static void playStart() {
  if (!recBuf || recLen == 0 || playing) return;
  playStop = false;
  playing = true;
  xTaskCreate(playTask, "play", 4096, NULL, 1, &playTaskHandle);
}

static void playStopPlayback() {
  if (!playing) return;
  playStop = true;
  playing = false;
}

static void recClear() {
  recStopRecording();
  playStopPlayback();
  recLen = 0;
}

// 440-Hz-Testton abspielen (nicht blockierend, läuft in eigenem Task)
static void toneTask(void *arg) {
  int16_t *tone = (int16_t *)arg;
  size_t w1 = i2s.write((uint8_t *)tone, REC_SAMPLE_RATE * 4);
  size_t w2 = i2s.write((uint8_t *)tone, REC_SAMPLE_RATE * 4);
  USBSerial.printf("[audio] Testton fertig. I2S-TX geschrieben: %u + %u Bytes (von %u)\n",
                   (unsigned)w1, (unsigned)w2, (unsigned)(REC_SAMPLE_RATE * 4));
  playing = false;
  vTaskDelete(NULL);
}

static void playTestTone() {
  static int16_t *tone = nullptr;
  if (!tone) {
    tone = (int16_t *)malloc(REC_SAMPLE_RATE * 4);
    if (!tone) return;
    for (int i = 0; i < REC_SAMPLE_RATE; i++) {
      int16_t s = (int16_t)(sinf(2.0f * PI * 440.0f * i / REC_SAMPLE_RATE) * 12000);
      tone[i * 2] = s;
      tone[i * 2 + 1] = s;
    }
  }
  playStopPlayback();
  playing = true;
  xTaskCreate(toneTask, "tone", 4096, (void *)tone, 1, NULL);
  USBSerial.println("[audio] Testton gestartet.");
}
