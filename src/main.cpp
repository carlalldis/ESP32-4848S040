

/*******************************************************************************
 * ESP32-4848S040 (ST7701S 480x480 RGB panel + GT911 touch).
 * This file sets up the display, LVGL and touch, then starts VIEWER 3D, a
 * software-rendered STL viewer (viewer3d.cpp; models from src/stl/).
 *
 * Serial: send 'S' (240x240) or 'F' (480x480) to get a screenshot of the
 * frame buffer; scripts/screenshot.py turns it into a PNG.
 ******************************************************************************/
#include "viewer3d.h"
#define APP_NAME "VIEWER 3D"
#include <lvgl.h>
#include <Arduino.h>
#include <ArduinoLog.h>
#include <esp_task_wdt.h>
#include <esp32s3/rom/cache.h>
#include <soc/soc_memory_layout.h>

/*******************************************************************************
 * Start of Arduino_GFX setting
 *
 * Arduino_GFX try to find the settings depends on selected board in Arduino IDE
 * Or you can define the display dev kit not in the board list
 * Defalult pin list for non display dev kit:
 * Arduino Nano, Micro and more: CS:  9, DC:  8, RST:  7, BL:  6, SCK: 13, MOSI:
 * 11, MISO: 12 ESP32 various dev board     : CS:  5, DC: 27, RST: 33, BL: 22,
 * SCK: 18, MOSI: 23, MISO: nil ESP32-C3 various dev board  : CS:  7, DC:  2,
 * RST:  1, BL:  3, SCK:  4, MOSI:  6, MISO: nil ESP32-S2 various dev board  :
 * CS: 34, DC: 35, RST: 33, BL: 21, SCK: 36, MOSI: 35, MISO: nil ESP32-S3
 * various dev board  : CS: 40, DC: 41, RST: 42, BL: 48, SCK: 36, MOSI: 35,
 * MISO: nil ESP8266 various dev board   : CS: 15, DC:  4, RST:  2, BL:  5, SCK:
 * 14, MOSI: 13, MISO: 12 Raspberry Pi Pico dev board : CS: 17, DC: 27, RST: 26,
 * BL: 28, SCK: 18, MOSI: 19, MISO: 16 RTL8720 BW16 old patch core : CS: 18, DC:
 * 17, RST:  2, BL: 23, SCK: 19, MOSI: 21, MISO: 20 RTL8720_BW16 Official core
 * : CS:  9, DC:  8, RST:  6, BL:  3, SCK: 10, MOSI: 12, MISO: 11 RTL8722 dev
 * board           : CS: 18, DC: 17, RST: 22, BL: 23, SCK: 13, MOSI: 11, MISO:
 * 12 RTL8722_mini dev board      : CS: 12, DC: 14, RST: 15, BL: 13, SCK: 11,
 * MOSI:  9, MISO: 10 Seeeduino XIAO dev board    : CS:  3, DC:  2, RST:  1, BL:
 *  0, SCK:  8, MOSI: 10, MISO:  9 Teensy 4.1 dev board        : CS: 39, DC: 41,
 * RST: 40, BL: 22, SCK: 13, MOSI: 11, MISO: 12
 ******************************************************************************/
#include <Arduino_GFX_Library.h>

#define GFX_BL 38

/* Vendor ST7701S init sequence for ESP32-4848S040 (0xCD = 0x00 instead of 0x08
 * in Arduino_GFX 1.2.9) */
