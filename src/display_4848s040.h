/*******************************************************************************
 * ESP32-4848S040 (Guition, ST7701S 480x480 RGB panel) display setup for
 * Arduino_GFX 1.2.9. Drop this file into any project and `#include` it once
 * (in one .cpp) to get `bus` and `gfx`; then call gfx->begin(pclk_hz).
 *
 * Colour inversion fix:
 *  The panel's correct state is INVOFF. The board has no LCD reset line, so
 *  the controller keeps whatever state an earlier firmware left it in across
 *  an ESP32 reset / flash (e.g. INVON from IPS=true), which is why toggling
 *  the IPS flag alone appeared to do nothing until a power cycle. This
 *  sequence therefore:
 *    1. issues a software reset (0x01) first, so every boot starts clean;
 *    2. explicitly sends INVOFF (0x20);
 *  and the panel is constructed with IPS=false so the library never sends
 *  INVON (0x21) afterwards.
 *
 *  The rest is the manufacturer's sequence (identical to Arduino_GFX's later
 *  st7701_type9_init_operations for this board).
 ******************************************************************************/
#pragma once
#include <Arduino_GFX_Library.h>

#define GFX_BL 38

static const uint8_t st7701_4848s040_init_operations[] = {
    BEGIN_WRITE, WRITE_COMMAND_8, 0x01, // Software reset: known state every boot
    END_WRITE,

    DELAY, 120,

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

    WRITE_COMMAND_8, 0x20, // Display inversion OFF (correct for this panel)

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
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */,
    false /* IPS: must stay false, true sends INVON and inverts colours */,
    480 /* width */, 480 /* height */, st7701_4848s040_init_operations,
    sizeof(st7701_4848s040_init_operations), true /* BGR */,
    10 /* hsync_front_porch */, 8 /* hsync_pulse_width */,
    50 /* hsync_back_porch */, 10 /* vsync_front_porch */,
    8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);
