/*******************************************************************************
 * LAVA - a lava lamp for the ESP32-4848S040 (480x480 + GT911).
 *
 * Wax sits in a pool on the heater at the bottom of the lamp. Blobs bud off
 * it as they warm, rise, linger under the cap while they cool, then sink back
 * and melt into the pool again. Wax is drawn as metaballs, so blobs that touch
 * merge, stretch into necks and pinch apart like the real thing.
 *
 *   drag a blob        pick the wax up and fling it; your finger warms it,
 *                      so it rises when you let go
 *   two fingers        grab one blob with both and pull: it stretches, then
 *                      tears in two
 *   swipe through one  slice it in half
 *   stir the liquid    blobs nearby follow the current
 *   hold on liquid     blow a new blob of hot wax; it grows until you let go
 *   double tap         next colour theme
 *
 * Threads / memory:
 *   LVGL (core 1)   touch, blob physics (a couple of dozen blobs, cheap) and
 *                   rendering: DRAW_MAIN draws the 240x240 field straight into
 *                   LVGL's strip buffer as an exact 2x bilinear upscale of both
 *                   field lanes at once (SWAR), 4x4 ordered dither and a 2-D
 *                   colour LUT (wax density x light -> RGB565).
 *   builder (core 0) turns the blob list into the field: per cell, the wax
 *                   density (sum of each blob's (1-d^2/R^2)^2 kernel) and the
 *                   light (lamp glow + the heat the wax gives off). A frame
 *                   and a build never overlap.
 *   Everything hot lives in internal SRAM: PSRAM bandwidth belongs to the LCD.
 ******************************************************************************/
#include "lava.h"
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
constexpr int G = 240;          // field cells per side; one cell = 2x2 px
constexpr int GS = G + 2;       // stored with a 1-cell border for the upscale
constexpr int FMAX = 250;       // lane ceilings: keep 16x sums + dither < 4096
constexpr int LMAX = 247;
constexpr int ISO = 96;         // wax surface, in density units (0..255)
constexpr float KR = 1.609f;    // kernel radius / visible radius for ISO at 255
constexpr int MAX_BLOBS = 22;
constexpr int MAX_KERNELS = MAX_BLOBS + TOUCH_MAX_POINTS + 3;
constexpr int LUT_F = 128, LUT_L = 32, LUT_SIZE = LUT_F * LUT_L;
constexpr float POOL_R = 24;    // pool surface falloff (cells)
constexpr float TAU = 6.2831853f;

/* ------------------------------------------------------------- physics -- */
constexpr float BUOYANCY = 70;    // cells/s^2 per unit of temperature off neutral
constexpr float DRAG = 1.6f;      // 1/s: the liquid is thick
constexpr float T_LIFT = 0.8f;    // wax must get this hot to leave the pool...
constexpr float T_DROP = 0.4f;    // ...and this cold to leave the cap
constexpr float WAX_LIGHT = 0.55f; // how much light hot wax gives off
constexpr float HEAT_Y0 = 210, HEAT_Y1 = 238; // heater zone (cells from top)

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
inline float sstep(float e0, float e1, float x) { return smooth((x - e0) / (e1 - e0)); }
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
/* Unpack a field cell (density in the low byte, light in the high byte) into
 * two 16-bit lanes so both are blended in one 32-bit op. */
inline uint32_t wide(uint32_t c) { return (c & 0xFFu) | ((c & 0xFF00u) << 8); }

/* --------------------------------------------------------------- colour -- */
struct RGBf {
  float r, g, b;
};
struct Theme {
  const char *name;
  RGBf liquid, cool, hot, core; // liquid, cold wax, hot wax, white-hot heart
};
const Theme THEMES[] = {
    {"MAGMA", {0.34f, 0.05f, 0.52f}, {0.55f, 0.02f, 0.06f}, {1.00f, 0.30f, 0.02f},
     {1.00f, 0.85f, 0.40f}},
    {"CLASSIC", {0.80f, 0.58f, 0.06f}, {0.50f, 0.00f, 0.04f}, {1.00f, 0.08f, 0.10f},
     {1.00f, 0.55f, 0.35f}},
    {"ULTRAVIOLET", {0.04f, 0.06f, 0.50f}, {0.36f, 0.02f, 0.42f}, {1.00f, 0.12f, 0.75f},
     {1.00f, 0.75f, 1.00f}},
    {"ACID", {0.00f, 0.22f, 0.30f}, {0.10f, 0.32f, 0.02f}, {0.55f, 1.00f, 0.05f},
     {0.95f, 1.00f, 0.60f}},
    {"LAGOON", {0.06f, 0.04f, 0.30f}, {0.00f, 0.20f, 0.40f}, {0.10f, 0.85f, 1.00f},
     {0.85f, 1.00f, 1.00f}},
    {"SUNSET", {0.45f, 0.02f, 0.12f}, {0.60f, 0.22f, 0.00f}, {1.00f, 0.78f, 0.10f},
     {1.00f, 1.00f, 0.75f}},
    {"MERCURY", {0.10f, 0.12f, 0.18f}, {0.22f, 0.25f, 0.32f}, {0.80f, 0.88f, 1.00f},
     {1.00f, 1.00f, 1.00f}},
};
constexpr int N_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);

/* f: wax density 0..1 (surface at ISO/255), l: light 0..1. Wax glows in its
 * colour, brighter and hotter toward the heater and in thick cores, with a
 * darker skin at the rim; the liquid is lit by the lamp and by the wax. */
