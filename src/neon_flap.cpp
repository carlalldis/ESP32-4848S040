/*******************************************************************************
 * NEON FLAP - cyberpunk flappy-bird style game for the ESP32-4848S040.
 *
 * Rendering: one full-screen LVGL object whose DRAW_MAIN handler writes pixels
 * straight into LVGL's strip draw buffer, row by row (sky, synth sun, two
 * parallax skyline layers, perspective grid floor, neon pillars, the bird,
 * trail/particles, glitch + scanline post effects). Text is ordinary LVGL
 * labels drawn on top. The game object is invalidated every tick, so every
 * frame is a full redraw through the existing double-buffered flush path.
 ******************************************************************************/
#include "neon_flap.h"
#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>

namespace {

/* ---------------------------------------------------------------- tuning -- */
constexpr int W = 480, H = 480;
constexpr int GROUND_Y = 420;        // top of the grid floor
constexpr int BIRD_X = 140;          // bird's fixed screen x
constexpr float BIRD_R = 12.0f;      // collision radius
constexpr int BIRD_BOX = 26;         // render half-extent (incl. glow)
constexpr int PIPE_W = 72;
constexpr int CAP_H = 18;
constexpr int CAP_X = 7;             // cap overhang each side
constexpr int PIPE_SPACING = 245;
constexpr int MAX_PIPES = 4;
constexpr float GRAVITY = 1450.0f;   // px/s^2
constexpr float FLAP_V = -440.0f;    // px/s
constexpr float MAX_FALL = 720.0f;
constexpr int TRAIL_N = 18;
constexpr int MAX_PARTICLES = 72;

/* -------------------------------------------------------------- colours -- */
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t CYAN = rgb(0, 240, 255);
constexpr uint16_t MAGENTA = rgb(255, 42, 157);
constexpr uint16_t YELLOW = rgb(255, 211, 25);
constexpr uint16_t LIME = rgb(57, 255, 20);
constexpr uint16_t WHITE = 0xFFFF;

inline uint16_t scale565(uint16_t c, int s) { // s: 0..256
  int r = ((c >> 11) * s) >> 8;
  int g = (((c >> 5) & 63) * s) >> 8;
  int b = ((c & 31) * s) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
inline uint16_t mix565(uint16_t a, uint16_t b, int t) { // t: 0..256 toward b
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + (((br - ar) * t) >> 8)) << 11) |
                    ((ag + (((bg - ag) * t) >> 8)) << 5) |
                    (ab + (((bb - ab) * t) >> 8)));
}
inline uint16_t add565(uint16_t a, uint16_t b) {
  uint32_t r = (a & 0xF800u) + (b & 0xF800u);
  uint32_t g = (a & 0x07E0u) + (b & 0x07E0u);
  uint32_t bl = (a & 0x001Fu) + (b & 0x001Fu);
  if (r > 0xF800u) r = 0xF800u;
  if (g > 0x07E0u) g = 0x07E0u;
  if (bl > 0x001Fu) bl = 0x001Fu;
  return (uint16_t)(r | g | bl);
}
inline uint16_t lerpRGB(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1,
                        uint8_t g1, uint8_t b1, float t) {
  return rgb((uint8_t)(r0 + (r1 - r0) * t), (uint8_t)(g0 + (g1 - g0) * t),
             (uint8_t)(b0 + (b1 - b0) * t));
}

inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^
               (c + 0x165667B1u) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}
inline int pmod(int a, int m) {
  int r = a % m;
  return r < 0 ? r + m : r;
}
inline float frand(float lo, float hi) {
  return lo + (hi - lo) * (esp_random() / 4294967295.0f);
}

/* Fill row[a..b) with c using 32-bit stores where possible. */
inline void fillSpan(uint16_t *row, int a, int b, uint16_t c) {
  if (a >= b) return;
  uint16_t *p = row + a, *e = row + b;
  if (((uintptr_t)p & 2) != 0) *p++ = c;
  uint32_t c2 = c | ((uint32_t)c << 16);
  while (p + 2 <= e) {
    *(uint32_t *)p = c2;
    p += 2;
  }
  if (p < e) *p = c;
}
inline void addSpan(uint16_t *row, int a, int b, uint16_t c) {
  for (int x = a; x < b; x++) row[x] = add565(row[x], c);
}

/* ------------------------------------------------------- background LUTs -- */
uint16_t skyLUT[GROUND_Y];
int16_t starX[GROUND_Y];
constexpr int SUN_CX = 330, SUN_CY = 318, SUN_R = 92;
int16_t sunHalf[GROUND_Y], halo1Half[GROUND_Y], halo2Half[GROUND_Y];
uint16_t sunCol[GROUND_Y], halo1Col[GROUND_Y], halo2Col[GROUND_Y];

constexpr int GROUND_ROWS = H - GROUND_Y;
constexpr int VANISH_OFF = 60; // vanishing point this far above the horizon
constexpr int GRID_SP = 60;    // grid line spacing at the bottom edge
int32_t gridT[GROUND_ROWS];    // 16.16 perspective scale per floor row
uint16_t groundBase[GROUND_ROWS], groundLine[GROUND_ROWS];
bool groundHLine[GROUND_ROWS];

