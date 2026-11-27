/*******************************************************************************
 * PHYSARUM - a living slime mould for the ESP32-4848S040 (480x480 + GT911).
 *
 * Tens of thousands of agents of two rival species crawl over a chemical trail
 * map. Every step each agent sniffs the trail at three sensors ahead of it
 * (left, centre, right), turns toward the strongest scent, steps forward and
 * deposits scent of its own; then the whole map diffuses (3x3 blur) and
 * evaporates. That is Jeff Jones' 2010 model of Physarum polycephalum - the
 * brainless slime mould that famously rebuilt the Tokyo rail network between
 * oat flakes. The agents self-organise into glowing vein networks that never
 * stop rewiring themselves.
 *
 *   touch           food: the organism streams to your fingers (up to five)
 *                   and grows a network between them; dragging leaves crumbs
 *   double tap      next colour theme
 *   leave it alone  it drifts between behaviours, sheds spore bursts, hunts
 *                   phantom food... and now and then it writes you a message.
 *
 * Threads / memory:
 *   sim task (core 0)  agents + diffusion on a 256x256 torus (power-of-two so
 *                      every wrap is a mask); a helper on core 1 steals agent
 *                      chunks whenever LVGL is idle.
 *   LVGL (core 1)      DRAW_MAIN renders the centre 240x240 of the map straight
 *                      into LVGL's strip buffer: exact 2x bilinear upscale of
 *                      both species at once (SWAR), 4x4 ordered dither, 2-D
 *                      colour LUT (A x B -> RGB565). A frame and the in-place
 *                      diffuse never overlap.
 *   Everything hot lives in internal SRAM: PSRAM bandwidth belongs to the LCD,
 *   which streams the frame buffer from it nonstop (starve it -> flicker).
 ******************************************************************************/
#include "physarum.h"
#include "touch_points.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>

extern volatile uint32_t g_flush_us; // main.cpp: CPU time spent copying to the panel

namespace {

/* ---------------------------------------------------------------- sizes -- */
constexpr int W = 480, H = 480;
constexpr int SW = 256, SH = 256, NCELL = SW * SH; // trail map torus
constexpr int OFF = 8, VIS = 240;    // visible window: cells 8..247, shown 2x
constexpr int SUB = 16;              // agent position units per cell
constexpr int PMASK = SW * SUB - 1;  // 4095: positions wrap with a mask
constexpr int FW = SW / 2, FH = SH / 2; // food field, one sample per 2x2 cells
constexpr int TMAX = 251;            // trail ceiling: keeps 16x sums < 4096
#ifndef PHYSARUM_AGENTS
#define PHYSARUM_AGENTS 9000
#endif
constexpr int AGENTS_PER_SPECIES = PHYSARUM_AGENTS;
constexpr int MAX_FOOD = 24;
constexpr int MAX_TEXT_CELLS = 16000;
constexpr int LUT_N = 64, LUT_SIZE = LUT_N * LUT_N; // colour LUT, A x B
constexpr uint32_t STEP_US = 1000000 / 60;          // cap: 60 sim steps/s
constexpr float TAU = 6.2831853f;

/* -------------------------------------------------------------- helpers -- */
inline uint32_t xs32(uint32_t &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
uint32_t uiRng = 0x9E3779B9u;
inline float frand(float lo, float hi) {
  return lo + (hi - lo) * ((xs32(uiRng) >> 8) * (1.0f / 16777216.0f));
}
inline int iround(float v) { return (int)(v + (v >= 0 ? 0.5f : -0.5f)); }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smooth(float t) {
  t = clamp01(t);
  return t * t * (3 - 2 * t);
}
inline uint16_t rgb565f(float r, float g, float b) {
  return (uint16_t)((iround(clamp01(r) * 31) << 11) |
                    (iround(clamp01(g) * 63) << 5) | iround(clamp01(b) * 31));
}
inline uint16_t mix565(uint16_t a, uint16_t b, int t) { // t: 0..256 toward b
  const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  const int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + (((br - ar) * t) >> 8)) << 11) |
                    ((ag + (((bg - ag) * t) >> 8)) << 5) |
                    (ab + (((bb - ab) * t) >> 8)));
}
/* Unpack a trail cell (species A in the low byte, B in the high byte) into
 * two 16-bit lanes so both species are summed/blended in one 32-bit op. */
inline uint32_t wide(uint32_t c) { return (c & 0xFFu) | ((c & 0xFF00u) << 8); }

float cosT[256], sinT[256]; // heading (1/256 turn) -> unit vector

/* ------------------------------------------------------------ behaviour -- */
struct Params {
  float sa;    // sensor angle (degrees either side of the heading)
  float ra;    // rotation per step when turning (degrees)
  float so;    // sensor offset (cells ahead)
  float ss;    // step size (cells per step)
  float dep;   // scent deposited per step (0..251)
  float decay; // fraction of the trail that survives each step
  float repel; // how strongly the rival species' scent is avoided
};
struct Preset {
  const char *name;
  Params p;
};
const Preset PRESETS[] = {
    {"VEINS", {22.5f, 45.0f, 9.0f, 1.0f, 28.0f, 0.90f, 0.70f}},
    {"LABYRINTH", {45.0f, 30.0f, 5.5f, 1.0f, 24.0f, 0.88f, 0.85f}},
    {"CELLS", {60.0f, 20.0f, 15.0f, 1.3f, 24.0f, 0.92f, 1.00f}},
    {"CURRENTS", {12.0f, 8.0f, 20.0f, 1.6f, 20.0f, 0.93f, 0.55f}},
};
constexpr int N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

Params lerpParams(const Params &a, const Params &b, float t) {
  auto L = [t](float x, float y) { return x + (y - x) * t; };
  return {L(a.sa, b.sa),   L(a.ra, b.ra),       L(a.so, b.so),
          L(a.ss, b.ss),   L(a.dep, b.dep),     L(a.decay, b.decay),
          L(a.repel, b.repel)};
}

/* Everything the agent loop needs to know about one heading, in one place so
 * the loop keeps a single table pointer live (Xtensa has few registers). */
struct Heading {
  int32_t c, l, r; // centre / left / right sensor as a cell-index delta
  int16_t dx, dy;  // one step, in position units
};

/* Integer tables the agent loop uses, rebuilt from Params every step. */
struct Derived {
  Heading h[256];
  int ra, dep, repel, foodGain;
  uint32_t decay88; // decay / 9 * 256, in 8.8 fixed point (3x3 blur folded in)
};

void derive(const Params &p, float breath, Derived &d) {
  const float so = p.so * breath, ss = p.ss * SUB;
  const int sa = max(1, iround(p.sa * (256.0f / 360.0f)));
  int32_t sen[256];
  for (int i = 0; i < 256; i++) sen[i] = iround(sinT[i] * so) * SW + iround(cosT[i] * so);
  for (int i = 0; i < 256; i++) {
    Heading &h = d.h[i];
    h.c = sen[i];
    h.l = sen[(i - sa) & 255];
    h.r = sen[(i + sa) & 255];
    h.dx = (int16_t)iround(cosT[i] * ss);
    h.dy = (int16_t)iround(sinT[i] * ss);
  }
  d.ra = max(1, iround(p.ra * (256.0f / 360.0f)));
  d.dep = constrain(iround(p.dep), 1, TMAX);
  d.repel = iround(p.repel * 256.0f);
  d.foodGain = 200;
  d.decay88 = (uint32_t)(constrain(p.decay, 0.5f, 0.995f) * (256.0f / 9.0f) * 256.0f);
}