uint16_t shade(const Theme &t, float f, float l) {
  constexpr float iso = ISO / 255.0f;
  const float edge = sstep(iso - 0.028f, iso + 0.028f, f);
  const float depth = clamp01((f - iso) / (0.97f - iso));
  const float heat = smooth(l * 1.15f - 0.08f);
  float br = (0.16f + 1.0f * l) * (0.72f + 0.5f * sqrtf(depth));
  br *= 1.0f - 0.32f * (1.0f - sstep(0.0f, 0.2f, depth)); // skin
  const float glowCore = 0.9f * depth * l * l * l;
  const float halo = sstep(iso - 0.22f, iso, f) * (1.0f - edge);
  const float *C = &t.cool.r, *Hh = &t.hot.r, *K = &t.core.r, *L = &t.liquid.r;
  float o[3];
  for (int c = 0; c < 3; c++) {
    const float wax = (C[c] + (Hh[c] - C[c]) * heat) * br + K[c] * glowCore;
    const float liq = L[c] * (0.10f + 1.05f * l) + Hh[c] * 0.22f * halo * (0.25f + l);
    const float e = liq + (wax - liq) * edge;
    o[c] = 1.0f - expf(-1.5f * e);
  }
  return rgb565f(o[0], o[1], o[2]);
}

/* ------------------------------------------------ shared UI/build state -- */
/* One kernel: e = A dx^2 + B dx dy + C dy^2 is the squared distance in kernel
 * radii (an ellipse, so blobs can stretch); weight (1-e)^2 for e < 1. */
struct Kernel {
  float x, y, A, B, C, ext; // ext: half-height of the e < 1 ellipse
  float fs, ls;             // density / light strength (0..255)
};
struct BuildIn {
  Kernel k[MAX_KERNELS];
  int nk;
  float poolT;  // pool surface animation clock
  float poolL;  // light from the hot pool wax
  float glow;   // lamp brightness multiplier
};
BuildIn shared;
portMUX_TYPE inMux = portMUX_INITIALIZER_UNLOCKED;

uint16_t *field;      // SRAM, GS x GS: density low byte, light high byte
uint16_t *lut;        // SRAM: active colour LUT, [light][density]
uint16_t *themeLuts;  // PSRAM: every theme's LUT
uint16_t *lutFrom;    // PSRAM: LUT snapshot when a crossfade starts
uint8_t gx[G], gy[G]; // lamp glow, separable: across (glass) and up (bulb)

/* ------------------------------------------------------ frame handshake -- */
/* The builder writes the next field while the current one is still being
 * drawn, chasing the renderer down the screen: it only overwrites a field row
 * once every output row that reads it has been drawn.
 *   frameActive  set by the UI tick when it starts a frame, cleared by the
 *                renderer after the last strip
 *   renderedTo   output rows of the current frame drawn so far
 *   shownBuild   the build the current frame shows; the builder starts the
 *                next one only after that frame has begun */
volatile bool frameActive = false;
volatile int renderedTo = H;
volatile uint32_t statBuilds, shownBuild, statBuildUs, statWaitUs;
portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t statFrames, statRenderUs;

/* Field row s (stored, border included) feeds output rows up to 2s + 1. */
inline void waitRendered(int s, uint32_t buildStart) {
  const int need = 2 * s + 2;
  if (!frameActive || renderedTo >= need) return;
  const uint32_t t0 = micros();
  while (frameActive && renderedTo < need && t0 - buildStart < 150000 &&
         micros() - t0 < 150000) { // never deadlock on a frame that stalls
  }
  statWaitUs += micros() - t0;
}

/* ------------------------------------------------------ field building -- */
uint32_t acc[G];      // one row: density in the low 16 bits, light in the high
float poolLine[G];

inline float poolDepth(int x, float t) {
  return 9.0f + 3.0f * sinf(x * 0.035f + t * 0.21f) + 2.0f * sinf(x * 0.083f - t * 0.37f) +
         1.2f * sinf(x * 0.19f + t * 0.6f);
}

__attribute__((noinline)) void IRAM_ATTR buildField(const BuildIn &in) {
  const uint32_t start = micros();
  float top = G;
  for (int x = 0; x < G; x++) {
    poolLine[x] = G - poolDepth(x, in.poolT);
    top = fminf(top, poolLine[x]);
  }
  const int poolFrom = max(0, (int)(top - POOL_R));
  constexpr float invRp2 = 1.0f / (POOL_R * POOL_R);
  const uint32_t glow = (uint32_t)(in.glow * 256);
  const float pl = in.poolL;
  for (int y = 0; y < G; y++) {
    const uint32_t gyv = (gy[y] * glow) >> 8;
    for (int x = 0; x < G; x++) acc[x] = ((gx[x] * gyv) >> 8) << 16;

    if (y >= poolFrom) // the pool: a wavy horizontal metaball
      for (int x = 0; x < G; x++) {
        const float d = poolLine[x] - y;
        float w;
        if (d <= 0) w = 1;
        else if (d < POOL_R) {
          const float q = 1 - d * d * invRp2;
          w = q * q;
        } else continue;
        acc[x] += (uint32_t)(w * 255) | ((uint32_t)(w * pl) << 16);
      }

    for (int i = 0; i < in.nk; i++) {
      const Kernel &k = in.k[i];
      const float dy = y - k.y;
      if (dy <= -k.ext || dy >= k.ext) continue;
      // solve A dx^2 + b dx + (c0 - 1) < 0 for this row's span
      const float b = k.B * dy, c0 = k.C * dy * dy;
      const float disc = b * b - 4 * k.A * (c0 - 1);
      if (disc <= 0) continue;
      const float sq = sqrtf(disc), inv2A = 0.5f / k.A;
      int x0 = (int)ceilf(k.x + (-b - sq) * inv2A);
      int x1 = (int)floorf(k.x + (-b + sq) * inv2A);
      if (x0 < 0) x0 = 0;
      if (x1 > G - 1) x1 = G - 1;
      float dx = x0 - k.x;
      const float A = k.A, fs = k.fs, ls = k.ls;
      for (int x = x0; x <= x1; x++, dx += 1.0f) {
        const float q = 1.0f - ((A * dx + b) * dx + c0);
        if (q <= 0) continue;
        const float w = q * q;
        acc[x] += (uint32_t)(w * fs) | ((uint32_t)(w * ls) << 16);
      }
    }

    waitRendered(y + 1, start);
    uint16_t *row = field + (y + 1) * GS;
    for (int x = 0; x < G; x++) {
      const uint32_t a = acc[x];
      const uint32_t f = min<uint32_t>(a & 0xFFFF, FMAX), l = min<uint32_t>(a >> 16, LMAX);
      row[x + 1] = (uint16_t)(f | (l << 8));
    }
    row[0] = row[1];
    row[G + 1] = row[G];
  }
  memcpy(field, field + GS, GS * 2);
  waitRendered(G + 1, start);
  memcpy(field + (G + 1) * GS, field + G * GS, GS * 2);
}

