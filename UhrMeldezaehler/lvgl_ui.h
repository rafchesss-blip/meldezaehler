#pragma once
/*
 * lvgl_ui.h
 * ----------------------------------------------------------------------------
 * LVGL-Oberfläche (Schritt 2: nur das Apps-Menü läuft über LVGL)
 *
 * Das Watchface (Screen 0) bleibt vorerst auf der Canvas-Darstellung, damit
 * alle 6 Zifferblätter (Minimal/Farbig/Analog/Digital/Geometrisch/Schule) und
 * die Zifferblatt-Auswahl (Screen 3, langes Drücken auf das Watchface)
 * unverändert funktionieren.
 *
 * Nur der App-Launcher (Screen 4) läuft über LVGL, weil dort die großen
 * Buttons liegen und Partial-Redraw das Antippen deutlich flüssiger macht.
 *
 * Bedienung:
 *   - App antippen            -> App öffnet (Canvas)
 *   - ZURUECK / nach unten    -> zurück zum Watchface
 */
#include <lvgl.h>

static lv_display_t *lvDisp = nullptr;
static lv_indev_t   *lvTouchIndev = nullptr;
static lv_obj_t *scrApps = nullptr;

static lv_obj_t *btnMelde = nullptr;
static lv_obj_t *btnSettings = nullptr;
static lv_obj_t *btnZeit = nullptr;
static lv_obj_t *btnSensor = nullptr;
static lv_obj_t *btnTest = nullptr;

// true = LVGL rendert gerade (Screen 4), false = Canvas-UI
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
// Navigation
// ---------------------------------------------------------------------------
static void lvglBackToWatchface() {
  screen = 0;
  lvglActive = false;
  redrawNow = true;   // Watchface (Canvas) sofort neu zeichnen
}

// Nach unten wischen -> Watchface
static void lvglAppsGesture(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_BOTTOM) {
    lvglBackToWatchface();
  }
}

static void lvglBackBtnEvent(lv_event_t *e) {
  lvglBackToWatchface();
}

// App-Buttons: Tippen wechselt zurück zur Canvas-UI
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
  } else if (btn == btnSensor) {
    screen = 9;
  } else if (btn == btnTest) {
    screen = 8;
  }
  lvglActive = false;
  redrawNow = true;   // Canvas-App sofort zeichnen (kein 1-s-Versatz)
}

// ---------------------------------------------------------------------------
// Apps-Menü aufbauen (5 Apps, wie im Canvas-App-Tray)
// ---------------------------------------------------------------------------
static lv_obj_t *lvglMakeBtn(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *text, lv_color_t bg, lv_color_t border) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, bg, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 14, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, border, 0);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_center(lbl);
  return btn;
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

  // Gleiche Anordnung wie der Canvas-App-Tray (inkl. Sensor-Aufnahme).
  lv_color_t cMelde  = LV_COLOR_MAKE(0x18, 0xC3, 0x00);
  lv_color_t cSet    = LV_COLOR_MAKE(0xE5, 0xA0, 0x00);
  lv_color_t cZeit   = LV_COLOR_MAKE(0xD6, 0x9A, 0x00);
  lv_color_t cSensor = LV_COLOR_MAKE(0xFC, 0x18, 0x00);
  lv_color_t cTest   = LV_COLOR_MAKE(0xBD, 0xF7, 0x00);

  lv_color_t cyan    = lv_color_make(0x00, 0xFF, 0xFF);
  lv_color_t yellow  = lv_color_make(0xFF, 0xFF, 0x00);
  lv_color_t magenta = lv_color_make(0xFF, 0x00, 0xFF);
  lv_color_t green   = lv_color_make(0x00, 0xFF, 0x00);
  lv_color_t white   = lv_color_white();

  btnMelde    = lvglMakeBtn(scrApps, 20, 80, 180, 120, "MELDE-\nZAEHLER", cMelde, cyan);
  btnSettings = lvglMakeBtn(scrApps, 210, 80, 180, 120, "EINSTEL-\nLUNGEN", cSet, yellow);
  btnZeit     = lvglMakeBtn(scrApps, 20, 210, 180, 120, "ZEIT\nTimer +\nStoppuhr", cZeit, magenta);
  btnSensor   = lvglMakeBtn(scrApps, 210, 210, 180, 120, "SENSOR-\nAUFNAHME", cSensor, green);
  btnTest     = lvglMakeBtn(scrApps, 20, 340, 370, 80, "TEST", cTest, white);

  lv_obj_add_event_cb(btnMelde, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btnSettings, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btnZeit, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btnSensor, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btnTest, lvglAppBtnEvent, LV_EVENT_CLICKED, NULL);

  // ZURUECK-Button unten links (wie in der Canvas-UI)
  lv_obj_t *btnBack = lv_button_create(scrApps);
  lv_obj_set_pos(btnBack, 40, 445);
  lv_obj_set_size(btnBack, 130, 40);
  lv_obj_set_style_bg_color(btnBack, lv_color_make(0x28, 0x28, 0x38), 0);
  lv_obj_set_style_bg_opa(btnBack, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btnBack, 8, 0);
  lv_obj_set_style_border_width(btnBack, 2, 0);
  lv_obj_set_style_border_color(btnBack, lv_color_white(), 0);
  lv_obj_t *lblBack = lv_label_create(btnBack);
  lv_label_set_text(lblBack, "ZURUECK");
  lv_obj_set_style_text_color(lblBack, lv_color_white(), 0);
  lv_obj_set_style_text_font(lblBack, &lv_font_montserrat_20, 0);
  lv_obj_center(lblBack);
  lv_obj_add_event_cb(btnBack, lvglBackBtnEvent, LV_EVENT_CLICKED, NULL);

  // Nach unten wischen -> Watchface
  lv_obj_add_event_cb(scrApps, lvglAppsGesture, LV_EVENT_GESTURE, NULL);
}

// ---------------------------------------------------------------------------
// Init + Enter
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

  lvglBuildApps();

  USBSerial.println("[lvgl] Apps-Menue init OK");
}

static void lvglUiEnter(int scr) {
  lv_screen_load(scrApps);
  lvglActive = true;
}