/* --------------------------------------------------------------- colour -- */
struct RGBf {
  float r, g, b;
};
struct Theme {
  const char *name;
  RGBf bg, a, b, both; // background, species A, species B, where they meet
};
const Theme THEMES[] = {
    {"BIOLUMINESCENCE", {0.000f, 0.010f, 0.030f}, {0.05f, 0.80f, 1.00f},
     {1.00f, 0.12f, 0.60f}, {0.80f, 0.70f, 1.00f}},
    {"POLYCEPHALUM", {0.030f, 0.018f, 0.006f}, {1.00f, 0.80f, 0.10f},
     {1.00f, 0.42f, 0.02f}, {1.00f, 1.00f, 0.70f}},
    {"EMBER", {0.020f, 0.000f, 0.004f}, {1.00f, 0.40f, 0.04f},
     {0.90f, 0.03f, 0.18f}, {1.00f, 0.90f, 0.50f}},
    {"TOXIC", {0.000f, 0.016f, 0.010f}, {0.40f, 1.00f, 0.10f},
     {0.60f, 0.20f, 1.00f}, {0.90f, 1.00f, 0.90f}},
    {"GLACIER", {0.000f, 0.008f, 0.024f}, {0.60f, 0.90f, 1.00f},
     {0.15f, 0.30f, 1.00f}, {1.00f, 1.00f, 1.00f}},
    {"SYNTHWAVE", {0.016f, 0.000f, 0.036f}, {1.00f, 0.82f, 0.12f},
     {1.00f, 0.16f, 0.62f}, {1.00f, 0.95f, 0.95f}},
};
constexpr int N_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);

/* Emission model: each species glows in its colour, overlap adds a third
 * colour, dense vein cores run white-hot; 1-exp(-e) tone-maps it all. */
uint16_t shade(const Theme &t, float a, float b) {
  const float fa = powf(a, 0.75f), fb = powf(b, 0.75f);
  const float both = fa * fb;
  const float hot = fmaxf(0.0f, fmaxf(fa, fb) - 0.6f);
  const float *A = &t.a.r, *B = &t.b.r, *M = &t.both.r, *G = &t.bg.r;
  float o[3];
  for (int c = 0; c < 3; c++) {
    const float e = 2.4f * (A[c] * fa + B[c] * fb) + 3.0f * M[c] * both +
                    6.0f * hot * hot;
    o[c] = G[c] + (1.0f - G[c]) * (1.0f - expf(-e));
  }
  return rgb565f(o[0], o[1], o[2]);
}

/* ------------------------------------------------ shared sim/UI state -- */
struct Food {
  float x, y, amount, radius; // sim cells; amount 0..255
};
struct Burst {
  float x, y, radius;
  int count;
  uint8_t species;
};
struct SimInput { // written by the UI under simMux, copied by the sim
  Params params;
  float breath; // slow multiplier on the sensor offset: the network breathes
  Food food[MAX_FOOD];
  int nFood;
  int16_t touchX[TOUCH_MAX_POINTS], touchY[TOUCH_MAX_POINTS]; // sim cells
  int nTouch;
  float textAmt; // 0..1: how hard the organism is pulled toward the text
  Burst burst[6];
  int nBurst;
  bool reseed;
};
SimInput simIn;
portMUX_TYPE simMux = portMUX_INITIALIZER_UNLOCKED;

uint16_t *trail;   // SRAM: per cell, species A low byte, species B high byte
uint8_t *food;     // SRAM: food attraction, FW x FH
uint32_t *agentsOf[2]; // SRAM, per species: x:12 | y:12 | heading:8
int nAgents[2] = {AGENTS_PER_SPECIES, AGENTS_PER_SPECIES};
uint16_t *lut;       // SRAM: active colour LUT
uint16_t *themeLuts; // PSRAM: every theme's LUT
uint16_t *lutFrom;   // PSRAM: LUT snapshot when a crossfade starts
uint8_t *textMask;   // PSRAM: FW x FH coarse pull toward the message
uint16_t *textCells; // PSRAM: trail cells under the message's strokes
volatile int nTextCells = 0;

// Running totals (sim task writes, UI reads deltas for the log line).
volatile uint32_t statSteps, statAgentUs, statDiffuseUs, statPubUs, statPubs;
uint32_t statFrames, statRenderUs;

/* ------------------------------------------------------------ the agents -- */
template <int S, bool FOOD>
inline __attribute__((always_inline)) int sense(const uint16_t *T,
                                                const uint8_t *F, uint32_t idx,
                                                int repel, int foodGain) {
  idx &= NCELL - 1; // the torus wraps for free (x overflow lands in the
                    // hidden border band, one row off - harmless)
  const uint32_t v = T[idx];
  const int own = S ? (int)(v >> 8) : (int)(v & 0xFF);
  const int rival = S ? (int)(v & 0xFF) : (int)(v >> 8);
  int f = (own << 8) - rival * repel;
  if (FOOD) f += F[((idx >> 2) & 0x3F80) | ((idx >> 1) & 0x7F)] * foodGain;
  return f;
}

template <int S, bool FOOD>
inline __attribute__((always_inline)) void
stepAgentsT(uint32_t *ag, int n, uint16_t *T, const uint8_t *F,
            const Derived &d, uint32_t &rng) {
  const Heading *H = d.h;
  uint32_t r = rng;
  for (int i = 0; i < n; i++) {
    const uint32_t p = ag[i];
    int a = p >> 24;
    // x = bits 0..11, y = bits 12..23; the cell is their top 8 bits each
    const uint32_t c = (((p >> 16) & 0xFF) << 8) | ((p >> 4) & 0xFF);
    const Heading &h = H[a];
    const int fc = sense<S, FOOD>(T, F, c + h.c, d.repel, d.foodGain);
    const int fl = sense<S, FOOD>(T, F, c + h.l, d.repel, d.foodGain);
    const int fr = sense<S, FOOD>(T, F, c + h.r, d.repel, d.foodGain);
    if (fc > fl && fc > fr) {
      // straight on
    } else if (fc < fl && fc < fr) {
      a += (xs32(r) & 1) ? d.ra : -d.ra; // both sides better: pick at random
    } else if (fl < fr) {
      a += d.ra;
    } else if (fr < fl) {
      a -= d.ra;
    } else { // nothing to smell: wander a little
      const uint32_t q = xs32(r);
      a += (int)((q >> 3) & 3) - (int)((q >> 5) & 3);
    }
    a &= 255;
    const Heading &h2 = H[a];
    const uint32_t x = (p + h2.dx) & PMASK;
    const uint32_t y = ((p >> 12) + h2.dy) & PMASK;
    const int dep = d.dep;
    uint16_t &cell = T[((y >> 4) << 8) | (x >> 4)];
    if (S) {
      const int hi = min((int)(cell >> 8) + dep, TMAX);
      cell = (uint16_t)((cell & 0xFF) | (hi << 8));
    } else {
      const int lo = min((int)(cell & 0xFF) + dep, TMAX);
      cell = (uint16_t)((cell & 0xFF00) | lo);
    }
    ag[i] = x | (y << 12) | ((uint32_t)a << 24);
  }
  rng = r;
}