/* --------------------------------------------------------- the builder -- */
void builderTask(void *) {
  static BuildIn in;
  for (;;) {
    portENTER_CRITICAL(&inMux);
    memcpy(&in, &shared, sizeof in);
    portEXIT_CRITICAL(&inMux);
    const uint32_t b0 = micros();
    buildField(in);
    statBuildUs += micros() - b0;
    const uint32_t done = statBuilds + 1;
    statBuilds = done;
    // Wait until a frame showing this build has started, then go straight on
    // to the next one behind it.
    const uint32_t t1 = millis();
    do vTaskDelay(1); // also lets IDLE0 feed the task watchdog
    while (shownBuild != done && millis() - t1 < 100);
  }
}

/* ------------------------------------------------------------ rendering -- */
uint32_t wideRows[3][GS];
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
  const uint16_t *src = field + r * GS;
  uint32_t *dst = wideRows[s];
  for (int i = 0; i < GS; i++) dst[i] = wide(src[i]);
  wideKey[s] = r;
  wideStamp[s] = ++wideClock;
  return dst;
}

/* Lanes are 16x the field value plus dither, < 4096. Density gets 7 bits
 * (dither 0..30 spans one step of 32), light 5 bits (dither 0..120, step 128):
 * index = density + 128 * light. */
constexpr uint32_t DITH = 2u | (8u << 16);
#define LUT_IDX(p) ((((p) >> 5) & 0x7Fu) | (((p) >> 16) & 0xF80u))

/* One 480-px output row. For an exact 2x upscale with centred pixels the
 * bilinear weights are always 3/4 + 1/4, so it is all adds and shifts. */