static const uint8_t st7701_4848s040_init_operations[] = {
    BEGIN_WRITE, WRITE_COMMAND_8, 0xFF, WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00,
    0x10,

    WRITE_C8_D16, 0xC0, 0x3B, 0x00, WRITE_C8_D16, 0xC1, 0x0D, 0x02,
    WRITE_C8_D16, 0xC2, 0x31, 0x05, WRITE_C8_D8, 0xCD, 0x00,

    WRITE_COMMAND_8, 0xB0, // Positive Voltage Gamma Control
    WRITE_BYTES, 16, 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22,
    0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18,

    WRITE_COMMAND_8, 0xB1, // Negative Voltage Gamma Control
    WRITE_BYTES, 16, 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22,
    0x04, 0x11, 0x11, 0xA9, 0x32, 0x18,

    // PAGE1
    WRITE_COMMAND_8, 0xFF, WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,

    WRITE_C8_D8, 0xB0, 0x60,                          // Vop=4.7375v
    WRITE_C8_D8, 0xB1, 0x32,                          // VCOM=32
    WRITE_C8_D8, 0xB2, 0x07,                          // VGH=15v
    WRITE_C8_D8, 0xB3, 0x80, WRITE_C8_D8, 0xB5, 0x49, // VGL=-10.17v
    WRITE_C8_D8, 0xB7, 0x85, WRITE_C8_D8, 0xB8, 0x21, // AVDD=6.6 & AVCL=-4.6
    WRITE_C8_D8, 0xC1, 0x78, WRITE_C8_D8, 0xC2, 0x78,

    WRITE_COMMAND_8, 0xE0, WRITE_BYTES, 3, 0x00, 0x1B, 0x02,

    WRITE_COMMAND_8, 0xE1, WRITE_BYTES, 11, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0,
    0x00, 0x00, 0x00, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE2, WRITE_BYTES, 12, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0,
    0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00,

    WRITE_COMMAND_8, 0xE3, WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE4, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE5, WRITE_BYTES, 16, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB,
    0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xE6, WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE7, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE8, WRITE_BYTES, 16, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA,
    0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xEB, WRITE_BYTES, 7, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00,
    0x40,

    WRITE_C8_D16, 0xEC, 0x3C, 0x00,

    WRITE_COMMAND_8, 0xED, WRITE_BYTES, 16, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA,

    //-----------VAP & VAN---------------
    WRITE_COMMAND_8, 0xFF, WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,

    WRITE_C8_D8, 0xE5, 0xE4,

    WRITE_COMMAND_8, 0xFF, WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x00,

    WRITE_C8_D8, 0x3A, 0x60, // 0x70 RGB888, 0x60 RGB666, 0x50 RGB565

    WRITE_COMMAND_8, 0x11, // Sleep Out
    END_WRITE,

    DELAY, 120,

    BEGIN_WRITE, WRITE_COMMAND_8, 0x29, // Display On
    END_WRITE};

Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    39 /* CS */, 48 /* SCK */, 47 /* SDA */, 18 /* DE */, 17 /* VSYNC */,
    16 /* HSYNC */, 21 /* PCLK */, 11 /* R0 */, 12 /* R1 */, 13 /* R2 */,
    14 /* R3 */, 0 /* R4 */, 8 /* G0 */, 20 /* G1 */, 3 /* G2 */, 46 /* G3 */,
    9 /* G4 */, 10 /* G5 */, 4 /* B0 */, 5 /* B1 */, 6 /* B2 */, 7 /* B3 */,
    15 /* B4 */
);
Arduino_ST7701_RGBPanel *gfx = new Arduino_ST7701_RGBPanel(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */,
    480 /* width */, 480 /* height */, st7701_4848s040_init_operations,
    sizeof(st7701_4848s040_init_operations), true /* BGR */,
    10 /* hsync_front_porch */, 8 /* hsync_pulse_width */,
    50 /* hsync_back_porch */, 10 /* vsync_front_porch */,
    8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);

/*******************************************************************************
 * Please config the touch panel in touch.h
 ******************************************************************************/
#include "touch.h"

/* LVGL draw buffers (internal SRAM): LVGL renders into one while the other is
 * copied to the PSRAM frame buffer by a GDMA channel (dma_copy.cpp) - no CPU
 * time at all, and no write-allocate. Both cores are busy rasterising, so
 * the copy has to be free. (The 3-D viewer keeps a z-buffer the same size.) */
#define DRAW_BUF_LINES 32
#define DMA_FLUSH 1
#include "dma_copy.h"

/* RGB pixel clock (14MHz ~= 49Hz refresh). The panel streams its frame
 * buffer out of PSRAM continuously (~23MB/s at 14MHz); if anything else hogs
 * PSRAM the LCD's DMA FIFO runs dry and the picture flickers. So: apps keep
 * their hot data in SRAM, and the LCD's GDMA channel gets top priority. */
#ifndef DISPLAY_PCLK_HZ
#define DISPLAY_PCLK_HZ 8000000
#endif

