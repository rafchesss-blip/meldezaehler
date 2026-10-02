#pragma once
// ---------------------------------------------------------------------------
// Die sechs Zifferblätter (Minimal, Farbig, Analog, Digital, Geometrisch,
// Schule). Inhalte wie in der Canvas-Fassung; aktualisiert wird 1x/s und nur
// geänderter Text (LVGL zeichnet dann nur diese Flächen neu).
// ---------------------------------------------------------------------------

static const char *WF_NAMES[6] = {"Minimal", "Farbig", "Analog", "Digital", "Geometrisch", "Schule"};

// --- Textbausteine aus den gecachten RTC-/Akku-Werten -----------------------
static void uiTimeStr(char *buf, size_t n, bool seconds) {
  if (cachedH < 0) snprintf(buf, n, seconds ? "--:--:--" : "--:--");
  else if (seconds) snprintf(buf, n, "%02d:%02d:%02d", cachedH, cachedM, cachedS);
  else snprintf(buf, n, "%02d:%02d", cachedH, cachedM);
}

static int uiWeekday() {
  if (cachedDay < 1 || cachedMon < 1 || cachedYr < 0) return -1;
  return weekdayOf(cachedDay, cachedMon, 2000 + cachedYr);
}

static void uiDateStr(char *buf, size_t n) {
  static const char *WD_LANG[7] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
  int wd = uiWeekday();
  if (wd < 0) snprintf(buf, n, "Datum unbekannt");
  else snprintf(buf, n, "%s, %d.%d.", WD_LANG[wd], cachedDay, cachedMon);
}

static lv_color_t uiBattColor(int pct) {
  if (pct < 20) return C_RED;
  if (pct < 50) return C_AMBER;
  return C_GREEN;
}

