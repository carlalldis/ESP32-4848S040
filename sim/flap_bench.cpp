/* Times NEON FLAP's renderer (src/neon_flap.cpp, built against stub LVGL)
 * the same way whack_sim times NEON WHACK: 480x480 in 60-line strips. */
#include <chrono>
#include <stdio.h>
#include "lvgl.h"
#include "Arduino.h"
#include "../src/neon_flap.h"

int main() {
  neon_flap_start();
  static uint16_t strip[480 * 60];
  double total = 0;
  int frames = 0;
  for (int f = 0; f < 900; f++) {
    g_stub_millis += 16;
    if (f % 22 == 0) { lv_event_t e{LV_EVENT_PRESSED, nullptr, nullptr}; g_stub_event_cb(&e); }
    g_stub_timer_cb(nullptr);
    auto t0 = std::chrono::steady_clock::now();
    for (int y = 0; y < 480; y += 60) {
      lv_area_t a{0, (lv_coord_t)y, 479, (lv_coord_t)(y + 59)};
      lv_draw_ctx_t ctx{strip, &a, &a};
      lv_event_t e{LV_EVENT_DRAW_MAIN, nullptr, &ctx};
      g_stub_event_cb(&e);
    }
    auto t1 = std::chrono::steady_clock::now();
    if (f >= 60) { total += std::chrono::duration<double, std::micro>(t1 - t0).count(); frames++; }
  }
  printf("NEON FLAP render: %.1f us/frame (host)\n", total / frames);
}
