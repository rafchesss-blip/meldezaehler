#pragma once
// ---------------------------------------------------------------------------
// Masken der Uhr. Jede Maske wird beim ersten Aufruf aufgebaut und bleibt
// erhalten; uiRefresh*() setzen nur die Live-Werte (alle 100 ms aus ui.h).
// Aktionen rufen ausschließlich Logikfunktionen aus UhrMeldezaehler_core.h
// (ctl*, setSensorOn, …) – die Masken halten keinen eigenen Zustand.
// ---------------------------------------------------------------------------

static void uiBackCb(lv_event_t *) { powerBack(); }

// ===========================================================================
// Apps
// ===========================================================================
struct UiApp {
  const char *icon, *name;
  uint32_t color;
  int screen;
};
static const UiApp UI_APP_LIST[5] = {
  {LV_SYMBOL_OK, "Melden", 0x30D158, 1},
  {LV_SYMBOL_BELL, "Zeit", 0xFF9F0A, 7},
  {LV_SYMBOL_SETTINGS, "Einstellungen", 0x8E8E93, 2},
  {LV_SYMBOL_SD_CARD, "Aufnahme", 0xFF453A, 9},
  {LV_SYMBOL_EYE_OPEN, "Test", 0x64D2FF, 8},
};

static void uiAppCb(lv_event_t *e) { ctlOpen((int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *uiAppsMelde;   // Unterzeile der Melden-Kachel (Zähler heute)

static lv_obj_t *uiBuildApps() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Apps", uiBackCb);
  for (int i = 0; i < 5; i++) {
    const UiApp &a = UI_APP_LIST[i];
    bool wide = (i == 4);
    lv_obj_t *t = lv_button_create(s);
    int w = wide ? UI_W - 2 * UI_PAD : (UI_W - 2 * UI_PAD - 14) / 2;
    lv_obj_set_size(t, w, wide ? 96 : 128);
    lv_obj_set_pos(t, UI_PAD + (wide ? 0 : (i % 2) * (w + 14)), UI_HEADER_H + 4 + (i / 2) * 142);
    lv_obj_set_style_radius(t, 26, 0);
    lv_obj_set_style_bg_color(t, C_SURFACE, 0);
    lv_obj_set_style_bg_color(t, C_SURFACE2, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(t, 0, 0);
    lv_obj_add_event_cb(t, uiAppCb, LV_EVENT_CLICKED, (void *)(intptr_t)a.screen);

    lv_obj_t *dot = uiBox(t);
    lv_obj_set_size(dot, 52, 52);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(a.color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_t *ic = uiLabel(dot, &font_m24, lv_color_black(), a.icon);
    lv_obj_center(ic);
    lv_obj_t *nm = uiLabel(t, &font_m20, C_TEXT, a.name);
    if (wide) {
      lv_obj_align(dot, LV_ALIGN_LEFT_MID, 6, 0);
      lv_obj_align(nm, LV_ALIGN_LEFT_MID, 76, 0);
    } else {
      lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 4, 4);
      lv_obj_align(nm, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    }
    if (a.screen == 1) {
      uiAppsMelde = uiLabel(t, &font_m16, C_TEXT2, "");
      lv_obj_align(uiAppsMelde, LV_ALIGN_TOP_RIGHT, -4, 8);
    }
  }
  return s;
}

static void uiRefreshApps() { uiSetTextFmt(uiAppsMelde, "%d heute", totalHeute); }

// ===========================================================================
// Melden: Seiten Zähler | Statistik | Kalibrierung (seitlich wischen)
// ===========================================================================
static struct {
  lv_obj_t *tv, *title, *dots[3];
  lv_obj_t *count, *state, *stateDot, *model, *hint;
  lv_obj_t *stSession, *stDrange, *stZeit, *stRF, *chart;
  lv_chart_series_t *ser;
  lv_obj_t *calStatus, *calSub;
} um;
static const char *UM_TITLES[3] = {"Melden", "Statistik", "Kalibrierung"};

static void uiResetConfirmed() {
  resetAllStats();
  uiToast(LV_SYMBOL_OK "  Zurückgesetzt", C_GREEN);
}

static void uiCounterLongCb(lv_event_t *) {
  uiConfirm("Zurücksetzen?", "Alle Meldungen und die Statistik von heute werden gelöscht.",
            "Zurücksetzen", C_RED, uiResetConfirmed);
}

static void uiTileCb(lv_event_t *e) {
  lv_obj_t *tile = lv_tileview_get_tile_active(um.tv);
  view = lv_obj_get_index(tile);
}

static void uiCalCb(lv_event_t *e) { ctlRequestCalibration((int)(intptr_t)lv_event_get_user_data(e)); }

static void uiCalClearConfirmed() { ctlRequestCalibClear(); }

static void uiCalClearCb(lv_event_t *) {
  uiConfirm("Kalibrierung löschen?", "Die Uhr startet neu und fragt danach eine neue Kalibrierung ab.",
            "Löschen + Neustart", C_RED, uiCalClearConfirmed);
}

static lv_obj_t *uiStatCard(lv_obj_t *parent, const char *head, lv_color_t c, lv_obj_t **val) {
  lv_obj_t *k = uiCard(parent);
  lv_obj_set_width(k, lv_pct(48));
  lv_obj_set_style_pad_all(k, 12, 0);
  uiLabel(k, &font_m16, C_TEXT2, head);
  *val = uiLabel(k, &font_m24, c, "");
  lv_obj_set_pos(*val, 0, 24);
  return k;
}

static lv_obj_t *uiBuildMelde() {
  lv_obj_t *s = uiScreen();
  lv_obj_t *back = uiHeader(s, "", uiBackCb);
  um.title = lv_obj_get_child(s, lv_obj_get_index(back) + 1);

  um.tv = lv_tileview_create(s);
  lv_obj_set_size(um.tv, UI_W, UI_H - UI_HEADER_H - 30);
  lv_obj_set_pos(um.tv, 0, UI_HEADER_H);
  lv_obj_set_style_bg_opa(um.tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(um.tv, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(um.tv, uiTileCb, LV_EVENT_VALUE_CHANGED, nullptr);

  // Seitenpunkte
  for (int i = 0; i < 3; i++) {
    um.dots[i] = uiBox(s);
    lv_obj_set_size(um.dots[i], 10, 10);
    lv_obj_set_style_radius(um.dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(um.dots[i], LV_OPA_COVER, 0);
    lv_obj_set_pos(um.dots[i], UI_W / 2 - 25 + i * 20, UI_H - 22);
  }

  // --- Seite 1: Zähler ---
  lv_obj_t *t0 = lv_tileview_add_tile(um.tv, 0, 0, LV_DIR_RIGHT);
  lv_obj_add_event_cb(t0, uiCounterLongCb, LV_EVENT_LONG_PRESSED, nullptr);
  um.count = uiLabel(t0, &font_d96, C_GREEN, "0");
  lv_obj_align(um.count, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_t *sub = uiLabel(t0, &font_m24, C_TEXT2, "Meldungen heute");
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 104);

  lv_obj_t *chip = uiBox(t0);
  lv_obj_set_size(chip, LV_SIZE_CONTENT, 52);
  lv_obj_set_style_pad_hor(chip, 22, 0);
  lv_obj_set_style_radius(chip, 26, 0);
  lv_obj_set_style_bg_color(chip, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(chip, 12, 0);
  lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 160);
  um.stateDot = uiBox(chip);
  lv_obj_set_size(um.stateDot, 14, 14);
  lv_obj_set_style_radius(um.stateDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(um.stateDot, LV_OPA_COVER, 0);
  um.state = uiLabel(chip, &font_m24, C_TEXT, "");

  um.model = uiLabel(t0, &font_m20, C_CYAN, "");
  lv_obj_align(um.model, LV_ALIGN_TOP_MID, 0, 230);
  um.hint = uiLabel(t0, &font_m20, C_TEXT3, "");
  lv_obj_set_width(um.hint, UI_W - 2 * UI_PAD);
  lv_obj_set_style_text_align(um.hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(um.hint, LV_ALIGN_TOP_MID, 0, 290);
  lv_obj_t *hold = uiLabel(t0, &font_m16, C_TEXT3, "Lange drücken: zurücksetzen");
  lv_obj_align(hold, LV_ALIGN_BOTTOM_MID, 0, -6);

  // --- Seite 2: Statistik ---
  lv_obj_t *t1 = lv_tileview_add_tile(um.tv, 1, 0, LV_DIR_HOR);
  lv_obj_t *c1 = uiBox(t1);
  lv_obj_set_size(c1, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_hor(c1, UI_PAD, 0);
  lv_obj_set_flex_flow(c1, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(c1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(c1, 10, 0);
  uiStatCard(c1, "Diese Stunde", C_CYAN, &um.stSession);
  uiStatCard(c1, "Drangenommen", C_GREEN, &um.stDrange);
  uiStatCard(c1, "Meldezeit", C_AMBER, &um.stZeit);
  uiStatCard(c1, "Richtig / Falsch", C_TEXT, &um.stRF);
  lv_obj_t *cc = uiCard(c1);
  lv_obj_set_style_pad_all(cc, 12, 0);
  uiLabel(cc, &font_m16, C_TEXT2, "Meldungen pro Minute (letzte Stunde)");
  um.chart = lv_chart_create(cc);
  lv_obj_set_size(um.chart, lv_pct(100), 120);
  lv_obj_set_pos(um.chart, 0, 26);
  lv_chart_set_type(um.chart, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(um.chart, 60);
  lv_chart_set_div_line_count(um.chart, 0, 0);
  lv_obj_set_style_bg_opa(um.chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(um.chart, 0, 0);
  lv_obj_set_style_pad_all(um.chart, 0, 0);
  lv_obj_set_style_pad_column(um.chart, 1, 0);
  lv_obj_set_style_radius(um.chart, 2, LV_PART_ITEMS);
  um.ser = lv_chart_add_series(um.chart, C_GREEN, LV_CHART_AXIS_PRIMARY_Y);

  // --- Seite 3: Kalibrierung ---
  lv_obj_t *t2 = lv_tileview_add_tile(um.tv, 2, 0, LV_DIR_LEFT);
  lv_obj_t *c2 = uiBox(t2);
  lv_obj_set_size(c2, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_hor(c2, UI_PAD, 0);
  lv_obj_set_style_pad_bottom(c2, 16, 0);
  lv_obj_set_style_pad_row(c2, 10, 0);
  lv_obj_set_flex_flow(c2, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(c2, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(c2, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(c2, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t *st = uiCard(c2);
  um.calStatus = uiLabel(st, &font_m24, C_GREEN, "");
  um.calSub = uiLabel(st, &font_m16, C_TEXT2, "");
  lv_obj_set_pos(um.calSub, 0, 32);
  lv_obj_set_width(um.calSub, lv_pct(100));
  lv_label_set_long_mode(um.calSub, LV_LABEL_LONG_WRAP);
  uiRow(c2, LV_SYMBOL_REFRESH, C_YELLOW, "Komplett (3 Schritte)", uiCalCb, (void *)0);
  uiRow(c2, LV_SYMBOL_DOWN, C_CYAN, "Arm unten", uiCalCb, (void *)1);
  uiRow(c2, LV_SYMBOL_UP, C_CYAN, "Arm hoch", uiCalCb, (void *)2);
  uiRow(c2, LV_SYMBOL_SHUFFLE, C_CYAN, "Nicht melden", uiCalCb, (void *)3);
  uiRow(c2, LV_SYMBOL_HOME, C_CYAN, "Tisch (Uhr hinlegen)", uiCalCb, (void *)4);
  uiRow(c2, LV_SYMBOL_TRASH, C_RED, "Kalibrierung löschen", uiCalClearCb, nullptr, C_RED);
  return s;
}

static void uiShowMeldeView() {
  int v = (view < 0 || view > 2) ? 0 : view;
  lv_tileview_set_tile_by_index(um.tv, v, 0, LV_ANIM_OFF);
}

static void uiRefreshMelde() {
  lv_obj_t *tile = lv_tileview_get_tile_active(um.tv);
  int v = tile ? lv_obj_get_index(tile) : 0;
  uiSetText(um.title, UM_TITLES[v]);
  for (int i = 0; i < 3; i++)
    uiSetBg(um.dots[i], i == v ? C_TEXT : C_TEXT3);

  // Zähler
  uiSetTextFmt(um.count, "%d", totalHeute);
  if (!sensorOn) {
    uiSetText(um.state, "Sensor aus");
    uiSetBg(um.stateDot, C_RED);
  } else if (imHoch) {
    uiSetText(um.state, "Arm oben");
    uiSetBg(um.stateDot, C_AMBER);
  } else {
    uiSetText(um.state, "Arm unten");
    uiSetBg(um.stateDot, C_GREEN);
  }
  if (sensorOn)
    uiSetTextFmt(um.model, "Erkannt: %s  %d %%", aktuellKlasse == 0 ? "Meldung" : "keine Meldung",
                 (int)(aktuellProb[aktuellKlasse] * 100));
  else
    uiSetText(um.model, "");
  if (!calibrated) {
    uiSetText(um.hint, "Nicht kalibriert – Seite Kalibrierung");
    uiSetColor(um.hint, C_RED);
  } else if (meldungenSeitCalib >= 10) {
    uiSetText(um.hint, "Bitte neu kalibrieren");
    uiSetColor(um.hint, C_RED);
  } else {
    uiSetText(um.hint, "Wischen: Statistik");
    uiSetColor(um.hint, C_TEXT3);
  }

  // Statistik
  uiSetTextFmt(um.stSession, "%d", sessionCount);
  uiSetTextFmt(um.stDrange, "%d", drange);
  unsigned long sec = meldeZeitMs / 1000;
  uiSetTextFmt(um.stZeit, "%lu:%02lu", sec / 60, sec % 60);
  uiSetTextFmt(um.stRF, "%d / %d", richtig, falsch);
  int mx = 1;
  for (int i = 0; i < 60; i++) {
    int vv = minHist[(minHistIdx + i) % 60];
    if (vv > mx) mx = vv;
  }
  static int lastMx = -1;
  bool changed = false;
  if (mx != lastMx) {
    lv_chart_set_range(um.chart, LV_CHART_AXIS_PRIMARY_Y, 0, mx);
    lastMx = mx;
    changed = true;
  }
  for (int i = 0; i < 60; i++) {
    int32_t vv = minHist[(minHistIdx + i) % 60];
    if (lv_chart_get_y_array(um.chart, um.ser)[i] != vv) {
      lv_chart_set_value_by_id(um.chart, um.ser, i, vv);
      changed = true;
    }
  }
  if (changed) lv_chart_refresh(um.chart);

  // Kalibrierung
  if (calibrated) {
    uiSetText(um.calStatus, LV_SYMBOL_OK "  Kalibriert");
    uiSetColor(um.calStatus, meldungenSeitCalib >= 10 ? C_AMBER : C_GREEN);
    uiSetTextFmt(um.calSub, "%d Meldungen seit Kalibrierung%s", meldungenSeitCalib,
                 tischCalibrated ? " · Tisch gelernt" : "");
  } else {
    uiSetText(um.calStatus, LV_SYMBOL_WARNING "  Nicht kalibriert");
    uiSetColor(um.calStatus, C_RED);
    uiSetText(um.calSub, "Komplett-Kalibrierung starten");
  }
}

// ===========================================================================
// Einstellungen (eine Seite statt Untermenüs)
// ===========================================================================
static struct {
  lv_obj_t *slider, *pct, *swBt, *swSensor, *swMotor, *swMute, *btInfo;
} us;

static void uiBrightCb(lv_event_t *e) {
  bool released = lv_event_get_code(e) == LV_EVENT_RELEASED;
  ctlSetBrightness(lv_slider_get_value(us.slider), released);   // NVS nur beim Loslassen
}
static void uiSwBtCb(lv_event_t *e) {
  if (lv_obj_has_state(us.swBt, LV_STATE_CHECKED)) btEnable();
  else btDisable();
}
static void uiSwSensorCb(lv_event_t *) { setSensorOn(lv_obj_has_state(us.swSensor, LV_STATE_CHECKED)); }
static void uiSwMotorCb(lv_event_t *) { setMotorOn(lv_obj_has_state(us.swMotor, LV_STATE_CHECKED)); }
static void uiSwMuteCb(lv_event_t *) { setMuteInLessons(lv_obj_has_state(us.swMute, LV_STATE_CHECKED)); }

static lv_obj_t *uiBuildSettings() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Einstellungen", uiBackCb);
  lv_obj_t *c = uiContent(s, 10);

  lv_obj_t *bc = uiCard(c);
  uiLabel(bc, &font_m20, C_TEXT, LV_SYMBOL_IMAGE "  Helligkeit");
  us.pct = uiLabel(bc, &font_m20, C_TEXT2, "");
  lv_obj_align(us.pct, LV_ALIGN_TOP_RIGHT, 0, 0);
  us.slider = lv_slider_create(bc);
  lv_obj_set_width(us.slider, lv_pct(94));
  lv_obj_set_height(us.slider, 14);
  lv_obj_align(us.slider, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_pad_bottom(bc, 26, 0);
  lv_slider_set_range(us.slider, 10, 255);
  lv_obj_set_style_bg_color(us.slider, C_SURFACE2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(us.slider, C_YELLOW, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(us.slider, C_TEXT, LV_PART_KNOB);
  lv_obj_set_ext_click_area(us.slider, 20);
  lv_obj_add_event_cb(us.slider, uiBrightCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(us.slider, uiBrightCb, LV_EVENT_RELEASED, nullptr);

  us.swBt = uiSwitchRow(c, LV_SYMBOL_BLUETOOTH, C_BLUE, "Bluetooth", uiSwBtCb);
  us.btInfo = uiLabel(c, &font_m16, C_TEXT2, "");
  lv_obj_set_style_pad_left(us.btInfo, 18, 0);
  us.swSensor = uiSwitchRow(c, LV_SYMBOL_EYE_OPEN, C_GREEN, "Meldungs-Sensor", uiSwSensorCb);
  us.swMotor = uiSwitchRow(c, LV_SYMBOL_BELL, C_AMBER, "Vibration", uiSwMotorCb);
  us.swMute = uiSwitchRow(c, LV_SYMBOL_MUTE, C_PURPLE, "Stumm im Unterricht", uiSwMuteCb);
  lv_obj_t *mi = uiLabel(c, &font_m16, C_TEXT2, "Stumm: keine Vibration während einer Stunde laut Stundenplan.");
  lv_obj_set_width(mi, lv_pct(100));
  lv_label_set_long_mode(mi, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_hor(mi, 18, 0);
  return s;
}

static void uiRefreshSettings() {
  if (!lv_obj_has_state(us.slider, LV_STATE_PRESSED)) lv_slider_set_value(us.slider, brightness, LV_ANIM_OFF);
  uiSetTextFmt(us.pct, "%d %%", brightness * 100 / 255);
  uiSetChecked(us.swBt, btOn);
  uiSetChecked(us.swSensor, sensorOn);
  uiSetChecked(us.swMotor, motorOn);
  uiSetChecked(us.swMute, muteInLessons);
  uiSetText(us.btInfo, btOn ? "Sichtbar als „Meldezaehler“ für die App" : "Aus – App kann sich nicht verbinden");
}

// ===========================================================================
// Zifferblatt-Auswahl
// ===========================================================================
static lv_obj_t *uiPickRows[6], *uiPickChecks[6];
static const uint32_t WF_COLORS[6] = {0x9A9AA2, 0xFF9F0A, 0xFFFFFF, 0x64D2FF, 0xBF5AF2, 0x30D158};

static void uiPickCb(lv_event_t *e) { ctlSelectWatchface((int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *uiBuildPicker() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Zifferblatt", uiBackCb);
  lv_obj_t *c = uiContent(s, 8);
  for (int i = 0; i < 6; i++) {
    uiPickRows[i] = uiRow(c, LV_SYMBOL_IMAGE, lv_color_hex(WF_COLORS[i]), WF_NAMES[i], uiPickCb, (void *)(intptr_t)i);
    lv_obj_set_height(uiPickRows[i], 60);   // alle 6 ohne Scrollen -> Wischen nach unten bleibt Geste
    uiPickChecks[i] = uiRowValue(uiPickRows[i], LV_SYMBOL_OK, C_GREEN);
  }
  return s;
}

static void uiRefreshPicker() {
  for (int i = 0; i < 6; i++) {
    uiSetHidden(uiPickChecks[i], i != watchface);
    int bw = i == watchface ? 2 : 0;
    if (lv_obj_get_style_border_width(uiPickRows[i], 0) != bw) {
      lv_obj_set_style_border_width(uiPickRows[i], bw, 0);
      lv_obj_set_style_border_color(uiPickRows[i], C_GREEN, 0);
    }
  }
}

// ===========================================================================
// Meldungen bearbeiten (BOOT-Taste): Menü | letzte Meldung | Antwort
// ===========================================================================
static struct {
  lv_obj_t *title, *mode[3], *count0, *count1;
} ue;
static const char *UE_TITLES[3] = {"Meldungen", "Letzte Meldung", "Drangenommen"};

static void uiEditDelLast(lv_event_t *) {
  if (totalHeute <= 0) { uiToast("Keine Meldung vorhanden", C_AMBER); return; }
  removeLastMeldung();
  uiToast(LV_SYMBOL_OK "  Letzte gelöscht", C_GREEN);
}
static void uiEditAdd(lv_event_t *) {
  registerMeldung(0);
  meldeEditMode = 1;
}
static void uiEditEdit(lv_event_t *) {
  if (totalHeute <= 0) { uiToast("Keine Meldung vorhanden", C_AMBER); return; }
  meldeEditMode = 1;
}
static void uiEditDelAllOk() {
  resetAllStats();
  uiToast(LV_SYMBOL_OK "  Alle gelöscht", C_GREEN);
}
static void uiEditDelAll(lv_event_t *) {
  uiConfirm("Alle löschen?", "Alle Meldungen und die Statistik von heute werden gelöscht.",
            "Alle löschen", C_RED, uiEditDelAllOk);
}
static void uiEditDrange(lv_event_t *) { ctlMarkDrange(); }
static void uiEditNotCalled(lv_event_t *) {
  meldeEditMode = 0;
  screen = 0;
}
static void uiEditAnswer(lv_event_t *e) {
  ctlMarkAnswer(lv_event_get_user_data(e) != nullptr);
  uiToast(LV_SYMBOL_OK "  Gespeichert", C_GREEN);
}

static lv_obj_t *uiEditPage(lv_obj_t *s) {
  lv_obj_t *c = uiContent(s, 12);
  return c;
}

static lv_obj_t *uiBuildEdit() {
  lv_obj_t *s = uiScreen();
  lv_obj_t *back = uiHeader(s, "", uiBackCb);
  ue.title = lv_obj_get_child(s, lv_obj_get_index(back) + 1);

  lv_obj_t *p0 = ue.mode[0] = uiEditPage(s);
  ue.count0 = uiLabel(p0, &font_m24, C_TEXT2, "");
  lv_obj_set_style_pad_bottom(ue.count0, 4, 0);
  uiButton(p0, LV_SYMBOL_PLUS "  Hinzufügen", C_GREEN, uiEditAdd);
  uiButtonQuiet(p0, LV_SYMBOL_EDIT "  Letzte bearbeiten", C_CYAN, uiEditEdit);
  uiButtonQuiet(p0, LV_SYMBOL_BACKSPACE "  Letzte löschen", C_AMBER, uiEditDelLast);
  uiButtonQuiet(p0, LV_SYMBOL_TRASH "  Alle löschen", C_RED, uiEditDelAll);

  lv_obj_t *p1 = ue.mode[1] = uiEditPage(s);
  ue.count1 = uiLabel(p1, &font_m24, C_TEXT2, "");
  lv_obj_set_style_pad_bottom(ue.count1, 4, 0);
  uiButton(p1, "Drangenommen", C_GREEN, uiEditDrange, nullptr, 110);
  uiButtonQuiet(p1, "Nicht drangenommen", C_TEXT, uiEditNotCalled, nullptr, 110);

  lv_obj_t *p2 = ue.mode[2] = uiEditPage(s);
  uiLabel(p2, &font_m24, C_TEXT2, "Die Antwort war …");
  uiButton(p2, LV_SYMBOL_OK "  Richtig", C_GREEN, uiEditAnswer, (void *)1, 120);
  uiButton(p2, LV_SYMBOL_CLOSE "  Falsch", C_RED, uiEditAnswer, nullptr, 120);
  return s;
}

static void uiRefreshEdit() {
  int m = (meldeEditMode < 0 || meldeEditMode > 2) ? 0 : meldeEditMode;
  uiSetText(ue.title, UE_TITLES[m]);
  for (int i = 0; i < 3; i++) uiSetHidden(ue.mode[i], i != m);
  uiSetTextFmt(ue.count0, "Heute: %d", totalHeute);
  uiSetTextFmt(ue.count1, "Meldung Nr. %d", totalHeute);
}

// ===========================================================================
// Zeit: Timer | Stoppuhr; Alarm als eigene Maske
// ===========================================================================
static struct {
  lv_obj_t *seg[2], *page[2];
  lv_obj_t *tBig, *tRollers, *rMin, *rSec, *tStart, *tStartLbl;
  lv_obj_t *swBig, *swStartLbl, *swStart;
} uz;

static void uiZeitTabCb(lv_event_t *e) { zeitTab = (int)(intptr_t)lv_event_get_user_data(e); }
static void uiRollerCb(lv_event_t *) {
  ctlTimerSet((lv_roller_get_selected(uz.rMin) * 60UL + lv_roller_get_selected(uz.rSec)) * 1000UL);
}
static void uiTimerStartCb(lv_event_t *) { ctlTimerToggle(); }
static void uiTimerResetCb(lv_event_t *) { ctlTimerReset(); }
static void uiSwStartCb(lv_event_t *) { ctlStopwatchToggle(); }
static void uiSwResetCb(lv_event_t *) { ctlStopwatchReset(); }

static lv_obj_t *uiRoller(lv_obj_t *parent, int maxVal, const char *unit) {
  static char opts[2][400];
  static int n = 0;
  char *o = opts[n++ & 1];
  o[0] = 0;
  for (int i = 0; i <= maxVal; i++) {
    char b[8];
    snprintf(b, sizeof(b), i ? "\n%02d" : "%02d", i);
    strcat(o, b);
  }
  lv_obj_t *col = uiBox(parent);
  lv_obj_set_size(col, 150, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 6, 0);
  lv_obj_t *r = lv_roller_create(col);
  // Schrift vor der Zeilenzahl setzen – die Höhe wird aus der Schrift berechnet
  lv_obj_set_style_text_font(r, &font_m32, 0);
  lv_obj_set_style_text_font(r, &font_m32, LV_PART_SELECTED);
  lv_roller_set_options(r, o, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(r, 3);
  lv_obj_set_width(r, 130);
  lv_obj_set_style_bg_color(r, C_SURFACE, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_radius(r, 20, 0);
  lv_obj_set_style_text_color(r, C_TEXT3, 0);
  lv_obj_set_style_bg_color(r, C_SURFACE2, LV_PART_SELECTED);
  lv_obj_set_style_text_color(r, C_AMBER, LV_PART_SELECTED);
  lv_obj_add_event_cb(r, uiRollerCb, LV_EVENT_VALUE_CHANGED, nullptr);
  uiLabel(col, &font_m16, C_TEXT2, unit);
  return r;
}

static lv_obj_t *uiBuildZeit() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Zeit", uiBackCb);

  static const char *tabs[2] = {"Timer", "Stoppuhr"};
  for (int i = 0; i < 2; i++) {
    uz.seg[i] = lv_button_create(s);
    lv_obj_set_size(uz.seg[i], (UI_W - 2 * UI_PAD - 8) / 2, 52);
    lv_obj_set_pos(uz.seg[i], UI_PAD + i * ((UI_W - 2 * UI_PAD) / 2 + 4), UI_HEADER_H);
    lv_obj_set_style_radius(uz.seg[i], 26, 0);
    lv_obj_set_style_shadow_width(uz.seg[i], 0, 0);
    lv_obj_t *l = uiLabel(uz.seg[i], &font_m20, C_TEXT, tabs[i]);
    lv_obj_center(l);
    lv_obj_add_event_cb(uz.seg[i], uiZeitTabCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }

  for (int i = 0; i < 2; i++) {
    uz.page[i] = uiBox(s);
    lv_obj_set_size(uz.page[i], UI_W - 2 * UI_PAD, UI_H - UI_HEADER_H - 64);
    lv_obj_set_pos(uz.page[i], UI_PAD, UI_HEADER_H + 64);
  }

  // Timer
  lv_obj_t *tp = uz.page[0];
  uz.tBig = uiLabel(tp, &font_d96, C_AMBER, "");
  lv_obj_align(uz.tBig, LV_ALIGN_TOP_MID, 0, 20);
  uz.tRollers = uiBox(tp);
  lv_obj_set_size(uz.tRollers, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(uz.tRollers, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(uz.tRollers, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_align(uz.tRollers, LV_ALIGN_TOP_MID, 0, 0);
  uz.rMin = uiRoller(uz.tRollers, 99, "Minuten");
  uz.rSec = uiRoller(uz.tRollers, 59, "Sekunden");
  lv_obj_t *row = uiBox(tp);
  lv_obj_set_size(row, lv_pct(100), 80);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 12, 0);
  uz.tStart = uiButton(row, "", C_GREEN, uiTimerStartCb, nullptr, 80);
  lv_obj_set_flex_grow(uz.tStart, 2);
  uz.tStartLbl = lv_obj_get_child(uz.tStart, 0);
  lv_obj_t *tr = uiButtonQuiet(row, LV_SYMBOL_REFRESH, C_TEXT, uiTimerResetCb, nullptr, 80);
  lv_obj_set_flex_grow(tr, 1);

  // Stoppuhr
  lv_obj_t *sp = uz.page[1];
  uz.swBig = uiLabel(sp, &font_d64, C_TEXT, "");
  lv_obj_align(uz.swBig, LV_ALIGN_TOP_MID, 0, 70);
  lv_obj_t *row2 = uiBox(sp);
  lv_obj_set_size(row2, lv_pct(100), 80);
  lv_obj_align(row2, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row2, 12, 0);
  uz.swStart = uiButton(row2, "", C_GREEN, uiSwStartCb, nullptr, 80);
  lv_obj_set_flex_grow(uz.swStart, 2);
  uz.swStartLbl = lv_obj_get_child(uz.swStart, 0);
  lv_obj_t *sr = uiButtonQuiet(row2, LV_SYMBOL_REFRESH, C_TEXT, uiSwResetCb, nullptr, 80);
  lv_obj_set_flex_grow(sr, 1);
  return s;
}

static void uiRefreshZeit() {
  int tab = zeitTab == 1 ? 1 : 0;
  for (int i = 0; i < 2; i++) {
    uiSetHidden(uz.page[i], i != tab);
    uiSetBg(uz.seg[i], i == tab ? C_AMBER : C_SURFACE);
    uiSetColor(lv_obj_get_child(uz.seg[i], 0), i == tab ? lv_color_black() : C_TEXT);
  }
  // Timer: Rollen zum Einstellen, solange er steht und voll ist; sonst Restzeit groß
  bool setMode = !timerRunning && timerRemainingMs == timerSetMs;
  uiSetHidden(uz.tRollers, !setMode);
  uiSetHidden(uz.tBig, setMode);
  unsigned long r = (timerRemainingMs + 999) / 1000;
  uiSetTextFmt(uz.tBig, "%02lu:%02lu", r / 60, r % 60);
  if (setMode) {
    unsigned long s = timerSetMs / 1000;
    if ((unsigned long)lv_roller_get_selected(uz.rMin) != s / 60) lv_roller_set_selected(uz.rMin, s / 60, LV_ANIM_OFF);
    if ((unsigned long)lv_roller_get_selected(uz.rSec) != s % 60) lv_roller_set_selected(uz.rSec, s % 60, LV_ANIM_OFF);
  }
  uiSetText(uz.tStartLbl, timerRunning ? LV_SYMBOL_PAUSE "  Pause" : (setMode ? LV_SYMBOL_PLAY "  Start" : LV_SYMBOL_PLAY "  Weiter"));
  uiSetBg(uz.tStart, timerRunning ? C_AMBER : C_GREEN);

  // Stoppuhr
  unsigned long ms = ctlStopwatchMs();
  uiSetTextFmt(uz.swBig, "%02lu:%02lu.%02lu", (ms / 60000) % 100, (ms / 1000) % 60, (ms / 10) % 100);
  uiSetText(uz.swStartLbl, stopwatchRunning ? LV_SYMBOL_STOP "  Stopp" : LV_SYMBOL_PLAY "  Start");
  uiSetBg(uz.swStart, stopwatchRunning ? C_RED : C_GREEN);
}

static void uiAlarmStopCb(lv_event_t *) { ctlAlarmStop(); }

static lv_obj_t *uiBuildAlarm() {
  lv_obj_t *s = uiScreen();
  lv_obj_t *ring = wfCircle(s, UI_W / 2, 150, 70, C_AMBER);
  lv_obj_t *ic = uiLabel(ring, &font_m32, lv_color_black(), LV_SYMBOL_BELL);
  lv_obj_center(ic);
  lv_obj_t *t = uiLabel(s, &font_m32, C_TEXT, "Timer abgelaufen");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 250);
  lv_obj_t *box = uiBox(s);
  lv_obj_set_size(box, UI_W - 2 * UI_PAD, 120);
  lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -36);
  uiButton(box, LV_SYMBOL_STOP "  Stoppen", C_RED, uiAlarmStopCb, nullptr, 120);
  return s;
}

// ===========================================================================
// Test: Motor, Sensor, Akku
// ===========================================================================
static struct {
  lv_obj_t *sensor, *batt;
} ut;

static void uiTestMotorCb(lv_event_t *) {
  vibrate(300);
  uiToast(motorOn ? "Motor: 300 ms" : "Vibration ist ausgeschaltet", motorOn ? C_TEXT : C_AMBER);
}

static lv_obj_t *uiBuildTest() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Test", uiBackCb);
  lv_obj_t *c = uiContent(s, 12);
  uiButton(c, LV_SYMBOL_BELL "  Motor testen", C_AMBER, uiTestMotorCb);
  lv_obj_t *k1 = uiCard(c);
  uiLabel(k1, &font_m16, C_TEXT2, "Sensor");
  ut.sensor = uiLabel(k1, &font_m24, C_CYAN, "");
  lv_obj_set_pos(ut.sensor, 0, 26);
  lv_obj_t *k2 = uiCard(c);
  uiLabel(k2, &font_m16, C_TEXT2, "Akku");
  ut.batt = uiLabel(k2, &font_m24, C_GREEN, "");
  lv_obj_set_pos(ut.batt, 0, 26);
  return s;
}

static void uiRefreshTest() {
  if (sensorOn)
    uiSetTextFmt(ut.sensor, "%s  %d %%", aktuellKlasse == 0 ? "Meldung" : "keine Meldung",
                 (int)(aktuellProb[aktuellKlasse] * 100));
  else
    uiSetText(ut.sensor, "Sensor aus");
  if (cachedPct >= 0)
    uiSetTextFmt(ut.batt, "%d %%  ·  %d.%02d V%s", cachedPct, battMV / 1000, (battMV % 1000) / 10,
                 battCharging ? "  ·  lädt" : "");
  else
    uiSetText(ut.batt, "kein Akku erkannt");
}

// ===========================================================================
// Sensor-Aufnahme (Trainingsdaten auf SD-Karte)
// ===========================================================================
static struct {
  lv_obj_t *idle, *rec, *cls, *count;
} ur;

static void uiRecStartCb(lv_event_t *e) {
  bool meld = lv_event_get_user_data(e) != nullptr;
  startSensorRec(meld ? "meldung" : "nicht_meldung");
  if (!sensorRec) uiToast(LV_SYMBOL_WARNING "  SD-Karte nicht lesbar", C_RED);
}
static void uiRecStopCb(lv_event_t *) {
  stopSensorRec();
  uiToast(LV_SYMBOL_OK "  Gespeichert", C_GREEN);
}

static lv_obj_t *uiBuildRec() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Aufnahme", uiBackCb);
  ur.idle = uiContent(s, 12);
  lv_obj_t *h = uiLabel(ur.idle, &font_m20, C_TEXT2, "Klasse wählen – Aufnahme startet sofort auf die SD-Karte.");
  lv_obj_set_width(h, lv_pct(100));
  lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  uiButton(ur.idle, LV_SYMBOL_UP "  Meldung", C_GREEN, uiRecStartCb, (void *)1, 110);
  uiButton(ur.idle, LV_SYMBOL_SHUFFLE "  Nicht melden", C_BLUE, uiRecStartCb, nullptr, 110);
  lv_obj_t *h2 = uiLabel(ur.idle, &font_m16, C_TEXT3, "Nicht melden: einfach normal bewegen.");
  lv_obj_set_width(h2, lv_pct(100));
  lv_label_set_long_mode(h2, LV_LABEL_LONG_WRAP);

  ur.rec = uiContent(s, 12);
  lv_obj_t *k = uiCard(ur.rec);
  lv_obj_t *dot = wfCircle(k, 8, 14, 8, C_RED);
  LV_UNUSED(dot);
  lv_obj_t *l = uiLabel(k, &font_m20, C_RED, "Aufnahme läuft");
  lv_obj_set_pos(l, 26, 2);
  ur.cls = uiLabel(k, &font_m24, C_TEXT, "");
  lv_obj_set_pos(ur.cls, 0, 38);
  ur.count = uiLabel(k, &font_d64, C_CYAN, "");
  lv_obj_set_pos(ur.count, 0, 76);
  lv_obj_t *u = uiLabel(k, &font_m16, C_TEXT2, "Messwerte");
  lv_obj_set_pos(u, 0, 150);
  uiButton(ur.rec, LV_SYMBOL_STOP "  Stoppen", C_RED, uiRecStopCb, nullptr, 96);
  return s;
}

static void uiRefreshRec() {
  uiSetHidden(ur.idle, sensorRec);
  uiSetHidden(ur.rec, !sensorRec);
  if (sensorRec) {
    uiSetTextFmt(ur.cls, "%s  ·  #%u", sensorRecLabel.c_str(), (unsigned)sensorRecTrial);
    uiSetTextFmt(ur.count, "%lu", (unsigned long)sensorRecCount);
  }
}
