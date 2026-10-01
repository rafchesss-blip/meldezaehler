#pragma once
/*
 * lvgl_ui.h
 * ----------------------------------------------------------------------------
 * LVGL-Oberfläche (Schritt 1: Watchface + Apps-Menü)
 *
 * LVGL zeichnet nur geänderte Bereiche (Partial-Redraw) statt den kompletten
 * 410x502-Framebuffer. Dadurch blockiert die UI den Prozessor nicht mehr und
 * Touch/Tasten reagieren sofort.
 *
 * Screens 0 (Watchface) und 4 (Apps) laufen über LVGL. Alle übrigen Screens
 * nutzen vorerst weiterhin die alte Canvas-Darstellung, bis sie migriert sind.
 */
#include <lvgl.h>

static lv_display_t *lvDisp = nullptr;
static lv_indev_t   *lvTouchIndev = nullptr;
static lv_obj_t *scrWatchface = nullptr;
static lv_obj_t *scrApps = nullptr;

static lv_obj_t *lblTime = nullptr;
static lv_obj_t *lblDate = nullptr;
static lv_obj_t *lblBatt = nullptr;
static lv_obj_t *lblHeute = nullptr;

static lv_obj_t *btnMelde = nullptr;
static lv_obj_t *btnSettings = nullptr;
static lv_obj_t *btnZeit = nullptr;
static lv_obj_t *btnTest = nullptr;

// true = LVGL rendert gerade (Screens 0/4), false = alte Canvas-UI
bool lvglActive = false;

#define LVGL_BUF_PIXELS (LCD_WIDTH * 50)   // Teilpuffer: 410 x 50
static lv_color_t *lvglBuf = nullptr;

// ---------------------------------------------------------------------------
// Display: nur den geänderten Bereich übertragen
// ---------------------------------------------------------------------------
static void lvglFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint16_t w = lv_area_get_width(area);
  uint16_t h = lv_area_get_height(area);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_disp_flush_ready(disp);
}

// ---------------------------------------------------------------------------
// Touch: bestehendes touchRead() (FT3168 über I2C) verwenden
// ---------------------------------------------------------------------------
static void lvglTouchCb(lv_indev_t *indev, lv_indev_data_t *data) {
  uint16_t x = 0, y = 0;
  bool down = touchRead(x, y);
  data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = x;
  data->point.y = y;
}

static uint32_t lvglTickCb(void) {
  return millis();
}

// ---------------------------------------------------------------------------
// Watchface: nach oben wischen -> Apps
// ---------------------------------------------------------------------------
static void lvglWatchfaceGesture(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_TOP) {
    screen = 4;
    lv_screen_load(scrApps);
  }
}

static void lvglAppsGesture(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_BOTTOM) {
    screen = 0;
    lv_screen_load(scrWatchface);
  }
}

// ---------------------------------------------------------------------------
// App-Buttons: Tippen wechselt zurück zur Canvas-UI (Screen noch nicht migriert)
// ---------------------------------------------------------------------------
static void lvglAppBtnEvent(lv_event_t *e) {
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  if (btn == btnMelde) {
    screen = 1;
    bufHead = bufCount = 0;
  } else if (btn == btnSettings) {
    screen = 2;
    settingsItem = 0;
  } else if (btn == btnZeit) {
    screen = 7;
    zeitTab = 0;
  } else if (btn == btnTest) {
    screen = 8;
  }
  lvglActive = false;
}

// ---------------------------------------------------------------------------
// Screens aufbauen
// ---------------------------------------------------------------------------
static lv_obj_t *lvglMakeBtn(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *text, lv_color_t bg) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, bg, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 18, 0);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
  lv_obj_center(lbl);
  return btn;
}

static void lvglBuildWatchface() {
  scrWatchface = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrWatchface, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scrWatchface, LV_OPA_COVER, 0);

  // Uhrzeit (groß)
  lblTime = lv_label_create(scrWatchface);
  lv_label_set_text(lblTime, "--:--");
  lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lblTime, lv_color_white(), 0);
  lv_obj_align(lblTime, LV_ALIGN_TOP_MID, 0, 45);

  // Datum
  lblDate = lv_label_create(scrWatchface);
  lv_label_set_text(lblDate, "--");
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblDate, lv_color_white(), 0);
  lv_obj_align(lblDate, LV_ALIGN_TOP_MID, 0, 125);

  // Akku
  lblBatt = lv_label_create(scrWatchface);
  lv_label_set_text(lblBatt, "--%");
  lv_obj_set_style_text_font(lblBatt, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblBatt, lv_color_white(), 0);
  lv_obj_align(lblBatt, LV_ALIGN_TOP_MID, 0, 165);

  // Meldungen heute
  lblHeute = lv_label_create(scrWatchface);
  lv_label_set_text(lblHeute, "0 Meldungen");
  lv_obj_set_style_text_font(lblHeute, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(lblHeute, lv_color_white(), 0);
  lv_obj_align(lblHeute, LV_ALIGN_BOTTOM_MID, 0, -60);

  // Nach oben wischen -> Apps
  lv_obj_add_event_cb(scrWatchface, lvglWatchfaceGesture, LV_EVENT_GESTURE, NULL);
}

