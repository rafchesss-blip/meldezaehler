#pragma once
// ---------------------------------------------------------------------------
// Labyrinth: Kugel durch Neigen der Uhr ins grüne Ziel rollen.
// Jedes Level ein zufälliges Labyrinth (Tiefensuche), von 6x6 bis 12x12 Zellen.
// „Gerade“ ist die Haltung beim Levelstart (Bezugsvektor), gesteuert wird mit
// der Neigung relativ dazu. Eigener Takt (30 ms) für flüssige Bewegung; die
// normale Aktualisierung (100 ms) setzt nur Level und Zeit.
// Während des Spiels zählt die Uhr keine Meldungen (evaluate(), SCREEN_SPIEL).
// ---------------------------------------------------------------------------

#define SP_FELD   344                       // Spielfläche in Pixeln (quadratisch)
#define SP_X0     ((UI_W - SP_FELD) / 2)
#define SP_Y0     (UI_HEADER_H + 34)
#define SP_WAND   4                         // Wandstärke
#define SP_MAXN   12
#define SP_MAXW   (2 * SP_MAXN * SP_MAXN + 8)
#define SP_TAKT   30                        // ms je Bild
#define SP_BESCHL 1400.0f                   // px/s² je g Neigung
#define SP_REIB   0.985f                    // Dämpfung je Bild

// Achsen: QMI8658 auf der Rückseite, X zeigt zur Displayoberkante, Y nach
// rechts (Draufsicht aufs Display). Bei falscher Richtung hier umdrehen.
#define SP_DIR_X  (-1.0f)                   // Bildschirm-x aus Sensor-Y
#define SP_DIR_Y  (+1.0f)                   // Bildschirm-y aus Sensor-X

static struct {
  lv_obj_t *feld, *kugel, *ziel, *info, *msg;
  lv_timer_t *timer;
  int level = 1, n = 6;
  float zelle, r;                           // Zellgröße, Kugelradius
  float x, y, vx, vy;                       // Kugelmitte relativ zum Feld, Geschwindigkeit
  float nx = 0, ny = 0;                     // Bezug „gerade“ (Sensor-X/Y in g)
  int16_t wx[SP_MAXW], wy[SP_MAXW], ww[SP_MAXW], wh[SP_MAXW];
  int nw = 0;
  unsigned long startMs, zeitMs, letztMs, tickMs;
  bool fertig;
} sp;

static bool spLese(float &gx, float &gy) {
  int16_t ax, ay, az, g1, g2, g3;
  if (!qmiReadData(ax, ay, az, g1, g2, g3)) return false;
  gx = ax * ACCEL_SCALE;
  gy = ay * ACCEL_SCALE;
  return true;
}