struct Building {
  int16_t x, w, top;
  uint16_t seed;
  uint8_t ant;     // antenna height (0 = none)
  uint8_t signLen; // vertical neon sign length (0 = none)
  uint8_t signCol;
};
struct Layer {
  Building b[40];
  int n = 0;
  int period = 0;
  uint16_t body, rim, win[3];
  int winW, pitchX, pitchY, winH, litPct;
};
Layer farLayer, nearLayer;
const uint16_t SIGN_COLS[4] = {MAGENTA, CYAN, YELLOW, LIME};

uint32_t rngState = 0x1234567u;
uint32_t xrand() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}
int xrange(int lo, int hi) { return lo + (int)(xrand() % (uint32_t)(hi - lo + 1)); }

void genLayer(Layer &L, int period, int wmin, int wmax, int hmin, int hmax,
              int gapMax, bool withSigns) {
  L.period = period;
  L.n = 0;
  int x = 0;
  while (x < period - wmin && L.n < 40) {
    Building &B = L.b[L.n];
    int w = xrange(wmin, wmax);
    if (x + w > period) w = period - x;
    int h = xrange(hmin, hmax);
    B.x = x;
    B.w = w;
    B.top = GROUND_Y - h;
    B.seed = (uint16_t)xrand();
    B.ant = (xrand() % 4 == 0) ? xrange(8, 26) : 0;
    B.signLen = 0;
    B.signCol = 0;
    if (withSigns && w >= 50 && h > 70 && xrand() % 3 != 0) {
      B.signLen = xrange(30, min(80, h - 30));
      B.signCol = 1 + xrand() % 4;
    }
    L.n++;
    x += w + xrange(0, gapMax);
  }
}

void buildBackground() {
  // Sky gradient: deep night -> violet -> hot pink haze at the horizon.
  for (int y = 0; y < GROUND_Y; y++) {
    if (y < 200)
      skyLUT[y] = lerpRGB(4, 0, 14, 22, 4, 48, y / 200.0f);
    else if (y < 330)
      skyLUT[y] = lerpRGB(22, 4, 48, 62, 8, 84, (y - 200) / 130.0f);
    else
      skyLUT[y] = lerpRGB(62, 8, 84, 130, 20, 112, (y - 330) / 90.0f);
  }
  // Stars in the upper sky: at most one per row.
  for (int y = 0; y < GROUND_Y; y++)
    starX[y] = (y < 280 && xrand() % 3 == 0) ? xrange(0, W - 1) : -1;

  // Striped synthwave sun plus two soft halo discs.
  for (int y = 0; y < GROUND_Y; y++) {
    int dy = y - SUN_CY;
    float t = constrain((y - (SUN_CY - SUN_R)) / (2.0f * SUN_R), 0.0f, 1.0f);
    uint16_t sc = lerpRGB(255, 236, 90, 255, 30, 150, t);
    sunCol[y] = sc;
    sunHalf[y] = (abs(dy) < SUN_R) ? (int16_t)sqrtf(SUN_R * SUN_R - dy * dy) : 0;
    int stripeStart = SUN_CY - 24;
    if (y > stripeStart) {
      int d = y - stripeStart;
      int gapW = 1 + d / 20;
      if (d % 14 >= 14 - gapW) sunHalf[y] = 0;
    }
    const int r1 = SUN_R + 34, r2 = SUN_R + 14;
    halo1Half[y] = (abs(dy) < r1) ? (int16_t)sqrtf(r1 * r1 - dy * dy) : 0;
    halo2Half[y] = (abs(dy) < r2) ? (int16_t)sqrtf(r2 * r2 - dy * dy) : 0;
    halo1Col[y] = mix565(skyLUT[y], sc, 34);
    halo2Col[y] = mix565(skyLUT[y], sc, 72);
  }

  // Skylines.
  genLayer(farLayer, 1100, 26, 62, 60, 175, 4, false);
  farLayer.body = rgb(28, 10, 56);
  farLayer.rim = rgb(60, 24, 104);
  farLayer.win[0] = rgb(80, 46, 150);
  farLayer.win[1] = rgb(120, 34, 110);
  farLayer.win[2] = rgb(160, 120, 70);
  farLayer.winW = 2;
  farLayer.pitchX = 5;
  farLayer.pitchY = 7;
  farLayer.winH = 3;
  farLayer.litPct = 30;

  genLayer(nearLayer, 1400, 50, 104, 34, 140, 14, true);
  nearLayer.body = rgb(10, 4, 24);
  nearLayer.rim = rgb(90, 24, 124);
  nearLayer.win[0] = rgb(0, 170, 205);
  nearLayer.win[1] = rgb(200, 30, 136);
  nearLayer.win[2] = rgb(235, 185, 60);
  nearLayer.winW = 4;
  nearLayer.pitchX = 8;
  nearLayer.pitchY = 10;
  nearLayer.winH = 5;
  nearLayer.litPct = 34;

  // Grid floor.
  for (int gy = 0; gy < GROUND_ROWS; gy++) {
    float t = (gy + VANISH_OFF) / (float)(GROUND_ROWS + VANISH_OFF);
    float tn = (gy + 1 + VANISH_OFF) / (float)(GROUND_ROWS + VANISH_OFF);
    gridT[gy] = (int32_t)(t * 65536.0f);
    groundHLine[gy] = (int)(6.0f / t) != (int)(6.0f / tn);
    float k = gy / (float)GROUND_ROWS;
    groundBase[gy] = lerpRGB(36, 6, 54, 8, 2, 20, k);
    groundLine[gy] = lerpRGB(130, 20, 120, 255, 50, 210, k);
  }
}