__attribute__((noinline)) void IRAM_ATTR stepSpecies(int s, bool withFood,
                                                     uint32_t *ag, int n,
                                                     const Derived &d,
                                                     uint32_t &rng) {
  if (s == 0) {
    if (withFood) stepAgentsT<0, true>(ag, n, trail, food, d, rng);
    else stepAgentsT<0, false>(ag, n, trail, food, d, rng);
  } else {
    if (withFood) stepAgentsT<1, true>(ag, n, trail, food, d, rng);
    else stepAgentsT<1, false>(ag, n, trail, food, d, rng);
  }
}

/* --------------------------------------------- diffusion + evaporation -- */
/* In-place 3x3 box blur on the torus, both species per 32-bit op. Each row's
 * horizontal 3-sum is computed from the original data just before that row
 * is overwritten, so only three line buffers are needed. */
uint32_t hb[3][SW], hbFirst[SW];

inline __attribute__((always_inline)) void hsum(const uint16_t *row, uint32_t *out) {
  uint32_t a = wide(row[SW - 1]), b = wide(row[0]);
  for (int x = 0; x < SW - 1; x++) {
    const uint32_t c = wide(row[x + 1]);
    out[x] = a + b + c;
    a = b;
    b = c;
  }
  out[SW - 1] = a + b + wide(row[0]);
}

/* k = decay/9 * 256 (<= 29): one multiply scales both lanes, since the
 * biggest lane (9 * 251) times 29 still fits 16 bits. */
__attribute__((noinline)) void IRAM_ATTR diffuse(uint32_t k) {
  uint32_t *prev = hb[0], *cur = hb[1], *next = hb[2];
  hsum(trail + (SH - 1) * SW, prev);
  hsum(trail, cur);
  memcpy(hbFirst, cur, sizeof hbFirst);
  for (int y = 0; y < SH; y++) {
    if (y + 1 < SH) hsum(trail + (y + 1) * SW, next);
    else memcpy(next, hbFirst, sizeof hbFirst);
    uint16_t *row = trail + y * SW;
    for (int x = 0; x < SW; x++) {
      const uint32_t t = (prev[x] + cur[x] + next[x]) * k;
      row[x] = (uint16_t)(((t >> 8) & 0xFF) | ((t >> 16) & 0xFF00));
    }
    uint32_t *t = prev;
    prev = cur;
    cur = next;
    next = t;
  }
}

/* ----------------------------------------------------- food and scents -- */
void buildFood(const SimInput &in) {
  if (in.textAmt > 0.01f) {
    const uint32_t k = (uint32_t)(in.textAmt * 256);
    for (int i = 0; i < FW * FH; i++) food[i] = (uint8_t)((textMask[i] * k) >> 8);
  } else {
    memset(food, 0, FW * FH);
  }
  for (int s = 0; s < in.nFood; s++) {
    const Food &f = in.food[s];
    const float cx = f.x * 0.5f, cy = f.y * 0.5f, r = f.radius * 0.5f;
    const int ir = (int)r + 1, icx = (int)cx, icy = (int)cy;
    const float inv = 1.0f / (r * r);
    for (int dy = -ir; dy <= ir; dy++) {
      const float fy = icy + dy + 0.5f - cy;
      uint8_t *row = food + ((icy + dy) & (FH - 1)) * FW;
      for (int dx = -ir; dx <= ir; dx++) {
        const float fx = icx + dx + 0.5f - cx;
        const float q = 1.0f - (fx * fx + fy * fy) * inv;
        if (q <= 0) continue;
        uint8_t &v = row[(icx + dx) & (FW - 1)];
        v = (uint8_t)min(255, v + (int)(f.amount * q * q));
      }
    }
  }
}

inline uint16_t addBoth(uint16_t c, int amt) {
  const int lo = min((int)(c & 0xFF) + amt, TMAX);
  const int hi = min((int)(c >> 8) + amt, TMAX);
  return (uint16_t)(lo | (hi << 8));
}

void depositDisc(int cx, int cy, int r, int amt) { // both species
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy > r * r) continue;
      uint16_t &c = trail[((cy + dy) & (SH - 1)) * SW + ((cx + dx) & (SW - 1))];
      c = addBoth(c, amt);
    }
}

void depositText(float amt) {
  const int a = (int)(amt * 10.0f);
  if (a <= 0) return;
  const int n = nTextCells;
  for (int i = 0; i < n; i++) trail[textCells[i]] = addBoth(trail[textCells[i]], a);
}

/* ------------------------------------------------------------- seeding -- */
inline void setAgent(int i, float x, float y, int heading) { // i: 0 .. nA+nB
  const uint32_t px = iround(x * SUB) & PMASK, py = iround(y * SUB) & PMASK;
  uint32_t *a = i < nAgents[0] ? &agentsOf[0][i] : &agentsOf[1][i - nAgents[0]];
  *a = px | (py << 12) | ((uint32_t)(heading & 255) << 24);
}

/* Species A starts as a tight spore in the centre heading out; species B as a
 * ring at the rim heading in. They meet, recoil, and the network takes over. */
void seedIntro(uint32_t &rng) {
  const float cx = SW * 0.5f, cy = SH * 0.5f;
  for (int i = 0; i < nAgents[0]; i++) {
    const uint32_t r = xs32(rng);
    const int h = r & 255;
    const float rad = 5.0f * sqrtf(((r >> 8) & 0xFFFF) * (1.0f / 65535.0f));
    setAgent(i, cx + cosT[h] * rad, cy + sinT[h] * rad, h);
  }
  for (int i = 0; i < nAgents[1]; i++) {
    const uint32_t r = xs32(rng);
    const int h = r & 255;
    const float rad = 110.0f + 8.0f * (((r >> 8) & 0xFFFF) * (1.0f / 65535.0f));
    setAgent(nAgents[0] + i, cx + cosT[h] * rad, cy + sinT[h] * rad, h + 128);
  }
}

int burstCursor[2];
/* Re-seed `count` agents of one species as an outward-flying spore cloud. */
void applyBurst(const Burst &b, uint32_t &rng) {
  const int s = b.species & 1, base = s ? nAgents[0] : 0, cnt = nAgents[s];
  const int n = min(b.count, cnt);
  for (int k = 0; k < n; k++) {
    const int i = base + burstCursor[s];
    if (++burstCursor[s] >= cnt) burstCursor[s] = 0;
    const uint32_t r = xs32(rng);
    const int h = r & 255;
    const float rad = b.radius * sqrtf(((r >> 8) & 0xFFFF) * (1.0f / 65535.0f));
    setAgent(i, b.x + cosT[h] * rad, b.y + sinT[h] * rad, h);
  }
}