static void spWand(int x, int y, int w, int h) {
  if (sp.nw >= SP_MAXW) return;
  sp.wx[sp.nw] = x;
  sp.wy[sp.nw] = y;
  sp.ww[sp.nw] = w;
  sp.wh[sp.nw] = h;
  sp.nw++;
  lv_obj_t *o = uiBox(sp.feld);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, 2, 0);
  lv_obj_set_style_bg_color(o, C_CYAN, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

// Neues zufälliges Labyrinth (Tiefensuche mit Stapel) für das aktuelle Level
static void spNeuesLevel() {
  sp.n = LV_MIN(5 + sp.level, SP_MAXN);
  const int n = sp.n;
  sp.zelle = (float)SP_FELD / n;
  sp.r = sp.zelle * 0.28f;
  lv_obj_clean(sp.feld);
  sp.nw = 0;

  // offen[i]: Bit 0 = Weg nach rechts, Bit 1 = Weg nach unten
  static uint8_t offen[SP_MAXN * SP_MAXN], besucht[SP_MAXN * SP_MAXN];
  static int16_t stapel[SP_MAXN * SP_MAXN];
  memset(offen, 0, sizeof(offen));
  memset(besucht, 0, sizeof(besucht));
  int sp_n = 0;
  stapel[sp_n++] = 0;
  besucht[0] = 1;
  while (sp_n > 0) {
    int c = stapel[sp_n - 1], cx = c % n, cy = c / n;
    int nb[4], k = 0;
    if (cx > 0 && !besucht[c - 1]) nb[k++] = c - 1;
    if (cx < n - 1 && !besucht[c + 1]) nb[k++] = c + 1;
    if (cy > 0 && !besucht[c - n]) nb[k++] = c - n;
    if (cy < n - 1 && !besucht[c + n]) nb[k++] = c + n;
    if (!k) {
      sp_n--;
      continue;
    }
    int z = nb[esp_random() % k];
    if (z == c + 1) offen[c] |= 1;
    else if (z == c - 1) offen[z] |= 1;
    else if (z == c + n) offen[c] |= 2;
    else offen[z] |= 2;
    besucht[z] = 1;
    stapel[sp_n++] = z;
  }

  // Ziel (unten rechts) zuerst, damit Wände und Kugel darüber liegen
  int zg = (int)(sp.zelle * 0.7f);
  sp.ziel = uiBox(sp.feld);
  lv_obj_set_size(sp.ziel, zg, zg);
  lv_obj_set_pos(sp.ziel, (int)((n - 0.5f) * sp.zelle - zg / 2), (int)((n - 0.5f) * sp.zelle - zg / 2));
  lv_obj_set_style_radius(sp.ziel, 6, 0);
  lv_obj_set_style_bg_color(sp.ziel, C_GREEN, 0);
  lv_obj_set_style_bg_opa(sp.ziel, LV_OPA_COVER, 0);

  // Wände: außen, dann je Zelle rechts/unten, wo kein Weg ist
  spWand(0, 0, SP_FELD, SP_WAND);
  spWand(0, SP_FELD - SP_WAND, SP_FELD, SP_WAND);
  spWand(0, 0, SP_WAND, SP_FELD);
  spWand(SP_FELD - SP_WAND, 0, SP_WAND, SP_FELD);
  for (int c = 0; c < n * n; c++) {
    int cx = c % n, cy = c / n;
    int x1 = (int)((cx + 1) * sp.zelle), y1 = (int)((cy + 1) * sp.zelle);
    int x0 = (int)(cx * sp.zelle), y0 = (int)(cy * sp.zelle);
    if (cx < n - 1 && !(offen[c] & 1)) spWand(x1 - SP_WAND / 2, y0, SP_WAND, y1 - y0 + SP_WAND / 2);
    if (cy < n - 1 && !(offen[c] & 2)) spWand(x0, y1 - SP_WAND / 2, x1 - x0 + SP_WAND / 2, SP_WAND);
  }

  sp.kugel = uiBox(sp.feld);
  lv_obj_set_size(sp.kugel, (int)(2 * sp.r), (int)(2 * sp.r));
  lv_obj_set_style_radius(sp.kugel, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(sp.kugel, C_YELLOW, 0);
  lv_obj_set_style_bg_opa(sp.kugel, LV_OPA_COVER, 0);

  sp.x = sp.y = sp.zelle * 0.5f;
  sp.vx = sp.vy = 0;
  // „Gerade“ = jetzige Haltung (Mittel aus einigen Lesungen)
  float sx = 0, sy = 0;
  int m = 0;
  for (int i = 0; i < 8; i++) {
    float gx, gy;
    if (spLese(gx, gy)) {
      sx += gx;
      sy += gy;
      m++;
    }
    delay(3);
  }
  if (m) {
    sp.nx = sx / m;
    sp.ny = sy / m;
  }
  sp.fertig = false;
  sp.startMs = sp.letztMs = millis();
  uiSetHidden(sp.msg, true);
}

// Kugel (Kreis) gegen alle Wände (Rechtecke): herausschieben, abprallen
static void spKollision() {
  for (int i = 0; i < sp.nw; i++) {
    float qx = LV_CLAMP(sp.wx[i], sp.x, sp.wx[i] + sp.ww[i]);
    float qy = LV_CLAMP(sp.wy[i], sp.y, sp.wy[i] + sp.wh[i]);
    float dx = sp.x - qx, dy = sp.y - qy, d2 = dx * dx + dy * dy;
    if (d2 >= sp.r * sp.r) continue;
    float d = sqrtf(d2);
    float nxw, nyw;
    if (d > 0.001f) {
      nxw = dx / d;
      nyw = dy / d;
    } else {   // Mitte in der Wand: auf kürzestem Weg hinaus
      nxw = 0;
      nyw = -1;
      d = 0;
    }
    sp.x += nxw * (sp.r - d);
    sp.y += nyw * (sp.r - d);
    float vn = sp.vx * nxw + sp.vy * nyw;
    if (vn < 0) {
      sp.vx -= 1.4f * vn * nxw;   // Abprall mit 40 % Rückstoß
      sp.vy -= 1.4f * vn * nyw;
      if (vn < -180 && millis() - sp.tickMs > 120) {   // spürbarer Aufprall
        sp.tickMs = millis();
        vibrate(20, 1, 0);
      }
    }
  }
}

static void spTakt(lv_timer_t *) {
  if (screen != SCREEN_SPIEL || standby || sp.fertig || !sp.kugel) return;
  unsigned long now = millis();
  float dt = LV_MIN((now - sp.letztMs) / 1000.0f, 0.06f);
  sp.letztMs = now;
  float gx, gy;
  if (spLese(gx, gy)) {
    sp.vx += SP_DIR_X * (gy - sp.ny) * SP_BESCHL * dt;
    sp.vy += SP_DIR_Y * (gx - sp.nx) * SP_BESCHL * dt;
  }
  sp.vx *= SP_REIB;
  sp.vy *= SP_REIB;
  // in kleinen Schritten bewegen, damit die Kugel nicht durch Wände springt
  float weg = fmaxf(fabsf(sp.vx), fabsf(sp.vy)) * dt;
  int schritte = LV_MAX(1, (int)ceilf(weg / (sp.r * 0.5f)));
  for (int i = 0; i < schritte; i++) {
    sp.x += sp.vx * dt / schritte;
    sp.y += sp.vy * dt / schritte;
    spKollision();
  }
  lv_obj_set_pos(sp.kugel, (int)(sp.x - sp.r), (int)(sp.y - sp.r));

  // Ziel erreicht?
  float zx = (sp.n - 0.5f) * sp.zelle, zy = zx;
  if (fabsf(sp.x - zx) < sp.zelle * 0.35f && fabsf(sp.y - zy) < sp.zelle * 0.35f) {
    sp.fertig = true;
    sp.zeitMs = now - sp.startMs;
    vibrate(120, 3, 100);
    uiSetTextFmt(sp.msg, LV_SYMBOL_OK "  Level %d geschafft!\n%lu,%lu s  ·  Tippen = weiter", sp.level,
                 sp.zeitMs / 1000, (sp.zeitMs % 1000) / 100);
    uiSetHidden(sp.msg, false);
  }
}

static void spTippen(lv_event_t *) {
  if (sp.fertig) {
    sp.level++;
    spNeuesLevel();
  }
}

static void spNeuStart(lv_event_t *) {
  sp.level = 1;
  spNeuesLevel();
}

static lv_obj_t *uiBuildSpiel() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Labyrinth", uiBackCb);
  sp.info = uiLabel(s, &font_m20, C_TEXT2, "");
  lv_obj_set_pos(sp.info, SP_X0, UI_HEADER_H + 2);
  lv_obj_t *neu = uiLabel(s, &font_m20, C_CYAN, LV_SYMBOL_REFRESH " Neu");
  lv_obj_set_pos(neu, SP_X0 + SP_FELD - 80, UI_HEADER_H + 2);
  lv_obj_add_flag(neu, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(neu, 14);
  lv_obj_add_event_cb(neu, spNeuStart, LV_EVENT_CLICKED, nullptr);

  sp.feld = uiBox(s);
  lv_obj_set_pos(sp.feld, SP_X0, SP_Y0);
  lv_obj_set_size(sp.feld, SP_FELD, SP_FELD);
  lv_obj_add_flag(sp.feld, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sp.feld, spTippen, LV_EVENT_CLICKED, nullptr);

  sp.msg = uiLabel(s, &font_m24, C_TEXT, "");
  lv_obj_set_width(sp.msg, UI_W - 2 * UI_PAD);
  lv_obj_set_style_text_align(sp.msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(sp.msg, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(sp.msg, LV_OPA_90, 0);
  lv_obj_set_style_radius(sp.msg, 20, 0);
  lv_obj_set_style_pad_all(sp.msg, 16, 0);
  lv_obj_align(sp.msg, LV_ALIGN_CENTER, 0, 30);
  lv_obj_add_flag(sp.msg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sp.msg, spTippen, LV_EVENT_CLICKED, nullptr);

  spNeuesLevel();
  if (!sp.timer) sp.timer = lv_timer_create(spTakt, SP_TAKT, nullptr);
  return s;
}

// alle 100 ms (ui.h): Level und Zeit
static void uiRefreshSpiel() {
  unsigned long t = sp.fertig ? sp.zeitMs : millis() - sp.startMs;
  uiSetTextFmt(sp.info, "Level %d  ·  %lu,%lu s", sp.level, t / 1000, (t % 1000) / 100);
}

// Beim Öffnen der App: Level neu ausrichten (Haltung kann sich geändert haben)
static void spOeffnen() {
  if (sp.feld) spNeuesLevel();
}