__attribute__((noinline)) void IRAM_ATTR renderRow(uint16_t *dst, int y) {
  const int sr = 1 + (y >> 1), sf = (y & 1) ? sr + 1 : sr - 1;
  const uint32_t *Wn = getWide(sr), *Wf = getWide(sf);
  const uint8_t *bay = BAYER4[y & 3];
  const uint32_t d0 = bay[0] * DITH, d1 = bay[1] * DITH;
  const uint32_t d2 = bay[2] * DITH, d3 = bay[3] * DITH;
  const uint16_t *L = lut;
  uint32_t *out = (uint32_t *)dst;
  uint32_t vp = Wn[0] * 3 + Wf[0], vc = Wn[1] * 3 + Wf[1];
  for (int j = 0; j < G; j += 2) { // 4 output pixels per iteration
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
  if (y0 == 0) wideKey[0] = wideKey[1] = wideKey[2] = -1; // field moved on
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
  if (frameActive) {
    if (y1 + 1 > renderedTo) renderedTo = y1 + 1; // the builder may follow
    if (y1 == H - 1) frameActive = false;
  }
  statRenderUs += micros() - t0;
}

/* ---------------------------------------------------------------- blobs -- */
struct Blob {
  float x, y, vx, vy, r, T; // cells, cells/s, visible radius, temperature 0..~1.2
  float q1, q2;             // stretch as a doubled-angle vector (axes add up right)
  float ph1, ph2, w1, w2;   // wobble / drift
  float sliceCD;            // can't be sliced again until this runs out
  int8_t grab[2];           // finger slots holding it (-1: none)
  bool rising;              // heated past T_LIFT and not yet cooled to T_DROP
  bool melting;             // surplus wax going back into the pool
};
Blob blobs[MAX_BLOBS];
int nBlobs = 0;

struct Finger {
  bool active, moved, blowing;
  uint8_t id, missing;
  float x, y, x0, y0, lx, ly; // now, at touch-down, at the last change
  float vx, vy, offX, offY;   // velocity (cells/s); held blob's offset
  uint32_t downMs, lastMoveMs;
  int8_t blob;                // index of the blob it holds, -1: none
};
Finger fingers[TOUCH_MAX_POINTS];

float tLive = 0, idle = 0, heater = 1, surgeT = 0, nextSpawn = 0.5f, nextTheme = 200;
float lastTapT = -10, lastTapX = 0, lastTapY = 0;

inline int holders(const Blob &b) { return (b.grab[0] >= 0) + (b.grab[1] >= 0); }

int addBlob(float x, float y, float r, float T) {
  if (nBlobs >= MAX_BLOBS) return -1;
  Blob &b = blobs[nBlobs];
  b = {};
  b.x = x;
  b.y = y;
  b.r = r;
  b.T = T;
  b.ph1 = frand(0, TAU);
  b.ph2 = frand(0, TAU);
  b.w1 = frand(0.10f, 0.22f);
  b.w2 = frand(0.6f, 1.1f);
  b.grab[0] = b.grab[1] = -1;
  b.rising = T > 0.6f;
  return nBlobs++;
}

void removeBlob(int i) {
  const int last = --nBlobs;
  if (i == last) return;
  blobs[i] = blobs[last];
  for (Finger &f : fingers)
    if (f.blob == last) f.blob = i;
}

void spawnFromPool(float r) {
  for (int tries = 0; tries < 8; tries++) {
    const float x = frand(r + 10, G - r - 10);
    bool clear = true;
    for (int i = 0; i < nBlobs && clear; i++)
      if (blobs[i].y > 190 && fabsf(blobs[i].x - x) < (blobs[i].r + r) * 0.9f) clear = false;
    if (!clear) continue;
    // hidden under the pool surface; it bulges out as it heats and rises
    addBlob(x, G + r * 0.75f, r, frand(0.6f, 0.7f));
    return;
  }
}

/* Cut a blob in two along the line (nx, ny); the halves fly apart across it.
 * Returns the new half's index, or -1. */
int split(int i, float nx, float ny, float kick) {
  if (nBlobs >= MAX_BLOBS) return -1;
  const int j = addBlob(0, 0, 0, 0);
  Blob &a = blobs[i], &b = blobs[j];
  const float ph1 = b.ph1, ph2 = b.ph2, w1 = b.w1, w2 = b.w2;
  b = a;
  b.ph1 = ph1;
  b.ph2 = ph2;
  b.w1 = w1;
  b.w2 = w2;
  b.grab[0] = b.grab[1] = -1;
  a.r = b.r = a.r * 0.7071f; // the two halves keep the wax's area
  const float o = a.r * 0.45f;
  a.x += nx * o;
  a.y += ny * o;
  b.x -= nx * o;
  b.y -= ny * o;
  a.vx += nx * kick;
  a.vy += ny * kick;
  b.vx -= nx * kick;
  b.vy -= ny * kick;
  a.q1 = b.q1 = a.q2 = b.q2 = 0;
  a.sliceCD = b.sliceCD = 0.7f;
  return j;
}

/* The heater warms the wax at the bottom, the cap cools it at the top and the
 * liquid in between slowly takes it toward its own temperature. Wax resting
 * on the heater has to get properly hot (T_LIFT) before it breaks free, and
 * wax under the cap properly cold (T_DROP) before it lets go: without that
 * hysteresis every blob just finds the height where it is neutral and hovers
 * there. Small blobs change temperature faster, big ones move faster. */
void physics(float dt) {
  for (int i = 0; i < nBlobs; i++) {
    Blob &b = blobs[i];
    const float sz = sqrtf(b.r * (1.0f / 16));
    const float hot = sstep(HEAT_Y0, HEAT_Y1, b.y);
    const float cold = sstep(b.r + 36, b.r + 4, b.y); // near the cap
    const float Tenv = 0.28f + 0.08f * clamp01(b.y / G) + 1.1f * heater * hot - 0.25f * cold;
    const float kT = 0.05f * (1 + 3.0f * cold + 5.0f * hot) / sz;
    if (!b.melting) b.T += (Tenv - b.T) * fminf(1.0f, kT * dt);
    if (b.T > T_LIFT) b.rising = true;
    if (b.T < T_DROP) b.rising = false;
    const int nh = holders(b);
    if (nh) b.T += (1.15f - b.T) * fminf(1.0f, 0.7f * dt); // warm hands

    float ax, ay;
    if (!nh) {
      ay = b.melting ? 30.0f : -BUOYANCY * (b.T - (b.rising ? T_DROP : T_LIFT)) * sz;
      ax = 2.5f * sinf(tLive * b.w1 + b.ph1);
      const float m = b.r * 0.92f;
      if (b.x < m) ax += (m - b.x) * 14;
      if (b.x > G - m) ax -= (b.x - (G - m)) * 14;
      const float top = b.r * 0.85f;
      if (b.y < top) ay += (top - b.y) * 14;
      const float floor = G + b.r * 0.55f; // resting in the pool, mostly hidden
      if (!b.melting && b.y > floor) ay -= (b.y - floor) * 14;
      ax -= b.vx * DRAG;
      ay -= b.vy * DRAG;
    } else {
      // held: spring to the finger (or to the midpoint of two)
      float tx = 0, ty = 0, fvx = 0, fvy = 0;
      for (int k = 0; k < 2; k++)
        if (b.grab[k] >= 0) {
          const Finger &f = fingers[b.grab[k]];
          tx += f.x + (nh == 1 ? f.offX : 0);
          ty += f.y + (nh == 1 ? f.offY : 0);
          fvx += f.vx;
          fvy += f.vy;
        }
      tx /= nh;
      ty /= nh;
      fvx /= nh;
      fvy /= nh;
      ax = (tx - b.x) * 80 - (b.vx - fvx) * 16;
      ay = (ty - b.y) * 80 - (b.vy - fvy) * 16;
    }
    b.vx += ax * dt;
    b.vy += ay * dt;
    b.sliceCD -= dt;
  }

  // Touching blobs stick (their relative motion damps), share heat, and keep
  // a little apart so a clump stays a clump rather than one point.
  for (int i = 0; i < nBlobs; i++)
    for (int j = i + 1; j < nBlobs; j++) {
      Blob &a = blobs[i], &b = blobs[j];
      const float dx = b.x - a.x, dy = b.y - a.y, rs = a.r + b.r;
      const float d2 = dx * dx + dy * dy, reach = rs * 1.3f;
      if (d2 >= reach * reach) continue;
      const float d = sqrtf(d2) + 1e-3f, nx = dx / d, ny = dy / d;
      const float ma = holders(a) ? 1e6f : a.r * a.r, mb = holders(b) ? 1e6f : b.r * b.r;
      const float wa = mb / (ma + mb), wb = ma / (ma + mb);
      const float near = 1 - d / reach;
      const float stick = fminf(1.0f, near * 2.5f * dt);
      const float rvx = b.vx - a.vx, rvy = b.vy - a.vy;
      a.vx += rvx * stick * wa;
      a.vy += rvy * stick * wa;
      b.vx -= rvx * stick * wb;
      b.vy -= rvy * stick * wb;
      const float dT = (b.T - a.T) * fminf(1.0f, near * 0.25f * dt);
      a.T += dT * (b.r * b.r) / (a.r * a.r + b.r * b.r);
      b.T -= dT * (a.r * a.r) / (a.r * a.r + b.r * b.r);
      const float keep = rs * 0.62f;
      if (d < keep) {
        const float push = (keep - d) * 9 * dt;
        a.vx -= nx * push * wa;
        a.vy -= ny * push * wa;
        b.vx += nx * push * wb;
        b.vy += ny * push * wb;
      }
    }

  for (int i = 0; i < nBlobs; i++) {
    Blob &b = blobs[i];
    const float sp2 = b.vx * b.vx + b.vy * b.vy;
    if (sp2 > 140 * 140) { // flung blobs don't break the sound barrier
      const float s = 140 / sqrtf(sp2);
      b.vx *= s;
      b.vy *= s;
    }
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (!holders(b)) b.x = constrain(b.x, b.r * 0.3f, G - b.r * 0.3f);

    // Shape: moving wax trails into a teardrop, wax pressed under the cap
    // flattens, everything wobbles a little; small blobs stay rounder.
    const float sp = sqrtf(b.vx * b.vx + b.vy * b.vy);
    const float kq = 0.011f * fminf(1.0f, b.r / 14);
    float t1 = 0, t2 = 0;
    if (sp > 0.5f) {
      t1 = (b.vx * b.vx - b.vy * b.vy) / sp * kq;
      t2 = 2 * b.vx * b.vy / sp * kq;
    }
    if (!holders(b) && b.y < b.r * 1.05f) t1 += 0.3f * clamp01((b.r * 1.05f - b.y) / (b.r * 0.3f));
    t1 += 0.06f * sinf(tLive * b.w2 + b.ph2);
    t2 += 0.045f * sinf(tLive * b.w2 * 0.77f + b.ph1);
    if (holders(b) == 2) { // pulled between two fingers: stretch along them
      const Finger &f1 = fingers[b.grab[0]], &f2 = fingers[b.grab[1]];
      const float ex = f2.x - f1.x, ey = f2.y - f1.y, e2 = ex * ex + ey * ey + 1e-3f;
      const float s = constrain(sqrtf(e2) / (2 * b.r) - 0.55f, 0.0f, 0.6f);
      t1 = (ex * ex - ey * ey) / e2 * s;
      t2 = 2 * ex * ey / e2 * s;
    }
    const float k = fminf(1.0f, 5 * dt);
    b.q1 += (t1 - b.q1) * k;
    b.q2 += (t2 - b.q2) * k;
    const float q = sqrtf(b.q1 * b.q1 + b.q2 * b.q2);
    if (q > 0.6f) {
      b.q1 *= 0.6f / q;
      b.q2 *= 0.6f / q;
    }
  }

  // Two fingers pulled far enough apart tear the blob: each keeps a half.
  for (int i = 0; i < nBlobs; i++) {
    Blob &b = blobs[i];
    if (holders(b) != 2) continue;
    Finger &f1 = fingers[b.grab[0]], &f2 = fingers[b.grab[1]];
    const float ex = f2.x - f1.x, ey = f2.y - f1.y, e = sqrtf(ex * ex + ey * ey);
    if (e < b.r * 2.6f || b.r < 5) continue;
    const int s2 = b.grab[1];
    const int j = split(i, -ex / e, -ey / e, 0); // a goes to f1, b to f2
    if (j < 0) continue;
    blobs[i].grab[1] = -1;
    blobs[j].grab[0] = (int8_t)s2;
    fingers[s2].blob = (int8_t)j;
    f1.offX = blobs[i].x - f1.x;
    f1.offY = blobs[i].y - f1.y;
    f2.offX = blobs[j].x - f2.x;
    f2.offY = blobs[j].y - f2.y;
  }

  // Surplus wax (from slicing and blowing) melts back into the pool.
  int target = 8 + (int)(2.5f * sinf(tLive * TAU / 150));
  int live = 0;
  for (int i = 0; i < nBlobs; i++) live += !blobs[i].melting;
  for (int i = 0; i < nBlobs && live > target; i++) {
    Blob &b = blobs[i];
    if (!b.melting && !b.rising && !holders(b) && b.y > 200 && b.vy > -3) {
      b.melting = true;
      live--;
    }
  }
  for (int i = 0; i < nBlobs;) {
    const Blob &b = blobs[i];
    if (b.melting && !holders(b) && b.y > G + b.r * 0.95f) removeBlob(i);
    else i++;
  }
  if (live < target && tLive > nextSpawn) {
    spawnFromPool(frand(10, 22));
    nextSpawn = tLive + frand(2.0f, 5.5f);
  }
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
const char *toastText = nullptr; // applied when the next frame starts
float captionHideAt = 0, toastHideAt = 0;
bool introCaption = false;
int themeIdx = 0;
float themeFade = 1;

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
  toastText = s;
  toast.target = 1;
  toastHideAt = tLive + 1.8f;
}

void nextThemeToast() {
  setTheme(themeIdx + 1, false);
  showToast(THEMES[themeIdx].name);
}

/* Screen pixel -> field cell (cell i is centred on screen px 2i + 0.5). */
inline float toCell(int px) { return (px - 0.5f) * 0.5f; }

/* --------------------------------------------------------------- touch -- */
void fingerDown(int slot, uint8_t id, float x, float y, uint32_t now) {
  Finger &f = fingers[slot];
  f = {};
  f.active = true;
  f.id = id;
  f.x = f.x0 = f.lx = x;
  f.y = f.y0 = f.ly = y;
  f.downMs = f.lastMoveMs = now;
  f.blob = -1;
  idle = 0;
  // Grab the blob under the finger (fingers are fat: be generous).
  int best = -1;
  float bestD = 1e9f;
  for (int i = 0; i < nBlobs; i++) {
    const Blob &b = blobs[i];
    if (holders(b) == 2) continue;
    const float d = hypotf(b.x - x, b.y - y);
    if (d < b.r * 1.2f + 5 && d - b.r < bestD) {
      bestD = d - b.r;
      best = i;
    }
  }
  if (best < 0) return;
  Blob &b = blobs[best];
  b.grab[b.grab[0] < 0 ? 0 : 1] = (int8_t)slot;
  f.blob = (int8_t)best;
  f.offX = b.x - x;
  f.offY = b.y - y;
}

void fingerMove(Finger &f, float x, float y, uint32_t now) {
  if (x == f.x && y == f.y) {
    if (now - f.lastMoveMs > 60) { // finger at rest: let the velocity settle
      f.vx *= 0.8f;
      f.vy *= 0.8f;
    }
    return;
  }
  const float dt = fmaxf(0.008f, (now - f.lastMoveMs) / 1000.0f);
  f.vx = f.vx * 0.4f + (x - f.x) / dt * 0.6f;
  f.vy = f.vy * 0.4f + (y - f.y) / dt * 0.6f;
  f.x = x;
  f.y = y;
  f.lastMoveMs = now;
  idle = 0;
  const float dx = x - f.x0, dy = y - f.y0;
  if (dx * dx + dy * dy > 8 * 8) f.moved = true;
}

void fingerUp(int slot, uint32_t now) {
  Finger &f = fingers[slot];
  f.active = false;
  if (f.blob >= 0) {
    Blob &b = blobs[f.blob];
    for (int k = 0; k < 2; k++)
      if (b.grab[k] == slot) b.grab[k] = -1;
    if (b.grab[0] < 0 && b.grab[1] >= 0) { // the other finger keeps it
      b.grab[0] = b.grab[1];
      b.grab[1] = -1;
    }
    if (!holders(b)) { // fling
      b.vx = f.vx * 0.85f;
      b.vy = f.vy * 0.85f;
    } else {
      Finger &o = fingers[b.grab[0]];
      o.offX = b.x - o.x;
      o.offY = b.y - o.y;
    }
    f.blob = -1;
  }
  const float dx = f.x - f.x0, dy = f.y - f.y0;
  if (now - f.downMs < 260 && dx * dx + dy * dy < 6 * 6) { // a tap
    const float tx = f.x - lastTapX, ty = f.y - lastTapY;
    if (tLive - lastTapT < 0.45f && tx * tx + ty * ty < 20 * 20) {
      nextThemeToast();
      lastTapT = -10;
    } else {
      lastTapT = tLive;
      lastTapX = f.x;
      lastTapY = f.y;
    }
  }
}

/* What a finger does to the wax each tick. */
void fingerAct(int slot, float dt, uint32_t now) {
  Finger &f = fingers[slot];
  if (f.blob >= 0) {
    if (f.blowing) { // still inflating the blob it blew
      Blob &b = blobs[f.blob];
      b.r = fminf(26.0f, b.r + 11.0f * dt * (1 - b.r / 30));
      b.T = 1.15f;
    }
    return;
  }
  // Hold still on the liquid: blow a new blob of hot wax.
  if (!f.moved && now - f.downMs > 450) {
    const int i = addBlob(f.x, f.y, 3.5f, 1.15f);
    if (i >= 0) {
      blobs[i].grab[0] = (int8_t)slot;
      f.blob = (int8_t)i;
      f.blowing = true;
      f.offX = f.offY = 0;
      return;
    }
  }
  // Stir: blobs nearby are dragged along by the current and nudged aside by
  // the finger itself; a fast swipe straight through one slices it.
  const float speed = sqrtf(f.vx * f.vx + f.vy * f.vy);
  for (int i = 0; i < nBlobs; i++) {
    Blob &b = blobs[i];
    if (holders(b)) continue;
    const float dx = b.x - f.x, dy = b.y - f.y, R = 50 + b.r;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= R * R) continue;
    const float d = sqrtf(d2) + 1e-3f;
    if (speed > 110 && b.sliceCD <= 0 && b.r >= 8 && d < b.r * 0.8f && !b.melting) {
      split(i, -f.vy / speed, f.vx / speed, 24);
      continue;
    }
    float w = 1 - d / R;
    w *= w;
    const float k = fminf(1.0f, w * 6 * dt);
    b.vx += (f.vx - b.vx) * k;
    b.vy += (f.vy - b.vy) * k;
    const float pr = b.r * 0.9f + 6;
    if (d < pr) {
      const float push = (pr - d) * 20 * dt;
      b.vx += dx / d * push;
      b.vy += dy / d * push;
    }
    b.T += w * 0.5f * dt;
  }
}