/* ------------------------------------------------------------ game state -- */
enum State { READY, PLAYING, DEAD };

struct Pipe {
  float x;
  int16_t gapY, gap;
  bool scored;
  uint8_t hue;
};
struct PipePal {
  uint16_t edge, mid, inner, fill, hatch, glow1, glow2, capFill;
};
PipePal pipePal[2];

struct Particle {
  float x, y, vx, vy, life, maxLife;
  uint16_t col;
  uint8_t size;
};
struct TrailPt {
  float x, y;
};

State state = READY;
float tGlobal = 0;
double scroll = 0;
float speed = 155;
float birdY = 200, birdVy = 0, birdAngle = 0, birdCos = 1, birdSin = 0;
float wingPhase = 0;
Pipe pipes[MAX_PIPES];
int pipeCount = 0;
Particle parts[MAX_PARTICLES];
int partCount = 0;
TrailPt trail[TRAIL_N];
int trailCount = 0;
float trailTimer = 0;
int score = 0, best = 0;
bool newBest = false;
float deadTime = 0;
float flashAmt = 0, shakeAmt = 0, glitchT = 0, glitchI = 0, nextAmbientGlitch = 4;
int shakeX = 0, shakeY = 0;
uint32_t frameNo = 0, nowMs = 0;
int lastGapY = 220;

Preferences prefs;

/* --------------------------------------------------------------- objects -- */
lv_obj_t *gameObj;
struct NeonText {
  lv_obj_t *main, *shadow;
};
NeonText scoreTxt, titleTxt;
lv_obj_t *subtitleLbl, *bestReadyLbl;
lv_obj_t *panel, *panelScoreLbl, *panelBestLbl, *panelNewLbl, *panelTapLbl;

/* ------------------------------------------------------------ rendering -- */
void drawLayerRow(const Layer &L, uint16_t *row, int ry, int xa, int xb,
                  int off) {
  for (int i = 0; i < L.n; i++) {
    const Building &B = L.b[i];
    for (int rep = 0; rep < 2; rep++) {
      const int cx = B.x - off + rep * L.period;
      const int cxe = cx + B.w;
      if (cxe <= xa || cx >= xb) continue;

      if (ry < B.top) { // antenna + blinking beacon
        if (B.ant && ry >= B.top - B.ant) {
          const int ax = cx + B.w / 2;
          if (ry < B.top - B.ant + 3) {
            bool on = (((nowMs >> 9) + B.seed) & 3) == 0;
            uint16_t c = on ? rgb(255, 40, 60) : rgb(90, 10, 30);
            for (int x = max(ax - 1, xa); x < min(ax + 2, xb); x++) row[x] = c;
          } else if (ax >= xa && ax < xb) {
            row[ax] = L.rim;
          }
        }
        continue;
      }

      const int s0 = max(cx, xa), s1 = min(cxe, xb);
      if (ry == B.top) {
        fillSpan(row, s0, s1, L.rim);
        continue;
      }
      fillSpan(row, s0, s1, L.body);
      if (cx >= xa) row[cx] = L.rim;

      // Windows on a grid; lit state is a stable hash per window.
      const int ly = ry - B.top - 4;
      if (ly >= 0 && (ly % L.pitchY) < L.winH) {
        const int wy = ly / L.pitchY;
        int k = 0;
        for (int wx = 3; wx + L.winW <= B.w - 2; wx += L.pitchX, k++) {
          uint32_t h = hash3(B.seed, k, wy);
          if ((int)(h % 100) >= L.litPct) continue;
          uint16_t c = L.win[((h >> 8) & 7) == 0 ? 2 : ((h >> 11) & 1)];
          const int p0 = max(cx + wx, xa), p1 = min(cx + wx + L.winW, xb);
          for (int p = p0; p < p1; p++) row[p] = c;
        }
      }

      // Vertical neon sign with flicker and a soft glow.
      if (B.signLen) {
        const int sTop = B.top + 14;
        const int ly2 = ry - sTop;
        if (ly2 >= 0 && ly2 < B.signLen) {
          const int sx = cx + B.w - 15;
          bool flick = (hash3(B.seed, nowMs >> 6, 3) % 29) == 0;
          uint16_t base = SIGN_COLS[B.signCol - 1];
          uint16_t bright = flick ? scale565(base, 60) : base;
          uint16_t dark = scale565(base, flick ? 20 : 46);
          uint16_t glow = scale565(base, flick ? 10 : 40);
          for (int x = sx - 3; x < sx + 10; x++) {
            if (x < xa || x >= xb) continue;
            int lx = x - sx;
            if (lx < 0 || lx > 6) {
              row[x] = add565(row[x], glow);
            } else if (lx == 0 || lx == 6 || ly2 == 0 || ly2 == B.signLen - 1) {
              row[x] = bright;
            } else {
              bool glyph = (ly2 % 8) < 6 && (ly2 % 8) > 0 &&
                           (hash3(B.seed, ly2 / 8, lx) & 1);
              row[x] = glyph ? bright : dark;
            }
          }
        }
      }
    }
  }
}

