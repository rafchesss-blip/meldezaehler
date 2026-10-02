#pragma once
// ---------------------------------------------------------------------------
// Display-Anbindung für LVGL (CO5300-AMOLED 410x502 über Quad-SPI)
//
// Warum esp_lcd statt Arduino_GFX: Arduino_GFX überträgt blockierend (polling)
// und tauscht die Bytes jedes Pixels per CPU – ein Vollbild kostete ~75 ms, in
// denen loop() (Touch, Sensor) stand (PERF_ANALYSE_2026-10-01.md). esp_lcd
// überträgt per DMA im Hintergrund; LVGL rendert währenddessen in den zweiten
// Puffer und zeichnet nur geänderte Bereiche. Aufbau wie in der Hersteller-
// Demo (Waveshare BSP esp32_s3_touch_amoled_2_06, Treiber esp_lcd_sh8601).
//
// USE_ESP_LCD 0 = Rückfallebene über Arduino_GFX (langsam, aber erprobt),
// falls die esp_lcd-Ansteuerung auf einer Uhr kein Bild liefert.
//
// Erwartet vor dem Einbinden: LCD_*-Pins, LCD_WIDTH/LCD_HEIGHT, USBSerial.
// ---------------------------------------------------------------------------
#include <lvgl.h>

#ifndef USE_ESP_LCD
#define USE_ESP_LCD 1
#endif

// Teilpuffer-Höhe in Zeilen (2 Puffer, internes DMA-RAM: 2 x 410 x 40 x 2 B = 64 KB)
#ifndef HAL_BUF_LINES
#define HAL_BUF_LINES 40
#endif

#if USE_ESP_LCD
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_lcd_sh8601.h"
#else
#include "Arduino_GFX_Library.h"
#endif

static lv_display_t *halDisp = nullptr;

#if USE_ESP_LCD
static esp_lcd_panel_io_handle_t halIo = nullptr;
static esp_lcd_panel_handle_t halPanel = nullptr;

// CO5300-Initialisierung aus dem Hersteller-BSP; Spalten 22..431 (Gap 0x16).
static const sh8601_lcd_init_cmd_t HAL_CO5300_INIT[] = {
  {0x11, (uint8_t[]){0x00}, 0, 120},
  {0xC4, (uint8_t[]){0x80}, 1, 0},
  {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
  {0x35, (uint8_t[]){0x00}, 1, 0},
  {0x53, (uint8_t[]){0x20}, 1, 10},
  {0x63, (uint8_t[]){0xFF}, 1, 10},
  {0x51, (uint8_t[]){0x00}, 1, 10},
  {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
  {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
  {0x29, (uint8_t[]){0x00}, 0, 10},
};

// Kommando im QSPI-Rahmen des CO5300: Opcode 0x02, Befehl im mittleren Byte
static void halTxCmd(uint8_t cmd, const uint8_t *data, size_t len) {
  if (halIo) esp_lcd_panel_io_tx_param(halIo, (0x02 << 24) | (cmd << 8), data, len);
}

// DMA fertig -> LVGL darf den Puffer wiederverwenden (läuft im Interrupt)
static bool halOnColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  if (halDisp) lv_display_flush_ready(halDisp);
  return false;
}

static void halFlush(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  // Panel erwartet RGB565 big-endian; Tausch auf dem Teilpuffer ist billig
  lv_draw_sw_rgb565_swap(px, lv_area_get_size(a));
  esp_lcd_panel_draw_bitmap(halPanel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
  // flush_ready kommt aus halOnColorDone, sobald der DMA fertig ist
}
#else
static Arduino_DataBus *halBus = nullptr;
static Arduino_CO5300 *gfx = nullptr;

static void halFlush(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, lv_area_get_width(a), lv_area_get_height(a));
  lv_display_flush_ready(disp);
}
#endif

// CO5300 nimmt Fensterkoordinaten nur gerade an (Start gerade, Breite gerade)
static void halRounder(lv_event_t *e) {
  lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
  a->x1 &= ~1;
  a->y1 &= ~1;
  a->x2 |= 1;
  a->y2 |= 1;
}

static uint32_t halTick() { return millis(); }

static bool halPanelInit() {
#if USE_ESP_LCD
  spi_bus_config_t bus = {};
  bus.sclk_io_num = LCD_SCLK;
  bus.data0_io_num = LCD_SDIO0;
  bus.data1_io_num = LCD_SDIO1;
  bus.data2_io_num = LCD_SDIO2;
  bus.data3_io_num = LCD_SDIO3;
  bus.max_transfer_sz = LCD_WIDTH * HAL_BUF_LINES * 2 + 64;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    USBSerial.println("[hal] spi_bus_initialize fehlgeschlagen");
    return false;
  }
  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = LCD_CS;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  io.pclk_hz = 40 * 1000 * 1000;
  io.trans_queue_depth = 10;
  io.on_color_trans_done = halOnColorDone;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = true;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &halIo) != ESP_OK) {
    USBSerial.println("[hal] panel_io_spi fehlgeschlagen");
    return false;
  }
  sh8601_vendor_config_t vendor = {};
  vendor.init_cmds = HAL_CO5300_INIT;
  vendor.init_cmds_size = sizeof(HAL_CO5300_INIT) / sizeof(HAL_CO5300_INIT[0]);
  vendor.flags.use_qspi_interface = 1;
  esp_lcd_panel_dev_config_t dev = {};
  dev.reset_gpio_num = LCD_RESET;
  dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  dev.bits_per_pixel = 16;
  dev.vendor_config = &vendor;
  if (esp_lcd_new_panel_sh8601(halIo, &dev, &halPanel) != ESP_OK) {
    USBSerial.println("[hal] panel_sh8601 fehlgeschlagen");
    return false;
  }
  esp_lcd_panel_reset(halPanel);
  esp_lcd_panel_init(halPanel);
  esp_lcd_panel_set_gap(halPanel, 0x16, 0);
  esp_lcd_panel_disp_on_off(halPanel, true);
  return true;