/* ---------------------------------------------------------- work sharing -- */
/* The agents are split into chunks. Core 0 claims chunks every step, and a
 * helper task on core 1 - below LVGL's priority, so it only ever runs while
 * the renderer is idle - claims them too. Nobody waits for the helper's last
 * chunk: an agent that is a step late (or, rarely, stepped twice) is
 * invisible in a stochastic swarm, and every racing write is a whole word. */
constexpr int CHUNK = 512;
Derived simD;               // this step's tables, shared with the helper
volatile int workNext;      // next chunk to claim (atomic add)
volatile int workTotal;     // chunks in this step
volatile bool workFood;
TaskHandle_t helperHandle;

inline int claimChunk() { return __atomic_fetch_add(&workNext, 1, __ATOMIC_RELAXED); }

void runChunk(int idx, uint32_t &rng) {
  const int chunksA = (nAgents[0] + CHUNK - 1) / CHUNK;
  const int s = idx < chunksA ? 0 : 1;
  const int first = (s ? idx - chunksA : idx) * CHUNK;
  stepSpecies(s, workFood, agentsOf[s] + first, min(CHUNK, nAgents[s] - first), simD, rng);
}

volatile uint32_t statHelperChunks;
void helperTask(void *) {
  uint32_t rng = esp_random() | 1;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (int idx; (idx = claimChunk()) < workTotal;) {
      runChunk(idx, rng);
      statHelperChunks++;
    }
  }
}

/* --------------------------------------------------------- the sim task -- */
/* The renderer reads the trail map in SRAM directly - no copies through PSRAM,
 * whose bandwidth belongs to the LCD (starve it and the panel flickers).
 * Agents may keep depositing while a frame is drawn (invisible), but the
 * in-place diffuse would tear it, so a frame and a diffuse never overlap:
 *   frameActive  set by the UI tick when it starts a frame, cleared by the
 *                renderer after the last strip
 *   diffusing    set by the sim around diffuse(); no frame starts meanwhile */
volatile bool frameActive = false, diffusing = false;
portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;

void beginDiffuse() {
  const uint32_t t0 = millis();
  for (;;) {
    portENTER_CRITICAL(&frameMux);
    const bool ok = !frameActive || millis() - t0 > 100; // never deadlock
    if (ok) diffusing = true;
    portEXIT_CRITICAL(&frameMux);
    if (ok) return;
    vTaskDelay(1); // also lets IDLE0 feed the task watchdog
  }
}

void simTask(void *) {
  static SimInput in;
  Derived &d = simD;
  uint32_t rng = esp_random() | 1;
  for (;;) {
    const uint32_t t0 = micros();
    portENTER_CRITICAL(&simMux);
    memcpy(&in, &simIn, sizeof in);
    simIn.nBurst = 0;
    simIn.reseed = false;
    portEXIT_CRITICAL(&simMux);

    if (in.reseed) {
      memset(trail, 0, NCELL * sizeof(uint16_t));
      seedIntro(rng);
    }
    for (int i = 0; i < in.nBurst; i++) applyBurst(in.burst[i], rng);
    derive(in.params, in.breath, d);
    buildFood(in);
    for (int i = 0; i < in.nTouch; i++) depositDisc(in.touchX[i], in.touchY[i], 3, 40);
    if (in.textAmt > 0.02f) depositText(in.textAmt);
    const bool withFood = in.nFood > 0 || in.textAmt > 0.01f;

    const uint32_t t1 = micros();
    workFood = withFood;
    workTotal = (nAgents[0] + CHUNK - 1) / CHUNK + (nAgents[1] + CHUNK - 1) / CHUNK;
    __atomic_store_n(&workNext, 0, __ATOMIC_RELEASE);
    xTaskNotifyGive(helperHandle); // core 1 joins in whenever LVGL is idle
    for (int idx; (idx = claimChunk()) < workTotal;) runChunk(idx, rng);
    const uint32_t t2 = micros();
    beginDiffuse(); // wait for the frame being drawn to finish
    const uint32_t t3 = micros();
    // integer decay factor, dithered over steps so the average is exact
    static uint32_t decayFrac = 0;
    decayFrac += d.decay88 & 0xFF;
    diffuse((d.decay88 >> 8) + (decayFrac >> 8));
    decayFrac &= 0xFF;
    diffusing = false;
    const uint32_t t4 = micros();
    statSteps++;
    statAgentUs += t2 - t1;
    statDiffuseUs += t4 - t3;
    statPubUs += t3 - t2; // time spent waiting for the renderer
    statPubs++;

    const uint32_t el = micros() - t0;
    vTaskDelay(el + 2000 < STEP_US ? (STEP_US - el) / 1000 : 1); // IDLE0 needs a turn
  }
}

/* ------------------------------------------------------------ rendering -- */
/* Unpacked ("wide") copies of the source rows the current output rows need:
 * each visible source row is used by three output rows but unpacked once. */
uint32_t wideRows[3][VIS + 2];
int wideKey[3] = {-1, -1, -1};
uint32_t wideStamp[3], wideClock;
uint16_t rowTmp[W] __attribute__((aligned(4)));
const uint8_t BAYER4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

inline const uint32_t *getWide(int r) {
  for (int s = 0; s < 3; s++)
    if (wideKey[s] == r) {
      wideStamp[s] = ++wideClock;
      return wideRows[s];
    }
  int s = 0;
  for (int k = 1; k < 3; k++)
    if (wideStamp[k] < wideStamp[s]) s = k;
  const uint16_t *src = trail + r * SW + OFF - 1;
  uint32_t *dst = wideRows[s];
  for (int i = 0; i < VIS + 2; i++) dst[i] = wide(src[i]);
  wideKey[s] = r;
  wideStamp[s] = ++wideClock;
  return dst;
}

/* Lanes are 16x the trail value (<= 4016) plus dither (<= 60), so the top six
 * bits of each lane index the 64x64 LUT directly. */
#define LUT_IDX(p) (((p) & 0xFC0u) | ((p) >> 22))

/* One 480-px output row. For an exact 2x upscale with centred pixels the
 * bilinear weights are always 3/4 + 1/4, so it is all adds and shifts. */
__attribute__((noinline)) void IRAM_ATTR renderRow(uint16_t *dst, int y) {
  const int sr = OFF + (y >> 1), sf = (y & 1) ? sr + 1 : sr - 1;
  const uint32_t *Wn = getWide(sr), *Wf = getWide(sf);
  const uint8_t *bay = BAYER4[y & 3];
  const uint32_t d0 = bay[0] * 4u * 0x10001u, d1 = bay[1] * 4u * 0x10001u;
  const uint32_t d2 = bay[2] * 4u * 0x10001u, d3 = bay[3] * 4u * 0x10001u;
  const uint16_t *L = lut;
  uint32_t *out = (uint32_t *)dst;
  uint32_t vp = Wn[0] * 3 + Wf[0], vc = Wn[1] * 3 + Wf[1]; // columns 7, 8
  for (int j = 0; j < VIS; j += 2) { // 4 output pixels per iteration
    const uint32_t vn = Wn[j + 2] * 3 + Wf[j + 2];
    const uint32_t vnn = Wn[j + 3] * 3 + Wf[j + 3];
    const uint32_t m0 = vc * 3, m1 = vn * 3;
    const uint32_t p0 = m0 + vp + d0, p1 = m0 + vn + d1;
    const uint32_t p2 = m1 + vc + d2, p3 = m1 + vnn + d3;
    out[j] = L[LUT_IDX(p0)] | ((uint32_t)L[LUT_IDX(p1)] << 16);
    out[j + 1] = L[LUT_IDX(p2)] | ((uint32_t)L[LUT_IDX(p3)] << 16);
    vp = vn;
    vc = vnn;
  }
}