/* Serial-driven "fingers" for testing without touching the glass. */
struct Phantom {
  bool on;
  int16_t x, y;
} phantoms[2] = {{false, 150, 300}, {false, 330, 200}};
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
  for (int k = 0; k < 2 && n < TOUCH_MAX_POINTS; k++)
    if (phantoms[k].on) {
      px[n] = phantoms[k].x;
      py[n] = phantoms[k].y;
      pid[n++] = 200 + k;
    }
  if (phantomCircleT >= 0 && n < TOUCH_MAX_POINTS) {
    if ((phantomCircleT += 0.01f) > 5.0f) phantomCircleT = -1;
    px[n] = 240 + (int)(150 * cosf(phantomCircleT * 2.4f));
    py[n] = 240 + (int)(150 * sinf(phantomCircleT * 2.4f));
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
      fingerMove(fingers[slot], x, y, now);
    }
    seen[slot] = true;
    fingers[slot].missing = 0;
  }
  // The GT911 sometimes reports "no touch" between samples: only treat a
  // finger as lifted once it has been missing for a few polls.
  for (int f = 0; f < TOUCH_MAX_POINTS; f++)
    if (fingers[f].active && !seen[f] && ++fingers[f].missing >= 3) fingerUp(f, now);
}

/* --------------------------------------------------------------- story -- */
void seedLamp() {
  nBlobs = 0;
  for (Finger &f : fingers) f.active = false, f.blob = -1;
  // mid-cycle, so there is something to see at once: two blobs cooling under
  // the cap, two on their way up, two budding off the pool
  int i;
  if ((i = addBlob(70, 26, 19, 0.42f)) >= 0) blobs[i].vy = -2;
  if ((i = addBlob(175, 20, 14, 0.40f)) >= 0) blobs[i].vy = -1;
  if ((i = addBlob(150, 120, 17, 0.85f)) >= 0) blobs[i].vy = -14;
  if ((i = addBlob(60, 165, 12, 0.90f)) >= 0) blobs[i].vy = -12;
  addBlob(110, G + 6, 21, 0.75f);
  addBlob(200, G + 4, 15, 0.70f);
  nextSpawn = 3;
}

