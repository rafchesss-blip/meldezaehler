#pragma once
// ---------------------------------------------------------------------------
// Kamera-Auslöser: Die Uhr ist zusätzlich eine Bluetooth-Medientaste (HID,
// Consumer Control). „Lauter“ löst in jeder Handy-Kamera aus. Einmalig in den
// Bluetooth-Einstellungen des Handys „Meldezaehler“ koppeln.
// ---------------------------------------------------------------------------

static struct {
  lv_obj_t *knopf, *status, *btKnopf, *zaehler;
  int fotos = 0;
} ua2;

static void uaAusloesen(lv_event_t *) {
  if (hidVolumeUp()) {
    ua2.fotos++;
    vibrate(60);
  } else {
    uiToast(LV_SYMBOL_WARNING "  Kein Handy verbunden", C_RED);
  }
}

static void uaBtAn(lv_event_t *) {
  if (!btOn) btEnable();
}

static lv_obj_t *uiBuildAusloeser() {
  lv_obj_t *s = uiScreen();
  uiHeader(s, "Auslöser", uiBackCb);
  ua2.status = wfCenter(s, &font_m20, C_TEXT2, UI_HEADER_H + 6);
  lv_label_set_long_mode(ua2.status, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_hor(ua2.status, UI_PAD, 0);

  // großer runder Knopf
  ua2.knopf = lv_button_create(s);
  lv_obj_set_size(ua2.knopf, 220, 220);
  lv_obj_align(ua2.knopf, LV_ALIGN_TOP_MID, 0, UI_HEADER_H + 80);
  lv_obj_set_style_radius(ua2.knopf, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ua2.knopf, C_TEXT, 0);
  lv_obj_set_style_bg_color(ua2.knopf, C_TEXT2, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(ua2.knopf, 0, 0);
  lv_obj_set_style_border_color(ua2.knopf, C_SURFACE2, 0);
  lv_obj_set_style_border_width(ua2.knopf, 10, 0);
  lv_obj_t *ic = uiLabel(ua2.knopf, &font_m32, lv_color_black(), LV_SYMBOL_IMAGE);
  lv_obj_center(ic);
  lv_obj_add_event_cb(ua2.knopf, uaAusloesen, LV_EVENT_CLICKED, nullptr);

  ua2.zaehler = wfCenter(s, &font_m20, C_TEXT2, UI_HEADER_H + 312);

  ua2.btKnopf = uiButton(s, LV_SYMBOL_BLUETOOTH "  Bluetooth einschalten", C_BLUE, uaBtAn, nullptr, 72);
  lv_obj_set_width(ua2.btKnopf, UI_W - 2 * UI_PAD);
  lv_obj_align(ua2.btKnopf, LV_ALIGN_BOTTOM_MID, 0, -30);
  return s;
}

static void uiRefreshAusloeser() {
  bool verbunden = btOn && hidConnected();
  if (!btOn) uiSetText(ua2.status, "Bluetooth ist aus");
  else if (!verbunden) uiSetText(ua2.status, "Am Handy „Meldezaehler“ koppeln");
  else uiSetText(ua2.status, LV_SYMBOL_OK "  Handy verbunden – Kamera öffnen");
  uiSetColor(ua2.status, verbunden ? C_GREEN : C_TEXT2);
  uiSetHidden(ua2.btKnopf, btOn);
  if (ua2.fotos) uiSetTextFmt(ua2.zaehler, "%d× ausgelöst", ua2.fotos);
  else uiSetText(ua2.zaehler, "Tippen = Foto");
}