#include <soc/gdma_struct.h>
/* Find the GDMA channel feeding the LCD (peripheral 5 = LCD_CAM) and put it
 * at the top of the arbitration priority (everyone else defaults to 0). */
static void lcd_dma_prioritise() {
  for (int i = 0; i < 5; i++)
    if (GDMA.channel[i].out.peri_sel.sel == 5) GDMA.channel[i].out.pri.tx_pri = 9;
}

/* VSYNC. The panel has one frame buffer that it scans out top to bottom
 * (~28 times a second at 8MHz), so anything drawn while it scans tears.
 * IDF 4.4's RGB driver can call back at the end of every vsync, but
 * Arduino_GFX keeps its panel struct private: find it by scanning the bus
 * object for the pointer whose frame buffer is ours (and only trust it if
 * the size agrees too), then hook the callback. Apps call display_wait_vsync()
 * right before drawing; drawing faster than the scan (~15 rows/ms) from the
 * top then never tears. */
static SemaphoreHandle_t vsync_sem = NULL;
volatile uint32_t g_vsync_count = 0;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, esp_lcd_rgb_panel_event_data_t *, void *) {
  g_vsync_count++;
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR(vsync_sem, &woke);
  return woke == pdTRUE;
}

static bool hook_vsync(Arduino_ESP32RGBPanel *b, uint16_t *fb, size_t fbBytes) {
  const uintptr_t *words = (const uintptr_t *)b;
  for (size_t i = 0; i < sizeof(Arduino_ESP32RGBPanel) / sizeof(uintptr_t); i++) {
    const uintptr_t v = words[i];
    if (!v || (v & 3) || !esp_ptr_internal((void *)v)) continue;
    esp_rgb_panel_t *p = (esp_rgb_panel_t *)v;
    if (p->fb != (uint8_t *)fb || p->fb_size != fbBytes) continue;
    vsync_sem = xSemaphoreCreateBinary();
    p->user_ctx = NULL;
    p->on_frame_trans_done = on_vsync;
    return true;
  }
  return false;
}