void drawGroundRow(uint16_t *row, int ry, int xa, int xb) {
  const int gy = ry - GROUND_Y;
  if (gy == 0) {
    fillSpan(row, xa, xb, rgb(255, 110, 220));
    return;
  }
  if (gy == 1) {
    fillSpan(row, xa, xb, rgb(190, 40, 160));
    return;
  }
  fillSpan(row, xa, xb, groundHLine[gy] ? groundLine[gy] : groundBase[gy]);
  const int32_t t = gridT[gy];
  const int sc = pmod((int)scroll - shakeX, GRID_SP);
  const uint16_t lc = groundLine[gy];
  const bool thick = gy > 36;
  for (int k = -9; k <= 18; k++) {
    int xi = k * GRID_SP - sc;
    int x = W / 2 + (((xi - W / 2) * t) >> 16);
    if (x >= xa && x < xb) row[x] = lc;
    if (thick && x + 1 >= xa && x + 1 < xb) row[x + 1] = lc;
  }
}

void drawPipeSpan(uint16_t *row, int xa, int xb, int s0, int s1, int ry,
                  int px, const PipePal &P, bool cap, bool capEdge) {
  // Outer glow (additive), 2px each side.
  if (s0 - 2 >= xa && s0 - 2 < xb) row[s0 - 2] = add565(row[s0 - 2], P.glow2);
  if (s0 - 1 >= xa && s0 - 1 < xb) row[s0 - 1] = add565(row[s0 - 1], P.glow1);
  if (s1 >= xa && s1 < xb) row[s1] = add565(row[s1], P.glow1);
  if (s1 + 1 >= xa && s1 + 1 < xb) row[s1 + 1] = add565(row[s1 + 1], P.glow2);

  const int a = max(s0, xa), b = min(s1, xb);
  if (capEdge) {
    fillSpan(row, a, b, P.edge);
    return;
  }
  for (int x = a; x < b; x++) {
    int d = min(x - s0, s1 - 1 - x);
    uint16_t c;
    if (d == 0)
      c = P.edge;
    else if (d == 1)
      c = P.mid;
    else if (d == 2)
      c = P.inner;
    else if (cap)
      c = P.capFill;
    else
      c = (((x - px) + ry) & 15) == 0 ? P.hatch : P.fill;
    row[x] = c;
  }
}

void drawPipesRow(uint16_t *row, int ry, int xa, int xb) {
  if (ry >= GROUND_Y) return;
  for (int i = 0; i < pipeCount; i++) {
    const Pipe &p = pipes[i];
    const int px = (int)p.x + shakeX;
    if (px + PIPE_W + CAP_X + 2 <= xa || px - CAP_X - 2 >= xb) continue;
    const int gapTop = p.gapY - p.gap / 2, gapBot = p.gapY + p.gap / 2;
    const PipePal &P = pipePal[p.hue];
    if (ry < gapTop - CAP_H || ry >= gapBot + CAP_H) {
      drawPipeSpan(row, xa, xb, px, px + PIPE_W, ry, px, P, false, false);
    } else if (ry < gapTop) {
      bool edge = ry == gapTop - CAP_H || ry >= gapTop - 2;
      drawPipeSpan(row, xa, xb, px - CAP_X, px + PIPE_W + CAP_X, ry, px, P,
                   true, edge);
    } else if (ry >= gapBot) {
      bool edge = ry <= gapBot + 1 || ry == gapBot + CAP_H - 1;
      drawPipeSpan(row, xa, xb, px - CAP_X, px + PIPE_W + CAP_X, ry, px, P,
                   true, edge);
    }
  }
}

/* Bird shapes in bird-local coordinates (facing +u, v down). */
constexpr uint16_t B_BODY = rgb(40, 10, 72), B_BELLY = rgb(74, 20, 110);
constexpr uint16_t B_VISOR_D = rgb(0, 140, 185), B_VISOR_H = rgb(210, 255, 255);
constexpr uint16_t B_BEAK_D = rgb(255, 120, 0);
constexpr uint16_t B_WING = rgb(10, 70, 104);

inline bool birdPixel(float u, float v, float wy, uint16_t &c) {
  if (u >= 11 && u <= 21 && fabsf(v - 2) <= (21 - u) * 0.42f) {
    c = v > 2.5f ? B_BEAK_D : YELLOW;
    return true;
  }
  const float e = (u * u) * (1.0f / 225.0f) + (v * v) * (1.0f / 144.0f);
  if (u >= 2 && u <= 13.5f && v >= -8 && v <= -2.5f && e <= 1.1f) {
    c = v < -7 ? B_VISOR_H : ((((int)(u + 20)) % 3 == 0) ? B_VISOR_D : CYAN);
    return true;
  }
  const float eu = (u + 4) * 0.1f, ev = (v - wy) * 0.2f;
  const float ew = eu * eu + ev * ev;
  if (ew <= 1) {
    c = ew > 0.55f ? CYAN : B_WING;
    return true;
  }
  if (e <= 1) {
    c = e > 0.7f ? MAGENTA : (v > 4 ? B_BELLY : B_BODY);
    return true;
  }
  if (u >= -23 && u <= -12) {
    float half = (-12 - u) * 0.55f + 1;
    float av = fabsf(v + 1);
    if (av <= half && !(u < -18 && av < half * 0.35f)) {
      c = av < 1.2f ? CYAN : MAGENTA;
      return true;
    }
  }
  return false;
}

