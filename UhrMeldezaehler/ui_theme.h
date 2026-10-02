#pragma once
// ---------------------------------------------------------------------------
// Gestaltung der Uhr-Oberfläche: Farben, Schriften, wiederkehrende Bausteine.
// Dunkles Design – auf dem AMOLED kosten schwarze Pixel keinen Strom.
// Farben tragen Bedeutung: Grün = Melden/OK, Amber = Zeit, Cyan = Info,
// Rot = löschen/Warnung.
// ---------------------------------------------------------------------------
#include <lvgl.h>

LV_FONT_DECLARE(font_m16);
LV_FONT_DECLARE(font_m20);
LV_FONT_DECLARE(font_m24);
LV_FONT_DECLARE(font_m32);
LV_FONT_DECLARE(font_d48);   // nur Ziffern, : - % . und Leerzeichen
LV_FONT_DECLARE(font_d64);
LV_FONT_DECLARE(font_d96);

#define UI_W 410
#define UI_H 502
#define UI_PAD 20            // Seitenrand (Display hat abgerundete Ecken)
#define UI_HEADER_H 76

#define C_BG       lv_color_hex(0x000000)
#define C_SURFACE  lv_color_hex(0x1C1C1E)
#define C_SURFACE2 lv_color_hex(0x2C2C2E)
#define C_TEXT     lv_color_hex(0xFFFFFF)
#define C_TEXT2    lv_color_hex(0x9A9AA2)
#define C_TEXT3    lv_color_hex(0x5E5E66)
#define C_GREEN    lv_color_hex(0x30D158)
#define C_AMBER    lv_color_hex(0xFF9F0A)
#define C_YELLOW   lv_color_hex(0xFFD60A)
#define C_CYAN     lv_color_hex(0x64D2FF)
#define C_BLUE     lv_color_hex(0x0A84FF)
#define C_RED      lv_color_hex(0xFF453A)
#define C_PURPLE   lv_color_hex(0xBF5AF2)
#define C_PINK     lv_color_hex(0xFF375F)

// Label-Text nur setzen, wenn er sich geändert hat – sonst zeichnet LVGL den
// Bereich unnötig neu (Live-Werte werden mehrmals pro Sekunde aktualisiert).
static void uiSetText(lv_obj_t *lbl, const char *txt) {
  const char *cur = lv_label_get_text(lbl);
  if (cur && strcmp(cur, txt) == 0) return;
  lv_label_set_text(lbl, txt);
}

static void uiSetTextFmt(lv_obj_t *lbl, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  uiSetText(lbl, buf);
}

static void uiSetColor(lv_obj_t *obj, lv_color_t c) {
  if (lv_color_eq(lv_obj_get_style_text_color(obj, 0), c)) return;
  lv_obj_set_style_text_color(obj, c, 0);
}

static void uiSetHidden(lv_obj_t *obj, bool hidden) {
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
  if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *uiLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, txt);
  return l;
}

// Leerer, schwarzer Bildschirm ohne Scrollen
static lv_obj_t *uiScreen() {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, C_BG, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s, C_TEXT, 0);
  lv_obj_set_style_text_font(s, &font_m20, 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

// Unsichtbarer Container ohne Rahmen/Hintergrund
static lv_obj_t *uiBox(lv_obj_t *parent) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  return b;
}

// Kopfzeile mit Zurück-Knopf und Titel; Rückgabe: Zurück-Knopf
static lv_obj_t *uiHeader(lv_obj_t *scr, const char *title, lv_event_cb_t onBack) {
  lv_obj_t *back = lv_button_create(scr);
  lv_obj_set_size(back, 56, 56);
  lv_obj_set_pos(back, UI_PAD, 12);
  lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(back, C_SURFACE2, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_set_ext_click_area(back, 12);
  lv_obj_t *ic = uiLabel(back, &font_m24, C_TEXT, LV_SYMBOL_LEFT);
  lv_obj_center(ic);
  if (onBack) lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *t = uiLabel(scr, &font_m24, C_TEXT, title);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 18, 26);
  return back;
}

