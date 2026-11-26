

/*******************************************************************************
 * LVGL Widgets
 * This is a widgets demo for LVGL - Light and Versatile Graphics Library
 * import from: https://github.com/lvgl/lv_demos.git
 *
 * Dependent libraries:
 * LVGL: https://github.com/lvgl/lvgl.git

 * Touch libraries:
 * FT6X36: https://github.com/strange-v/FT6X36.git
 * GT911: https://github.com/TAMCTec/gt911-arduino.git
 * XPT2046: https://github.com/PaulStoffregen/XPT2046_Touchscreen.git
 *
 * LVGL Configuration file:
 * Copy your_arduino_path/libraries/lvgl/lv_conf_template.h
 * to your_arduino_path/libraries/lv_conf.h
 * Then find and set:
 * #define LV_COLOR_DEPTH     16
 * #define LV_TICK_CUSTOM     1
 *
 * For SPI display set color swap can be faster, parallel screen don't set!
 * #define LV_COLOR_16_SWAP   1
 *
 * Optional: Show CPU usage and FPS count
 * #define LV_USE_PERF_MONITOR 1
 ******************************************************************************/
#include "lv_demo_widgets.h"
#include <Arduino.h>
#include <ArduinoLog.h>
#include <esp_task_wdt.h>

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

/* Change to your screen resolution */
static uint32_t screenWidth;
static uint32_t screenHeight;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;
static lv_disp_drv_t disp_drv;

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area,
                   lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w,
                            h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

  lv_disp_flush_ready(disp);
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
  logSection("LVGL Widgets Demo");

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

  gfx->begin(8000000); /* 8MHz (160MHz/20) avoids PSRAM GDMA underrun tearing/warping */

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
#ifdef ESP32
  disp_draw_buf =
      (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * screenWidth * 40,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  disp_draw_buf = (lv_color_t *)malloc(sizeof(lv_color_t) * screenWidth * 40);
#endif
  if (!disp_draw_buf) {
    Serial.println("LVGL disp_draw_buf allocate failed!");
  } else {
    logSection("lv_disp_draw_buf_init");
    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, screenWidth * 40);

    /* Initialize the display */
    lv_disp_drv_init(&disp_drv);
    /* Change the following line to your display resolution */
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* Initialize the (dummy) input device driver */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    lv_demo_widgets();

    Serial.println("DEmo done");
  }
  Serial.println("Setup done");
}

void loop() {
  lv_timer_handler(); /* let the GUI do its work */
  delay(5);
}