void restart() {
  seedLamp();
  tLive = 0;
  idle = 0;
  surgeT = 0;
  introCaption = false;
}

void story(float dt) {
  tLive += dt;
  idle += dt;
  if (surgeT > 0) surgeT -= dt;
  // the heater breathes slowly; a surge makes the lamp boil for a while
  const float h = 1.0f + 0.08f * sinf(tLive * TAU / 97) + (surgeT > 0 ? 0.35f * smooth(surgeT / 3) : 0);
  heater += (h - heater) * fminf(1.0f, dt * 0.5f);

  if (!introCaption && tLive > 2.5f) {
    introCaption = true;
    caption.target = 1;
    captionHideAt = tLive + 8.0f;
  }
  if (caption.target > 0 && tLive > captionHideAt) caption.target = 0;
  if (toast.target > 0 && tLive > toastHideAt) toast.target = 0;

  if (idle > 60 && tLive > nextTheme) { // left alone, it changes colour now and then
    nextThemeToast();
    nextTheme = tLive + frand(180, 300);
  }
}

void pushBuildInput() {
  static BuildIn s;
  int n = 0;
  for (int i = 0; i < nBlobs; i++) {
    const Blob &b = blobs[i];
    Kernel &k = s.k[n++];
    float q = sqrtf(b.q1 * b.q1 + b.q2 * b.q2), c2 = 1, s2 = 0;
    if (q > 1e-4f) {
      c2 = b.q1 / q;
      s2 = b.q2 / q;
    }
    const float Ra = KR * b.r * (1 + q), Rb = KR * b.r / (1 + q); // area kept
    const float ia = 1 / (Ra * Ra), ib = 1 / (Rb * Rb);
    const float ux2 = (1 + c2) * 0.5f, uy2 = (1 - c2) * 0.5f;
    k.x = b.x;
    k.y = b.y;
    k.A = ux2 * ia + uy2 * ib;
    k.C = uy2 * ia + ux2 * ib;
    k.B = s2 * (ia - ib);
    k.ext = sqrtf(k.A / (k.A * k.C - 0.25f * k.B * k.B));
    k.fs = 255;
    k.ls = 255 * WAX_LIGHT * clamp01(b.T);
  }
  for (const Finger &f : fingers) { // a warm glow under each fingertip
    if (!f.active || n >= MAX_KERNELS) continue;
    Kernel &k = s.k[n++];
    const float R = 34;
    k.x = f.x;
    k.y = f.y;
    k.A = k.C = 1 / (R * R);
    k.B = 0;
    k.ext = R;
    k.fs = 0;
    k.ls = f.blob >= 0 ? 40 : 75;
  }
  s.nk = n;
  s.poolT = tLive;
  s.poolL = 255 * WAX_LIGHT * 0.55f * fminf(1.0f, heater);
  s.glow = 0.9f + 0.1f * heater;
  portENTER_CRITICAL(&inMux);
  memcpy(&shared, &s, sizeof s);
  portEXIT_CRITICAL(&inMux);
}

