#pragma once
#include <stdint.h>

/* Every touch point from the most recent GT911 read, mapped to screen
 * coordinates. Updated by touch.h each time LVGL polls the touch panel (on the
 * LVGL thread), so apps should read these from LVGL timers/events only. */
#define TOUCH_MAX_POINTS 5

extern int touch_count;                          /* 0 when nothing is touching */
extern int16_t touch_pts_x[TOUCH_MAX_POINTS];
extern int16_t touch_pts_y[TOUCH_MAX_POINTS];
extern uint8_t touch_pts_id[TOUCH_MAX_POINTS];   /* GT911 track id per finger */