/* Blocks until the next vsync ends (false on timeout or with no hook). */
bool display_wait_vsync(uint32_t timeout_ms) {
  if (!vsync_sem) return false;
  xSemaphoreTake(vsync_sem, 0); /* drop a stale one: we want the next */
  return xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static uint32_t screenWidth;
static uint32_t screenHeight;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;

volatile uint32_t g_flush_us = 0; /* total time spent in copy_to_framebuffer */

/* Copy a rendered area straight into the PSRAM framebuffer with memcpy, then
 * write the cache back once so the RGB DMA sees the new pixels. */
static void IRAM_ATTR copy_to_framebuffer(const lv_area_t *area,
                                          const lv_color_t *color_p) {
  const uint32_t t0 = micros();
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  uint16_t *fb = gfx->getFramebuffer();
  uint16_t *dst = fb + area->y1 * screenWidth + area->x1;
  const uint16_t *src = (const uint16_t *)&color_p->full;
  if (w == screenWidth) {
    memcpy(dst, src, w * h * 2);
  } else {
    for (uint32_t y = 0; y < h; y++) {
      memcpy(dst, src, w * 2);
      dst += screenWidth;
      src += w;
    }
  }
  Cache_WriteBack_Addr((uint32_t)(fb + area->y1 * screenWidth),
                       screenWidth * h * 2);
#if DMA_FLUSH
  /* DMA writes bypass the cache: drop these lines so a stale copy can never
   * be written back over newer DMA'd pixels. */
  Cache_Invalidate_Addr((uint32_t)(fb + area->y1 * screenWidth),
                        screenWidth * h * 2);
#endif
  g_flush_us += micros() - t0;
}

static DmaCopy *flush_dma = NULL;
static void IRAM_ATTR dma_flush_done(void *) { lv_disp_flush_ready(&disp_drv); }

/* Async flush: the copy runs in a task on core 0 while LVGL keeps rendering
 * the next chunk into the other buffer on core 1. */
struct flush_job_t {
  lv_area_t area;
  lv_color_t *color_p;
};
static QueueHandle_t flush_queue = NULL;

static void flush_task(void *arg) {
  flush_job_t job;
  for (;;) {
    if (xQueueReceive(flush_queue, &job, portMAX_DELAY) == pdTRUE) {
      copy_to_framebuffer(&job.area, job.color_p);
      lv_disp_flush_ready(&disp_drv);
    }
  }
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area,
                   lv_color_t *color_p) {
  /* Full-width strips are contiguous in the frame buffer (and every row
   * starts 64-byte aligned): hand them to the DMA engine. */
  if (flush_dma && area->x1 == 0 && area->x2 == (int)screenWidth - 1) {
    uint16_t *dst = gfx->getFramebuffer() + area->y1 * screenWidth;
    const size_t n = screenWidth * (area->y2 - area->y1 + 1) * 2;
    if (dma_copy_start(flush_dma, dst, color_p, n, dma_flush_done, NULL))
      return;
  }
  if (flush_queue) {
    flush_job_t job = {*area, color_p};
    xQueueSend(flush_queue, &job, portMAX_DELAY);
  } else {
    copy_to_framebuffer(area, color_p);
    lv_disp_flush_ready(disp);
  }
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  if (touch_has_signal()) {
    if (touch_touched()) {
      data->state = LV_INDEV_STATE_PR;

      /*Set the coordinates*/
      data->point.x = touch_last_x;
      data->point.y = touch_last_y;
    } else if (touch_released()) {
      data->state = LV_INDEV_STATE_REL;
    }
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_VERBOSE
#endif

#define logSection(section)                                                    \
  Log.setShowLevel(false);                                                     \
  Log.notice(F("************* " section " **************" CR));                \
  Log.setShowLevel(true);

void setup() {
  Serial.begin(SERIAL_BAUD);
  //  delay(1000);
  esp_task_wdt_init(120, true); // enable panic so ESP32 restarts
  esp_task_wdt_add(NULL);       // add current thread to WDT watch
  // Log.setPrefix(logPrintPrefix);
  Log.begin(LOG_LEVEL, &Serial);
  delay(4000); // Wait for serial monitor to open
  logSection(APP_NAME);

  logSection("ESP Information");

  Log.verbose("SRAM free size: %d\n",
              heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  Log.verbose("PSRAM free size: %d\n",
              heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  Log.verbose(F("Free heap: %d bytes" CR), ESP.getFreeHeap());
  Log.verbose(F("Free sketch space: %d bytes" CR), ESP.getFreeSketchSpace());
  Log.verbose(F("Chip ID: %x" CR), ESP.getEfuseMac());

  Log.verbose(F("Flash chip size: %d bytes" CR), ESP.getFlashChipSize());
  //  Log.verbose(F("Flash chip speed: %d Hz" CR), ESP.getFlashChipSpeed());
  Log.verbose(F("CPU frequency: %d Hz" CR), ESP.getCpuFreqMHz());
  Log.verbose(F("SDK version: %s" CR), ESP.getSdkVersion());

  logSection("touch_init");
  // Init touch device
  touch_init();

  // Init Display
  // gfx->begin();

  gfx->begin(DISPLAY_PCLK_HZ);
  lcd_dma_prioritise();
  if (hook_vsync(bus, gfx->getFramebuffer(), gfx->width() * gfx->height() * 2))
    Log.verbose(F("VSYNC callback hooked" CR));
  else
    Log.warning(F("VSYNC callback not found: frames may tear" CR));

  gfx->fillScreen(NAVY);

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif
  logSection("lv_init");
  lv_init();

  screenWidth = gfx->width();
  screenHeight = gfx->height();
  Log.verbose(F("Width: %l, Height: %l" CR), screenWidth, screenHeight);
  const size_t buf_px = screenWidth * DRAW_BUF_LINES;
  const uint32_t buf_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
  lv_color_t *buf1 =
      (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * buf_px, buf_caps);
  lv_color_t *buf2 =
      (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * buf_px, buf_caps);
  if (!buf1) {
    Serial.println("LVGL disp_draw_buf allocate failed!");
  } else {
#if DMA_FLUSH
    flush_dma = dma_copy_create(sizeof(lv_color_t) * buf_px);
    if (flush_dma) { /* self-test: one small SRAM -> PSRAM copy */
      uint8_t *s = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      uint8_t *t = (uint8_t *)heap_caps_aligned_alloc(64, 4096, MALLOC_CAP_SPIRAM);
      bool ok = s && t;
      if (ok) {
        for (int i = 0; i < 4096; i++) s[i] = i * 7;
        memset(t, 0, 4096);
        Cache_WriteBack_Addr((uint32_t)t, 4096);
        Cache_Invalidate_Addr((uint32_t)t, 4096);
        ok = dma_copy_start(flush_dma, t, s, 4096, NULL, NULL);
        const uint32_t t0 = micros();
        while (dma_copy_busy(flush_dma) && micros() - t0 < 100000) {
        }
        Cache_Invalidate_Addr((uint32_t)t, 4096);
        for (int i = 0; ok && i < 4096; i++) ok = t[i] == (uint8_t)(i * 7);
        ok = ok && !dma_copy_busy(flush_dma);
      }
      heap_caps_free(s);
      heap_caps_free(t);
      if (!ok) {
        Log.warning(F("GDMA self-test failed, using CPU flush" CR));
        flush_dma = NULL;
      }
    }
    if (flush_dma) {
      /* From here on only DMA writes the frame buffer: flush the CPU's view
       * of it once so no stale dirty line is ever written back on top. */
      uint16_t *fb = gfx->getFramebuffer();
      Cache_WriteBack_Addr((uint32_t)fb, screenWidth * screenHeight * 2);
      Cache_Invalidate_Addr((uint32_t)fb, screenWidth * screenHeight * 2);
      Log.verbose(F("Double buffered, GDMA flush" CR));
    }
#endif
    if (buf2 && !flush_dma) {
      /* Double buffered: hand flushing off to core 0 */
      flush_queue = xQueueCreate(1, sizeof(flush_job_t));
      xTaskCreatePinnedToCore(flush_task, "lv_flush", 4096, NULL,
                              configMAX_PRIORITIES - 2, NULL, 0);
      Log.verbose(F("Double buffered, async flush on core 0" CR));
    } else if (!buf2) {
      Log.warning(F("Only one draw buffer, flushing synchronously" CR));
    }
    logSection("lv_disp_draw_buf_init");
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);

    /* Initialize the display */
    lv_disp_drv_init(&disp_drv);
    /* Change the following line to your display resolution */
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.antialiasing = 1; /* smooth text and shape edges (0 also thresholds font glyphs) */
    lv_disp_drv_register(&disp_drv);

    /* Initialize the (dummy) input device driver */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    viewer3d_start();
  }
  Serial.println("Setup done");
}

/* Screenshot over serial: 'S' = every other pixel (240x240), 'F' = full
 * 480x480. Prints "SHOT w h" at the normal baud, switches to 921600 for the
 * raw little-endian RGB565 rows, then switches back. LVGL is not serviced
 * while sending, so the image is one consistent frame. */
#define SHOT_BAUD 921600
static void serial_screenshot_poll() {
  if (!Serial.available()) return;
  const int c = Serial.read();
  if (c != 'S' && c != 'F') {
    viewer3d_command(c); /* single-letter debug commands, see viewer3d.h */
    return;
  }
  const int step = c == 'F' ? 1 : 2;
  const int w = screenWidth / step, h = screenHeight / step;
  const uint16_t *fb = gfx->getFramebuffer();
#if DMA_FLUSH
  /* the frame buffer is written by DMA behind the cache's back */
  Cache_Invalidate_Addr((uint32_t)fb, screenWidth * screenHeight * 2);
#endif
  static uint16_t line[480];
  Serial.printf("\nSHOT %d %d\n", w, h);
  Serial.flush();
  Serial.updateBaudRate(SHOT_BAUD);
  delay(300); /* give the host time to follow */
  for (int y = 0; y < (int)screenHeight; y += step) {
    const uint16_t *src = fb + y * screenWidth;
    for (int x = 0, i = 0; x < (int)screenWidth; x += step) line[i++] = src[x];
    Serial.write((const uint8_t *)line, w * 2);
    esp_task_wdt_reset();
  }
  Serial.flush();
  delay(50);
  Serial.updateBaudRate(SERIAL_BAUD);
}

void loop() {
  esp_task_wdt_reset(); /* feed the watchdog, otherwise it reboots the board */
  uint32_t wait_ms = lv_timer_handler(); /* let the GUI do its work */
  serial_screenshot_poll();
  delay(wait_ms > 5 ? 5 : (wait_ms ? wait_ms : 1));
}
