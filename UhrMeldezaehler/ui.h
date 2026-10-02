#pragma once
// ---------------------------------------------------------------------------
// Oberfläche: Navigation, Aktualisierung, Touch, modale Abläufe.
//
// Quelle der Wahrheit für die Navigation bleibt `screen` (+ zeitTab), weil auch
// die Logik ihn setzt (Power-Taste, BOOT-Taste, Timer-Ablauf, Standby).
// uiSync() lädt bei Änderung die passende Maske.
// ---------------------------------------------------------------------------
#include "ui_theme.h"
#include "ui_watchfaces.h"
#include "ui_screens.h"

enum {
  UI_WF = 0, UI_MELDE = 1, UI_SETTINGS = 2, UI_PICKER = 3, UI_APPS = 4,
  UI_EDIT = 6, UI_ZEIT = 7, UI_TEST = 8, UI_REC = 9, UI_ALARM = 10, UI_COUNT = 11
};

// Dauer der Übergangsanimation (0 = sofort). Während einer Schiebe-Animation
// werden alte und neue Maske gezeichnet: auf der Uhr bis 168 ms je Bild, also
// 2–3 ruckelnde Bilder. Sofortiger Wechsel kostet ein Bild (~45–75 ms).
#ifndef UI_ANIM_MS
#define UI_ANIM_MS 0
#endif

static lv_obj_t *uiScr[UI_COUNT];
static int uiShown = -1;          // aktuell geladene Maske (UI_*), -1 = modal/keine
static int uiWfBuilt = -1;        // für welches Zifferblatt uiScr[UI_WF] gebaut ist
static lv_obj_t *uiModalScr = nullptr;
static lv_obj_t *uiBattBar = nullptr;
static lv_indev_t *uiTouch = nullptr;
static struct {
  lv_obj_t *title, *sub, *big, *bar, *rep;
  bool active;   // Kalibrier-Maske ist die aktuelle modale Maske
} uc;

// Tiefe in der Navigation: bestimmt die Richtung der Übergangsanimation
static int uiDepth(int k) {
  switch (k) {
    case UI_WF: return 0;
    case UI_APPS: case UI_PICKER: case UI_EDIT: return 1;
    case UI_ALARM: return 3;
    default: return 2;
  }
}

// ---------------------------------------------------------------------------
// Zifferblatt: Gesten (hoch wischen = Apps, 2 s halten = Auswahl)
// ---------------------------------------------------------------------------
static uint32_t uiWfPressMs = 0;
static bool uiWfHoldFired = false;
static lv_point_t uiWfPressPt;

static void uiWfEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    uiWfPressMs = lv_tick_get();
    uiWfHoldFired = false;
    lv_indev_get_point(lv_indev_active(), &uiWfPressPt);
  } else if (code == LV_EVENT_PRESSING) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    bool still = LV_ABS(p.x - uiWfPressPt.x) < 40 && LV_ABS(p.y - uiWfPressPt.y) < 40;
    if (!uiWfHoldFired && still && lv_tick_elaps(uiWfPressMs) >= 2000) {
      uiWfHoldFired = true;
      vibrate(80);
      screen = UI_PICKER;
      lv_indev_wait_release(lv_indev_active());   // Loslassen löst nichts mehr aus
    }
  } else if (code == LV_EVENT_GESTURE) {
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_TOP) {
      screen = UI_APPS;
      lv_indev_wait_release(lv_indev_active());
    }
  }
}

// Apps und Zifferblatt-Auswahl: nach unten wischen = zurück zum Zifferblatt
static void uiSwipeDownHome(lv_event_t *e) {
  if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) {
    screen = UI_WF;
    // sonst kommt beim Loslassen noch ein CLICKED an der Kachel unter dem Finger an
    lv_indev_wait_release(lv_indev_active());
  }
}