void render(lv_draw_ctx_t *ctx) {
  const uint32_t t0 = micros();
  const lv_area_t *clip = ctx->clip_area, *ba = ctx->buf_area;
  const int bw = lv_area_get_width(ba);
  uint16_t *buf = (uint16_t *)ctx->buf;
  const bool fullRow = clip->x1 == 0 && clip->x2 == W - 1 && ba->x1 == 0 && bw == W;
  const int y0 = max<int>(clip->y1, 0), y1 = min<int>(clip->y2, H - 1);
  if (y0 == 0) wideKey[0] = wideKey[1] = wideKey[2] = -1; // map moved on
  for (int y = y0; y <= y1; y++) {
    uint16_t *row = buf + (y - ba->y1) * bw - ba->x1; // index by screen x
    if (fullRow) {
      renderRow(row, y);
    } else {
      renderRow(rowTmp, y);
      memcpy(row + clip->x1, rowTmp + clip->x1, (clip->x2 - clip->x1 + 1) * 2);
    }
  }
  if (clip->y1 == 0) statFrames++;
  if (y1 == H - 1) frameActive = false; // last strip: the sim may diffuse
  statRenderUs += micros() - t0;
}

/* ---------------------------------------------------- message in veins -- */
/* Render text with LVGL into a scratch canvas, then turn the glyphs into
 * (a) trail cells under the strokes, fed directly while the message shows,
 * (b) a blurred coarse mask that pulls agents in from further away. */
void blurMask(uint8_t *m, uint16_t *tmp, int radius) {
  const int n = 2 * radius + 1;
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) {
      int s = 0;
      for (int k = -radius; k <= radius; k++) s += m[y * FW + ((x + k) & (FW - 1))];
      tmp[y * FW + x] = s / n;
    }
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) {
      int s = 0;
      for (int k = -radius; k <= radius; k++) s += tmp[((y + k) & (FH - 1)) * FW + x];
      m[y * FW + x] = s / n;
    }
}