static const char *uiBattSymbol(int pct) {
  if (pct >= 90) return LV_SYMBOL_BATTERY_FULL;
  if (pct >= 65) return LV_SYMBOL_BATTERY_3;
  if (pct >= 40) return LV_SYMBOL_BATTERY_2;
  if (pct >= 15) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

// Stundenplan: aktuelle und nächste Stunde (Index oder -1)
struct UiLesson {
  int wd, cur, next;
};

static UiLesson uiLessonInfo() {
  UiLesson l = {-1, -1, -1};
  if (!ttActive || cachedH < 0) return l;
  l.wd = uiWeekday();
  if (l.wd < 0) return l;
  l.cur = findPeriod(l.wd, cachedH, cachedM);
  l.next = nextLessonIdx(l.wd, cachedH, cachedM);
  return l;
}

// --- Gemeinsame Referenzen; nicht jedes Zifferblatt nutzt alle -------------
struct WfRefs {
  lv_obj_t *time, *sec, *date, *lesson, *batt, *battIcon, *count, *session;
  lv_obj_t *battBar;
  lv_obj_t *nowHead, *nowName, *nowTime, *nextHead, *nextName, *noPlan;
  lv_obj_t *hHand, *mHand, *sHand;
};
static WfRefs wf;
static lv_point_precise_t wfPtsH[2], wfPtsM[2], wfPtsS[2];

static lv_obj_t *wfCenter(lv_obj_t *p, const lv_font_t *f, lv_color_t c, int y) {
  lv_obj_t *l = uiLabel(p, f, c, "");
  lv_obj_set_width(l, UI_W);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(l, 0, y);
  return l;
}

static lv_obj_t *wfCircle(lv_obj_t *p, int cx, int cy, int r, lv_color_t c) {
  lv_obj_t *o = uiBox(p);
  lv_obj_set_size(o, 2 * r, 2 * r);
  lv_obj_set_pos(o, cx - r, cy - r);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

// Zeiger: das Linienobjekt ist nur so groß wie der Zeiger selbst (wfSetHand
// verschiebt es). Ein bildschirmgroßes Objekt würde bei jedem Sekundenschritt
// das ganze Zifferblatt neu rendern lassen (~100 ms auf der Uhr).
static lv_obj_t *wfHand(lv_obj_t *p, lv_point_precise_t *pts, int w, lv_color_t c) {
  lv_obj_t *l = lv_line_create(p);
  lv_obj_set_style_line_width(l, w, 0);
  lv_obj_set_style_line_color(l, c, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  pts[0].x = pts[1].x = 0;
  pts[0].y = pts[1].y = 0;
  lv_line_set_points(l, pts, 2);
  return l;
}

// Akku-Symbol + Prozent nebeneinander
static void wfBattRow(lv_obj_t *p, int x, int y) {
  wf.battIcon = uiLabel(p, &font_m32, C_GREEN, LV_SYMBOL_BATTERY_FULL);
  lv_obj_set_pos(wf.battIcon, x, y);
  wf.batt = uiLabel(p, &font_m24, C_TEXT, "");
  lv_obj_set_pos(wf.batt, x + 48, y + 3);
}

static void wfBuildMinimal(lv_obj_t *s) {
  wf.time = wfCenter(s, &font_d96, C_TEXT, 120);
  wf.date = wfCenter(s, &font_m24, C_TEXT2, 240);
  wf.lesson = wfCenter(s, &font_m20, C_TEXT3, 280);
  lv_obj_t *line = uiBox(s);
  lv_obj_set_size(line, 160, 2);
  lv_obj_set_pos(line, 125, 330);
  lv_obj_set_style_bg_color(line, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  wf.count = wfCenter(s, &font_m20, C_TEXT2, 352);
}

static void wfBuildColorful(lv_obj_t *s) {
  static const uint32_t rainbow[6] = {0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x64D2FF, 0xBF5AF2};
  // Regenbogen oben, Uhrzeit darunter (nicht hinein – sonst überlappen Bögen und Ziffern)
  for (int i = 0; i < 6; i++) {
    lv_obj_t *a = lv_arc_create(s);
    lv_obj_set_size(a, 240 - i * 24, 240 - i * 24);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 24 + i * 12);
    lv_arc_set_bg_angles(a, 180, 360);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_width(a, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(rainbow[i]), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
  }
  wf.time = wfCenter(s, &font_d64, C_YELLOW, 168);
  wf.date = wfCenter(s, &font_m24, C_CYAN, 246);
  wf.count = wfCenter(s, &font_d96, C_GREEN, 282);
  wf.session = wfCenter(s, &font_m20, C_TEXT2, 388);
  lv_label_set_text(wf.session, "Meldungen heute");
  wf.batt = wfCenter(s, &font_m24, C_GREEN, 432);
}

static void wfBuildAnalog(lv_obj_t *s) {
  const int cx = 205, cy = 208, R = 150;
  wfCircle(s, cx, cy, R, C_SURFACE);
  lv_obj_t *sc = lv_scale_create(s);
  lv_obj_set_size(sc, 2 * R - 16, 2 * R - 16);
  lv_obj_set_pos(sc, cx - R + 8, cy - R + 8);
  lv_scale_set_mode(sc, LV_SCALE_MODE_ROUND_INNER);
  lv_scale_set_range(sc, 0, 60);
  lv_scale_set_total_tick_count(sc, 61);
  lv_scale_set_major_tick_every(sc, 5);
  lv_scale_set_angle_range(sc, 360);
  lv_scale_set_rotation(sc, 270);
  lv_scale_set_label_show(sc, false);
  lv_obj_remove_flag(sc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_length(sc, 6, LV_PART_ITEMS);
  lv_obj_set_style_length(sc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(sc, C_TEXT3, LV_PART_ITEMS);
  lv_obj_set_style_line_color(sc, C_TEXT, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(sc, 2, LV_PART_ITEMS);
  lv_obj_set_style_line_width(sc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
  static const char *nums[4] = {"12", "3", "6", "9"};
  static const int nx[4] = {0, 1, 0, -1}, ny[4] = {-1, 0, 1, 0};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *l = uiLabel(s, &font_m32, C_TEXT2, nums[i]);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_update_layout(l);
    int w = lv_obj_get_width(l), h = lv_obj_get_height(l);
    lv_obj_set_pos(l, cx + nx[i] * 104 - w / 2, cy + ny[i] * 104 - h / 2);
  }
  wf.date = wfCenter(s, &font_m20, C_TEXT2, 160);
  lv_obj_set_y(wf.date, cy + 40);
  wf.hHand = wfHand(s, wfPtsH, 10, C_TEXT);
  wf.mHand = wfHand(s, wfPtsM, 7, C_TEXT);
  wf.sHand = wfHand(s, wfPtsS, 3, C_RED);
  wfCircle(s, cx, cy, 9, C_RED);
  wfCircle(s, cx, cy, 4, C_BG);
  wfBattRow(s, 40, 410);
  wf.count = uiLabel(s, &font_m24, C_YELLOW, "");
  lv_obj_align(wf.count, LV_ALIGN_TOP_RIGHT, -40, 413);
}

static void wfBuildDigital(lv_obj_t *s) {
  wf.time = wfCenter(s, &font_d96, C_TEXT, 100);
  wf.sec = wfCenter(s, &font_d48, C_TEXT3, 208);
  wf.date = wfCenter(s, &font_m24, C_CYAN, 290);
  wf.battBar = lv_bar_create(s);
  lv_obj_set_size(wf.battBar, 270, 14);
  lv_obj_set_pos(wf.battBar, 70, 352);
  lv_bar_set_range(wf.battBar, 0, 100);
  lv_obj_remove_flag(wf.battBar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(wf.battBar, C_SURFACE2, LV_PART_MAIN);
  lv_obj_set_style_radius(wf.battBar, 7, LV_PART_MAIN);
  lv_obj_set_style_radius(wf.battBar, 7, LV_PART_INDICATOR);
  wf.batt = wfCenter(s, &font_m20, C_TEXT2, 374);
  wf.count = wfCenter(s, &font_m24, C_YELLOW, 430);
}

static void wfBuildGeometric(lv_obj_t *s) {
  wfCircle(s, 80, 100, 60, C_YELLOW);
  wfCircle(s, 330, 100, 60, C_CYAN);
  lv_obj_t *band = uiBox(s);
  lv_obj_set_size(band, UI_W, 150);
  lv_obj_set_pos(band, 0, 180);
  lv_obj_set_style_bg_color(band, C_PURPLE, 0);
  lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
  wf.time = wfCenter(s, &font_d96, C_TEXT, 190);
  wf.date = wfCenter(s, &font_m24, lv_color_black(), 292);
  wfBattRow(s, 40, 392);
  lv_obj_t *pill = uiBox(s);
  lv_obj_set_size(pill, 150, 52);
  lv_obj_set_pos(pill, 222, 386);
  lv_obj_set_style_radius(pill, 26, 0);
  lv_obj_set_style_bg_color(pill, C_GREEN, 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  wf.count = uiLabel(pill, &font_m24, lv_color_black(), "");
  lv_obj_center(wf.count);
}

static void wfBuildSchool(lv_obj_t *s) {
  wf.time = wfCenter(s, &font_d64, C_TEXT, 26);
  wf.date = wfCenter(s, &font_m20, C_TEXT2, 104);
  lv_obj_t *card = uiBox(s);
  lv_obj_set_size(card, UI_W - 2 * UI_PAD, 170);
  lv_obj_set_pos(card, UI_PAD, 142);
  lv_obj_set_style_bg_color(card, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 24, 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  wf.nowHead = uiLabel(card, &font_m16, C_CYAN, "JETZT");
  wf.nowName = uiLabel(card, &font_m32, C_TEXT, "");
  lv_obj_set_pos(wf.nowName, 0, 20);
  wf.nowTime = uiLabel(card, &font_m20, C_TEXT2, "");
  lv_obj_set_pos(wf.nowTime, 0, 60);
  wf.nextHead = uiLabel(card, &font_m16, C_AMBER, "DANACH");
  lv_obj_set_pos(wf.nextHead, 0, 96);
  wf.nextName = uiLabel(card, &font_m20, C_TEXT, "");
  lv_obj_set_pos(wf.nextName, 0, 116);
  wf.noPlan = uiLabel(card, &font_m20, C_TEXT2, "Kein Stundenplan.\nIn der App senden.");
  lv_obj_set_pos(wf.noPlan, 0, 30);
  wf.count = uiLabel(s, &font_m24, C_YELLOW, "");
  lv_obj_set_pos(wf.count, UI_PAD + 4, 334);
  wf.session = uiLabel(s, &font_m24, C_CYAN, "");
  lv_obj_set_pos(wf.session, UI_PAD + 4, 368);
  wfBattRow(s, UI_PAD + 4, 420);
}

static void wfBuild(lv_obj_t *s, int idx) {
  memset(&wf, 0, sizeof(wf));
  switch (idx) {
    case 1: wfBuildColorful(s); break;
    case 2: wfBuildAnalog(s); break;
    case 3: wfBuildDigital(s); break;
    case 4: wfBuildGeometric(s); break;
    case 5: wfBuildSchool(s); break;
    default: wfBuildMinimal(s); break;
  }
}

static void wfSetHand(lv_obj_t *line, lv_point_precise_t *pts, float deg, int tail, int len) {
  float r = (deg - 90.0f) * 3.14159265f / 180.0f;
  float c = cosf(r), s = sinf(r);
  int ax = (int)lroundf(205 - c * tail), ay = (int)lroundf(208 - s * tail);
  int bx = (int)lroundf(205 + c * len), by = (int)lroundf(208 + s * len);
  // Objekt = Hüllrechteck des Zeigers + Rand für Linienbreite/runde Enden
  int m = lv_obj_get_style_line_width(line, 0);
  int x0 = LV_MIN(ax, bx) - m, y0 = LV_MIN(ay, by) - m;
  lv_point_precise_t a = {(lv_value_precise_t)(ax - x0), (lv_value_precise_t)(ay - y0)};
  lv_point_precise_t b = {(lv_value_precise_t)(bx - x0), (lv_value_precise_t)(by - y0)};
  if (lv_obj_get_x(line) == x0 && lv_obj_get_y(line) == y0 && pts[0].x == a.x && pts[0].y == a.y &&
      pts[1].x == b.x && pts[1].y == b.y) return;
  pts[0] = a;
  pts[1] = b;
  lv_obj_set_pos(line, x0, y0);
  lv_obj_set_size(line, LV_ABS(bx - ax) + 2 * m + 1, LV_ABS(by - ay) + 2 * m + 1);
  lv_line_set_points(line, pts, 2);
}

static void wfUpdate(int idx) {
  char buf[64];
  int pct = cachedPct;

  if (wf.time) {
    uiTimeStr(buf, sizeof(buf), false);
    uiSetText(wf.time, buf);
  }
  if (wf.sec) {
    if (cachedS < 0) uiSetText(wf.sec, "--");
    else uiSetTextFmt(wf.sec, "%02d", cachedS);
  }
  if (wf.date) {
    uiDateStr(buf, sizeof(buf));
    uiSetText(wf.date, buf);
  }
  if (wf.battIcon) {
    uiSetText(wf.battIcon, uiBattSymbol(pct < 0 ? 0 : pct));
    uiSetColor(wf.battIcon, pct < 0 ? C_TEXT3 : uiBattColor(pct));
  }
  if (wf.battBar) {
    lv_bar_set_value(wf.battBar, pct < 0 ? 0 : pct, LV_ANIM_OFF);
    uiSetBg(wf.battBar, uiBattColor(pct < 0 ? 0 : pct), LV_PART_INDICATOR);
  }

  switch (idx) {
    case 0: {
      UiLesson l = uiLessonInfo();
      if (l.wd < 0) uiSetText(wf.lesson, "");
      else {
        const char *cur = l.cur >= 0 ? ttDays[l.wd][l.cur].name : "Pause";
        if (l.next >= 0) uiSetTextFmt(wf.lesson, "%s  |  %s", cur, ttDays[l.wd][l.next].name);
        else uiSetText(wf.lesson, cur);
      }
      if (pct >= 0) uiSetTextFmt(wf.count, "%d %%   ·   %d Meldungen", pct, totalHeute);
      else uiSetTextFmt(wf.count, "%d Meldungen", totalHeute);
      break;
    }
    case 1:
      uiSetTextFmt(wf.count, "%d", totalHeute);
      if (pct >= 0) uiSetTextFmt(wf.batt, "%s  %d %%", uiBattSymbol(pct), pct);
      else uiSetText(wf.batt, "");
      uiSetColor(wf.batt, uiBattColor(pct));
      break;
    case 2: {
      if (cachedH >= 0) {
        float sec = cachedS, min = cachedM + sec / 60.0f, hr = (cachedH % 12) + min / 60.0f;
        wfSetHand(wf.hHand, wfPtsH, hr * 30.0f, 0, 76);
        wfSetHand(wf.mHand, wfPtsM, min * 6.0f, 0, 112);
        wfSetHand(wf.sHand, wfPtsS, sec * 6.0f, 26, 128);
      }
      uiSetText(wf.batt, pct >= 0 ? (snprintf(buf, sizeof(buf), "%d %%", pct), buf) : "");
      uiSetTextFmt(wf.count, LV_SYMBOL_OK " %d", totalHeute);
      break;
    }
    case 3:
      uiSetText(wf.batt, pct >= 0 ? (snprintf(buf, sizeof(buf), "Akku %d %%", pct), buf) : "Akku unbekannt");
      uiSetTextFmt(wf.count, "%d Meldungen heute", totalHeute);
      break;
    case 4:
      uiSetText(wf.batt, pct >= 0 ? (snprintf(buf, sizeof(buf), "%d %%", pct), buf) : "");
      uiSetTextFmt(wf.count, "%d Mel.", totalHeute);
      break;
    case 5: {
      UiLesson l = uiLessonInfo();
      bool plan = l.wd >= 0;
      uiSetHidden(wf.noPlan, plan);
      uiSetText(wf.nowHead, plan ? "JETZT" : "STUNDENPLAN");
      uiSetHidden(wf.nowName, !plan);
      uiSetHidden(wf.nowTime, !plan);
      uiSetHidden(wf.nextHead, !plan || l.next < 0);
      uiSetHidden(wf.nextName, !plan || l.next < 0);
      if (plan) {
        if (l.cur >= 0) {
          const Period &p = ttDays[l.wd][l.cur];
          uiSetText(wf.nowName, p.name);
          uiSetTextFmt(wf.nowTime, "%02d:%02d – %02d:%02d", p.sh, p.sm, p.eh, p.em);
        } else {
          uiSetText(wf.nowName, "Pause");
          uiSetText(wf.nowTime, "frei");
        }
        if (l.next >= 0) {
          const Period &p = ttDays[l.wd][l.next];
          uiSetTextFmt(wf.nextName, "%s  ·  %02d:%02d", p.name, p.sh, p.sm);
        }
      }
      uiSetTextFmt(wf.count, LV_SYMBOL_OK "  %d Meldungen heute", totalHeute);
      uiSetTextFmt(wf.session, LV_SYMBOL_LOOP "  Diese Stunde: %d", sessionCount);
      uiSetText(wf.batt, pct >= 0 ? (snprintf(buf, sizeof(buf), "%d %%", pct), buf) : "");
      break;
    }
  }
}