void drawBirdRow(uint16_t *row, int ry, int xa, int xb) {
  const int by = (int)birdY;
  const int dyi = ry - by;
  if (dyi < -BIRD_BOX || dyi > BIRD_BOX) return;
  const int bx = BIRD_X + shakeX;
  const int x0 = max(bx - BIRD_BOX, xa), x1 = min(bx + BIRD_BOX + 1, xb);
  const float dy = dyi + 0.5f - (birdY - by);
  const float wy = 1.0f + 5.0f * sinf(wingPhase);
  const uint16_t glowBase = state == DEAD ? rgb(255, 60, 40) : MAGENTA;
  for (int x = x0; x < x1; x++) {
    const float dx = x - bx + 0.5f;
    const float u = dx * birdCos + dy * birdSin;
    const float v = -dx * birdSin + dy * birdCos;
    uint16_t c;
    if (birdPixel(u, v, wy, c)) {
      row[x] = c;
    } else {
      float r2 = dx * dx + dy * dy;
      if (r2 < (float)(BIRD_BOX * BIRD_BOX)) {
        float f = 1.0f - r2 * (1.0f / (BIRD_BOX * BIRD_BOX));
        int s = (int)(f * f * 100);
        if (s > 0) row[x] = add565(row[x], scale565(glowBase, s));
      }
    }
  }
}

void drawTrailRow(uint16_t *row, int ry, int xa, int xb) {
  for (int i = 0; i < trailCount; i++) {
    const int s = 1 + (i * 5) / TRAIL_N;
    const int ty = (int)trail[i].y;
    if (ry < ty - s || ry > ty + s) continue;
    const int tx = (int)trail[i].x + shakeX;
    const uint16_t c =
        scale565(((i >> 1) & 1) ? CYAN : MAGENTA, 30 + (i * 170) / TRAIL_N);
    addSpan(row, max(tx - s, xa), min(tx + s + 1, xb), c);
  }
}

void drawParticlesRow(uint16_t *row, int ry, int xa, int xb) {
  for (int i = 0; i < partCount; i++) {
    const Particle &p = parts[i];
    const int py = (int)p.y;
    if (ry < py || ry >= py + p.size) continue;
    const int px = (int)p.x + shakeX;
    const int k = (int)(256 * p.life / p.maxLife);
    addSpan(row, max(px, xa), min(px + p.size, xb), scale565(p.col, k));
  }
}

void renderRow(uint16_t *row, int y, int xa, int xb) {
  const int ry = constrain(y - shakeY, 0, H - 1);

  if (ry < GROUND_Y) {
    fillSpan(row, xa, xb, skyLUT[ry]);
    const int sx = starX[ry];
    if (sx >= xa && sx < xb && (hash3(ry, nowMs >> 8, 9) & 7) != 0)
      row[sx] = (ry & 3) ? rgb(150, 140, 200) : rgb(240, 240, 255);
    const int scx = SUN_CX + shakeX;
    if (halo1Half[ry])
      fillSpan(row, max(scx - halo1Half[ry], xa), min(scx + halo1Half[ry], xb),
               halo1Col[ry]);
    if (halo2Half[ry])
      fillSpan(row, max(scx - halo2Half[ry], xa), min(scx + halo2Half[ry], xb),
               halo2Col[ry]);
    if (sunHalf[ry])
      fillSpan(row, max(scx - sunHalf[ry], xa), min(scx + sunHalf[ry], xb),
               sunCol[ry]);
    drawLayerRow(farLayer, row, ry, xa, xb,
                 pmod((int)(scroll * 0.22) - shakeX, farLayer.period));
    drawLayerRow(nearLayer, row, ry, xa, xb,
                 pmod((int)(scroll * 0.5) - shakeX, nearLayer.period));
    if (ry >= GROUND_Y - 5)
      addSpan(row, xa, xb, scale565(MAGENTA, (ry - (GROUND_Y - 6)) * 16));
  } else {
    drawGroundRow(row, ry, xa, xb);
  }

  drawPipesRow(row, ry, xa, xb);
  drawTrailRow(row, ry, xa, xb);
  drawBirdRow(row, ry, xa, xb);
  drawParticlesRow(row, ry, xa, xb);

  // Glitch: shift and tint random horizontal bands.
  if (glitchT > 0) {
    uint32_t h = hash3(ry >> 3, frameNo, 77);
    uint32_t mask = glitchI > 0.7f ? 3 : 15;
    if ((h & mask) == 0) {
      int sh = (int)(((int)((h >> 4) % 41) - 20) * glitchI);
      int n = xb - xa - abs(sh);
      if (sh > 0 && n > 0)
        memmove(row + xa + sh, row + xa, n * 2);
      else if (sh < 0 && n > 0)
        memmove(row + xa, row + xa - sh, n * 2);
      addSpan(row, xa, xb, scale565((h >> 12) & 1 ? CYAN : MAGENTA, 48));
    }
  }

  if (flashAmt > 0) addSpan(row, xa, xb, scale565(WHITE, (int)(flashAmt * 220)));

  // Scanlines: darken odd rows by 1/8 per channel.
  if (y & 1)
    for (int x = xa; x < xb; x++) row[x] -= (row[x] >> 3) & 0x18E3;
}