bool prepareText(const char *txt, float scale) {
  constexpr int CW = 216, CH = 120;
  lv_color_t *cbuf = (lv_color_t *)heap_caps_malloc(CW * CH * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  uint16_t *tmp = (uint16_t *)heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
  if (!cbuf || !tmp) {
    heap_caps_free(cbuf);
    heap_caps_free(tmp);
    return false;
  }
  lv_obj_t *cv = lv_canvas_create(lv_scr_act());
  lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_buffer(cv, cbuf, CW, CH, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_COVER);
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font = &lv_font_montserrat_48;
  dsc.color = lv_color_white();
  dsc.align = LV_TEXT_ALIGN_CENTER;
  dsc.letter_space = 3;
  dsc.line_space = -6;
  lv_point_t sz;
  lv_txt_get_size(&sz, txt, dsc.font, dsc.letter_space, dsc.line_space, CW, LV_TEXT_FLAG_NONE);
  lv_canvas_draw_text(cv, 0, (CH - sz.y) / 2, CW, &dsc, txt);

  auto px = [&](int x, int y) -> float {
    if (x < 0 || y < 0 || x >= CW || y >= CH) return 0.0f;
    return cbuf[y * CW + x].ch.green * (1.0f / 63.0f);
  };
  auto coverage = [&](float sx, float sy) -> float { // screen px -> 0..1
    const float fx = (sx - W / 2) / scale + CW / 2.0f - 0.5f;
    const float fy = (sy - H / 2) / scale + CH / 2.0f - 0.5f;
    const int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    const float tx = fx - x0, ty = fy - y0;
    return (px(x0, y0) * (1 - tx) + px(x0 + 1, y0) * tx) * (1 - ty) +
           (px(x0, y0 + 1) * (1 - tx) + px(x0 + 1, y0 + 1) * tx) * ty;
  };

  int n = 0; // sim cell (OFF + i, OFF + j) is centred on screen px (2i+1, 2j+1)
  for (int j = 0; j < VIS; j++)
    for (int i = 0; i < VIS; i++)
      if (n < MAX_TEXT_CELLS && coverage(i * 2 + 1.0f, j * 2 + 1.0f) > 0.45f)
        textCells[n++] = (uint16_t)((OFF + j) * SW + OFF + i);
  nTextCells = n;

  for (int fy = 0; fy < FH; fy++) // food sample = 2x2 cells = 4x4 screen px
    for (int fx = 0; fx < FW; fx++)
      textMask[fy * FW + fx] = (uint8_t)(255 * clamp01(
          coverage((fx * 2 + 1 - OFF) * 2.0f, (fy * 2 + 1 - OFF) * 2.0f) * 1.6f));
  blurMask(textMask, tmp, 2);
  blurMask(textMask, tmp, 2);
  int peak = 1;
  for (int i = 0; i < FW * FH; i++) peak = max<int>(peak, textMask[i]);
  for (int i = 0; i < FW * FH; i++) textMask[i] = (uint8_t)min(255, textMask[i] * 400 / peak);

  lv_obj_del(cv);
  heap_caps_free(cbuf);
  heap_caps_free(tmp);
  return true;
}

/* -------------------------------------------------------------- UI state -- */
lv_obj_t *simObj;
struct Fade {
  lv_obj_t *obj = nullptr;
  float v = 0, target = 0, rate = 1;
  int shown = -1;
  uint8_t bgMax = 0;
};
Fade caption, toast;
lv_obj_t *toastLbl;

void fadeUpdate(Fade &f, float dt) {
  if (f.v < f.target) f.v = min(f.target, f.v + dt * f.rate);
  else if (f.v > f.target) f.v = max(f.target, f.v - dt * f.rate);
  const int o = (int)(smooth(f.v) * 255);
  if (o == f.shown) return;
  f.shown = o;
  lv_obj_set_style_text_opa(f.obj, o, 0);
  if (f.bgMax) lv_obj_set_style_bg_opa(f.obj, (o * f.bgMax) >> 8, 0);
  if (o == 0) lv_obj_add_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
}

struct FoodUI { // lingering food: crumbs, released fingers, phantom feasts
  float x, y, amount, radius, life, maxLife;
};
FoodUI foods[MAX_FOOD];
int nFoods = 0;

struct Finger {
  bool active;
  uint8_t id, missing;
  float x, y, x0, y0, crumbX, crumbY;
  uint32_t downMs;
};
Finger fingers[TOUCH_MAX_POINTS];

Burst pending[6];
int nPending = 0;
bool pendingReseed = false;

float tLive = 0, idle = 0, breath = 1;
int themeIdx = 0;
float themeFade = 1;
Params pFrom, pTo, pCur;
float pBlend = 1;
int presetIdx = 0;
float nextPreset = 30, nextEvent = 35, nextTheme = 180, lastFeedMe = -1000;
float lastTapT = -10, lastTapX = 0, lastTapY = 0;

struct TextAnim {
  bool on;
  float t, up, hold, down;
} textAnim;
float textAmt = 0;
bool introText = false, introCaption = false;
float captionHideAt = 0, toastHideAt = 0;

struct Wander {
  bool on;
  float t, dur, ph[2], w[2];
} wander;

/* Screen pixel -> sim cell (the visible window starts at cell OFF). */
inline float toCell(int px) { return OFF + px * 0.5f; }
inline float randCell(float margin) { return frand(OFF + margin, OFF + VIS - margin); }

void addFood(float x, float y, float amount, float radius, float life) {
  if (nFoods == MAX_FOOD) { // drop the one closest to running out
    int k = 0;
    for (int i = 1; i < nFoods; i++)
      if (foods[i].life < foods[k].life) k = i;
    foods[k] = foods[--nFoods];
  }
  foods[nFoods++] = {x, y, amount, radius, life, life};
}

void addBurst(float x, float y, float radius, int count, int species) {
  if (nPending < 6) pending[nPending++] = {x, y, radius, count, (uint8_t)species};
}

void setTheme(int idx, bool instant) {
  themeIdx = (idx % N_THEMES + N_THEMES) % N_THEMES;
  if (instant) {
    memcpy(lut, themeLuts + themeIdx * LUT_SIZE, LUT_SIZE * 2);
    themeFade = 1;
  } else {
    memcpy(lutFrom, lut, LUT_SIZE * 2);
    themeFade = 0;
  }
}

void updateTheme(float dt) {
  if (themeFade >= 1) return;
  themeFade = min(1.0f, themeFade + dt / 1.6f);
  const int t = (int)(smooth(themeFade) * 256);
  const uint16_t *to = themeLuts + themeIdx * LUT_SIZE;
  for (int i = 0; i < LUT_SIZE; i++) lut[i] = mix565(lutFrom[i], to[i], t);
}

void showToast(const char *s) {
  lv_label_set_text(toastLbl, s);
  toast.target = 1;
  toastHideAt = tLive + 1.8f;
}

void startText(const char *s, float scale, float up, float hold, float down) {
  if (textAmt > 0 || textAnim.on || !prepareText(s, scale)) return;
  textAnim = {true, 0, up, hold, down};
}

void selectPreset(int idx) {
  presetIdx = (idx % N_PRESETS + N_PRESETS) % N_PRESETS;
  pFrom = pCur;
  pTo = PRESETS[presetIdx].p;
  pBlend = 0;
}

/* --------------------------------------------------------------- touch -- */
void fingerDown(int slot, uint8_t id, float x, float y, uint32_t now) {
  fingers[slot] = {true, id, 0, x, y, x, y, x, y, now};
  idle = 0;
  addBurst(x, y, 2.5f, 380, 0); // a puff of fresh spores from your fingertip
  addBurst(x, y, 2.5f, 380, 1);
}

void fingerMove(Finger &f, float x, float y) {
  f.x = x;
  f.y = y;
  idle = 0;
  const float dx = x - f.crumbX, dy = y - f.crumbY;
  if (dx * dx + dy * dy > 14 * 14) { // leave crumbs along the stroke
    addFood(x, y, 190, 12, 5.0f);
    f.crumbX = x;
    f.crumbY = y;
  }
}

void fingerUp(Finger &f, uint32_t now) {
  f.active = false;
  addFood(f.x, f.y, 255, 30, 6.0f); // the scent lingers after you let go
  const float dx = f.x - f.x0, dy = f.y - f.y0;
  if (now - f.downMs < 260 && dx * dx + dy * dy < 10 * 10) { // a tap
    const float tx = f.x - lastTapX, ty = f.y - lastTapY;
    if (tLive - lastTapT < 0.45f && tx * tx + ty * ty < 30 * 30) {
      setTheme(themeIdx + 1, false);
      showToast(THEMES[themeIdx].name);
      lastTapT = -10;
    } else {
      lastTapT = tLive;
      lastTapX = f.x;
      lastTapY = f.y;
    }
  }
}

/* Serial-driven "fingers" for testing without touching the glass: up to three
 * fixed points, or one that traces a circle for a few seconds. */
struct Phantom {
  bool on;
  int16_t x, y;
} phantoms[3] = {{false, 130, 150}, {false, 350, 330}, {false, 360, 110}};
float phantomCircleT = -1;

void processTouches(uint32_t now) {
  int16_t px[TOUCH_MAX_POINTS], py[TOUCH_MAX_POINTS];
  uint8_t pid[TOUCH_MAX_POINTS];
  int n = 0;
  for (int i = 0; i < touch_count && n < TOUCH_MAX_POINTS; i++, n++) {
    px[n] = touch_pts_x[i];
    py[n] = touch_pts_y[i];
    pid[n] = touch_pts_id[i];
  }
  for (int k = 0; k < 3 && n < TOUCH_MAX_POINTS; k++)
    if (phantoms[k].on) {
      px[n] = phantoms[k].x;
      py[n] = phantoms[k].y;
      pid[n++] = 200 + k;
    }
  if (phantomCircleT >= 0 && n < TOUCH_MAX_POINTS) {
    if ((phantomCircleT += 0.01f) > 4.0f) phantomCircleT = -1;
    px[n] = 240 + (int)(150 * cosf(phantomCircleT * 1.6f));
    py[n] = 240 + (int)(150 * sinf(phantomCircleT * 1.6f));
    pid[n++] = 210;
  }

  bool seen[TOUCH_MAX_POINTS] = {};
  for (int i = 0; i < n; i++) {
    const uint8_t id = pid[i];
    const float x = toCell(px[i]), y = toCell(py[i]);
    int slot = -1;
    for (int f = 0; f < TOUCH_MAX_POINTS; f++)
      if (fingers[f].active && fingers[f].id == id) slot = f;
    if (slot < 0) {
      for (int f = 0; f < TOUCH_MAX_POINTS && slot < 0; f++)
        if (!fingers[f].active) slot = f;
      if (slot < 0) continue;
      fingerDown(slot, id, x, y, now);
    } else {
      fingerMove(fingers[slot], x, y);
    }
    seen[slot] = true;
    fingers[slot].missing = 0;
  }
  // The GT911 sometimes reports "no touch" between samples: only treat a
  // finger as lifted once it has been missing for a few polls.
  for (int f = 0; f < TOUCH_MAX_POINTS; f++)
    if (fingers[f].active && !seen[f] && ++fingers[f].missing >= 3) fingerUp(fingers[f], now);
}

/* -------------------------------------------------------------- ambient -- */
void spores() {
  const int species = xs32(uiRng) & 1;
  const float x = randCell(30), y = randCell(30);
  addBurst(x, y, 3.0f, nAgents[species] * 35 / 100, species);
  if (frand(0, 1) < 0.4f) // sometimes the rival answers from across the dish
    addBurst(2 * (OFF + VIS / 2) - x, 2 * (OFF + VIS / 2) - y, 3.0f,
             nAgents[1 - species] * 30 / 100, 1 - species);
}

void feast() {
  const int n = 3 + (xs32(uiRng) % 3);
  for (int i = 0; i < n; i++) addFood(randCell(20), randCell(20), 220, 22, 16.0f);
}

void startWander() {
  wander = {true, 0, 22.0f, {frand(0, TAU), frand(0, TAU)}, {frand(0.38f, 0.5f), frand(0.55f, 0.7f)}};
}

void fireEvent() {
  if (idle > 150 && tLive - lastFeedMe > 300) { // it's hungry, and it says so
    lastFeedMe = tLive;
    startText("FEED\nME", 2.0f, 2.0f, 7.0f, 3.5f);
    return;
  }
  const float r = frand(0, 1);
  if (r < 0.38f) spores();
  else if (r < 0.68f) feast();
  else if (r < 0.93f) startWander();
  else startText(frand(0, 1) < 0.5f ? "HELLO" : "ALIVE", 2.2f, 1.8f, 5.5f, 3.0f);
}

void restart() {
  pendingReseed = true;
  tLive = 0;
  idle = 0;
  introText = introCaption = false;
  textAnim.on = false;
  textAmt = 0;
  nFoods = 0;
  wander.on = false;
  nextPreset = 30;
  nextEvent = 35;
  prepareText("HELLO", 2.2f);
}

void story(float dt) {
  tLive += dt;
  idle += dt;
  breath = 1.0f + 0.10f * sinf(TAU * tLive / 6.5f);

  // Intro: the organism spells HELLO, then the caption fades in.
  if (!introText && tLive > 1.4f) {
    introText = true;
    textAnim = {true, 0, 1.6f, 5.5f, 3.0f};
  }
  if (!introCaption && tLive > 11.5f) {
    introCaption = true;
    caption.target = 1;
    captionHideAt = tLive + 6.5f;
  }
  if (caption.target > 0 && tLive > captionHideAt) caption.target = 0;
  if (toast.target > 0 && tLive > toastHideAt) toast.target = 0;

  if (textAnim.on) {
    TextAnim &a = textAnim;
    a.t += dt;
    if (a.t < a.up) textAmt = smooth(a.t / a.up);
    else if (a.t < a.up + a.hold) textAmt = 1;
    else if (a.t < a.up + a.hold + a.down) textAmt = 1 - smooth((a.t - a.up - a.hold) / a.down);
    else {
      a.on = false;
      textAmt = 0;
    }
  }

  // Behaviour drift: glide to another preset every half minute or so.
  if (tLive > nextPreset) {
    selectPreset(presetIdx + 1 + xs32(uiRng) % (N_PRESETS - 1));
    nextPreset = tLive + frand(28, 42);
  }
  if (pBlend < 1) {
    pBlend = min(1.0f, pBlend + dt / 9.0f);
    pCur = lerpParams(pFrom, pTo, smooth(pBlend));
  }

  // Left alone, it entertains itself.
  if (tLive > 14 && idle > 12 && tLive > nextEvent && !textAnim.on) {
    fireEvent();
    nextEvent = tLive + frand(22, 40);
  }
  if (tLive > 14 && idle > 40 && tLive > nextTheme) {
    setTheme(themeIdx + 1 + xs32(uiRng) % (N_THEMES - 1), false);
    nextTheme = tLive + frand(150, 240);
  }
  if (wander.on && (wander.t += dt) > wander.dur) wander.on = false;

  for (int i = 0; i < nFoods;) { // lingering food fades away
    if ((foods[i].life -= dt) <= 0) foods[i] = foods[--nFoods];
    else i++;
  }
}

void pushSimInput() {
  static SimInput s;
  s.params = pCur;
  s.breath = breath;
  int n = 0, nt = 0;
  for (int f = 0; f < TOUCH_MAX_POINTS; f++) {
    if (!fingers[f].active) continue;
    s.food[n++] = {fingers[f].x, fingers[f].y, 255, 30};
    s.touchX[nt] = (int16_t)fingers[f].x;
    s.touchY[nt++] = (int16_t)fingers[f].y;
  }
  if (wander.on) {
    const float e = smooth(wander.t / 2.0f) * (1 - smooth((wander.t - wander.dur + 3) / 3));
    s.food[n++] = {OFF + VIS / 2 + 85 * sinf(wander.ph[0] + wander.w[0] * wander.t),
                   OFF + VIS / 2 + 85 * sinf(wander.ph[1] + wander.w[1] * wander.t),
                   235 * e, 26};
  }
  for (int i = 0; i < nFoods && n < MAX_FOOD; i++) {
    const FoodUI &f = foods[i];
    s.food[n++] = {f.x, f.y, f.amount * smooth(f.life / f.maxLife * 1.5f), f.radius};
  }
  s.nFood = n;
  s.nTouch = nt;
  s.textAmt = textAmt;

  portENTER_CRITICAL(&simMux);
  const int keep = simIn.nBurst; // bursts/reseed the sim hasn't consumed yet
  const bool reseed = simIn.reseed || pendingReseed;
  Burst saved[6];
  memcpy(saved, simIn.burst, sizeof saved);
  memcpy(&simIn, &s, sizeof s);
  memcpy(simIn.burst, saved, sizeof saved);
  simIn.nBurst = keep;
  simIn.reseed = reseed;
  for (int i = 0; i < nPending && simIn.nBurst < 6; i++) simIn.burst[simIn.nBurst++] = pending[i];
  portEXIT_CRITICAL(&simMux);
  nPending = 0;
  pendingReseed = false;
}

void logStats() {
  static uint32_t lastMs, lSteps, lAg, lDi, lPu, lPubs, lFrames, lRender, lFlush, lHelp;
  const uint32_t now = millis();
  if (now - lastMs < 5000) return;
  const float secs = (now - lastMs) / 1000.0f;
  const uint32_t steps = statSteps - lSteps, frames = statFrames - lFrames;
  const uint32_t pubs = statPubs - lPubs;
  auto per = [](uint32_t us, uint32_t n) { return n ? us / 1000.0f / n : 0.0f; };
  const float helpPct = steps ? 100.0f * (statHelperChunks - lHelp) / (steps * (float)workTotal) : 0;
  Serial.printf("[physarum] %.1f fps (render %.2f + flush %.2f ms) | sim %.1f steps/s "
                "(agents %.2f [core1 did %.0f%%], diffuse %.2f, wait %.2f ms) | %s/%s | "
                "SRAM %u (min %u)\n",
                frames / secs, per(statRenderUs - lRender, frames),
                per(g_flush_us - lFlush, frames), steps / secs,
                per(statAgentUs - lAg, steps), helpPct, per(statDiffuseUs - lDi, steps),
                per(statPubUs - lPu, pubs), PRESETS[presetIdx].name,
                THEMES[themeIdx].name, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  lHelp = statHelperChunks;
  lastMs = now;
  lSteps = statSteps;
  lAg = statAgentUs;
  lDi = statDiffuseUs;
  lPu = statPubUs;
  lPubs = statPubs;
  lFrames = statFrames;
  lRender = statRenderUs;
  lFlush = g_flush_us;
}

void tick(lv_timer_t *) {
  const uint32_t now = millis();
  static uint32_t last = now;
  float dt = (now - last) / 1000.0f;
  last = now;
  if (dt > 0.1f) dt = 0.1f;

  processTouches(now);
  story(dt);
  updateTheme(dt);
  pushSimInput();

  // Start a frame once the sim has stepped since the last one and isn't
  // diffusing. Label fades are applied only here too, so nothing is ever
  // redrawn while the map is mid-diffuse.
  static uint32_t renderedStep = 0;
  static float fadeDt = 0;
  fadeDt += dt;
  if (statSteps != renderedStep && !frameActive) {
    portENTER_CRITICAL(&frameMux);
    const bool go = !diffusing;
    if (go) frameActive = true;
    portEXIT_CRITICAL(&frameMux);
    if (go) {
      renderedStep = statSteps;
      fadeUpdate(caption, fadeDt);
      fadeUpdate(toast, fadeDt);
      fadeDt = 0;
      lv_obj_invalidate(simObj);
    }
  }
  logStats();
}

void simEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_COVER_CHECK) {
    auto *info = (lv_cover_check_info_t *)lv_event_get_param(e);
    info->res = LV_COVER_RES_COVER; // we paint every pixel
  } else if (code == LV_EVENT_DRAW_MAIN) {
    render(lv_event_get_draw_ctx(e));
  }
}

lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t hex, int space, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
  lv_obj_set_style_text_letter_space(l, space, 0);
  lv_label_set_text(l, txt);
  return l;
}

void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  simObj = lv_obj_create(scr);
  lv_obj_remove_style_all(simObj);
  lv_obj_set_size(simObj, W, H);
  lv_obj_set_pos(simObj, 0, 0);
  lv_obj_clear_flag(simObj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(simObj, simEvent, LV_EVENT_ALL, NULL);

  lv_obj_t *cap = lv_obj_create(scr);
  lv_obj_remove_style_all(cap);
  lv_obj_set_size(cap, 340, 70);
  lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_radius(cap, 16, 0);
  lv_obj_set_style_bg_color(cap, lv_color_black(), 0);
  lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *t = mkLabel(cap, &lv_font_montserrat_20, 0xF2F7FF, 9, "PHYSARUM");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 4, 10);
  lv_obj_t *s = mkLabel(cap, &lv_font_montserrat_14, 0x9FC4E0, 2,
                        "a brainless slime mould  " LV_SYMBOL_BULLET "  touch to feed");
  lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -11);
  caption.obj = cap;
  caption.rate = 0.8f;
  caption.bgMax = 150;
  fadeUpdate(caption, 0);

  lv_obj_t *tst = lv_obj_create(scr);
  lv_obj_remove_style_all(tst);
  lv_obj_set_size(tst, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(tst, 16, 0);
  lv_obj_set_style_pad_ver(tst, 8, 0);
  lv_obj_set_style_radius(tst, 12, 0);
  lv_obj_set_style_bg_color(tst, lv_color_black(), 0);
  lv_obj_align(tst, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_clear_flag(tst, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  toastLbl = mkLabel(tst, &lv_font_montserrat_16, 0xFFFFFF, 5, "");
  toast.obj = tst;
  toast.rate = 3.0f;
  toast.bgMax = 150;
  fadeUpdate(toast, 0);
}

template <typename T>
T *alloc(size_t n, uint32_t caps, const char *what) {
  T *p = (T *)heap_caps_calloc(n, sizeof(T), caps);
  if (!p && (caps & MALLOC_CAP_INTERNAL)) {
    Serial.printf("[physarum] %s: no SRAM left, falling back to PSRAM (slower)\n", what);
    p = (T *)heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM);
  }
  if (!p) Serial.printf("[physarum] %s: allocation failed!\n", what);
  return p;
}

} // namespace

