/*******************************************************************************
 * STARLANE: THE KESTREL REACH - ESP32-4848S040 entry point.
 *
 * Build your own modular ship, fly anywhere in a star system, ride the
 * Architects' trade lanes, pick an alien crew, and find out what the silent
 * black ships want.  pio run -e ESP32-4848S040-starlane -t upload
 *
 * The game core (sl_*.cpp) is plain C++ and renders a 240x240 palette-indexed
 * screen. Each logical pixel is a 2x2 block on the 480x480 panel; only logical
 * pixels that changed since the last frame are written to the PSRAM frame
 * buffer. The core also builds in the desktop simulator (sim/).
 ******************************************************************************/
#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp32s3/rom/cache.h>

#include "display_4848s040.h" /* bus, gfx, panel init */
#include "touch.h"

#include "sl_api.h"

#define DISPLAY_PCLK_HZ 14000000
#define FRAME_MS 33 /* ~30 fps */
#define SCR_W 480

static uint16_t *fb = nullptr;
static uint8_t shown[sl::LW * sl::LH] __attribute__((aligned(4)));
static Preferences prefs;

/* Touch shared with the core-0 touch task: bit31 pressed, x<<16 | y */
static volatile uint32_t touchState = 0;

/* ------------------------------------------------------------- platform */
bool sl::platformLoad(void *buf, size_t len) {
  if (!prefs.begin("starlane", true)) return false;
  bool ok = prefs.getBytesLength("save") == len && prefs.getBytes("save", buf, len) == len;
  prefs.end();
  return ok;
}

void sl::platformSave(const void *buf, size_t len) {
  if (!prefs.begin("starlane", false)) return;
  prefs.putBytes("save", buf, len);
  prefs.end();
}

void *sl::platformAllocLarge(size_t len) {
  void *p = ps_malloc(len);
  return p ? p : malloc(len);
}

/* ---------------------------------------------------------------- input */
static void touchTask(void *) {
  for (;;) {
    uint32_t s = 0;
    if (touch_touched()) {
      int x = constrain(touch_last_x, 0, SCR_W - 1);
      int y = constrain(touch_last_y, 0, SCR_W - 1);
      s = 0x80000000u | ((uint32_t)x << 16) | (uint32_t)y;
    }
    touchState = s;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* -------------------------------------------------------------- present */
/* Push changed logical pixels to the panel as 2x2 blocks. */
static void IRAM_ATTR present() {
  const uint8_t *scr = sl::scr;
  for (int y = 0; y < sl::LH; y++) {
    const uint32_t *a = (const uint32_t *)&scr[y * sl::LW];
    uint32_t *b = (uint32_t *)&shown[y * sl::LW];
    uint32_t *row0 = (uint32_t *)(fb + (y * 2) * SCR_W); /* one u32 = 2 panel px */
    uint32_t *row1 = row0 + SCR_W / 2;
    bool wrote = false;
    for (int k = 0; k < sl::LW / 4; k++) {
      uint32_t ca = a[k], cb = b[k];
      if (ca == cb) continue;
      for (int n = 0; n < 4; n++) {
        uint8_t ia = (uint8_t)(ca >> (n * 8));
        if (ia == (uint8_t)(cb >> (n * 8))) continue;
        uint32_t c = sl::pal565[ia];
        c |= c << 16;
        row0[k * 4 + n] = c;
        row1[k * 4 + n] = c;
      }
      b[k] = ca;
      wrote = true;
    }
    if (wrote) Cache_WriteBack_Addr((uint32_t)row0, SCR_W * 2 * 2);
  }
}

/* ----------------------------------------------------------------- main */
void setup() {
  Serial.begin(115200);
  esp_task_wdt_init(120, true);
  esp_task_wdt_add(NULL);

  touch_init();
  gfx->begin(DISPLAY_PCLK_HZ);
  fb = gfx->getFramebuffer();
  gfx->fillScreen(BLACK);
#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  uint32_t t0 = millis();
  sl::init(esp_random());
  Serial.printf("STARLANE ready in %lu ms, free heap %u, free psram %u\n",
                (unsigned long)(millis() - t0), ESP.getFreeHeap(), ESP.getFreePsram());
  memset(shown, 0xFF, sizeof(shown)); /* force a full first frame */

  xTaskCreatePinnedToCore(touchTask, "touch", 4096, NULL, 5, NULL, 0);
}

void loop() {
  static TickType_t lastWake = xTaskGetTickCount();
  static uint32_t statFrames = 0, statDraw = 0, statPush = 0, statWorst = 0, statT0 = 0;
  esp_task_wdt_reset();

  uint32_t s = touchState;
  bool down = s >> 31;
  int lx = (int)((s >> 16) & 0x7FFF) / 2, ly = (int)(s & 0xFFFF) / 2;

  uint32_t t0 = micros();
  sl::tick(down, lx, ly);
  uint32_t t1 = micros();
  present();
  uint32_t t2 = micros();

  statDraw += t1 - t0;
  statPush += t2 - t1;
  if (t2 - t0 > statWorst) statWorst = t2 - t0;
  if (++statFrames >= 150) {
    Serial.printf("fps %.1f  tick %luus  push %luus  worst %luus\n",
                  statFrames * 1000.0f / (millis() - statT0), (unsigned long)(statDraw / statFrames),
                  (unsigned long)(statPush / statFrames), (unsigned long)statWorst);
    statFrames = statDraw = statPush = statWorst = 0;
    statT0 = millis();
  }

  vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(FRAME_MS));
}
#endif