#else
  halBus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  gfx = new Arduino_CO5300(halBus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);
  return gfx->begin(80000000);
#endif
}

// Panel + LVGL-Display einrichten. Danach ist der Schirm schwarz und LVGL bereit.
static bool halDisplayInit() {
  lv_init();
  lv_tick_set_cb(halTick);
  if (!halPanelInit()) return false;

  size_t bytes = LCD_WIDTH * HAL_BUF_LINES * 2;
  uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
  void *b1 = heap_caps_malloc(bytes, caps);
  void *b2 = heap_caps_malloc(bytes, caps);
  if (!b1 || !b2) {
    USBSerial.println("[hal] kein internes DMA-RAM fuer Displaypuffer");
    return false;
  }
  halDisp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_color_format(halDisp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(halDisp, halFlush);
  lv_display_set_buffers(halDisp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(halDisp, halRounder, LV_EVENT_INVALIDATE_AREA, nullptr);
  USBSerial.printf("[hal] Display bereit (%s, 2x%u Zeilen)\n",
                   USE_ESP_LCD ? "esp_lcd/DMA" : "Arduino_GFX", (unsigned)HAL_BUF_LINES);
  return true;
}

// Helligkeit 0..255 (CO5300-Register 0x51)
static void halSetBrightness(uint8_t b) {
#if USE_ESP_LCD
  halTxCmd(0x51, &b, 1);
#else
  gfx->setBrightness(b);
#endif
}

// Display-Controller schlafen legen / aufwecken (Standby, Deep-Sleep)
static void halDisplayPower(bool on) {
#if USE_ESP_LCD
  if (on) {
    halTxCmd(0x11, nullptr, 0);   // Sleep Out
    delay(120);                   // Datenblatt: 120 ms bis zum nächsten Befehl
    halTxCmd(0x29, nullptr, 0);   // Display On
  } else {
    halTxCmd(0x28, nullptr, 0);   // Display Off
    halTxCmd(0x10, nullptr, 0);   // Sleep In
  }
#else
  if (on) gfx->displayOn();
  else gfx->displayOff();
#endif
}
