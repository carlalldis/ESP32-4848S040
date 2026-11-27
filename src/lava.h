#pragma once

/* LAVA - a lava lamp for the 480x480 ESP32-4848S040.
 * Call lava_start() once after LVGL and its display/input drivers exist.
 * It starts its own field-builder task on core 0 and renders through LVGL.
 *
 * Touch:
 *   drag a blob        pick the wax up and fling it (your finger warms it)
 *   swipe through one  slice it in two
 *   stir the liquid    blobs nearby follow the current
 *   hold on liquid     blow a new blob; it grows until you let go
 *   double tap         next colour theme */
void lava_start();

/* Single-character commands from the serial port (handled on the LVGL thread):
 *   t next theme   b blow a blob from the pool   h heater surge
 *   r restart      1 2 toggle a pretend finger   o a pretend finger stirring
 *   in a circle */
void lava_command(int c);
