#pragma once
#include <stdint.h>

/* NEON WHACK game core: all game logic plus a software renderer that writes
 * RGB565 rows. It has no LVGL / Arduino dependencies, so the same file also
 * builds in the desktop simulator (sim/whack_sim.cpp). The LVGL side
 * (neon_whack.cpp) feeds it time and taps, calls render() from a draw event,
 * and puts the HUD text on top. */
namespace whack {

constexpr int W = 480, H = 480;
constexpr int ROUND_SECONDS = 60;

enum class Phase : uint8_t { Title, Countdown, Playing, Over };

struct Status {
  Phase phase;
  float phaseTime; // seconds since the phase started
  float timeLeft;  // round clock
  int score, best;
  int combo, mult; // hit streak and the score multiplier it gives
  bool newBest;
};

enum PopupKind : uint8_t { POP_NORMAL, POP_GOLD, POP_BOMB };
struct Popup {
  uint32_t id;      // changes whenever the slot is reused
  float x, y, life; // screen position (centre), seconds left (<= 0: unused)
  int value;
  PopupKind kind;
};
constexpr int MAX_POPUPS = 6;
constexpr float POPUP_LIFE = 0.85f;

void init(uint32_t best, uint32_t seed);
void update(float dt);
void tap(int x, int y);

/* Render the screen rectangle [x0,x1) x [y0,y1). `buf` holds screen pixel
 * (bufX0, bufY0) at index 0 and its rows are `stride` pixels apart. */
void render(uint16_t *buf, int stride, int bufX0, int bufY0, int x0, int y0,
            int x1, int y1);

const Status &status();
const Popup *popups();

#ifdef WHACK_SIM
/* desktop simulator only: where mole i can be tapped (false if not up) */
bool simMoleTarget(int i, int &x, int &y, int &type);
void simSpawn(int i, int type); // force mole i up (0 normal, 1 gold, 2 bomb)
int simMountainRuns();
#endif

} // namespace whack