void logStats() {
  static uint32_t lastMs, lBuilds, lBuildUs, lWaitUs, lFrames, lRender, lFlush;
  const uint32_t now = millis();
  if (now - lastMs < 5000) return;
  const float secs = (now - lastMs) / 1000.0f;
  const uint32_t builds = statBuilds - lBuilds, frames = statFrames - lFrames;
  auto per = [](uint32_t us, uint32_t n) { return n ? us / 1000.0f / n : 0.0f; };
  Serial.printf("[lava] %.1f fps (render %.2f + flush %.2f ms) | build %.2f ms "
                "(+ %.2f waiting for the renderer) | %d blobs, "
                "heater %.2f | %s | SRAM %u (min %u)\n",
                frames / secs, per(statRenderUs - lRender, frames),
                per(g_flush_us - lFlush, frames),
                per((statBuildUs - lBuildUs) - (statWaitUs - lWaitUs), builds),
                per(statWaitUs - lWaitUs, builds), nBlobs,
                heater, THEMES[themeIdx].name, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  lastMs = now;
  lBuilds = statBuilds;
  lBuildUs = statBuildUs;
  lWaitUs = statWaitUs;
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
  for (int f = 0; f < TOUCH_MAX_POINTS; f++)
    if (fingers[f].active) fingerAct(f, dt, now);
  story(dt);
  const int steps = 1 + (int)(dt / 0.02f);
  for (int i = 0; i < steps; i++) physics(dt / steps);
  updateTheme(dt);
  pushBuildInput();

  // Start a frame once the builder has finished a new field. Label changes
  // are applied only here too, so nothing redraws outside the handshake.
  static float fadeDt = 0;
  fadeDt += dt;
  const uint32_t ready = statBuilds;
  if (ready != shownBuild && !frameActive) {
    portENTER_CRITICAL(&frameMux);
    renderedTo = 0;
    frameActive = true;
    shownBuild = ready;
    portEXIT_CRITICAL(&frameMux);
    {
      if (toastText) {
        lv_label_set_text(toastLbl, toastText);
        toastText = nullptr;
      }
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
  lv_obj_set_size(cap, 360, 70);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 120);
  lv_obj_set_style_radius(cap, 16, 0);
  lv_obj_set_style_bg_color(cap, lv_color_black(), 0);
  lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *t = mkLabel(cap, &lv_font_montserrat_20, 0xFFF4E8, 9, "LAVA LAMP");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 4, 10);
  lv_obj_t *s = mkLabel(cap, &lv_font_montserrat_14, 0xE8B890, 1,
                        "drag the wax  " LV_SYMBOL_BULLET "  hold to blow a blob");
  lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -11);
  caption.obj = cap;
  caption.rate = 0.8f;
  caption.bgMax = 140;
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
    Serial.printf("[lava] %s: no SRAM left, falling back to PSRAM (slower)\n", what);
    p = (T *)heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM);
  }
  if (!p) Serial.printf("[lava] %s: allocation failed!\n", what);
  return p;
}

} // namespace