void physarum_command(int c) {
  switch (c) {
  case 'n':
    selectPreset(presetIdx + 1);
    nextPreset = tLive + 60;
    break;
  case 't':
    setTheme(themeIdx + 1, false);
    showToast(THEMES[themeIdx].name);
    break;
  case 'b': spores(); break;
  case 'f': feast(); break;
  case 'w': startWander(); break;
  case 'h': startText("HELLO", 2.2f, 1.8f, 5.5f, 3.0f); break;
  case 'm': startText("FEED\nME", 2.0f, 2.0f, 7.0f, 3.5f); break;
  case 'r': restart(); break;
  case '1': case '2': case '3': phantoms[c - '1'].on = !phantoms[c - '1'].on; break;
  case 'o': phantomCircleT = 0; break;
  default: return;
  }
  Serial.printf("[physarum] '%c' -> preset %s, theme %s\n", c, PRESETS[presetIdx].name,
                THEMES[themeIdx].name);
}

void physarum_start() {
  for (int i = 0; i < 256; i++) {
    cosT[i] = cosf(i * TAU / 256);
    sinT[i] = sinf(i * TAU / 256);
  }
  uiRng ^= esp_random();
  constexpr uint32_t SRAM = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  constexpr uint32_t PSRAM = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  Serial.printf("[physarum] SRAM free before alloc: %u (largest %u)\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  // Biggest first, so the agents get the rest of the large block before the
  // small buffers fragment it. Agents are read and rewritten every step: they
  // must stay out of PSRAM, whose bandwidth the LCD needs.
  trail = alloc<uint16_t>(NCELL, SRAM, "trail map");
  for (int s = 0; s < 2; s++) agentsOf[s] = alloc<uint32_t>(nAgents[s], SRAM, "agents");
  food = alloc<uint8_t>(FW * FH, SRAM, "food field");
  lut = alloc<uint16_t>(LUT_SIZE, SRAM, "colour LUT");
  themeLuts = alloc<uint16_t>(LUT_SIZE * N_THEMES, PSRAM, "theme LUTs");
  lutFrom = alloc<uint16_t>(LUT_SIZE, PSRAM, "LUT snapshot");
  textMask = alloc<uint8_t>(FW * FH, PSRAM, "text mask");
  textCells = alloc<uint16_t>(MAX_TEXT_CELLS, PSRAM, "text cells");
  Serial.printf("[physarum] %d agents; SRAM free after alloc: %u\n", nAgents[0] + nAgents[1],
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  for (int t = 0; t < N_THEMES; t++)
    for (int ia = 0; ia < LUT_N; ia++)
      for (int ib = 0; ib < LUT_N; ib++)
        themeLuts[t * LUT_SIZE + ia * LUT_N + ib] =
            shade(THEMES[t], ia / (LUT_N - 1.0f), ib / (LUT_N - 1.0f));
  setTheme(0, true);

  pCur = pFrom = pTo = PRESETS[0].p;
  uint32_t seed = esp_random() | 1;
  seedIntro(seed);

  buildUI();
  prepareText("HELLO", 2.2f);
  pushSimInput();

  // LVGL's loop task outranks the helper, so the helper only soaks up the
  // time core 1 would otherwise spend idle between frames.
  vTaskPrioritySet(NULL, 2);
  xTaskCreatePinnedToCore(helperTask, "physarum+", 3072, NULL, 1, &helperHandle, 1);
  xTaskCreatePinnedToCore(simTask, "physarum", 6144, NULL, 1, NULL, 0);
  lv_timer_create(tick, 10, NULL);
}