static lv_obj_t *uiBuildWatchface() {
  lv_obj_t *s = uiScreen();
  wfBuild(s, watchface);
  lv_obj_add_event_cb(s, uiWfEvent, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s, uiWfEvent, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(s, uiWfEvent, LV_EVENT_GESTURE, nullptr);
  return s;
}

static lv_obj_t *uiGetScreen(int k) {
  if (k == UI_WF && uiScr[UI_WF] && uiWfBuilt != watchface) {
    // anderes Zifferblatt gewählt -> neu aufbauen (altes nur löschen, wenn nicht sichtbar)
    if (lv_screen_active() != uiScr[UI_WF]) lv_obj_delete(uiScr[UI_WF]);
    else lv_obj_delete_delayed(uiScr[UI_WF], 400);
    uiScr[UI_WF] = nullptr;
  }
  if (uiScr[k]) return uiScr[k];
  lv_obj_t *s = nullptr;
  switch (k) {
    case UI_WF: s = uiBuildWatchface(); uiWfBuilt = watchface; break;
    case UI_MELDE: s = uiBuildMelde(); break;
    case UI_SETTINGS: s = uiBuildSettings(); break;
    case UI_PICKER: s = uiBuildPicker(); lv_obj_add_event_cb(s, uiSwipeDownHome, LV_EVENT_GESTURE, nullptr); break;
    case UI_APPS: s = uiBuildApps(); lv_obj_add_event_cb(s, uiSwipeDownHome, LV_EVENT_GESTURE, nullptr); break;
    case UI_EDIT: s = uiBuildEdit(); break;
    case UI_ZEIT: s = uiBuildZeit(); break;
    case UI_TEST: s = uiBuildTest(); break;
    case UI_REC: s = uiBuildRec(); break;
    case UI_ALARM: s = uiBuildAlarm(); break;
  }
  uiScr[k] = s;
  return s;
}

static void uiRefreshShown() {
  switch (uiShown) {
    case UI_WF: wfUpdate(uiWfBuilt); break;
    case UI_MELDE: uiRefreshMelde(); break;
    case UI_SETTINGS: uiRefreshSettings(); break;
    case UI_PICKER: uiRefreshPicker(); break;
    case UI_APPS: uiRefreshApps(); break;
    case UI_EDIT: uiRefreshEdit(); break;
    case UI_ZEIT: uiRefreshZeit(); break;
    case UI_TEST: uiRefreshTest(); break;
    case UI_REC: uiRefreshRec(); break;
  }
  // Akku-Warnleiste über allen Masken
  bool low = cachedPct >= 0 && cachedPct < 20;
  uiSetHidden(uiBattBar, !low);
  if (low) uiSetTextFmt(lv_obj_get_child(uiBattBar, 0), LV_SYMBOL_BATTERY_1 "  Akku %d %%", cachedPct);
}

static void uiRefreshTimer(lv_timer_t *) {
  if (uiShown >= 0) uiRefreshShown();
}

// Gewünschte Maske aus dem Logik-Zustand
static int uiWanted() {
  if (screen == UI_ZEIT && zeitTab == 2) return UI_ALARM;
  if (screen < 0 || screen >= UI_COUNT || screen == 5) return UI_WF;
  return screen;
}

// In loop() aufrufen: lädt bei Navigation die passende Maske
static void uiSync() {
  int k = uiWanted();
  if (k == uiShown) {
    if (k == UI_MELDE) {
      lv_obj_t *tile = lv_tileview_get_tile_active(um.tv);
      if (tile && lv_obj_get_index(tile) != view) uiShowMeldeView();
    }
    if (k == UI_WF && uiWfBuilt != watchface) uiShown = -2;   // Zifferblatt gewechselt
    else return;
  }
  uiConfirmClose();
  lv_obj_t *s = uiGetScreen(k);
  if (k == UI_MELDE) uiShowMeldeView();
  int from = uiShown;
  uiShown = k;
  uiRefreshShown();   // Werte setzen, bevor die Maske sichtbar wird

  lv_screen_load_anim_t anim = LV_SCR_LOAD_ANIM_NONE;
  if (from >= 0 && !uiModalScr) {
    if (from == UI_WF && k == UI_APPS) anim = LV_SCR_LOAD_ANIM_MOVE_TOP;
    else if (k == UI_WF && from == UI_APPS) anim = LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
    else if (uiDepth(k) > uiDepth(from)) anim = LV_SCR_LOAD_ANIM_MOVE_LEFT;
    else if (uiDepth(k) < uiDepth(from)) anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    else anim = LV_SCR_LOAD_ANIM_FADE_IN;
  }
  if (UI_ANIM_MS == 0) anim = LV_SCR_LOAD_ANIM_NONE;
  lv_screen_load_anim(s, anim, anim == LV_SCR_LOAD_ANIM_NONE ? 0 : UI_ANIM_MS, 0, false);
  if (uiModalScr) {
    lv_obj_delete_delayed(uiModalScr, 50);
    uiModalScr = nullptr;
    uc.active = false;
  }
}

// Nach dem Aufwachen aus dem Standby alles neu übertragen (Panel war im Sleep)
static void uiInvalidateAll() {
  if (!halOk) return;
  lv_obj_invalidate(lv_screen_active());
}

// ---------------------------------------------------------------------------
// Touch (FT3168) als LVGL-Eingabegerät
// ---------------------------------------------------------------------------
static void uiTouchRead(lv_indev_t *, lv_indev_data_t *d) {
  uint16_t x, y;
  if (touchRead(x, y)) {
    d->state = LV_INDEV_STATE_PRESSED;
    d->point.x = x;
    d->point.y = y;
  } else {
    d->state = LV_INDEV_STATE_RELEASED;
  }
}

static void uiInit() {
  uiTouch = lv_indev_create();
  lv_indev_set_type(uiTouch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(uiTouch, uiTouchRead);
  lv_timer_set_period(lv_indev_get_read_timer(uiTouch), 15);
  lv_indev_set_long_press_time(uiTouch, 700);
  lv_indev_set_scroll_limit(uiTouch, 12);

  lv_obj_set_style_bg_color(lv_layer_bottom(), C_BG, 0);
  lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_COVER, 0);

  uiBattBar = uiBox(lv_layer_top());
  lv_obj_set_size(uiBattBar, LV_SIZE_CONTENT, 34);
  lv_obj_set_style_pad_hor(uiBattBar, 16, 0);
  lv_obj_set_style_radius(uiBattBar, 17, 0);
  lv_obj_set_style_bg_color(uiBattBar, C_RED, 0);
  lv_obj_set_style_bg_opa(uiBattBar, LV_OPA_COVER, 0);
  lv_obj_align(uiBattBar, LV_ALIGN_TOP_RIGHT, -UI_PAD, 8);
  lv_obj_t *bl = uiLabel(uiBattBar, &font_m16, C_TEXT, "");
  lv_obj_center(bl);
  lv_obj_add_flag(uiBattBar, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(uiRefreshTimer, 100, nullptr);
}

// ---------------------------------------------------------------------------
// Modale Masken für blockierende Abläufe (Kalibrierung, Erststart, Fehler).
// Diese laufen außerhalb von lv_timer_handler(); lv_refr_now() überträgt sofort.
// Danach lädt uiSync() wieder die normale Maske.
// ---------------------------------------------------------------------------

static lv_obj_t *uiModal() {
  lv_obj_t *s = uiScreen();
  lv_obj_t *old = uiModalScr;
  uiModalScr = s;
  lv_screen_load(s);
  if (old) lv_obj_delete(old);
  uiShown = -1;
  uc.active = false;
  uiConfirmClose();
  return s;
}

static void uiCalibBuild() {
  lv_obj_t *s = uiModal();
  lv_obj_t *h = uiLabel(s, &font_m20, C_TEXT2, "Kalibrierung");
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 30);
  uc.title = wfCenter(s, &font_m32, C_YELLOW, 92);
  uc.sub = wfCenter(s, &font_m24, C_CYAN, 140);
  uc.big = wfCenter(s, &font_d96, C_TEXT, 210);
  uc.rep = wfCenter(s, &font_m24, C_TEXT, 250);
  uc.bar = lv_bar_create(s);
  lv_obj_set_size(uc.bar, UI_W - 4 * UI_PAD, 16);
  lv_obj_align(uc.bar, LV_ALIGN_TOP_MID, 0, 300);
  lv_obj_set_style_bg_color(uc.bar, C_SURFACE2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(uc.bar, C_GREEN, LV_PART_INDICATOR);
  lv_obj_t *n = wfCenter(s, &font_m16, C_TEXT3, 420);
  lv_label_set_text(n, "Bei jeder Vibration: Position halten");
  uc.active = true;
}

// countdown > 0: große Zahl; rep > 0: Fortschritt „Wiederholung rep/reps“
static void uiCalibShow(const char *title, const char *sub, int countdown, int rep, int reps) {
  if (!halOk) return;   // ohne Display läuft die Kalibrierung blind weiter
  if (!uc.active) uiCalibBuild();
  uiSetText(uc.title, title);
  uiSetText(uc.sub, sub);
  uiSetHidden(uc.big, countdown <= 0);
  if (countdown > 0) uiSetTextFmt(uc.big, "%d", countdown);
  uiSetHidden(uc.rep, rep <= 0);
  uiSetHidden(uc.bar, rep <= 0);
  if (rep > 0) {
    uiSetTextFmt(uc.rep, "Wiederholung %d / %d", rep, reps);
    lv_bar_set_range(uc.bar, 0, reps);
    lv_bar_set_value(uc.bar, rep, LV_ANIM_OFF);
  }
  lv_refr_now(nullptr);
}

// Vollbild-Meldung (z. B. Kalibrierung fertig, TISCH gespeichert)
static void uiMessage(const char *icon, lv_color_t c, const char *title, const char *sub) {
  if (!halOk) return;
  lv_obj_t *s = uiModal();
  lv_obj_t *ring = wfCircle(s, UI_W / 2, 170, 64, c);
  lv_obj_t *ic = uiLabel(ring, &font_m32, lv_color_black(), icon);
  lv_obj_center(ic);
  lv_obj_t *t = wfCenter(s, &font_m32, C_TEXT, 270);
  lv_label_set_text(t, title);
  lv_obj_t *l = wfCenter(s, &font_m20, C_TEXT2, 320);
  lv_label_set_text(l, sub);
  lv_obj_set_style_pad_hor(l, UI_PAD, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_refr_now(nullptr);
}

static void uiFirstBoot() {
  uiMessage(LV_SYMBOL_REFRESH, C_GREEN, "Willkommen",
            "Uhr am Handgelenk anlegen und dann auf das Display tippen – die Kalibrierung startet.");
}

static void uiFatal(const char *title, const char *sub) {
  uiMessage(LV_SYMBOL_WARNING, C_RED, title, sub);
}
