/* STARLANE - public interface between the game core and the platform.
 * The core is plain C++ (no Arduino headers) so it also builds in the desktop
 * simulator (sim/starlane_sim.cpp). */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace sl {

constexpr int LW = 240, LH = 240; /* logical screen; 2x2 panel pixels each */

extern uint8_t scr[LW * LH]; /* palette-indexed frame written by tick() */
extern uint16_t pal565[256];

void init(uint32_t seed);
/* One 1/30 s step: input (logical coords), update, render into scr. */
void tick(bool down, int lx, int ly);

/* ---- implemented by the platform (board or simulator) ---- */
bool platformLoad(void *buf, size_t len);
void platformSave(const void *buf, size_t len);
void *platformAllocLarge(size_t len); /* PSRAM on the board */

} // namespace sl
