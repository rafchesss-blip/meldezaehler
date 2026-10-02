// ---------------------------------------------------------------------------
// Host-Simulator der Uhr-Oberfläche: rendert alle Masken in PNG-Dateien und
// spielt Touch-Abläufe durch (Tippen, Wischen, Halten). Prüft dabei, ob die
// erwarteten Logikfunktionen aufgerufen werden.
//
//   tools/ui_sim/build.sh   ->  tools/ui_sim/out/*.png + Prüfprotokoll
// ---------------------------------------------------------------------------
#include "sim_model.h"
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#include "ui_ctl.h"
#include "ui.h"

static uint16_t fb[UI_W * UI_H];
static int fails = 0;
static const char *outDir = "out";

static void flushCb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  uint16_t *p = (uint16_t *)px;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) fb[y * UI_W + x] = *p++;
  lv_display_flush_ready(d);
}
static uint32_t tickCb() { return simMs; }

// wie halRounder in hal_display.h: CO5300 braucht gerade Fensterkoordinaten
static void simRounder(lv_event_t *e) {
  lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
  a->x1 &= ~1; a->y1 &= ~1; a->x2 |= 1; a->y2 |= 1;
}

// Eine Simulationsrunde wie loop(): Navigation, LVGL, vorgemerkte Abläufe
static void step(int ms) {
  for (int t = 0; t < ms; t += 5) {
    simMs += 5;
    ctlProcessPending();
    if (!standby) {
      uiSync();
      lv_timer_handler();
    }
  }
}

// Bild schreiben; settle = vorher Animationen abwarten (bei modalen Masken nicht,
// die stehen auf der Uhr nur, solange der blockierende Ablauf läuft)
static void shot(const char *name, bool settle = true) {
  if (settle) step(400);
  lv_refr_now(nullptr);
  char path[256];
  snprintf(path, sizeof(path), "%s/%s.ppm", outDir, name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
  for (int i = 0; i < UI_W * UI_H; i++) {
    uint16_t c = fb[i];
    unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 0x3F) << 2),
                            (unsigned char)((c & 0x1F) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  printf("  [shot] %s\n", name);
}

static void press(int x, int y, int ms) {
  simTouchDown = true;
  simTouchX = x;
  simTouchY = y;
  step(ms);
  simTouchDown = false;
  step(250);
}
static void tap(int x, int y) { press(x, y, 90); }

static void swipe(int x1, int y1, int x2, int y2) {
  simTouchDown = true;
  for (int i = 0; i <= 10; i++) {
    simTouchX = x1 + (x2 - x1) * i / 10;
    simTouchY = y1 + (y2 - y1) * i / 10;
    step(15);
  }
  simTouchDown = false;
  step(500);
}

// Sichtbares Objekt mit diesem Label-Text suchen (in Maske und oberster
// Ebene); exakte Treffer gehen vor Teiltreffern ("Zurücksetzen" vs. "Zurücksetzen?")
static lv_obj_t *findText(lv_obj_t *o, const char *txt, bool exact) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return nullptr;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char *t = lv_label_get_text(o);
    if (exact ? strcmp(t, txt) == 0 : strstr(t, txt) != nullptr) return o;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
    lv_obj_t *r = findText(lv_obj_get_child(o, i), txt, exact);
    if (r) return r;
  }
  return nullptr;
}

static bool tapText(const char *txt, int holdMs = 90) {
  lv_obj_t *o = nullptr;
  for (int exact = 1; exact >= 0 && !o; exact--) {
    o = findText(lv_layer_top(), txt, exact);
    if (!o) o = findText(lv_screen_active(), txt, exact);
  }
  if (!o) {
    printf("  FEHLT: Text \"%s\" nicht gefunden\n", txt);
    fails++;
    return false;
  }
  lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);   // wie Hochscrollen per Finger
  step(50);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  press((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, holdMs);
  return true;
}

static void expectCall(const char *what) {
  for (auto &s : simLog)
    if (s.find(what) == 0) {
      printf("  ok   %s\n", what);
      simLog.clear();
      return;
    }
  printf("  FEHLER: erwartet Aufruf %s\n", what);
  for (auto &s : simLog) printf("         gesehen: %s\n", s.c_str());
  fails++;
  simLog.clear();
}