void render(lv_draw_ctx_t *ctx) {
  const lv_area_t *clip = ctx->clip_area;
  const lv_area_t *ba = ctx->buf_area;
  const int bw = lv_area_get_width(ba);
  uint16_t *buf = (uint16_t *)ctx->buf;
  for (int y = clip->y1; y <= clip->y2; y++) {
    uint16_t *row = buf + (y - ba->y1) * bw - ba->x1; // index by screen x
    renderRow(row, y, clip->x1, clip->x2 + 1);
  }
}

/* ------------------------------------------------------------ game logic -- */
void emit(float x, float y, int n, float spd, uint16_t c0, uint16_t c1,
          float life) {
  for (int i = 0; i < n && partCount < MAX_PARTICLES; i++) {
    Particle &p = parts[partCount++];
    float a = frand(0, 6.2832f), s = frand(0.3f, 1.0f) * spd;
    p.x = x;
    p.y = y;
    p.vx = cosf(a) * s;
    p.vy = sinf(a) * s;
    p.maxLife = p.life = frand(0.5f, 1.0f) * life;
    p.col = (i & 1) ? c0 : c1;
    p.size = 2 + esp_random() % 3;
  }
}

void spawnPipe(float x) {
  if (pipeCount >= MAX_PIPES) return;
  Pipe &p = pipes[pipeCount++];
  p.x = x;
  p.gap = (int16_t)max(128, 168 - score * 2);
  int lo = 40 + p.gap / 2 + CAP_H, hi = GROUND_Y - 36 - p.gap / 2 - CAP_H;
  lo = max(lo, lastGapY - 165);
  hi = min(hi, lastGapY + 165);
  p.gapY = (int16_t)(lo + esp_random() % (uint32_t)(hi - lo + 1));
  lastGapY = p.gapY;
  p.scored = false;
  static uint8_t hueToggle = 0;
  p.hue = hueToggle ^= 1;
}

void resetGame() {
  state = READY;
  birdY = 200;
  birdVy = 0;
  pipeCount = 0;
  partCount = 0;
  trailCount = 0;
  score = 0;
  newBest = false;
  speed = 155;
  lastGapY = 220;
}

void flap() {
  birdVy = FLAP_V;
  wingPhase = -1.57f;
  emit(BIRD_X - 10, birdY + 6, 6, 160, CYAN, MAGENTA, 0.35f);
}

void die() {
  state = DEAD;
  deadTime = 0;
  if (birdVy > -150) birdVy = -260; // knock-back hop
  emit(BIRD_X, birdY, 44, 420, MAGENTA, CYAN, 0.9f);
  emit(BIRD_X, birdY, 14, 200, YELLOW, WHITE, 0.6f);
  flashAmt = 1.0f;
  shakeAmt = 12;
  glitchT = 0.5f;
  glitchI = 1.0f;
  if (score > best) {
    best = score;
    newBest = true;
    prefs.putUInt("best", best);
  }
}

bool circleRect(float cx, float cy, float r, float x0, float y0, float x1,
                float y1) {
  float nx = constrain(cx, x0, x1), ny = constrain(cy, y0, y1);
  float dx = cx - nx, dy = cy - ny;
  return dx * dx + dy * dy < r * r;
}

bool hitsPipe(const Pipe &p) {
  const float gapTop = p.gapY - p.gap / 2, gapBot = p.gapY + p.gap / 2;
  const float x = p.x;
  return circleRect(BIRD_X, birdY, BIRD_R, x, -1000, x + PIPE_W, gapTop) ||
         circleRect(BIRD_X, birdY, BIRD_R, x - CAP_X, gapTop - CAP_H,
                    x + PIPE_W + CAP_X, gapTop) ||
         circleRect(BIRD_X, birdY, BIRD_R, x, gapBot, x + PIPE_W, 2000) ||
         circleRect(BIRD_X, birdY, BIRD_R, x - CAP_X, gapBot,
                    x + PIPE_W + CAP_X, gapBot + CAP_H);
}

