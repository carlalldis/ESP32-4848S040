#pragma once

/* VIEWER 3D - a software-rasterised STL viewer for the 480x480 ESP32-4848S040.
 * Call viewer3d_start() once after LVGL and its display/input drivers exist.
 *
 *   tap                 next render mode (wireframe / flat / smooth / hidden line)
 *   tap < or > (bottom) previous / next model
 *   drag                spin it yourself (auto rotation resumes when you let go)
 *   pinch               zoom
 */
void viewer3d_start();

/* Single-character commands from the serial port (LVGL thread):
 *   m next render mode   n next model   p previous model   a auto rotation on/off
 *   v vsync lock on/off (off: render flat out, frames tear) */
void viewer3d_command(int c);
