/*******************************************************************************
 * NEON WHACK - LVGL front end for the whack-a-mole core (whack_core.cpp).
 *
 * One full-screen LVGL object whose DRAW_MAIN handler lets the core render
 * straight into LVGL's strip buffer; HUD text is ordinary LVGL labels on top.
 * The game object is invalidated every tick, so every frame is a full redraw
 * through the existing double-buffered flush path (same as NEON FLAP).
 *
 * Serial prints the frame rate and render cost every 5 s.
 ******************************************************************************/
#include "neon_whack.h"
#include "whack_core.h"
#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <math.h>

namespace {

using whack::Phase;

Preferences prefs;
int savedBest = 0;

lv_obj_t *gameObj;
struct NeonText {
  lv_obj_t *main, *shadow;
};
NeonText scoreTxt, timeTxt, multTxt, titleTxt, countTxt;
lv_obj_t *scoreCap, *timeCap, *subtitleLbl, *bestTitleLbl;
lv_obj_t *panel, *panelScoreLbl, *panelBestLbl, *panelNewLbl, *panelTapLbl;
NeonText popTxt[whack::MAX_POPUPS];
uint32_t popShown[whack::MAX_POPUPS];

/* frame-rate / render-cost stats */
uint32_t statFrames = 0, statRenderUs = 0, statT0 = 0;

/* ------------------------------------------------------------- helpers -- */
NeonText mkNeon(lv_obj_t *parent, const lv_font_t *font, uint32_t mainHex,
                uint32_t shadowHex, int letterSpace) {
  NeonText t;
  t.shadow = lv_label_create(parent);
  t.main = lv_label_create(parent);
  lv_obj_t *both[2] = {t.shadow, t.main};
  for (lv_obj_t *o : both) {
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_letter_space(o, letterSpace, 0);
  }
  lv_obj_set_style_text_color(t.shadow, lv_color_hex(shadowHex), 0);
  lv_obj_set_style_text_color(t.main, lv_color_hex(mainHex), 0);
  return t;
}
void neonText(NeonText &t, const char *s) {
  lv_label_set_text(t.shadow, s);
  lv_label_set_text(t.main, s);
}
void neonAlign(NeonText &t, lv_align_t a, int x, int y) {
  lv_obj_align(t.shadow, a, x + 3, y + 3);
  lv_obj_align(t.main, a, x, y);
}
void neonColor(NeonText &t, uint32_t mainHex, uint32_t shadowHex) {
  lv_obj_set_style_text_color(t.main, lv_color_hex(mainHex), 0);
  lv_obj_set_style_text_color(t.shadow, lv_color_hex(shadowHex), 0);
}
inline void show(lv_obj_t *o, bool s) {
  if (s)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
void neonShow(NeonText &t, bool s) {
  show(t.shadow, s);
  show(t.main, s);
}
lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t hex, int letterSpace) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
  lv_obj_set_style_text_letter_space(l, letterSpace, 0);
  return l;
}
inline bool blinkOn(float t, float period = 1.0f, float on = 0.65f) {
  return fmodf(t, period) < on;
}

/* --------------------------------------------------------------- events -- */
void onPress(lv_event_t *) {
  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);
  whack::tap(p.x, p.y);
}

void gameEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_COVER_CHECK) {
    // We paint every pixel, so LVGL can skip drawing the screen behind us.
    auto *info = (lv_cover_check_info_t *)lv_event_get_param(e);
    info->res = LV_COVER_RES_COVER;
  } else if (code == LV_EVENT_DRAW_MAIN) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const lv_area_t *clip = ctx->clip_area;
    const lv_area_t *ba = ctx->buf_area;
    const uint32_t t0 = micros();
    whack::render((uint16_t *)ctx->buf, lv_area_get_width(ba), ba->x1, ba->y1, clip->x1,
                  clip->y1, clip->x2 + 1, clip->y2 + 1);
    statRenderUs += micros() - t0;
  } else if (code == LV_EVENT_PRESSED) {
    onPress(e);
  }
}