void update(float dt) {
  tGlobal += dt;
  frameNo++;

  float worldSpeed = 0;
  if (state == READY) {
    worldSpeed = 120;
    birdY = 200 + sinf(tGlobal * 3.0f) * 12;
    birdVy = 0;
    wingPhase += dt * 9;
  } else if (state == PLAYING) {
    speed = min(232.0f, 155.0f + score * 3.0f);
    worldSpeed = speed;
    birdVy = min(birdVy + GRAVITY * dt, MAX_FALL);
    birdY += birdVy * dt;
    wingPhase += dt * (birdVy < 0 ? 24 : 7);
    if (birdY < BIRD_R) {
      birdY = BIRD_R;
      birdVy = 0;
    }

    for (int i = 0; i < pipeCount; i++) {
      Pipe &p = pipes[i];
      p.x -= speed * dt;
      if (!p.scored && p.x + PIPE_W / 2 < BIRD_X) {
        p.scored = true;
        score++;
        emit(BIRD_X + 4, birdY, 10, 220, YELLOW, CYAN, 0.4f);
      }
    }
    int j = 0;
    for (int i = 0; i < pipeCount; i++)
      if (pipes[i].x + PIPE_W + CAP_X > -10) pipes[j++] = pipes[i];
    pipeCount = j;
    float rightmost = pipeCount ? pipes[pipeCount - 1].x : -1e9f;
    if (pipeCount == 0)
      spawnPipe(W + 100);
    else if (rightmost <= W + 30 - PIPE_SPACING)
      spawnPipe(rightmost + PIPE_SPACING);

    for (int i = 0; i < pipeCount; i++)
      if (hitsPipe(pipes[i])) {
        die();
        break;
      }
    if (state == PLAYING && birdY + BIRD_R >= GROUND_Y) {
      birdY = GROUND_Y - BIRD_R;
      die();
    }
  } else { // DEAD: bird drops to the floor, world frozen
    deadTime += dt;
    if (birdY < GROUND_Y - BIRD_R) {
      birdVy = min(birdVy + GRAVITY * dt, MAX_FALL);
      birdY += birdVy * dt;
      if (birdY >= GROUND_Y - BIRD_R) {
        birdY = GROUND_Y - BIRD_R;
        shakeAmt = max(shakeAmt, 5.0f);
      }
    }
  }
  scroll += worldSpeed * dt;

  // Bird orientation.
  float target = state == READY ? 0.0f : constrain(birdVy * 0.0016f, -0.45f, 1.3f);
  birdAngle += (target - birdAngle) * min(1.0f, dt * 14);
  birdCos = cosf(birdAngle);
  birdSin = sinf(birdAngle);

  // Trail: points drift left with the world.
  for (int i = 0; i < trailCount; i++) trail[i].x -= worldSpeed * dt;
  trailTimer += dt;
  if (trailTimer >= 0.03f) {
    trailTimer = 0;
    if (state != DEAD) {
      if (trailCount == TRAIL_N) {
        memmove(trail, trail + 1, sizeof(TrailPt) * (TRAIL_N - 1));
        trailCount--;
      }
      trail[trailCount++] = {BIRD_X - 14.0f, birdY};
    } else if (trailCount > 0) {
      memmove(trail, trail + 1, sizeof(TrailPt) * (trailCount - 1));
      trailCount--;
    }
  }

  // Particles.
  int j = 0;
  for (int i = 0; i < partCount; i++) {
    Particle &p = parts[i];
    p.life -= dt;
    if (p.life <= 0) continue;
    p.vy += 600 * dt;
    p.x += (p.vx - worldSpeed) * dt;
    p.y += p.vy * dt;
    parts[j++] = p;
  }
  partCount = j;

  // Screen effects.
  flashAmt = max(0.0f, flashAmt - dt * 5);
  shakeAmt = max(0.0f, shakeAmt - dt * 30);
  shakeX = shakeAmt > 0.5f ? (int)frand(-shakeAmt, shakeAmt) : 0;
  shakeY = shakeAmt > 0.5f ? (int)frand(-shakeAmt, shakeAmt) : 0;
  glitchT -= dt;
  nextAmbientGlitch -= dt;
  if (nextAmbientGlitch <= 0) {
    nextAmbientGlitch = frand(4, 10);
    if (glitchT <= 0) {
      glitchT = frand(0.06f, 0.16f);
      glitchI = 0.4f;
    }
  }
}

/* -------------------------------------------------------------------- UI -- */
NeonText mkNeon(lv_obj_t *parent, const lv_font_t *font, uint32_t mainHex,
                uint32_t shadowHex, int letterSpace) {
  NeonText t;
  t.shadow = lv_label_create(parent);
  t.main = lv_label_create(parent);
  lv_obj_t *both[2] = {t.shadow, t.main};
  for (lv_obj_t *o : both) {
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_letter_space(o, letterSpace, 0);
  }
  lv_obj_set_style_text_color(t.shadow, lv_color_hex(shadowHex), 0);
  lv_obj_set_style_text_color(t.main, lv_color_hex(mainHex), 0);
  return t;
}
void neonText(NeonText &t, const char *s) {
  lv_label_set_text(t.shadow, s);
  lv_label_set_text(t.main, s);
}
void neonAlign(NeonText &t, lv_align_t a, int x, int y) {
  lv_obj_align(t.shadow, a, x + 3, y + 3);
  lv_obj_align(t.main, a, x, y);
}
void neonShow(NeonText &t, bool show) {
  if (show) {
    lv_obj_clear_flag(t.shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(t.main, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(t.shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(t.main, LV_OBJ_FLAG_HIDDEN);
  }
}
lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t hex,
                  int letterSpace) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
  lv_obj_set_style_text_letter_space(l, letterSpace, 0);
  return l;
}
inline void show(lv_obj_t *o, bool s) {
  if (s)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void onTap() {
  switch (state) {
  case READY:
    state = PLAYING;
    flap();
    break;
  case PLAYING:
    flap();
    break;
  case DEAD:
    if (deadTime > 0.8f) resetGame();
    break;
  }
}

void gameEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_COVER_CHECK) {
    // We paint every pixel, so LVGL can skip drawing the screen behind us.
    auto *info = (lv_cover_check_info_t *)lv_event_get_param(e);
    info->res = LV_COVER_RES_COVER;
  } else if (code == LV_EVENT_DRAW_MAIN) {
    render(lv_event_get_draw_ctx(e));
  } else if (code == LV_EVENT_PRESSED) {
    onTap();
  }
}

