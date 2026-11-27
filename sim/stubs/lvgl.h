/* Just enough of LVGL 8 for neon_flap.cpp to build on the desktop so its
 * renderer can be timed next to NEON WHACK's (see flap_bench.cpp). */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int16_t lv_coord_t;
struct lv_area_t { lv_coord_t x1, y1, x2, y2; };
inline lv_coord_t lv_area_get_width(const lv_area_t *a) { return a->x2 - a->x1 + 1; }
struct lv_draw_ctx_t { void *buf; const lv_area_t *buf_area; const lv_area_t *clip_area; };
typedef int lv_event_code_t;
enum { LV_EVENT_ALL = 0, LV_EVENT_PRESSED, LV_EVENT_COVER_CHECK, LV_EVENT_DRAW_MAIN };
struct lv_event_t { lv_event_code_t code; void *param; lv_draw_ctx_t *ctx; };
typedef void (*lv_event_cb_t)(lv_event_t *);
struct lv_obj_t { int d; };
struct lv_timer_t { int d; };
typedef void (*lv_timer_cb_t)(lv_timer_t *);
struct lv_font_t { int d; };
struct lv_color_t { uint16_t full; };
typedef int lv_align_t;
enum { LV_ALIGN_TOP_MID, LV_ALIGN_BOTTOM_MID, LV_ALIGN_CENTER, LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT };
enum { LV_OBJ_FLAG_HIDDEN = 1, LV_OBJ_FLAG_CLICKABLE = 2, LV_OBJ_FLAG_SCROLLABLE = 4 };
struct lv_cover_check_info_t { int res; const lv_area_t *area; };
enum { LV_COVER_RES_COVER = 0 };
inline lv_font_t lv_font_montserrat_14, lv_font_montserrat_16, lv_font_montserrat_20,
    lv_font_montserrat_28, lv_font_montserrat_48;
inline lv_event_cb_t g_stub_event_cb = nullptr;
inline lv_timer_cb_t g_stub_timer_cb = nullptr;
inline lv_obj_t g_stub_obj;
inline lv_obj_t *lv_scr_act() { return &g_stub_obj; }
inline lv_obj_t *lv_obj_create(lv_obj_t *) { return new lv_obj_t; }
inline lv_obj_t *lv_label_create(lv_obj_t *) { return new lv_obj_t; }
inline void lv_obj_add_event_cb(lv_obj_t *, lv_event_cb_t cb, int code, void *) {
  if (code == LV_EVENT_ALL) g_stub_event_cb = cb;
}
inline lv_timer_t *lv_timer_create(lv_timer_cb_t cb, int, void *) { g_stub_timer_cb = cb; return nullptr; }
inline lv_color_t lv_color_hex(uint32_t) { return {0}; }
inline bool lv_obj_has_flag(lv_obj_t *, int) { return false; }
inline lv_event_code_t lv_event_get_code(lv_event_t *e) { return e->code; }
inline void *lv_event_get_param(lv_event_t *e) { return e->param; }
inline lv_draw_ctx_t *lv_event_get_draw_ctx(lv_event_t *e) { return e->ctx; }
#define LV_STUB(name) template <class... A> inline void name(A...) {}
LV_STUB(lv_obj_remove_style_all) LV_STUB(lv_obj_set_size) LV_STUB(lv_obj_set_pos)
LV_STUB(lv_obj_add_flag) LV_STUB(lv_obj_clear_flag) LV_STUB(lv_obj_set_style_text_font)
LV_STUB(lv_obj_set_style_text_letter_space) LV_STUB(lv_obj_set_style_text_color)
LV_STUB(lv_label_set_text) LV_STUB(lv_obj_align) LV_STUB(lv_obj_set_style_bg_color)
LV_STUB(lv_obj_set_style_bg_opa) LV_STUB(lv_obj_set_style_border_color)
LV_STUB(lv_obj_set_style_border_width) LV_STUB(lv_obj_set_style_outline_color)
LV_STUB(lv_obj_set_style_outline_width) LV_STUB(lv_obj_set_style_outline_pad)
LV_STUB(lv_obj_invalidate)