/* ------------------------------------------------------------------- UI -- */
void updateUI() {
  const whack::Status &s = whack::status();
  static int shownScore = -1, shownTime = -1, shownMult = -1, shownBest = -1, shownCount = -1;
  static int lastPhase = -1;
  char buf[32];

  if (s.phase != (Phase)lastPhase) {
    lastPhase = (int)s.phase;
    const bool title = s.phase == Phase::Title;
    const bool hud = s.phase == Phase::Playing || s.phase == Phase::Countdown;
    neonShow(titleTxt, title);
    show(subtitleLbl, title);
    show(bestTitleLbl, title);
    neonShow(scoreTxt, hud);
    show(scoreCap, hud);
    neonShow(timeTxt, hud);
    show(timeCap, hud);
    neonShow(multTxt, false);
    neonShow(countTxt, s.phase == Phase::Countdown);
    show(panel, false);
    shownScore = shownTime = shownMult = shownCount = -1;
  }

  if (s.best != shownBest) {
    shownBest = s.best;
    snprintf(buf, sizeof buf, "HI-SCORE  %d", s.best);
    lv_label_set_text(bestTitleLbl, buf);
  }

  switch (s.phase) {
  case Phase::Title:
    show(subtitleLbl, blinkOn(s.phaseTime));
    break;

  case Phase::Countdown: {
    const int n = 3 - (int)s.phaseTime;
    if (n != shownCount) {
      shownCount = n;
      snprintf(buf, sizeof buf, "%d", n);
      neonText(countTxt, buf);
      neonAlign(countTxt, LV_ALIGN_TOP_MID, 0, 150);
    }
    break;
  }

  case Phase::Playing:
    if (s.phaseTime < 0.7f) { // "GO!" right after the countdown
      if (shownCount != 0) {
        shownCount = 0;
        neonText(countTxt, "GO!");
        neonColor(countTxt, 0x39FF14, 0x00F0FF);
        neonAlign(countTxt, LV_ALIGN_TOP_MID, 0, 150);
        neonShow(countTxt, true);
      }
    } else if (shownCount == 0) {
      shownCount = -2;
      neonShow(countTxt, false);
      neonColor(countTxt, 0xFFFFFF, 0xFF2A9D);
    }
    break;

  case Phase::Over:
    if (s.phaseTime > 0.6f && lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) {
      snprintf(buf, sizeof buf, "SCORE   %d", s.score);
      lv_label_set_text(panelScoreLbl, buf);
      snprintf(buf, sizeof buf, "BEST    %d", s.best);
      lv_label_set_text(panelBestLbl, buf);
      show(panelNewLbl, s.newBest);
      show(panel, true);
    }
    show(panelTapLbl, s.phaseTime > 1.0f && blinkOn(s.phaseTime));
    if (s.newBest) show(panelNewLbl, blinkOn(s.phaseTime, 0.4f, 0.25f));
    break;
  }

  if (s.phase == Phase::Playing || s.phase == Phase::Countdown) {
    if (s.score != shownScore) {
      shownScore = s.score;
      snprintf(buf, sizeof buf, "%d", s.score);
      neonText(scoreTxt, buf);
      neonAlign(scoreTxt, LV_ALIGN_TOP_LEFT, 14, 26);
    }
    const int secs = (int)ceilf(s.timeLeft);
    if (secs != shownTime) {
      shownTime = secs;
      snprintf(buf, sizeof buf, "%d", secs);
      neonText(timeTxt, buf);
      neonAlign(timeTxt, LV_ALIGN_TOP_RIGHT, -17, 26);
      if (secs <= 10)
        neonColor(timeTxt, 0xFF3030, 0x400000);
      else
        neonColor(timeTxt, 0x00F0FF, 0x07001A);
    }
    const int mult = s.phase == Phase::Playing ? s.mult : 1;
    if (mult != shownMult) {
      shownMult = mult;
      if (mult > 1) {
        snprintf(buf, sizeof buf, "x%d", mult);
        neonText(multTxt, buf);
        neonAlign(multTxt, LV_ALIGN_TOP_MID, 0, 104);
      }
      neonShow(multTxt, mult > 1);
    }
  }

  /* floating score popups */
  const whack::Popup *pops = whack::popups();
  for (int i = 0; i < whack::MAX_POPUPS; i++) {
    const whack::Popup &p = pops[i];
    NeonText &t = popTxt[i];
    if (p.life <= 0) {
      if (popShown[i]) {
        popShown[i] = 0;
        neonShow(t, false);
      }
      continue;
    }
    if (popShown[i] != p.id) {
      popShown[i] = p.id;
      if (p.kind == whack::POP_BOMB) {
        snprintf(buf, sizeof buf, "%ds", p.value);
        neonColor(t, 0xFF3030, 0x300000);
      } else {
        snprintf(buf, sizeof buf, "+%d", p.value);
        if (p.kind == whack::POP_GOLD)
          neonColor(t, 0xFFD319, 0x5A2000);
        else
          neonColor(t, 0xFFFFFF, 0xFF2A9D);
      }
      neonText(t, buf);
      neonShow(t, true);
    }
    // centred on p.x: TOP_MID alignment is resolved against the label's
    // current width at layout time, so a fresh text is centred right away
    const int x = (int)p.x - whack::W / 2, y = (int)p.y - 30;
    lv_obj_align(t.shadow, LV_ALIGN_TOP_MID, x + 2, y + 2);
    lv_obj_align(t.main, LV_ALIGN_TOP_MID, x, y);
    const int opa = p.life > 0.3f ? 255 : (int)(255 * p.life / 0.3f);
    lv_obj_set_style_text_opa(t.main, opa, 0);
    lv_obj_set_style_text_opa(t.shadow, opa, 0);
  }
}

