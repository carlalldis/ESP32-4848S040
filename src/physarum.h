#pragma once

/* PHYSARUM - a living slime-mould simulation for the 480x480 ESP32-4848S040.
 * Call physarum_start() once after LVGL and its display/input drivers exist.
 * It starts its own simulation task on core 0 and renders through LVGL. */
void physarum_start();

/* Single-character commands from the serial port (handled on the LVGL thread):
 *   n next behaviour preset   t next colour theme   b spore burst
 *   f phantom feast           w wandering food      h "HELLO"   m "FEED ME"
 *   r restart from the spore
 *   1 2 3 toggle a pretend finger (testing without touching the screen)
 *   o a pretend finger that draws a circle */
void physarum_command(int c);