void lava_command(int c) {
  switch (c) {
  case 't': nextThemeToast(); break;
  case 'b': spawnFromPool(frand(12, 22)); break;
  case 'h': surgeT = 15; break;
  case 'r': restart(); break;
  case '1': case '2': phantoms[c - '1'].on = !phantoms[c - '1'].on; break;
  case 'o': phantomCircleT = 0; break;
  default: return;
  }
  Serial.printf("[lava] '%c' -> %d blobs, theme %s\n", c, nBlobs, THEMES[themeIdx].name);
}

void lava_start() {
  uiRng ^= esp_random();
  constexpr uint32_t SRAM = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  constexpr uint32_t PSRAM = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  field = alloc<uint16_t>(GS * GS, SRAM, "field");
  lut = alloc<uint16_t>(LUT_SIZE, SRAM, "colour LUT");
  themeLuts = alloc<uint16_t>(LUT_SIZE * N_THEMES, PSRAM, "theme LUTs");
  lutFrom = alloc<uint16_t>(LUT_SIZE, PSRAM, "LUT snapshot");
  Serial.printf("[lava] SRAM free after alloc: %u\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  for (int t = 0; t < N_THEMES; t++)
    for (int il = 0; il < LUT_L; il++)
      for (int jf = 0; jf < LUT_F; jf++) // bucket centres: density 2j+1, light 8i+4
        themeLuts[t * LUT_SIZE + il * LUT_F + jf] =
            shade(THEMES[t], (2 * jf + 1) / 255.0f, (8 * il + 4) / 255.0f);
  setTheme(0, true);

  // Lamp glow: brighter toward the bulb, a cylinder of glass across (darker
  // at the sides) with a couple of vertical highlights on it.
  for (int x = 0; x < G; x++) {
    const float u = (x - (G - 1) * 0.5f) / (G * 0.5f);
    const float v = 0.62f + 0.38f * (1 - u * u) + 0.22f * expf(-sq((x - 40) / 5.0f)) +
                    0.08f * expf(-sq((x - 204) / 9.0f));
    gx[x] = (uint8_t)min(255, iround(v * 255));
  }
  for (int y = 0; y < G; y++) {
    const float v = y / (G - 1.0f);
    gy[y] = (uint8_t)min(255, iround((0.10f + 0.45f * powf(v, 2.2f) + 0.10f * sstep(0.8f, 1, v)) * 255));
  }

  seedLamp();
  buildUI();
  pushBuildInput();
  buildField(shared); // a first field, so the very first frame is a lamp

  vTaskPrioritySet(NULL, 2);
  xTaskCreatePinnedToCore(builderTask, "lava", 4096, NULL, 1, NULL, 0);
  lv_timer_create(tick, 10, NULL);
}