void updateUI() {
  static int shownScore = -1, shownBest = -1;
  static int lastState = -1;
  char buf[32];

  if (score != shownScore) {
    shownScore = score;
    snprintf(buf, sizeof buf, "%d", score);
    neonText(scoreTxt, buf);
    neonAlign(scoreTxt, LV_ALIGN_TOP_MID, 0, 22);
  }
  if (best != shownBest) {
    shownBest = best;
    snprintf(buf, sizeof buf, "HI-SCORE  %d", best);
    lv_label_set_text(bestReadyLbl, buf);
  }

  if (state != lastState) {
    lastState = state;
    neonShow(titleTxt, state == READY);
    show(subtitleLbl, state == READY);
    show(bestReadyLbl, state == READY);
    neonShow(scoreTxt, state == PLAYING);
    show(panel, false);
  }

  if (state == READY) {
    show(subtitleLbl, fmodf(tGlobal, 1.0f) < 0.65f);
  } else if (state == DEAD) {
    if (deadTime > 0.5f && lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) {
      snprintf(buf, sizeof buf, "SCORE   %d", score);
      lv_label_set_text(panelScoreLbl, buf);
      snprintf(buf, sizeof buf, "BEST    %d", best);
      lv_label_set_text(panelBestLbl, buf);
      show(panelNewLbl, newBest);
      show(panel, true);
    }
    show(panelTapLbl, deadTime > 0.8f && fmodf(tGlobal, 1.0f) < 0.65f);
    if (newBest) show(panelNewLbl, fmodf(tGlobal, 0.4f) < 0.25f);
  }
}

void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  gameObj = lv_obj_create(scr);
  lv_obj_remove_style_all(gameObj);
  lv_obj_set_size(gameObj, W, H);
  lv_obj_set_pos(gameObj, 0, 0);
  lv_obj_add_flag(gameObj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(gameObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(gameObj, gameEvent, LV_EVENT_ALL, NULL);

  scoreTxt = mkNeon(scr, &lv_font_montserrat_48, 0xFFFFFF, 0xFF2A9D, 2);

  titleTxt = mkNeon(scr, &lv_font_montserrat_48, 0x00F0FF, 0xFF2A9D, 6);
  neonText(titleTxt, "NEON FLAP");
  neonAlign(titleTxt, LV_ALIGN_TOP_MID, 0, 70);

  subtitleLbl = mkLabel(scr, &lv_font_montserrat_20, 0xFFD319, 5);
  lv_label_set_text(subtitleLbl, "TAP TO JACK IN");
  lv_obj_align(subtitleLbl, LV_ALIGN_TOP_MID, 0, 140);

  bestReadyLbl = mkLabel(scr, &lv_font_montserrat_16, 0xFF2A9D, 3);
  lv_obj_align(bestReadyLbl, LV_ALIGN_BOTTOM_MID, 0, -14);

  panel = lv_obj_create(scr);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 330, 220);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x07001A), 0);
  lv_obj_set_style_bg_opa(panel, 190, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x00F0FF), 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_outline_color(panel, lv_color_hex(0xFF2A9D), 0);
  lv_obj_set_style_outline_width(panel, 2, 0);
  lv_obj_set_style_outline_pad(panel, 4, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      panel, [](lv_event_t *) { onTap(); }, LV_EVENT_PRESSED, NULL);

  lv_obj_t *lost = mkLabel(panel, &lv_font_montserrat_28, 0xFF2A9D, 4);
  lv_label_set_text(lost, "SIGNAL LOST");
  lv_obj_align(lost, LV_ALIGN_TOP_MID, 0, 18);

  panelScoreLbl = mkLabel(panel, &lv_font_montserrat_28, 0x00F0FF, 2);
  lv_obj_align(panelScoreLbl, LV_ALIGN_TOP_MID, 0, 70);
  panelBestLbl = mkLabel(panel, &lv_font_montserrat_28, 0xFFD319, 2);
  lv_obj_align(panelBestLbl, LV_ALIGN_TOP_MID, 0, 110);
  panelNewLbl = mkLabel(panel, &lv_font_montserrat_14, 0x39FF14, 3);
  lv_label_set_text(panelNewLbl, "NEW HIGH SCORE");
  lv_obj_align(panelNewLbl, LV_ALIGN_TOP_MID, 0, 148);
  panelTapLbl = mkLabel(panel, &lv_font_montserrat_16, 0xFFFFFF, 4);
  lv_label_set_text(panelTapLbl, "TAP TO REBOOT");
  lv_obj_align(panelTapLbl, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void tick(lv_timer_t *) {
  const uint32_t now = millis();
  static uint32_t last = now;
  float dt = (now - last) / 1000.0f;
  last = now;
  nowMs = now;
  if (dt > 0.05f) dt = 0.05f; // keep physics stable after hiccups
  update(dt);
  updateUI();
  lv_obj_invalidate(gameObj);
}

void buildPalettes() {
  const uint16_t bases[2] = {CYAN, MAGENTA};
  for (int i = 0; i < 2; i++) {
    const uint16_t b = bases[i];
    PipePal &P = pipePal[i];
    P.edge = mix565(b, WHITE, 110);
    P.mid = b;
    P.inner = scale565(b, 130);
    P.fill = scale565(b, 26);
    P.hatch = scale565(b, 70);
    P.capFill = scale565(b, 58);
    P.glow1 = scale565(b, 110);
    P.glow2 = scale565(b, 45);
  }
}

} // namespace

void neon_flap_start() {
  prefs.begin("neonflap", false);
  best = prefs.getUInt("best", 0);
  buildPalettes();
  buildBackground();
  resetGame();
  buildUI();
  updateUI();
  lv_timer_create(tick, 15, NULL);
}