static void expectInt(const char *name, int is, int want) {
  if (is == want) printf("  ok   %s == %d\n", name, want);
  else {
    printf("  FEHLER: %s == %d, erwartet %d\n", name, is, want);
    fails++;
  }
}

static void setupTimetable() {
  // Freitag 02.10.2026 (Wochentag 5)
  const char *names[4] = {"Mathe", "Deutsch", "Englisch", "Bio"};
  for (int p = 0; p < 4; p++) {
    Period &q = ttDays[5][p];
    strcpy(q.name, names[p]);
    q.sh = 8 + p; q.sm = 0; q.eh = 8 + p; q.em = 45;
    q.active = true;
  }
  ttCount[5] = 4;
  int demo[60] = {0, 0, 1, 0, 0, 2, 0, 1, 0, 0, 0, 3, 1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
                  1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 1, 0, 0, 1};
  memcpy(minHist, demo, sizeof(demo));
}

// Bei einer LVGL-Assertion (abort) den Aufrufstapel ausgeben
static void onAbort(int) {
  void *bt[40];
  int n = backtrace(bt, 40);
  backtrace_symbols_fd(bt, n, 2);
  _exit(3);
}

int main(int argc, char **argv) {
  if (argc > 1) outDir = argv[1];
  setvbuf(stdout, nullptr, _IONBF, 0);
  signal(SIGABRT, onAbort);
  signal(SIGSEGV, onAbort);
  lv_init();
  lv_tick_set_cb(tickCb);
  lv_display_t *d = lv_display_create(UI_W, UI_H);
  static uint16_t buf[UI_W * 60];
  lv_display_set_flush_cb(d, flushCb);
  lv_display_set_buffers(d, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(d, simRounder, LV_EVENT_INVALIDATE_AREA, nullptr);
  uiInit();
  setupTimetable();

  printf("Zifferblätter\n");
  for (int i = 0; i < 6; i++) {
    watchface = i;
    char n[32];
    snprintf(n, sizeof(n), "wf%d_%s", i, WF_NAMES[i]);
    shot(n);
  }
  ttActive = false;
  watchface = 5;
  shot("wf5_Schule_ohne_Plan");
  ttActive = true;
  cachedPct = 15;
  watchface = 0;
  shot("wf0_akku_niedrig");
  cachedPct = 76;
  simLog.clear();

  printf("Zifferblatt: 2 s halten -> Auswahl, Analog wählen\n");
  press(205, 300, 2300);
  expectInt("screen", screen, 3);
  shot("picker");
  tapText("Analog");
  expectInt("screen", screen, 0);
  expectInt("watchface", watchface, 2);
  shot("wf_nach_auswahl");
  simLog.clear();

  printf("Hoch wischen -> Apps\n");
  swipe(205, 420, 205, 150);
  expectInt("screen", screen, 4);
  shot("apps");

  printf("Melden: Zähler, Statistik, Kalibrierung\n");
  tapText("Melden");
  expectInt("screen", screen, 1);
  shot("melden_zaehler");
  imHoch = true; aktuellKlasse = 0; aktuellProb[0] = 0.91f;
  shot("melden_arm_oben");
  imHoch = false; aktuellKlasse = 1;
  swipe(340, 300, 60, 300);
  expectInt("view", view, 1);
  shot("melden_statistik");
  swipe(340, 300, 60, 300);
  expectInt("view", view, 2);
  shot("melden_kalibrierung");
  tapText("Arm hoch");
  step(50);
  expectCall("runCalibrationPart(2)");
  shot("kalibrierung_fertig_meldung");
  step(2000);
  expectInt("screen nach Kalibrierung", screen, 1);
  expectInt("view nach Kalibrierung", view, 2);
  tapText("Kalibrierung löschen");
  shot("kalibrierung_loeschen_dialog");
  tapText("Löschen + Neustart");
  step(50);
  expectCall("clearCalibration");
  simLog.clear();
  // zurück zur Zähler-Seite, lange drücken -> Zurücksetzen mit Bestätigung
  screen = 1; view = 0;
  step(300);
  press(205, 200, 1000);
  shot("melden_zuruecksetzen_dialog");
  tapText("Zurücksetzen", 90);
  expectCall("resetAllStats");
  shot("melden_toast");
  totalHeute = 7;
  calibrated = false;
  shot("melden_nicht_kalibriert");
  calibrated = true;
  sensorOn = false;
  shot("melden_sensor_aus");
  sensorOn = true;

  printf("Zurück -> Apps -> Einstellungen\n");
  tapText(LV_SYMBOL_LEFT);
  expectInt("screen", screen, 4);
  tapText("Einstellungen");
  shot("einstellungen");
  if (us.swBt) {
    lv_obj_t *sw = us.swBt;
    lv_area_t a;
    lv_obj_get_coords(sw, &a);
    tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    expectCall("btDisable");
    shot("einstellungen_bt_aus");
  }
  tapText(LV_SYMBOL_LEFT);

  printf("Zeit: Timer, Stoppuhr, Alarm\n");
  tapText("Zeit");
  expectInt("screen", screen, 7);
  shot("zeit_timer");
  tapText("Start");
  expectInt("timerRunning", timerRunning, 1);
  step(3000);
  shot("zeit_timer_laeuft");
  tapText("Pause");
  tapText("Stoppuhr");
  expectInt("zeitTab", zeitTab, 1);
  tapText("Start");
  step(4321);
  shot("zeit_stoppuhr");
  alarmActive = true; zeitTab = 2;
  shot("alarm");
  tapText("Stoppen");
  expectCall("alarmStop");
  expectInt("zeitTab", zeitTab, 0);
  tapText(LV_SYMBOL_LEFT);

  printf("Test und Aufnahme\n");
  tapText("Test");
  shot("test");
  tapText("Motor testen");
  expectCall("vibrate(300)");
  tapText(LV_SYMBOL_LEFT);
  tapText("Aufnahme");
  shot("aufnahme");
  tapText("Nicht melden");
  expectCall("startSensorRec(nicht_meldung)");
  sensorRecCount = 1532;
  shot("aufnahme_laeuft");
  tapText("Stoppen");
  expectCall("stopSensorRec");
  tapText(LV_SYMBOL_LEFT);
  expectInt("screen", screen, 4);

  printf("Apps: runter wischen -> Zifferblatt; Power auf Zifferblatt -> Standby\n");
  swipe(205, 150, 205, 450);
  expectInt("screen", screen, 0);
  powerBack();
  expectCall("enterStandby");
  standby = false;

  printf("BOOT-Taste: Meldungen bearbeiten\n");
  screen = 6; meldeEditMode = 0;
  shot("bearbeiten_menue");
  tapText("Hinzufügen");
  expectCall("registerMeldung");
  expectInt("meldeEditMode", meldeEditMode, 1);
  shot("bearbeiten_letzte");
  tapText("Drangenommen");
  expectInt("meldeEditMode", meldeEditMode, 2);
  shot("bearbeiten_antwort");
  tapText("Richtig");
  expectInt("meldeEditMode", meldeEditMode, 0);
  tapText("Alle löschen");
  shot("bearbeiten_alle_loeschen_dialog");
  tapText("Abbrechen");
  simLog.clear();
  tapText(LV_SYMBOL_LEFT);
  expectInt("screen", screen, 0);

  printf("Modale Masken\n");
  uiCalibShow("Arm hoch", "wie beim Melden halten", 2, 0, 0);
  shot("kalib_countdown", false);
  uiCalibShow("Arm hoch", "wie beim Melden halten", 0, 6, 10);
  shot("kalib_wiederholung", false);
  uiFirstBoot();
  shot("erststart", false);
  uiMessage(LV_SYMBOL_HOME, C_CYAN, "Tisch lernen", "Uhr liegt flach auf dem Tisch …");
  shot("tisch", false);
  uiFatal("Sensorfehler", "Bewegungssensor QMI8658 nicht gefunden.");
  shot("fehler", false);
  step(100);
  screen = 0;
  step(500);
  shot("nach_modal_zifferblatt");

  printf("\n%s (%d Fehler)\n", fails ? "FEHLGESCHLAGEN" : "ALLES OK", fails);
  return fails ? 1 : 0;
}