// Inhaltsbereich unter der Kopfzeile, Spalten-Layout, scrollt bei Bedarf
static lv_obj_t *uiContent(lv_obj_t *scr, int gap = 12) {
  lv_obj_t *c = lv_obj_create(scr);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, UI_W, UI_H - UI_HEADER_H);
  lv_obj_set_pos(c, 0, UI_HEADER_H);
  lv_obj_set_style_pad_hor(c, UI_PAD, 0);
  lv_obj_set_style_pad_bottom(c, 24, 0);
  lv_obj_set_style_pad_row(c, gap, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  return c;
}

// Abgerundete Karte (Fläche für zusammengehörige Werte)
static lv_obj_t *uiCard(lv_obj_t *parent) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_style_bg_color(c, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  lv_obj_set_style_pad_all(c, 16, 0);
  lv_obj_set_width(c, lv_pct(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

// Großer Knopf in Akzentfarbe; Text dunkel auf hellen Farben für Kontrast
static lv_obj_t *uiButton(lv_obj_t *parent, const char *txt, lv_color_t bg, lv_event_cb_t cb,
                          void *user = nullptr, int h = 72) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_width(b, lv_pct(100));
  lv_obj_set_height(b, h);
  lv_obj_set_style_radius(b, 22, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_darken(bg, LV_OPA_30), LV_STATE_PRESSED);
  bool lightBg = lv_color_brightness(bg) > 140;
  lv_obj_t *l = uiLabel(b, &font_m24, lightBg ? lv_color_black() : C_TEXT, txt);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  return b;
}

// Gedeckter Knopf (Fläche) mit farbigem Text – für Nebenaktionen
static lv_obj_t *uiButtonQuiet(lv_obj_t *parent, const char *txt, lv_color_t fg, lv_event_cb_t cb,
                               void *user = nullptr, int h = 72) {
  lv_obj_t *b = uiButton(parent, txt, C_SURFACE2, cb, user, h);
  lv_obj_set_style_text_color(lv_obj_get_child(b, 0), fg, 0);
  return b;
}

// Listenzeile: Symbol, Text, optional Wert rechts; ganze Zeile antippbar
static lv_obj_t *uiRow(lv_obj_t *parent, const char *icon, lv_color_t iconColor, const char *txt,
                       lv_event_cb_t cb, void *user = nullptr, lv_color_t txtColor = C_TEXT) {
  lv_obj_t *r = lv_button_create(parent);
  lv_obj_set_width(r, lv_pct(100));
  lv_obj_set_height(r, 68);
  lv_obj_set_style_radius(r, 20, 0);
  lv_obj_set_style_bg_color(r, C_SURFACE, 0);
  lv_obj_set_style_bg_color(r, C_SURFACE2, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(r, 0, 0);
  lv_obj_set_style_pad_hor(r, 18, 0);
  if (icon) {
    lv_obj_t *i = uiLabel(r, &font_m24, iconColor, icon);
    lv_obj_align(i, LV_ALIGN_LEFT_MID, 0, 0);
  }
  lv_obj_t *l = uiLabel(r, &font_m20, txtColor, txt);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, icon ? 44 : 0, 0);
  if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
  return r;
}

// Rechtsbündiger Wert in einer Listenzeile
static lv_obj_t *uiRowValue(lv_obj_t *row, const char *txt, lv_color_t c = C_TEXT2) {
  lv_obj_t *v = uiLabel(row, &font_m20, c, txt);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  return v;
}

// Zeile mit Schalter; Rückgabe: Schalter
static lv_obj_t *uiSwitchRow(lv_obj_t *parent, const char *icon, lv_color_t iconColor, const char *txt,
                             lv_event_cb_t cb) {
  lv_obj_t *r = uiRow(parent, icon, iconColor, txt, nullptr);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *sw = lv_switch_create(r);
  lv_obj_set_size(sw, 62, 36);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(sw, C_SURFACE2, 0);
  lv_obj_set_style_bg_color(sw, C_GREEN, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_ext_click_area(sw, 14);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return sw;
}

static void uiSetChecked(lv_obj_t *sw, bool on) {
  if (lv_obj_has_state(sw, LV_STATE_CHECKED) == on) return;
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  else lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

// ---------------------------------------------------------------------------
// Bestätigungsdialog (über allem, dunkelt den Rest ab)
// ---------------------------------------------------------------------------
typedef void (*UiConfirmFn)();
static UiConfirmFn uiConfirmOk = nullptr;
static lv_obj_t *uiConfirmLayer = nullptr;

static void uiConfirmClose() {
  if (uiConfirmLayer) lv_obj_delete(uiConfirmLayer);
  uiConfirmLayer = nullptr;
}

static void uiConfirmBtn(lv_event_t *e) {
  bool ok = lv_event_get_user_data(e) != nullptr;
  UiConfirmFn fn = uiConfirmOk;
  uiConfirmClose();
  if (ok && fn) fn();
}

static void uiConfirm(const char *title, const char *text, const char *okTxt, lv_color_t okColor, UiConfirmFn onOk) {
  uiConfirmClose();
  uiConfirmOk = onOk;
  uiConfirmLayer = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(uiConfirmLayer);
  lv_obj_set_size(uiConfirmLayer, UI_W, UI_H);
  lv_obj_set_style_bg_color(uiConfirmLayer, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(uiConfirmLayer, LV_OPA_80, 0);
  lv_obj_add_flag(uiConfirmLayer, LV_OBJ_FLAG_CLICKABLE);   // Klicks dahinter abfangen

  lv_obj_t *box = lv_obj_create(uiConfirmLayer);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, UI_W - 2 * UI_PAD, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_bg_color(box, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 28, 0);
  lv_obj_set_style_pad_all(box, 22, 0);
  lv_obj_set_style_pad_row(box, 14, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  uiLabel(box, &font_m24, C_TEXT, title);
  lv_obj_t *t = uiLabel(box, &font_m20, C_TEXT2, text);
  lv_obj_set_width(t, lv_pct(100));
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_bottom(t, 6, 0);
  uiButton(box, okTxt, okColor, uiConfirmBtn, (void *)1, 68);
  uiButtonQuiet(box, "Abbrechen", C_TEXT, uiConfirmBtn, nullptr, 64);
}

// ---------------------------------------------------------------------------
// Kurze Rückmeldung unten im Bild, verschwindet von selbst
// ---------------------------------------------------------------------------
static lv_obj_t *uiToastObj = nullptr;
static lv_timer_t *uiToastTimer = nullptr;

static void uiToastDone(lv_timer_t *t) {
  if (uiToastObj) lv_obj_delete(uiToastObj);
  uiToastObj = nullptr;
  uiToastTimer = nullptr;
}

static void uiToast(const char *txt, lv_color_t c = C_TEXT) {
  if (uiToastTimer) lv_timer_delete(uiToastTimer);
  if (uiToastObj) lv_obj_delete(uiToastObj);
  uiToastObj = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(uiToastObj);
  lv_obj_set_size(uiToastObj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(uiToastObj, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(uiToastObj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(uiToastObj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(uiToastObj, 22, 0);
  lv_obj_set_style_pad_ver(uiToastObj, 12, 0);
  lv_obj_align(uiToastObj, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_remove_flag(uiToastObj, LV_OBJ_FLAG_CLICKABLE);
  uiLabel(uiToastObj, &font_m20, c, txt);
  uiToastTimer = lv_timer_create(uiToastDone, 1800, nullptr);
  lv_timer_set_repeat_count(uiToastTimer, 1);
}