void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  gameObj = lv_obj_create(scr);
  lv_obj_remove_style_all(gameObj);
  lv_obj_set_size(gameObj, whack::W, whack::H);
  lv_obj_set_pos(gameObj, 0, 0);
  lv_obj_add_flag(gameObj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(gameObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(gameObj, gameEvent, LV_EVENT_ALL, NULL);

  scoreCap = mkLabel(scr, &lv_font_montserrat_12, 0xFF2A9D, 3);
  lv_label_set_text(scoreCap, "SCORE");
  lv_obj_align(scoreCap, LV_ALIGN_TOP_LEFT, 16, 10);
  scoreTxt = mkNeon(scr, &lv_font_montserrat_28, 0xFFFFFF, 0xFF2A9D, 1);

  timeCap = mkLabel(scr, &lv_font_montserrat_12, 0x00F0FF, 3);
  lv_label_set_text(timeCap, "TIME");
  lv_obj_align(timeCap, LV_ALIGN_TOP_RIGHT, -16, 10);
  timeTxt = mkNeon(scr, &lv_font_montserrat_28, 0x00F0FF, 0x07001A, 1);

  multTxt = mkNeon(scr, &lv_font_montserrat_28, 0xFFD319, 0xFF2A9D, 2);

  titleTxt = mkNeon(scr, &lv_font_montserrat_48, 0x00F0FF, 0xFF2A9D, 4);
  neonText(titleTxt, "NEON WHACK");
  neonAlign(titleTxt, LV_ALIGN_TOP_MID, 0, 112);

  subtitleLbl = mkLabel(scr, &lv_font_montserrat_20, 0xFFD319, 5);
  lv_label_set_text(subtitleLbl, "TAP TO START");
  lv_obj_align(subtitleLbl, LV_ALIGN_TOP_MID, 0, 176);

  bestTitleLbl = mkLabel(scr, &lv_font_montserrat_16, 0xFF2A9D, 3);
  lv_obj_align(bestTitleLbl, LV_ALIGN_TOP_MID, 0, 6);

  countTxt = mkNeon(scr, &lv_font_montserrat_48, 0xFFFFFF, 0xFF2A9D, 2);

  for (int i = 0; i < whack::MAX_POPUPS; i++) {
    popTxt[i] = mkNeon(scr, &lv_font_montserrat_28, 0xFFFFFF, 0xFF2A9D, 1);
    neonShow(popTxt[i], false);
    popShown[i] = 0;
  }

  panel = lv_obj_create(scr);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 330, 220);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, -40);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x07001A), 0);
  lv_obj_set_style_bg_opa(panel, 190, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x00F0FF), 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_outline_color(panel, lv_color_hex(0xFF2A9D), 0);
  lv_obj_set_style_outline_width(panel, 2, 0);
  lv_obj_set_style_outline_pad(panel, 4, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(panel, onPress, LV_EVENT_PRESSED, NULL);

  lv_obj_t *over = mkLabel(panel, &lv_font_montserrat_28, 0xFF2A9D, 4);
  lv_label_set_text(over, "TIME UP");
  lv_obj_align(over, LV_ALIGN_TOP_MID, 0, 18);
  panelScoreLbl = mkLabel(panel, &lv_font_montserrat_28, 0x00F0FF, 2);
  lv_obj_align(panelScoreLbl, LV_ALIGN_TOP_MID, 0, 70);
  panelBestLbl = mkLabel(panel, &lv_font_montserrat_28, 0xFFD319, 2);
  lv_obj_align(panelBestLbl, LV_ALIGN_TOP_MID, 0, 110);
  panelNewLbl = mkLabel(panel, &lv_font_montserrat_14, 0x39FF14, 3);
  lv_label_set_text(panelNewLbl, "NEW HIGH SCORE");
  lv_obj_align(panelNewLbl, LV_ALIGN_TOP_MID, 0, 148);
  panelTapLbl = mkLabel(panel, &lv_font_montserrat_16, 0xFFFFFF, 4);
  lv_label_set_text(panelTapLbl, "TAP TO PLAY AGAIN");
  lv_obj_align(panelTapLbl, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void tick(lv_timer_t *) {
  const uint32_t now = millis();
  static uint32_t last = now;
  float dt = (now - last) / 1000.0f;
  last = now;
  if (dt > 0.05f) dt = 0.05f; // keep things stable after hiccups
  whack::update(dt);
  updateUI();
  lv_obj_invalidate(gameObj);

  const int best = whack::status().best;
  if (best > savedBest) {
    savedBest = best;
    prefs.putUInt("best", best);
  }

  statFrames++;
  if (now - statT0 >= 5000) {
    const float secs = (now - statT0) / 1000.0f;
    Serial.printf("NEON WHACK: %.1f fps, render %.2f ms/frame\n", statFrames / secs,
                  statRenderUs / 1000.0f / (statFrames ? statFrames : 1));
    statFrames = 0;
    statRenderUs = 0;
    statT0 = now;
  }
}

} // namespace

void neon_whack_start() {
  prefs.begin("neonwhack", false);
  savedBest = prefs.getUInt("best", 0);
  whack::init(savedBest, esp_random());
  buildUI();
  updateUI();
  statT0 = millis();
  lv_timer_create(tick, 15, NULL);
}