static void lvglBuildApps() {
  scrApps = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrApps, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scrApps, LV_OPA_COVER, 0);

  lv_obj_t *title = lv_label_create(scrApps);
  lv_label_set_text(title, "APPS");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

  lv_color_t cMelde = LV_COLOR_MAKE(0x18, 0xC3, 0x00);
  lv_color_t cSet   = LV_COLOR_MAKE(0xE5, 0xA0, 0x00);
  lv_color_t cZeit  = LV_COLOR_MAKE(0xD6, 0x9A, 0x00);
  lv_color_t cTest  = LV_COLOR_MAKE(0xBD, 0xF7, 0x00);

  btnMelde = lvglMakeBtn(scrApps, 20, 80, 180, 190, "MELDE-\nZAEHLER", cMelde);
  btnSettings = lvglMakeBtn(scrApps, 210, 80, 180, 190, "EINSTEL-\nLUNGEN", cSet);
  btnZeit = lvglMakeBtn(scrApps, 20, 285, 180, 190, "ZEIT", cZeit);
  btnTest = lvglMakeBtn(scrApps, 210, 285, 180, 190, "TEST", cTest);

  lv_obj_add_event_cb(btnMelde, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btnSettings, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btnZeit, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btnTest, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);

  // Nach unten wischen -> Watchface
  lv_obj_add_event_cb(scrApps, lvglAppsGesture, LV_EVENT_GESTURE, NULL);
}

// ---------------------------------------------------------------------------
// Init + Enter + Update
// ---------------------------------------------------------------------------
static void lvglUiInit() {
  lv_init();
  lv_tick_set_cb(lvglTickCb);

  lvglBuf = (lv_color_t *)heap_caps_malloc(LVGL_BUF_PIXELS * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM);
  if (!lvglBuf) lvglBuf = (lv_color_t *)malloc(LVGL_BUF_PIXELS * sizeof(lv_color_t));

  lvDisp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_flush_cb(lvDisp, lvglFlushCb);
  lv_display_set_buffers(lvDisp, lvglBuf, NULL, LVGL_BUF_PIXELS * sizeof(lv_color_t),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lvTouchIndev = lv_indev_create();
  lv_indev_set_type(lvTouchIndev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(lvTouchIndev, lvglTouchCb);

  lvglBuildWatchface();
  lvglBuildApps();

  USBSerial.println("[lvgl] init OK");
}

// LVGL-Screen für den aktuellen screen-Wert laden
static void lvglUiEnter(int scr) {
  if (scr == 0) lv_screen_load(scrWatchface);
  else lv_screen_load(scrApps);
  lvglActive = true;
}

// Labels 1x/s aktualisieren
static void lvglUiUpdateLabels() {
  char buf[40];

  if (lblTime) {
    if (cachedH >= 0) snprintf(buf, sizeof(buf), "%02d:%02d", cachedH, cachedM);
    else snprintf(buf, sizeof(buf), "--:--");
    lv_label_set_text(lblTime, buf);
  }
  if (lblDate) {
    if (cachedDay >= 1 && cachedMon >= 1) {
      int wd = weekdayOf(cachedDay, cachedMon, 2000 + cachedYr);
      snprintf(buf, sizeof(buf), "%s %02d.%02d.%02d", WD_DE[wd], cachedDay, cachedMon, cachedYr);
    } else {
      snprintf(buf, sizeof(buf), "-- --.--.--");
    }
    lv_label_set_text(lblDate, buf);
  }
  if (lblBatt) {
    snprintf(buf, sizeof(buf), "%d%%", cachedPct >= 0 ? cachedPct : 0);
    lv_label_set_text(lblBatt, buf);
  }
  if (lblHeute) {
    snprintf(buf, sizeof(buf), "%d Meldungen", totalHeute);
    lv_label_set_text(lblHeute, buf);
  }
}
