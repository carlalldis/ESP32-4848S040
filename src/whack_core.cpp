/*******************************************************************************
 * NEON WHACK - core: game logic + software renderer (see whack_core.h).
 *
 * The scene is a pinhole camera pitched down at a neon grid floor:
 *   sky      gradient, stars, striped synth sun, wireframe mountains
 *   floor    perspective grid with fog, travelling light pulses, sun
 *            reflection and shockwave rings (true world-space circles)
 *   holes    3x3 portals: shaded torus rim, animated tunnel, additive glow
 *   moles    capsule robots / bombs, shaded per pixel through 64x64 lighting
 *            tables (diffuse + specular + environment reflection + fresnel
 *            rim, all baked at start-up), clipped by the hole's front lip
 *   effects  3D particles (gravity, floor bounce), tap rings, shake, flash,
 *            glitch
 * Rendering is row by row straight into the caller's buffer. The hot loops
 * are integer table lookups; per-frame setup (projection, colour tables) is
 * done once in update(), not per strip.
 ******************************************************************************/
#include "whack_core.h"
#include <math.h>
#include <string.h>
#ifdef WHACK_SIM
#include <stdio.h>
#endif

namespace whack {
namespace {

/* --------------------------------------------------------------- colour -- */
constexpr uint16_t rgb(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
}
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline uint16_t rgbf(float r, float g, float b) {
  return rgb(clampi((int)r, 0, 255), clampi((int)g, 0, 255), clampi((int)b, 0, 255));
}
inline uint16_t scale565(uint16_t c, int s) { // s: 0..256
  const int r = ((c >> 11) * s) >> 8;
  const int g = (((c >> 5) & 63) * s) >> 8;
  const int b = ((c & 31) * s) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
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
inline uint16_t avg565(uint16_t a, uint16_t b) {
  return (uint16_t)(((a & 0xF7DEu) >> 1) + ((b & 0xF7DEu) >> 1));
}
inline uint16_t mix565(uint16_t a, uint16_t b, int t) { // t: 0..256 toward b
  const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  const int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + (((br - ar) * t) >> 8)) << 11) |
                    ((ag + (((bg - ag) * t) >> 8)) << 5) |
                    (ab + (((bb - ab) * t) >> 8)));
}
inline uint16_t lerpRGB(int r0, int g0, int b0, int r1, int g1, int b1, float t) {
  return rgbf(r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t);
}
inline void fillSpan(uint16_t *row, int a, int b, uint16_t c) {
  if (a >= b) return;
  uint16_t *p = row + a, *e = row + b;
  if (((uintptr_t)p & 2) != 0) *p++ = c;
  const uint32_t c2 = c | ((uint32_t)c << 16);
  while (p + 2 <= e) {
    *(uint32_t *)p = c2;
    p += 2;
  }
  if (p < e) *p = c;
}
inline void addSpan(uint16_t *row, int a, int b, uint16_t c) {
  for (int x = a; x < b; x++) row[x] = add565(row[x], c);
}
/* Cheap "screen"-like brighten: per channel a|b lies between max(a,b) and
 * a+b, and over the dark scene it is visually close to an additive glow at
 * a twelfth of the cost. Two pixels per 32-bit word where aligned. */
inline void orSpan(uint16_t *row, int a, int b, uint16_t c) {
  if (a >= b) return;
  uint16_t *p = row + a, *e = row + b;
  if (((uintptr_t)p & 2) != 0) *p++ |= c;
  const uint32_t c2 = c | ((uint32_t)c << 16);
  while (p + 2 <= e) {
    *(uint32_t *)p |= c2;
    p += 2;
  }
  if (p < e) *p |= c;
}

constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t CYAN = rgb(0, 235, 255);
constexpr uint16_t MAGENTA = rgb(255, 40, 170);
constexpr uint16_t GOLD = rgb(255, 196, 40);
constexpr uint16_t RED = rgb(255, 40, 40);
constexpr uint16_t ORANGE = rgb(255, 120, 20);
constexpr uint16_t VIOLET = rgb(150, 60, 255);

/* ------------------------------------------------------------------ rng -- */
uint32_t rng = 0x2545F491u;
inline uint32_t xr() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
inline float rf(float lo, float hi) {
  return lo + (hi - lo) * ((xr() >> 8) * (1.0f / 16777216.0f));
}
inline int ri(int lo, int hi) { return lo + (int)(xr() % (uint32_t)(hi - lo + 1)); }
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

/* --------------------------------------------------------------- camera -- */
/* World: X right, Y up, Z forward along the floor from the camera's foot.
 * The principal point is shifted down so the horizon sits at y = HOR. */
constexpr int HOR = 100;
constexpr float CAM_F = 420.0f, CAM_H = 4.0f, CAM_PITCH = 0.4363f; // 25 deg
float camS, camC, camCY;

inline bool project(float X, float Y, float Z, float &sx, float &sy, float &sc) {
  const float depth = (CAM_H - Y) * camS + Z * camC;
  if (depth < 0.5f) return false;
  const float up = (Y - CAM_H) * camC + Z * camS;
  sc = CAM_F / depth;
  sx = W * 0.5f + X * sc;
  sy = camCY - up * sc;
  return true;
}
float groundZ(float y) {
  const float t = (camCY - y) / CAM_F;
  return CAM_H * (camC + t * camS) / (camS - t * camC);
}

/* Floor rows. */
constexpr float GRID = 0.75f; // world units between grid lines
float rowZ[H], rowS[H];       // ground depth and px-per-world-unit per row
uint16_t floorBase[H], floorLine[H];
bool floorHLine[H];

/* Sky rows. */
constexpr int SUN_CX = 240, SUN_CY = HOR + 8, SUN_R = 82;
uint16_t skyLUT[HOR + 1], sunCol[HOR + 1], halo1Col[HOR + 1], halo2Col[HOR + 1];
int16_t sunHalf[HOR + 1], halo1Half[HOR + 1], halo2Half[HOR + 1], starX[HOR + 1];
uint8_t mountH[W];
constexpr int MOUNT_MAX = 46;
/* Mountain rows as runs of [x0,x1): body runs, then ridge-line pixels.
 * (The most any of 3000 random seeds needed was 439; extra runs are
 * dropped, never overflowed.) */
constexpr int MAX_MRUNS = 900;
int16_t mRun[MAX_MRUNS][2];
uint16_t mRow0[MOUNT_MAX + 2], mEdge0[MOUNT_MAX + 2];
#ifdef WHACK_SIM
int mRunsUsed = 0;
#endif

/* ---------------------------------------------------------------- ramps -- */
/* 64-step colour ramps: black -> colour -> white-hot. */
enum RampId : uint8_t { R_CYAN, R_MAGENTA, R_GOLD, R_RED, R_VIOLET, R_PINK, NRAMP };
uint16_t ramps[NRAMP][64];
const uint16_t RAMP_BASE[NRAMP] = {CYAN, MAGENTA, GOLD, RED, VIOLET, rgb(255, 90, 200)};

void buildRamp(uint16_t *r, uint16_t base) {
  const float R = (base >> 11) * (255.0f / 31), G = ((base >> 5) & 63) * (255.0f / 63),
              B = (base & 31) * (255.0f / 31);
  for (int i = 0; i < 64; i++) {
    const float t = i / 63.0f;
    if (t < 0.7f) {
      const float k = powf(t / 0.7f, 1.25f);
      r[i] = rgbf(R * k, G * k, B * k);
    } else {
      const float w = (t - 0.7f) / 0.3f;
      r[i] = rgbf(R + (255 - R) * w, G + (255 - G) * w, B + (255 - B) * w);
    }
  }
}

/* ---------------------------------------------------------------- holes -- */
constexpr int NHOLE = 9;
constexpr float RIM_IN = 0.76f;                // inner/outer rim radius
constexpr float GLOW_R = 1.3f;                 // glow radius (x outer)
constexpr int32_t RI2Q = (int32_t)(RIM_IN * RIM_IN * 1024);
constexpr int32_t GLOWQ = (int32_t)(GLOW_R * GLOW_R * 1024);
uint8_t pitDepth[512], pitBright[512], pitRing[512], rimProf[512], glowProf[512];

struct Hole {
  int cx, cy;    // screen centre
  float a, b;    // outer rim semi-axes (px)
  int32_t kA;    // (1024 << 14) / a^2: dx^2 * kA >> 14 = (dx/a)^2 in 1/1024
  float wx, wz;  // world position of the centre
  float wr;      // world radius
  float scale;   // px per world unit at the hole
  uint8_t rimRamp, pitRamp;
  float energy;  // rim flash, 0..1
  float pit;     // portal brightness, 0..1.5
  int32_t pitKey, rimKey, glowKey; // what the colour tables were built for
};
Hole holes[NHOLE];

/* Each hole's colours, indexed by e>>2 (e = normalised radius^2 in 1/1024),
 * rebuilt in prepareFrame() only when the hole's look changes. The rim has
 * one table per light band (the front lip is lit, the back lip is not). */
constexpr int PIT_HI = RI2Q >> 2;               // pit:  e>>2 in [0, PIT_HI]
constexpr int RIM_LO = RI2Q >> 2, RIM_N = 256 - RIM_LO;
constexpr int GLOW_LO = 256, GLOW_N = (GLOWQ >> 2) - 256 + 1;
constexpr int RIM_BANDS = 8;
struct HoleTabs {
  uint16_t pit[PIT_HI + 1];
  uint16_t rim[RIM_BANDS][RIM_N];
  uint16_t glow[GLOW_N];
};
HoleTabs tabs[NHOLE];

/* ---------------------------------------------------------------- moles -- */
enum MoleSt : uint8_t { MS_HIDDEN, MS_WARN, MS_RISE, MS_UP, MS_SINK, MS_HIT };
enum MoleType : uint8_t { MT_NORMAL, MT_GOLD, MT_BOMB, NTYPE };
constexpr float WARN_T = 0.24f, RISE_T = 0.17f, SINK_T = 0.2f;

struct Mole {
  MoleSt st;
  MoleType type;
  float t, upTime, h, h0, squash, flash, blinkT, wob, wobV;
};
Mole moles[NHOLE];

/* Per-frame screen geometry of each mole (filled in by prepareFrame). */
struct Geo {
  bool vis, sphere, hit, blink;
  uint8_t type;
  float cx, mw, top, bot, capEnd, capH, base, sc, sr;
  float aoY0, aoInv;
  uint16_t flashC;
  float eyeY, eyeDX, eyeRX, eyeRY;
  float noseY, noseRX, noseRY, toothY0, toothY1, toothW, cheekDX, cheekR;
  float antX0, antY0, antX1, antY1, tipR;
  uint16_t tipCol;
  int sheen;     // gold sheen sweep position
  int eyePulse;  // bomb eye glow strength
  float pawY, pawDX, pawRX, pawRY; // paws gripping the front lip (rx 0 = none)
};
Geo geo[NHOLE];

/* Lighting tables: colour for a unit-sphere normal (u, v) = 64x64 cells. */
uint16_t moleLUT[NTYPE][64][64];
uint16_t eyeRamp[NTYPE][8];
uint16_t eyeGlow[NTYPE];
uint16_t noseRamp[8];

/* ------------------------------------------------------------ particles -- */
enum : uint8_t { PF_GRAV = 1, PF_BOUNCE = 2, PF_FLOAT = 4, PF_DRAG = 8, PF_TWINKLE = 16 };
struct Part {
  float x, y, z, vx, vy, vz, life, maxLife, size;
  uint16_t col;
  uint8_t flags;
};
constexpr int MAX_PARTS = 260;
Part parts[MAX_PARTS];
int partN = 0;

constexpr int PLEV = 8;
struct PP { // projected particle
  int16_t x, y, r;
  int32_t r2, k;
  uint16_t lev[PLEV];
};
PP pp[MAX_PARTS];
int ppN = 0;

struct Wave {
  float wx, wz, r, spd, w, life, maxLife;
  uint16_t col, c1, c2;
};
constexpr int MAX_WAVES = 8;
Wave waves[MAX_WAVES];
int waveN = 0;

struct Ring {
  float x, y, r, life, maxLife;
  uint16_t col, cc;
};
constexpr int MAX_RINGS = 4;
Ring rings[MAX_RINGS];
int ringN = 0;

/* ---------------------------------------------------------------- state -- */
Status st;
Popup pops[MAX_POPUPS];
uint32_t popSerial = 0;
float tWorld = 0;
uint32_t frameNo = 0;
float spawnT = 0.5f, emberT = 0, lastCount = 0;
int lastHole = -1;
float flashAmt = 0, shakeAmt = 0, glitchT = 0, glitchI = 0, hitStop = 0;
uint16_t flashBase = WHITE, flashC = 0;
int shakeX = 0, shakeY = 0;
int tunnelPhase = 0;

/* ======================================================= table building == */
void envColor(float rx, float ry, float out[3]) {
  // Screen y points down, so ry < 0 looks up into the sky.
  float r, g, b;
  if (ry < 0) {
    const float s = sqrtf(-ry);
    r = 255 + (24 - 255) * s;
    g = 70 + (6 - 70) * s;
    b = 170 + (70 - 170) * s;
  } else {
    const float s = sqrtf(ry);
    r = 90 + (8 - 90) * s;
    g = 20 + (2 - 20) * s;
    b = 120 + (26 - 120) * s;
    // neon grid glow from the floor
    const float gg = 0.5f + 0.5f * sinf(ry * 18.0f + rx * 9.0f);
    r += 60 * gg * (1 - s);
    b += 90 * gg * (1 - s);
  }
  const float band = expf(-ry * ry * 140.0f); // bright horizon line
  out[0] = r + 255 * band;
  out[1] = g + 170 * band;
  out[2] = b + 230 * band;
}

struct Material {
  float base[3], amb, kd, km, ks, shin, rim[3], tint[3];
};

void buildMoleLUT(uint16_t lut[64][64], const Material &M) {
  // key light from the upper left, viewer along +z
  float L[3] = {-0.5f, -0.8f, 0.55f};
  float ln = 1.0f / sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
  for (float &c : L) c *= ln;
  float Hv[3] = {L[0], L[1], L[2] + 1};
  ln = 1.0f / sqrtf(Hv[0] * Hv[0] + Hv[1] * Hv[1] + Hv[2] * Hv[2]);
  for (float &c : Hv) c *= ln;
  for (int vi = 0; vi < 64; vi++)
    for (int ui = 0; ui < 64; ui++) {
      float u = (ui + 0.5f) / 32.0f - 1, v = (vi + 0.5f) / 32.0f - 1;
      const float rr = u * u + v * v;
      if (rr > 0.995f) {
        const float k = sqrtf(0.995f / rr);
        u *= k;
        v *= k;
      }
      const float nz = sqrtf(fmaxf(0.0f, 1 - u * u - v * v));
      const float diff = fmaxf(0.0f, u * L[0] + v * L[1] + nz * L[2]);
      const float spec = powf(fmaxf(0.0f, u * Hv[0] + v * Hv[1] + nz * Hv[2]), M.shin);
      float env[3];
      envColor(2 * nz * u, 2 * nz * v, env);
      const float fres = powf(1 - nz, 2.2f);
      float c[3];
      for (int k = 0; k < 3; k++)
        c[k] = M.base[k] * (M.amb + M.kd * diff) + M.km * env[k] * M.tint[k] +
               M.ks * spec * 255 + M.rim[k] * fres;
      lut[vi][ui] = rgbf(c[0], c[1], c[2]);
    }
}

void buildTables() {
  camS = sinf(CAM_PITCH);
  camC = cosf(CAM_PITCH);
  camCY = HOR + CAM_F * tanf(CAM_PITCH);

  for (int i = 0; i < NRAMP; i++) buildRamp(ramps[i], RAMP_BASE[i]);

  /* Floor rows: depth, scale, fogged colours, horizontal grid lines. */
  for (int y = 0; y < H; y++) {
    rowZ[y] = 1e6f;
    rowS[y] = 0;
    floorHLine[y] = false;
  }
  for (int y = HOR + 1; y < H; y++) {
    const float Z = groundZ(y + 0.5f);
    rowZ[y] = Z;
    rowS[y] = CAM_F / (CAM_H * camS + Z * camC);
    const float fog = powf(clampf((Z - 3.0f) / 45.0f, 0, 1), 0.55f);
    floorBase[y] = lerpRGB(6, 1, 18, 44, 8, 66, fog);
    floorLine[y] = lerpRGB(210, 40, 255, 64, 12, 92, fog);
  }
  for (int y = HOR + 1; y < H - 1; y++)
    floorHLine[y] = (int)floorf(rowZ[y] / GRID) != (int)floorf(rowZ[y + 1] / GRID);

  /* Sky: night -> violet -> hot pink haze at the horizon. */
  for (int y = 0; y <= HOR; y++) {
    if (y < 55)
      skyLUT[y] = lerpRGB(2, 0, 10, 26, 4, 56, y / 55.0f);
    else
      skyLUT[y] = lerpRGB(26, 4, 56, 120, 18, 118, (y - 55) / (float)(HOR - 55));
    starX[y] = (y < 80 && (xr() % 3) == 0) ? (int16_t)ri(0, W - 1) : (int16_t)-1;
    const int dy = y - SUN_CY;
    const float t = clampf((y - (SUN_CY - SUN_R)) / (float)SUN_R, 0, 1);
    const uint16_t sc = lerpRGB(255, 236, 110, 255, 40, 150, t);
    sunCol[y] = sc;
    sunHalf[y] = (abs(dy) < SUN_R) ? (int16_t)sqrtf((float)(SUN_R * SUN_R - dy * dy)) : 0;
    const int stripe0 = SUN_CY - 46;
    if (y > stripe0) {
      const int d = y - stripe0;
      const int gap = 1 + d / 9;
      if (d % 10 >= 10 - gap) sunHalf[y] = 0;
    }
    const int r1 = SUN_R + 40, r2 = SUN_R + 16;
    halo1Half[y] = (abs(dy) < r1) ? (int16_t)sqrtf((float)(r1 * r1 - dy * dy)) : 0;
    halo2Half[y] = (abs(dy) < r2) ? (int16_t)sqrtf((float)(r2 * r2 - dy * dy)) : 0;
    halo1Col[y] = mix565(skyLUT[y], sc, 30);
    halo2Col[y] = mix565(skyLUT[y], sc, 66);
  }

  /* Mountains: overlapping ridges, kept low in the middle so the sun shows. */
  float mh[W];
  for (int x = 0; x < W; x++) mh[x] = 0;
  for (int p = 0; p < 14; p++) {
    const float px = rf(-40, W + 40), ph = rf(14, MOUNT_MAX), sl = rf(0.35f, 0.8f);
    for (int x = 0; x < W; x++) {
      const float v = ph - fabsf(x - px) * sl;
      if (v > mh[x]) mh[x] = v;
    }
  }
  for (int x = 0; x < W; x++) {
    const float c = fabsf(x - W * 0.5f) / (W * 0.5f);
    const float att = 0.28f + 0.72f * c * c;
    const float jag = (hash3(x / 3, 7, 1) % 5) * 0.5f;
    mountH[x] = (uint8_t)clampi((int)(mh[x] * att + jag), 0, MOUNT_MAX);
  }
  int nr = 0;
  for (int my = 0; my <= MOUNT_MAX; my++) {
    mRow0[my] = (uint16_t)nr;
    for (int x = 0; x < W && nr < MAX_MRUNS;) { // body: mountH >= my
      if (mountH[x] < my) { x++; continue; }
      int e = x;
      while (e < W && mountH[e] >= my) e++;
      mRun[nr][0] = (int16_t)x;
      mRun[nr++][1] = (int16_t)e;
      x = e;
    }
    mEdge0[my] = (uint16_t)nr;
    for (int x = 0; x < W && nr < MAX_MRUNS;) { // ridge: mountH in {my, my+1}
      if (mountH[x] != my && mountH[x] != my + 1) { x++; continue; }
      int e = x;
      while (e < W && (mountH[e] == my || mountH[e] == my + 1)) e++;
      mRun[nr][0] = (int16_t)x;
      mRun[nr++][1] = (int16_t)e;
      x = e;
    }
  }
  mRow0[MOUNT_MAX + 1] = (uint16_t)nr;
#ifdef WHACK_SIM
  if (nr >= MAX_MRUNS) fprintf(stderr, "mountain runs overflow (%d)\n", nr);
  mRunsUsed = nr;
#endif

  /* Hole profiles, indexed by e>>2 where e = (normalised radius)^2 * 1024. */
  for (int i = 0; i < 512; i++) {
    const float e = (i * 4 + 2) / 1024.0f, r = sqrtf(e);
    const float rho = clampf(r / RIM_IN, 0, 1);
    pitDepth[i] = (uint8_t)clampi((int)(26.0f / (rho + 0.14f)), 0, 255);
    float br = powf(1 - rho, 2.2f) * 36 + 5;
    if (rho > 0.78f) br *= 1 - (rho - 0.78f) / 0.22f * 0.8f; // lip shadow
    pitBright[i] = (uint8_t)clampi((int)br, 0, 63);
    pitRing[i] = (uint8_t)clampi((int)(48 * rho * (1 - rho) + 2), 0, 63);
    const float t = clampf((r - RIM_IN) / (1 - RIM_IN), 0, 1);
    float p = 0.22f + 0.7f * powf(sinf(3.14159f * t), 0.8f) +
              0.45f * expf(-(t - 0.38f) * (t - 0.38f) / 0.012f);
    rimProf[i] = (uint8_t)clampi((int)(p * 62), 0, 255);
    const float g = clampf((e - 1) / (GLOW_R * GLOW_R - 1), 0, 1);
    glowProf[i] = (uint8_t)clampi((int)(15.99f * (1 - g) * (1 - g)), 0, 15);
  }

  /* 3x3 board in forced perspective: back rows get bigger holes and wider
   * spacing than true perspective would, so every target stays tappable.
   * Each hole's ellipse aspect comes from the camera at its row. */
  const int rowY[3] = {250, 336, 425};
  const float rowA[3] = {42, 52, 62};
  const int rowDX[3] = {124, 140, 158};
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) {
      Hole &h = holes[r * 3 + c];
      h.cx = W / 2 + (c - 1) * rowDX[r];
      h.cy = rowY[r];
      const float Z = groundZ((float)h.cy), dz = 0.05f;
      float x0, y0, s0, x1, y1, s1;
      project(0, 0, Z - dz, x0, y0, s0);
      project(0, 0, Z + dz, x1, y1, s1);
      const float sc = CAM_F / (CAM_H * camS + Z * camC);
      const float aspect = (y0 - y1) / (2 * dz * sc);
      h.a = rowA[r];
      h.b = h.a * aspect;
      h.kA = (int32_t)((1024.0f * 16384.0f) / (h.a * h.a));
      h.scale = sc;
      h.wz = Z;
      h.wx = (h.cx - W * 0.5f) / sc;
      h.wr = h.a / sc;
      h.rimRamp = ((r + c) & 1) ? R_MAGENTA : R_CYAN;
      h.pitRamp = R_VIOLET;
      h.energy = 0;
      h.pit = 0;
      h.pitKey = h.rimKey = h.glowKey = -1;
    }

  /* Materials: violet candy-plastic bot, gold chrome bot, black glass bomb. */
  const Material mats[NTYPE] = {
      {{125, 48, 220}, 0.05f, 1.05f, 0.16f, 1.0f, 46, {0, 210, 255}, {1, 1, 1}},
      {{255, 190, 45}, 0.05f, 0.72f, 0.45f, 1.0f, 60, {255, 120, 30}, {1.0f, 0.72f, 0.2f}},
      {{30, 24, 44}, 0.10f, 0.70f, 0.40f, 1.1f, 80, {255, 30, 50}, {0.9f, 0.7f, 1.0f}},
  };
  for (int t = 0; t < NTYPE; t++) buildMoleLUT(moleLUT[t], mats[t]);

  const uint16_t eyeC[NTYPE] = {CYAN, MAGENTA, RED};
  for (int t = 0; t < NTYPE; t++) {
    for (int i = 0; i < 8; i++) {
      if (i < 3)
        eyeRamp[t][i] = mix565(WHITE, eyeC[t], i * 85);
      else if (i < 6)
        eyeRamp[t][i] = eyeC[t];
      else
        eyeRamp[t][i] = scale565(eyeC[t], 190);
    }
    eyeGlow[t] = eyeC[t];
  }
  for (int i = 0; i < 8; i++) noseRamp[i] = mix565(rgb(255, 200, 235), rgb(220, 40, 120), i * 36);
}

/* ============================================================ effects ==== */
void emit(float x, float y, float z, int n, float spd, uint16_t c0, uint16_t c1,
          float life, float size, uint8_t flags, float upBias = 1.0f) {
  for (int i = 0; i < n && partN < MAX_PARTS; i++) {
    Part &p = parts[partN++];
    const float a = rf(0, 6.2832f), s = rf(0.25f, 1.0f) * spd;
    p.x = x;
    p.y = y;
    p.z = z;
    p.vx = cosf(a) * s;
    p.vz = sinf(a) * s * 0.8f;
    p.vy = rf(0.2f, 1.0f) * spd * upBias;
    p.maxLife = p.life = rf(0.55f, 1.0f) * life;
    p.size = size * rf(0.6f, 1.3f);
    p.col = (i & 1) ? c0 : c1;
    p.flags = flags;
  }
}
void addWave(float wx, float wz, float spd, float w, float life, uint16_t col) {
  if (waveN >= MAX_WAVES) { // replace the oldest
    memmove(waves, waves + 1, sizeof(Wave) * (MAX_WAVES - 1));
    waveN--;
  }
  waves[waveN++] = {wx, wz, 0.1f, spd, w, life, life, col, 0, 0};
}
void addRing(float x, float y, uint16_t col) {
  if (ringN >= MAX_RINGS) {
    memmove(rings, rings + 1, sizeof(Ring) * (MAX_RINGS - 1));
    ringN--;
  }
  rings[ringN++] = {x, y, 4, 0.3f, 0.3f, col, 0};
}
void addPopup(float x, float y, int value, PopupKind kind) {
  int best = 0;
  for (int i = 1; i < MAX_POPUPS; i++)
    if (pops[i].life < pops[best].life) best = i;
  pops[best] = {++popSerial, x, y, POPUP_LIFE, value, kind};
}
/* Screen point on a hole's mole -> world (vertical px are ~scale*cos(pitch)). */
inline void moleWorld(const Hole &h, float sx, float sy, float &X, float &Y) {
  X = h.wx + (sx - h.cx) / h.scale;
  Y = (h.cy - sy) / (h.scale * camC);
}

/* ========================================================= game logic ==== */
void setPhase(Phase p) {
  st.phase = p;
  st.phaseTime = 0;
}

void sinkAll() {
  for (Mole &m : moles)
    if (m.st == MS_WARN) {
      m.st = MS_HIDDEN;
    } else if (m.st == MS_RISE || m.st == MS_UP) {
      m.st = MS_SINK;
      m.t = 0;
      m.h0 = m.h;
    }
}

void startCountdown() {
  sinkAll();
  st.score = 0;
  st.combo = 0;
  st.mult = 1;
  st.newBest = false;
  st.timeLeft = ROUND_SECONDS;
  lastCount = 0;
  setPhase(Phase::Countdown);
}

void endRound() {
  st.timeLeft = 0;
  sinkAll();
  if (st.score > st.best) {
    st.best = st.score;
    st.newBest = true;
  }
  for (int i = 0; i < NHOLE; i++) {
    Hole &h = holes[i];
    addWave(h.wx, h.wz, 4.5f, 0.12f, 0.9f, ramps[h.rimRamp][44]);
    emit(h.wx, 0.1f, h.wz, 10, 5.0f, RAMP_BASE[h.rimRamp], WHITE, 1.3f, 0.05f,
         PF_GRAV | PF_BOUNCE, 1.6f);
    h.energy = 1;
  }
  flashAmt = 0.5f;
  flashBase = MAGENTA;
  shakeAmt = 6;
  setPhase(Phase::Over);
}

void spawner(float dt, bool attract) {
  spawnT -= dt;
  if (spawnT > 0) return;
  const float p = attract ? 0.25f : clampf(1 - st.timeLeft / ROUND_SECONDS, 0, 1);
  const int maxActive = attract ? 3 : 2 + (int)(p * 2.99f);
  spawnT = (attract ? 0.7f : 0.82f - 0.48f * p) * rf(0.7f, 1.25f);
  int active = 0;
  for (const Mole &m : moles) active += m.st != MS_HIDDEN;
  if (active >= maxActive) return;
  int i = -1;
  for (int tries = 0; tries < 16; tries++) {
    const int c = ri(0, NHOLE - 1);
    if (moles[c].st == MS_HIDDEN && c != lastHole) {
      i = c;
      break;
    }
  }
  if (i < 0) return;
  lastHole = i;
  Mole &m = moles[i];
  const float r = rf(0, 1);
  const float bombP = (attract || p > 0.06f) ? 0.11f + 0.07f * p : 0;
  m.type = r < bombP ? MT_BOMB : (r < bombP + 0.085f ? MT_GOLD : MT_NORMAL);
  m.upTime = (attract ? 1.1f : 1.15f - 0.55f * p) * rf(0.85f, 1.15f);
  if (m.type == MT_GOLD) m.upTime *= 0.62f;
  if (m.type == MT_BOMB) m.upTime *= 1.25f;
  m.st = MS_WARN;
  m.t = 0;
  m.h = 0;
  m.squash = 0;
  m.flash = 0;
  m.wob = 0;
  m.wobV = 0;
  m.blinkT = rf(0.8f, 3.0f);
}

void whackMole(int i) {
  Mole &m = moles[i];
  Hole &h = holes[i];
  const Geo &g = geo[i];
  const bool scoring = st.phase == Phase::Playing;
  m.st = MS_HIT;
  m.t = 0;
  m.h0 = m.h;
  m.flash = 1;
  m.wobV += 40;
  h.energy = 1;
  float X, Y;
  const float my = m.type == MT_BOMB ? g.sc : (g.top + g.capEnd) * 0.5f;
  moleWorld(h, g.cx, my, X, Y);
  const float popY = (m.type == MT_BOMB ? g.top : g.top - 8);

  if (m.type == MT_BOMB) {
    if (scoring) {
      st.combo = 0;
      st.mult = 1;
      st.timeLeft = fmaxf(0, st.timeLeft - 5);
      addPopup(g.cx, popY, -5, POP_BOMB);
    }
    emit(X, Y, h.wz, 5, 1.2f, WHITE, GOLD, 0.32f, 0.42f, PF_DRAG, 0.5f);           // fireball
    emit(X, Y, h.wz, 8, 2.4f, ORANGE, RED, 0.55f, 0.3f, PF_DRAG | PF_FLOAT, 0.8f);  // flames
    emit(X, Y, h.wz, 44, 8.0f, ORANGE, RED, 1.3f, 0.09f, PF_GRAV | PF_BOUNCE | PF_DRAG, 1.2f);
    emit(X, Y, h.wz, 30, 5.5f, GOLD, WHITE, 0.9f, 0.065f, PF_GRAV | PF_BOUNCE, 1.4f);
    emit(X, Y, h.wz, 14, 1.4f, rgb(110, 20, 40), rgb(70, 10, 70), 1.8f, 0.28f, PF_FLOAT | PF_DRAG, 0.8f);
    hitStop = 0.09f;
    addWave(h.wx, h.wz, 9.0f, 0.22f, 0.8f, ORANGE);
    addWave(h.wx, h.wz, 5.0f, 0.12f, 1.0f, RED);
    flashAmt = 1.0f;
    flashBase = rgb(255, 60, 40);
    shakeAmt = 15;
    glitchT = 0.45f;
    glitchI = 1.0f;
    return;
  }

  int pts = 10;
  if (scoring) {
    st.combo++;
    st.mult = imin(5, 1 + st.combo / 5);
  }
  if (m.type == MT_GOLD) {
    pts = 50;
    if (scoring) st.timeLeft += 2;
    emit(X, Y, h.wz, 3, 0.6f, WHITE, GOLD, 0.22f, 0.3f, PF_DRAG, 0.5f);
    emit(X, Y, h.wz, 40, 6.5f, GOLD, WHITE, 1.3f, 0.075f, PF_GRAV | PF_BOUNCE, 1.3f);
    emit(X, Y, h.wz, 24, 2.2f, GOLD, rgb(255, 240, 180), 1.5f, 0.06f, PF_FLOAT | PF_DRAG | PF_TWINKLE, 1.0f);
    hitStop = 0.07f;
    addWave(h.wx, h.wz, 6.5f, 0.16f, 0.8f, GOLD);
    addWave(h.wx, h.wz, 3.5f, 0.08f, 0.9f, WHITE);
    flashAmt = 0.35f;
    flashBase = GOLD;
    shakeAmt = fmaxf(shakeAmt, 5);
  } else {
    emit(X, Y, h.wz, 2, 0.4f, WHITE, CYAN, 0.16f, 0.22f, PF_DRAG, 0.5f);
    emit(X, Y, h.wz, 34, 5.5f, CYAN, VIOLET, 1.1f, 0.07f, PF_GRAV | PF_BOUNCE, 1.2f);
    emit(X, Y, h.wz, 10, 3.2f, WHITE, MAGENTA, 0.7f, 0.09f, PF_GRAV | PF_DRAG, 1.0f);
    hitStop = 0.05f;
    addWave(h.wx, h.wz, 5.5f, 0.13f, 0.7f, ramps[h.rimRamp][40]);
    shakeAmt = fmaxf(shakeAmt, 3);
  }
  if (scoring) {
    pts *= st.mult;
    st.score += pts;
    addPopup(g.cx, popY, pts, m.type == MT_GOLD ? POP_GOLD : POP_NORMAL);
  }
}

void updateMole(int i, float dt) {
  Mole &m = moles[i];
  m.flash = fmaxf(0, m.flash - dt * 8);
  m.blinkT -= dt;
  if (m.blinkT < -0.12f) m.blinkT = rf(1.5f, 4.0f);
  m.wobV += (-m.wob * 220 - m.wobV * 9) * dt; // antenna spring
  m.wob += m.wobV * dt;
  switch (m.st) {
  case MS_HIDDEN:
    break;
  case MS_WARN:
    m.t += dt;
    if (m.t >= WARN_T) {
      m.st = MS_RISE;
      m.t = 0;
      const Hole &h = holes[i];
      emit(h.wx, 0.05f, h.wz, 8, 2.2f, ramps[h.rimRamp][50], WHITE, 0.5f, 0.04f, PF_GRAV, 1.4f);
    }
    break;
  case MS_RISE: {
    m.t += dt;
    const float k = m.t / RISE_T;
    if (k >= 1) {
      m.st = MS_UP;
      m.t = 0;
      m.h = 1;
      m.wobV -= 25;
    } else {
      const float c1 = 1.9f, c3 = c1 + 1, q = k - 1; // ease-out-back
      m.h = 1 + c3 * q * q * q + c1 * q * q;
    }
    break;
  }
  case MS_UP:
    m.t += dt;
    m.h = 1;
    if (m.t >= m.upTime) {
      m.st = MS_SINK;
      m.t = 0;
      m.h0 = 1;
      if (st.phase == Phase::Playing && m.type != MT_BOMB) {
        st.combo = 0; // it got away
        st.mult = 1;
      }
    }
    break;
  case MS_SINK: {
    m.t += dt;
    const float k = m.t / SINK_T;
    m.h = m.h0 * (1 - k * k);
    if (k >= 1) m.st = MS_HIDDEN;
    break;
  }
  case MS_HIT: {
    m.t += dt;
    m.squash = m.t < 0.06f ? m.t / 0.06f : fmaxf(0, 1 - (m.t - 0.06f) / 0.3f);
    const float k = clampf((m.t - 0.1f) / 0.3f, 0, 1);
    m.h = m.h0 * (1 - k * k);
    if (m.t >= 0.42f) {
      m.st = MS_HIDDEN;
      m.squash = 0;
    }
    break;
  }
  }
}

/* ========================================================= per frame ===== */
void prepareFrame() {
  /* hole colour tables */
  tunnelPhase = ((int)(tWorld * 26)) & 31;
  for (int i = 0; i < NHOLE; i++) {
    Hole &h = holes[i];
    HoleTabs &T = tabs[i];
    const Mole &m = moles[i];
    const bool on = m.st == MS_WARN || m.st == MS_RISE || m.st == MS_UP;
    if (on) h.pitRamp = m.type == MT_BOMB ? R_RED : (m.type == MT_GOLD ? R_GOLD : R_CYAN);

    const int pitGain = 200 + (int)(h.pit * 150);
    const int32_t pitKey = (pitGain << 12) | (h.pitRamp << 5) | tunnelPhase;
    if (pitKey != h.pitKey) {
      h.pitKey = pitKey;
      const uint16_t *R = ramps[h.pitRamp];
      for (int k = 0; k <= PIT_HI; k++) {
        int b = pitBright[k];
        if (((pitDepth[k] + tunnelPhase) & 31) < 5) b += pitRing[k];
        T.pit[k] = R[imin(63, (b * pitGain) >> 8)];
      }
    }
    const int rimBoost = (int)(h.energy * 26);
    const int32_t rimKey = (rimBoost << 4) | h.rimRamp;
    if (rimKey != h.rimKey) {
      h.rimKey = rimKey;
      const uint16_t *R = ramps[h.rimRamp];
      for (int band = 0; band < RIM_BANDS; band++) {
        const float dyn = (band + 0.5f) * (2.0f / RIM_BANDS) - 1;
        const int light = clampi((int)(165 + 100 * dyn), 50, 256);
        for (int k = 0; k < RIM_N; k++)
          T.rim[band][k] = R[imin(63, ((rimProf[RIM_LO + k] * light) >> 8) + rimBoost)];
      }
    }
    const int inten = 70 + (int)(h.energy * 150) + (int)(h.pit * 40);
    const uint8_t gr = h.energy > 0.3f ? h.pitRamp : h.rimRamp;
    const int32_t glowKey = (inten << 4) | gr;
    if (glowKey != h.glowKey) {
      h.glowKey = glowKey;
      const uint16_t gc = RAMP_BASE[gr];
      for (int k = 0; k < GLOW_N; k++) {
        const float e = ((GLOW_LO + k) * 4 + 2) / 1024.0f;
        const float g = clampf((e - 1) / (GLOW_R * GLOW_R - 1), 0, 1);
        T.glow[k] = scale565(gc, imin(256, (int)((1 - g) * (1 - g) * inten)));
      }
    }
  }

  /* mole geometry */
  for (int i = 0; i < NHOLE; i++) {
    const Mole &m = moles[i];
    const Hole &h = holes[i];
    Geo &g = geo[i];
    g.vis = m.st >= MS_RISE && m.h > 0.002f;
    if (!g.vis) continue;
    g.type = m.type;
    g.sphere = m.type == MT_BOMB;
    g.hit = m.st == MS_HIT;
    g.blink = m.blinkT < 0;
    g.cx = (float)h.cx;
    g.base = h.cy + h.b * 0.55f;
    const float sq = m.squash;
    float stretch = 1;
    if (m.st == MS_UP) stretch = 1 + 0.11f * sinf(m.t * 26) * expf(-m.t * 8);
    const float wob = clampf(m.wob, -1.2f, 1.2f);
    if (!g.sphere) {
      g.mw = h.a * 0.56f * (1 + 0.3f * sq) / sqrtf(stretch);
      const float hm = h.a * 1.42f * (1 - 0.42f * sq) * stretch;
      g.top = g.base - hm * m.h;
      g.capH = g.mw * 0.92f * (1 - 0.3f * sq);
      g.capEnd = g.top + g.capH;
      g.bot = g.base;
      const float s = g.mw;
      g.eyeY = g.top + g.capH * 0.86f;
      g.eyeDX = s * 0.38f;
      g.eyeRX = s * 0.2f;
      g.eyeRY = s * 0.27f * (1 - 0.45f * sq);
      g.noseY = g.top + g.capH * 1.28f;
      g.noseRX = s * 0.15f;
      g.noseRY = s * 0.11f;
      g.toothY0 = g.noseY + s * 0.12f;
      g.toothY1 = g.toothY0 + s * 0.2f;
      g.toothW = s * 0.11f;
      g.cheekDX = s * 0.64f;
      g.cheekR = s * 0.17f;
      g.antX0 = g.cx;
      g.antY0 = g.top + 1;
      g.antX1 = g.cx + wob * s * 0.55f;
      g.antY1 = g.top - s * 0.6f * (1 - 0.3f * sq);
      g.tipR = fmaxf(2.0f, s * 0.13f);
      if (m.type == MT_GOLD) {
        g.tipCol = rgb(255, 250, 220);
      } else {
        const int pulse = 160 + (int)(96 * sinf(tWorld * 7 + i));
        g.tipCol = scale565(MAGENTA, pulse);
      }
      g.sheen = (int)fmodf(tWorld * 160 + i * 37, s * 6) - (int)(s * 2);
      const float pk = m.st == MS_HIT ? 0 : clampf((m.h - 0.6f) / 0.35f, 0, 1);
      g.pawY = h.cy + h.b * 0.6f;
      g.pawDX = s * 0.62f;
      g.pawRX = s * 0.27f * pk;
      g.pawRY = g.pawRX * 0.72f;
    } else {
      g.sr = h.a * 0.6f * (1 + 0.22f * sq);
      g.mw = g.sr;
      const float c0 = g.base + g.sr, c1 = h.cy - g.sr * 0.8f;
      g.sc = c0 + (c1 - c0) * m.h;
      g.top = g.sc - g.sr;
      g.bot = g.sc + g.sr;
      g.capEnd = g.capH = 0;
      g.eyeY = g.sc - g.sr * 0.05f;
      g.eyeDX = g.sr * 0.36f;
      g.eyeRX = g.sr * 0.2f;
      g.eyeRY = g.sr * 0.19f;
      g.antX0 = g.cx + g.sr * 0.12f;
      g.antY0 = g.top - g.sr * 0.08f;
      g.antX1 = g.cx + g.sr * (0.42f + 0.25f * wob);
      g.antY1 = g.top - g.sr * 0.5f;
      g.tipR = g.sr * rf(0.1f, 0.2f);
      g.tipCol = (xr() & 1) ? rgb(255, 230, 120) : rgb(255, 140, 40);
      g.pawRX = 0;
      g.eyePulse = 150 + (int)(100 * sinf(tWorld * 12));
    }
    g.aoY0 = h.cy - h.b * 0.3f;
    g.aoInv = 1.0f / fmaxf(1.0f, g.base + 2 - g.aoY0);
    g.flashC = m.flash > 0 ? scale565(WHITE, (int)(m.flash * 170)) : 0;
  }

  /* particles: project and pre-scale their colours */
  ppN = 0;
  static const int levW[PLEV] = {256, 230, 190, 150, 110, 72, 40, 16};
  for (int i = 0; i < partN; i++) {
    const Part &p = parts[i];
    float sx, sy, sc;
    if (!project(p.x, p.y, p.z, sx, sy, sc)) continue;
    const float rf_ = fmaxf(0.7f, fminf(30.0f, p.size * sc));
    const int r = (int)(rf_ + 0.5f);
    if (sx + r < 0 || sx - r >= W || sy + r < 0 || sy - r >= H) continue;
    float k = fminf(1.0f, 1.6f * p.life / p.maxLife);
    if (p.flags & PF_TWINKLE) k *= 0.45f + 0.55f * ((hash3(i, frameNo >> 1, 5) & 3) != 0);
    PP &q = pp[ppN++];
    q.x = (int16_t)sx;
    q.y = (int16_t)sy;
    q.r = (int16_t)r;
    q.r2 = (int32_t)(rf_ * rf_) + 1;
    q.k = (PLEV << 16) / q.r2;
    const int kk = (int)(k * 256);
    for (int j = 0; j < PLEV; j++) q.lev[j] = scale565(p.col, (kk * levW[j]) >> 8);
  }

  for (int i = 0; i < waveN; i++) {
    Wave &w = waves[i];
    const float f = w.life / w.maxLife;
    w.c1 = scale565(w.col, (int)(f * f * 110));
    w.c2 = scale565(w.col, (int)(f * 200));
  }
  for (int i = 0; i < ringN; i++) {
    Ring &r = rings[i];
    r.cc = scale565(r.col, (int)(256 * r.life / r.maxLife));
  }
  flashC = flashAmt > 0 ? scale565(flashBase, (int)(flashAmt * flashAmt * 200)) : 0;
}

/* =========================================================== rendering === */
void drawSkyRow(uint16_t *row, int y) {
  fillSpan(row, 0, W, skyLUT[y]);
  const int sx = starX[y];
  if (sx >= 0 && (hash3(y, frameNo >> 3, 9) & 7) != 0)
    row[sx] = (y & 3) ? rgb(150, 140, 210) : rgb(240, 240, 255);
  if (halo1Half[y]) fillSpan(row, imax(0, SUN_CX - halo1Half[y]), imin(W, SUN_CX + halo1Half[y]), halo1Col[y]);
  if (halo2Half[y]) fillSpan(row, imax(0, SUN_CX - halo2Half[y]), imin(W, SUN_CX + halo2Half[y]), halo2Col[y]);
  if (sunHalf[y]) fillSpan(row, imax(0, SUN_CX - sunHalf[y]), imin(W, SUN_CX + sunHalf[y]), sunCol[y]);

  const int my = HOR - y; // height above the horizon
  if (my <= MOUNT_MAX) {
    const float k = my / (float)MOUNT_MAX;
    const uint16_t fill = lerpRGB(26, 6, 58, 14, 3, 34, k);
    const uint16_t contour = (my % 5 == 2) ? lerpRGB(90, 24, 150, 50, 14, 100, k) : fill;
    const uint16_t edge = rgb(255, 70, 210), edge2 = rgb(130, 30, 170);
    for (int i = mRow0[my]; i < mEdge0[my]; i++) fillSpan(row, mRun[i][0], mRun[i][1], contour);
    for (int i = mEdge0[my]; i < mRow0[my + 1]; i++)
      for (int x = mRun[i][0]; x < mRun[i][1]; x++)
        if (mountH[x] >= my) row[x] = mountH[x] == my ? edge : edge2;
  }
  if (y >= HOR - 3) orSpan(row, 0, W, scale565(MAGENTA, (y - (HOR - 4)) * 30));
}

void drawFloorRow(uint16_t *row, int y) {
  if (y == HOR) {
    fillSpan(row, 0, W, rgb(255, 150, 230));
    return;
  }
  const float Z = rowZ[y];
  // energy pulse rolling toward the camera
  float pz = fmodf(Z + tWorld * 3.2f, 9.0f);
  const float pulse = pz < 1.4f ? 1 - pz / 1.4f : 0;
  uint16_t lc = floorLine[y];
  if (pulse > 0) lc = mix565(lc, rgb(255, 180, 255), (int)(pulse * 150));
  fillSpan(row, 0, W, floorHLine[y] ? lc : floorBase[y]);

  const float step = GRID * rowS[y];
  if (step >= 2.6f) {
    const int n = (int)(W * 0.5f / step) + 1;
    const bool thick = step > 40;
    const float x0 = W * 0.5f - n * step;
    for (int k = 0; k <= 2 * n; k++) {
      const int x = (int)(x0 + k * step);
      if (x >= 0 && x < W) row[x] = lc;
      if (thick && x + 1 >= 0 && x + 1 < W) row[x + 1] = lc;
    }
  }
  // the sun's reflection shimmering on the floor
  const int d = y - HOR;
  if (d < 130) {
    const float fade = 1 - d / 130.0f;
    const int ph = (y * 13 + (int)(tWorld * 80)) & 31; // shimmer: triangle wave
    const int hw = (int)(SUN_R * 0.7f * (0.5f + 0.5f * fade)) + (ph < 16 ? ph : 31 - ph) * 3 / 4 - 6;
    if (((y + (frameNo >> 2)) % 3) != 0)
      orSpan(row, imax(0, SUN_CX - hw), imin(W, SUN_CX + hw), scale565(rgb(255, 60, 140), (int)(fade * fade * 90)));
  }
  if (d <= 4) orSpan(row, 0, W, scale565(MAGENTA, (5 - d) * 40));
}

void drawWavesRow(uint16_t *row, int y) {
  const float Z = rowZ[y], s = rowS[y];
  for (int i = 0; i < waveN; i++) {
    const Wave &w = waves[i];
    const float dz = Z - w.wz, ro = w.r + w.w;
    if (fabsf(dz) >= ro) continue;
    const float dz2 = dz * dz;
    const float xo = sqrtf(ro * ro - dz2);
    const float riw = w.r - w.w;
    const float xi = riw > 0 && riw * riw > dz2 ? sqrtf(riw * riw - dz2) : 0;
    const float cx = W * 0.5f + w.wx * s;
    const int a0 = imax(0, (int)(cx - xo * s)), a1 = imin(W, (int)(cx - xi * s));
    const int b0 = imax(0, (int)(cx + xi * s)), b1 = imin(W, (int)(cx + xo * s));
    addSpan(row, a0, a1, w.c1);
    addSpan(row, b0, b1, w.c1);
    // brighter core band
    const float rc = w.r + w.w * 0.35f, rci = w.r - w.w * 0.35f;
    if (fabsf(dz) < rc) {
      const float xc = sqrtf(rc * rc - dz2);
      const float xci = rci > 0 && rci * rci > dz2 ? sqrtf(rci * rci - dz2) : 0;
      addSpan(row, imax(0, (int)(cx - xc * s)), imin(W, (int)(cx - xci * s)), w.c2);
      addSpan(row, imax(0, (int)(cx + xci * s)), imin(W, (int)(cx + xc * s)), w.c2);
    }
  }
}

/* Smallest dx >= 0 whose e = (dx^2 * kA >> 14) + dyQ reaches T. */
inline int edgeDX(int32_t T, int32_t dyQ, int32_t kA) {
  if (dyQ >= T) return 0;
  int d = (int)sqrtf((float)(T - dyQ) * 16384.0f / kA);
  while (d > 0 && ((((d - 1) * (d - 1) * kA) >> 14) + dyQ) >= T) d--;
  while ((((d * d * kA) >> 14) + dyQ) < T) d++;
  return d;
}

/* One row of a hole: pixels with |dx| < dPit are the pit, then the rim out
 * to dRim, then additive glow out to dGlow. Each region is a branch-free
 * loop over e, stepped incrementally, and a table lookup. */
void drawHoleRow(uint16_t *row, int y, int hi) {
  const Hole &h = holes[hi];
  const HoleTabs &T = tabs[hi];
  const float dyn = (y + 0.5f - h.cy) / h.b;
  const float dy2 = dyn * dyn;
  if (dy2 >= GLOW_R * GLOW_R) return;
  const int32_t dyQ = (int32_t)(dy2 * 1024.0f), kA = h.kA;
  const int dGlow = edgeDX(GLOWQ, dyQ, kA);
  if (dGlow == 0) return;
  const int dRim = edgeDX(1024, dyQ, kA), dPit = edgeDX(RI2Q, dyQ, kA);
  const uint16_t *rim = T.rim[clampi((int)((dyn + 1) * (RIM_BANDS / 2)), 0, RIM_BANDS - 1)] - RIM_LO;
  const uint16_t *glow = T.glow - GLOW_LO;
  const uint16_t *pit = T.pit;
  const int cx = h.cx;
  // region edges as x ranges: [g0,r0) glow, [r0,p0) rim, [p0,p1) pit, [p1,r1) rim, [r1,g1) glow
  const int g0 = cx - dGlow + 1, r0 = cx - dRim + 1, p0 = cx - dPit + 1;
  const int p1 = cx + dPit, r1 = cx + dRim, g1 = cx + dGlow;
#define HOLE_SPAN(A, B, BODY)                                                       {                                                                                   const int xa_ = imax(A, 0), xb_ = imin(B, W);                                     if (xa_ < xb_) {                                                                    int dx = xa_ - cx;                                                                int32_t q = dx * dx * kA, dq = (2 * dx + 1) * kA;                                 const int32_t k2 = 2 * kA;                                                        for (int x = xa_; x < xb_; x++, q += dq, dq += k2) {                                const int e = (q >> 14) + dyQ;                                                    BODY;                                                                           }                                                                               }                                                                               }
  HOLE_SPAN(g0, r0, row[x] |= glow[e >> 2])
  HOLE_SPAN(r0, p0, row[x] = rim[e >> 2])
  HOLE_SPAN(p0, p1, row[x] = pit[e >> 2])
  HOLE_SPAN(p1, r1, row[x] = rim[e >> 2])
  HOLE_SPAN(r1, g1, row[x] |= glow[e >> 2])
#undef HOLE_SPAN
}

/* Clip [x0,x1) of a mole row to what can be seen: below the hole's centre
 * line the mole is only visible through the pit opening. */
inline bool visClip(const Hole &h, int y, int &x0, int &x1) {
  if (y > h.cy) {
    const float dyn = (y + 0.5f - h.cy) / (h.b * RIM_IN);
    if (dyn >= 1) return false;
    const float ih = h.a * RIM_IN * sqrtf(1 - dyn * dyn);
    x0 = imax(x0, (int)ceilf(h.cx - ih));
    x1 = imin(x1, (int)(h.cx + ih) + 1);
  }
  x0 = imax(x0, 0);
  x1 = imin(x1, W);
  return x0 < x1;
}

/* Solid ellipse coloured by an 8-step ramp, with an additive halo out to
 * `halo` x its radius. Clipped to [cx0, cx1). */
void ellipseGlow(uint16_t *row, int y, int cx0, int cx1, float ex, float ey, float rx,
                 float ry, const uint16_t *ramp8, uint16_t glow, float halo) {
  const float dyn = (y + 0.5f - ey) / ry;
  const float h2 = halo * halo;
  if (dyn * dyn >= h2) return;
  const float hw = rx * sqrtf(h2 - dyn * dyn);
  const int x0 = imax(cx0, (int)(ex - hw)), x1 = imin(cx1, (int)(ex + hw) + 1);
  const float irx = 1.0f / rx, dy2 = dyn * dyn;
  for (int x = x0; x < x1; x++) {
    const float dx = (x + 0.5f - ex) * irx;
    const float e = dx * dx + dy2;
    if (e < 1) {
      row[x] = ramp8[(int)(e * 7.99f)];
    } else if (glow && e < h2) {
      const float k = (h2 - e) / (h2 - 1);
      row[x] = add565(row[x], scale565(glow, (int)(k * k * 200)));
    }
  }
}

void drawMoleRow(uint16_t *row, int y, const Hole &h, const Geo &g) {
  const int cxI = (int)g.cx;

  /* antenna / fuse above the body */
  if (y >= (int)(g.antY1 - g.tipR * 2.6f) && y < (int)g.antY0 + 1 && !g.hit) {
    int x0 = 0, x1 = W;
    if (visClip(h, y, x0, x1)) {
      if (y >= (int)g.antY1 && y <= (int)g.antY0) {
        const float k = (g.antY0 - y) / fmaxf(1.0f, g.antY0 - g.antY1);
        float ax = g.antX0 + (g.antX1 - g.antX0) * k;
        if (g.sphere) ax += sinf(k * 3.0f) * g.sr * 0.08f; // curly fuse
        const int xi = (int)ax;
        const uint16_t c = g.sphere ? rgb(150, 120, 90) : rgb(170, 160, 220);
        if (xi >= x0 && xi < x1) row[xi] = c;
        if (xi + 1 >= x0 && xi + 1 < x1) row[xi + 1] = scale565(c, 140);
      }
      static uint16_t tipRamp[8];
      for (int i = 0; i < 8; i++) tipRamp[i] = i < 4 ? WHITE : g.tipCol;
      ellipseGlow(row, y, x0, x1, g.antX1, g.antY1, g.tipR, g.tipR, tipRamp, g.tipCol, 2.6f);
    }
  }

  if (y < (int)g.top || y > (int)g.bot) return;

  /* body silhouette for this row */
  float half;
  int vi;
  if (g.sphere) {
    const float v = (y + 0.5f - g.sc) / g.sr;
    if (v <= -1 || v >= 1) return;
    half = g.sr * sqrtf(1 - v * v);
    vi = (int)((v + 1) * 32);
  } else if (y + 0.5f < g.capEnd) {
    const float v = (y + 0.5f - g.capEnd) / g.capH;
    if (v <= -1) return;
    half = g.mw * sqrtf(1 - v * v);
    vi = (int)((v + 1) * 32);
  } else {
    half = g.mw;
    vi = 32 + (int)(16 * (y - g.capEnd) / fmaxf(1.0f, g.base - g.capEnd));
  }
  vi = clampi(vi, 0, 63);
  int x0 = (int)ceilf(g.cx - half), x1 = (int)(g.cx + half) + 1;
  const int sx0 = x0, sx1 = x1; // silhouette before clipping (for edge AA)
  if (!visClip(h, y, x0, x1)) return;

  const uint16_t *lut = moleLUT[g.type][vi];
  const int32_t uStep = (int32_t)(32.0f * 65536.0f / g.mw);
  int32_t uq = (int32_t)((x0 + 0.5f - g.cx) * 32.0f * 65536.0f / g.mw) + (32 << 16);
  int ao = 256;
  if (y > g.aoY0) ao = clampi((int)(256 * (1 - (y - g.aoY0) * g.aoInv)), 0, 256);
  const uint16_t fc = g.flashC;
  const bool sheen = g.type == MT_GOLD;
  const int sheenRow = sheen ? (int)((y - g.top) * 0.6f) - g.sheen : 0;
  for (int x = x0; x < x1; x++, uq += uStep) {
    uint16_t c = lut[clampi(uq >> 16, 0, 63)];
    if (sheen) {
      const int sp = (x - cxI) + sheenRow;
      if (sp >= 0 && sp < 7) c = add565(c, scale565(rgb(255, 250, 220), 150 - sp * 18));
    }
    if (ao < 256) c = scale565(c, ao);
    if (fc) c = add565(c, fc);
    row[x] = c;
  }
  // soften the silhouette edges
  if (x0 == sx0 && x0 > 0) row[x0] = avg565(row[x0], row[x0 - 1]);
  if (x1 == sx1 && x1 < W) row[x1 - 1] = avg565(row[x1 - 1], row[x1]);

  /* face */
  const uint16_t *er = eyeRamp[g.type];
  if (g.sphere) {
    // angry slanted eyes
    for (int side = -1; side <= 1; side += 2) {
      const float ex = g.cx + side * g.eyeDX;
      const float dyn = (y + 0.5f - g.eyeY) / g.eyeRY;
      if (dyn * dyn >= 4.4f) continue;
      const float hw = g.eyeRX * 2.1f;
      const int a = imax(x0, (int)(ex - hw)), b = imin(x1, (int)(ex + hw) + 1);
      for (int x = a; x < b; x++) {
        const float dx = (x + 0.5f - ex) / g.eyeRX;
        const float e = dx * dx + dyn * dyn;
        const float brow = -0.15f - side * dx * 0.55f; // slopes down toward the nose
        if (e < 1 && dyn > brow) {
          row[x] = er[(int)(e * 7.99f)];
        } else if (e < 4.4f && !g.hit) {
          const float k = (4.4f - e) / 3.4f;
          row[x] = add565(row[x], scale565(RED, (int)(k * k * g.eyePulse)));
        }
      }
    }
    return;
  }
  for (int side = -1; side <= 1; side += 2) {
    const float ex = g.cx + side * g.eyeDX;
    if (g.hit) { // X eyes
      const int dy = y - (int)g.eyeY;
      const int r = (int)(g.eyeRX * 1.1f) + 1;
      if (dy >= -r && dy <= r) {
        const int xs[4] = {(int)ex + dy, (int)ex + dy + 1, (int)ex - dy, (int)ex - dy + 1};
        for (int xx : xs)
          if (xx >= x0 && xx < x1) row[xx] = WHITE;
      }
    } else {
      const float ry = g.blink ? g.eyeRY * 0.16f : g.eyeRY;
      ellipseGlow(row, y, x0, x1, ex, g.eyeY, g.eyeRX, ry, er, eyeGlow[g.type], 1.9f);
    }
    // blush
    const float cdy = (y + 0.5f - g.noseY) / (g.cheekR * 0.7f);
    if (cdy * cdy < 1) {
      const float hw = g.cheekR * sqrtf(1 - cdy * cdy);
      const float cx = g.cx + side * g.cheekDX;
      const uint16_t bc = scale565(rgb(255, 60, 150), (int)(110 * (1 - cdy * cdy)));
      addSpan(row, imax(x0, (int)(cx - hw)), imin(x1, (int)(cx + hw) + 1), bc);
    }
  }
  ellipseGlow(row, y, x0, x1, g.cx, g.noseY, g.noseRX, g.noseRY, noseRamp, 0, 1.0f);
  if (y + 0.5f >= g.toothY0 && y + 0.5f < g.toothY1) {
    const int tw = (int)(g.toothW + 0.5f);
    fillSpan(row, imax(x0, cxI - tw), imin(x1, cxI), rgb(240, 240, 255));
    fillSpan(row, imax(x0, cxI + 1), imin(x1, cxI + 1 + tw), rgb(225, 225, 245));
  }
}

/* Additive 1px line, the part of it that falls in row y, clipped to [c0,c1). */
void lineRow(uint16_t *row, int y, float xa, float ya, float xb, float yb, uint16_t c,
             int c0, int c1) {
  if (ya > yb) {
    float t = xa; xa = xb; xb = t;
    t = ya; ya = yb; yb = t;
  }
  const int ry0 = (int)floorf(ya), ry1 = (int)floorf(yb);
  if (y < ry0 || y > ry1) return;
  float t0 = 0, t1 = 1;
  if (ry1 != ry0) {
    t0 = clampf((y - ya) / (yb - ya), 0, 1);
    t1 = clampf((y + 1 - ya) / (yb - ya), 0, 1);
  }
  float x0 = xa + (xb - xa) * t0, x1 = xa + (xb - xa) * t1;
  if (x0 > x1) {
    const float t = x0; x0 = x1; x1 = t;
  }
  addSpan(row, imax(c0, (int)x0), imin(c1, (int)x1 + 1), c);
}

void drawWhiskersRow(uint16_t *row, int y, const Hole &h, const Geo &g) {
  if (g.sphere || g.hit) return;
  const float s = g.mw;
  if (y < (int)(g.noseY - s * 0.2f) || y > (int)(g.noseY + s * 0.45f)) return;
  int x0 = 0, x1 = W;
  if (!visClip(h, y, x0, x1)) return;
  const uint16_t c = rgb(110, 150, 180);
  for (int side = -1; side <= 1; side += 2)
    for (int j = 0; j < 2; j++)
      lineRow(row, y, g.cx + side * s * 0.3f, g.noseY + s * (0.04f + j * 0.1f),
              g.cx + side * s * 1.25f, g.noseY + s * (-0.12f + j * 0.3f), c, x0, x1);
}

/* Two little shaded paws gripping the hole's front lip (drawn over it). */
void drawPawsRow(uint16_t *row, int y, const Geo &g) {
  if (g.pawRX < 1.5f) return;
  const float dyn = (y + 0.5f - g.pawY) / g.pawRY;
  if (dyn <= -1 || dyn >= 1) return;
  const int vi = clampi((int)((dyn + 1) * 32), 0, 63);
  const uint16_t *lut = moleLUT[g.type][vi];
  const float hw = g.pawRX * sqrtf(1 - dyn * dyn);
  for (int side = -1; side <= 1; side += 2) {
    const float px = g.cx + side * g.pawDX;
    const int a = imax(0, (int)ceilf(px - hw)), b = imin(W, (int)(px + hw) + 1);
    for (int x = a; x < b; x++) {
      const float u = (x + 0.5f - px) / g.pawRX;
      uint16_t c = lut[clampi((int)((u + 1) * 32), 0, 63)];
      if (dyn > -0.1f && (fabsf(u - 0.33f) < 0.09f || fabsf(u + 0.33f) < 0.09f))
        c = scale565(c, 110); // toes
      row[x] = c;
    }
  }
}

void drawParticlesRow(uint16_t *row, int y, const uint16_t *idx, int n) {
  for (int j = 0; j < n; j++) {
    const PP &p = pp[idx[j]];
    const int dy = y - p.y;
    const int dy2 = dy * dy;
    if (dy2 >= p.r2) continue;
    const int hw = (int)sqrtf((float)(p.r2 - dy2));
    const int a = imax(0, p.x - hw), b = imin(W, p.x + hw + 1);
    for (int x = a; x < b; x++) {
      const int dx = x - p.x;
      const int lvl = ((dx * dx + dy2) * p.k) >> 16;
      if (lvl < PLEV) row[x] = add565(row[x], p.lev[lvl]);
    }
  }
}

void drawRingsRow(uint16_t *row, int y) {
  for (int i = 0; i < ringN; i++) {
    const Ring &r = rings[i];
    const float dy = y + 0.5f - r.y;
    const float ro = r.r, rin = r.r - 3.5f;
    if (fabsf(dy) >= ro) continue;
    const float xo = sqrtf(ro * ro - dy * dy);
    const float xi = rin > 0 && fabsf(dy) < rin ? sqrtf(rin * rin - dy * dy) : 0;
    addSpan(row, imax(0, (int)(r.x - xo)), imin(W, (int)(r.x - xi)), r.cc);
    addSpan(row, imax(0, (int)(r.x + xi)), imin(W, (int)(r.x + xo)), r.cc);
  }
}

void drawTimerRow(uint16_t *row, int y) {
  const float f = clampf(st.timeLeft / ROUND_SECONDS, 0, 1);
  const int w = (int)(W * f);
  const bool low = st.timeLeft < 10;
  const uint16_t c = low ? ((frameNo >> 3) & 1 ? RED : ORANGE) : (y < 3 ? CYAN : rgb(0, 120, 160));
  fillSpan(row, 0, w, c);
  addSpan(row, w, W, rgb(16, 8, 30));
  if (w > 2 && w < W) row[w - 1] = WHITE;
}

/* The whole scene for one row (row is indexed by screen x, full width). */
/* WHACK_PROFILE (desktop only): g_skip bits switch scene layers off so the
 * simulator can time each one. */
#ifdef WHACK_PROFILE
} // namespace
int g_skip = 0;
namespace {
#define SKIP(bit) (g_skip & (bit))
#else
#define SKIP(bit) 0
#endif
void renderScene(uint16_t *row, int y, const uint16_t *pidx, int pn) {
  if (y <= HOR - 1) {
    if (!SKIP(1)) drawSkyRow(row, y);
  } else if (!SKIP(2))
    drawFloorRow(row, y);
  if (y > HOR) drawWavesRow(row, y);

  for (int r = 0; r < 3; r++) { // back to front
    if (!SKIP(4))
      for (int c = 0; c < 3; c++) drawHoleRow(row, y, r * 3 + c);
    if (!SKIP(8))
      for (int c = 0; c < 3; c++) {
        const int i = r * 3 + c;
        if (!geo[i].vis) continue;
        drawMoleRow(row, y, holes[i], geo[i]);
        drawWhiskersRow(row, y, holes[i], geo[i]);
        drawPawsRow(row, y, geo[i]);
      }
  }
  if (SKIP(16)) pn = 0;
  if (pn) drawParticlesRow(row, y, pidx, pn);
  if (ringN) drawRingsRow(row, y);
  if (y < 5 && (st.phase == Phase::Playing || st.phase == Phase::Countdown)) drawTimerRow(row, y);
}

} // namespace

/* ============================================================== public === */
void init(uint32_t best, uint32_t seed) {
  rng = seed ? seed : 0x2545F491u;
  buildTables();
  memset(moles, 0, sizeof moles);
  memset(&st, 0, sizeof st);
  st.best = (int)best;
  st.mult = 1;
  st.timeLeft = ROUND_SECONDS;
  for (Popup &p : pops) p = {0, 0, 0, 0, 0, POP_NORMAL};
  setPhase(Phase::Title);
  prepareFrame();
}

void update(float dt) {
  const float realDt = dt;
  if (hitStop > 0) { // freeze-frame on impact: the world crawls for a moment
    hitStop -= dt;
    dt *= 0.12f;
  }
  tWorld += dt;
  frameNo++;
  st.phaseTime += realDt;

  switch (st.phase) {
  case Phase::Title:
    spawner(dt, true);
    break;
  case Phase::Countdown: {
    const int n = (int)st.phaseTime; // a beat each second: 3, 2, 1
    if (n > lastCount || st.phaseTime <= dt) {
      lastCount = (float)n;
      const Hole &h = holes[4];
      addWave(h.wx, h.wz, 7.0f, 0.15f, 0.9f, n < 3 ? CYAN : MAGENTA);
    }
    if (st.phaseTime >= 3.0f) {
      setPhase(Phase::Playing);
      spawnT = 0.15f;
      for (const Hole &h : holes) addWave(h.wx, h.wz, 5.0f, 0.1f, 0.6f, MAGENTA);
      flashAmt = 0.4f;
      flashBase = CYAN;
    }
    break;
  }
  case Phase::Playing:
    st.timeLeft -= realDt;
    if (st.timeLeft <= 0)
      endRound();
    else
      spawner(dt, false);
    break;
  case Phase::Over:
    break;
  }

  for (int i = 0; i < NHOLE; i++) {
    updateMole(i, dt);
    Hole &h = holes[i];
    const Mole &m = moles[i];
    h.energy = fmaxf(0, h.energy - dt * 2.5f);
    float target = 0;
    if (m.st == MS_WARN) target = 1.0f + 0.5f * sinf(m.t * 60);
    else if (m.st == MS_RISE || m.st == MS_UP) target = 0.55f;
    h.pit += (target - h.pit) * fminf(1.0f, dt * 14);
    if (m.st == MS_HIDDEN && h.pit < 0.05f && h.energy < 0.3f) h.pitRamp = R_VIOLET;
  }

  /* ambient embers drifting up out of the portals, sparks off bomb fuses and
   * glitter off gold bots */
  emberT -= dt;
  if (emberT <= 0) {
    emberT = 0.07f;
    const Hole &h = holes[ri(0, NHOLE - 1)];
    const float a = rf(0, 6.2832f);
    Part &p = parts[partN < MAX_PARTS ? partN++ : ri(0, MAX_PARTS - 1)];
    p = {h.wx + cosf(a) * h.wr * 0.9f, 0.05f, h.wz + sinf(a) * h.wr * 0.9f, 0, rf(0.5f, 1.2f), 0,
         0, 0, rf(0.025f, 0.05f), RAMP_BASE[h.rimRamp], PF_FLOAT | PF_TWINKLE};
    p.maxLife = p.life = rf(1.2f, 2.2f);
  }
  for (int i = 0; i < NHOLE; i++) {
    const Geo &g = geo[i];
    if (!g.vis || g.hit) continue;
    const Hole &h = holes[i];
    float X, Y;
    if (g.type == MT_BOMB) {
      moleWorld(h, g.antX1, g.antY1, X, Y);
      emit(X, Y, h.wz, 1, 1.6f, GOLD, ORANGE, 0.35f, 0.03f, PF_GRAV, 1.5f);
    } else if (g.type == MT_GOLD && (frameNo & 3) == 0) {
      moleWorld(h, g.cx + rf(-g.mw, g.mw), rf(g.top, g.capEnd + g.mw), X, Y);
      emit(X, Y, h.wz, 1, 0.4f, WHITE, GOLD, 0.6f, 0.04f, PF_TWINKLE | PF_FLOAT);
    }
  }

  /* particles */
  const float GRAV = 13.0f;
  int j = 0;
  for (int i = 0; i < partN; i++) {
    Part p = parts[i];
    p.life -= dt;
    if (p.life <= 0) continue;
    if (p.flags & PF_GRAV) p.vy -= GRAV * dt;
    if (p.flags & PF_FLOAT) p.vy += 0.6f * dt;
    if (p.flags & PF_DRAG) {
      const float d = 1 - 2.2f * dt;
      p.vx *= d;
      p.vy *= d;
      p.vz *= d;
    }
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    p.z += p.vz * dt;
    if ((p.flags & PF_BOUNCE) && p.y < 0) {
      p.y = 0;
      if (p.vy < 0) p.vy = -p.vy * 0.42f;
      p.vx *= 0.7f;
      p.vz *= 0.7f;
    }
    parts[j++] = p;
  }
  partN = j;

  j = 0;
  for (int i = 0; i < waveN; i++) {
    Wave w = waves[i];
    w.life -= dt;
    if (w.life <= 0) continue;
    w.r += w.spd * dt;
    w.spd *= 1 - 1.2f * dt;
    w.w *= 1 + 0.8f * dt;
    waves[j++] = w;
  }
  waveN = j;

  j = 0;
  for (int i = 0; i < ringN; i++) {
    Ring r = rings[i];
    r.life -= dt;
    if (r.life <= 0) continue;
    r.r += 160 * dt;
    rings[j++] = r;
  }
  ringN = j;

  for (Popup &p : pops)
    if (p.life > 0) {
      p.life -= dt;
      p.y -= 70 * dt;
    }

  flashAmt = fmaxf(0, flashAmt - dt * 4);
  shakeAmt = fmaxf(0, shakeAmt - dt * 40);
  shakeX = shakeAmt > 0.5f ? (int)rf(-shakeAmt, shakeAmt) : 0;
  shakeY = shakeAmt > 0.5f ? (int)rf(-shakeAmt, shakeAmt) : 0;
  glitchT -= dt;

  prepareFrame();
}

void tap(int x, int y) {
  /* find the mole under the finger (generous box around the visible body) */
  int hit = -1;
  float bestD = 1e9f;
  for (int i = 0; i < NHOLE; i++) {
    const Mole &m = moles[i];
    const Geo &g = geo[i];
    if (!g.vis || m.st == MS_HIT || m.h < 0.25f) continue;
    const Hole &h = holes[i];
    const float top = g.top - g.mw * 0.6f, bot = h.cy + h.b * 1.1f;
    if (fabsf(x - g.cx) > g.mw * 1.4f || y < top || y > bot) continue;
    const float d = fabsf(x - g.cx) + fabsf(y - (top + bot) * 0.5f) * 0.5f;
    if (d < bestD) {
      bestD = d;
      hit = i;
    }
  }

  switch (st.phase) {
  case Phase::Title:
    if (hit >= 0) whackMole(hit);
    addRing((float)x, (float)y, CYAN);
    startCountdown();
    return;
  case Phase::Countdown:
    addRing((float)x, (float)y, VIOLET);
    return;
  case Phase::Over:
    addRing((float)x, (float)y, CYAN);
    if (st.phaseTime > 1.0f) startCountdown();
    return;
  case Phase::Playing:
    break;
  }

  if (hit >= 0) {
    whackMole(hit);
    addRing((float)x, (float)y, moles[hit].type == MT_BOMB ? RED : WHITE);
    return;
  }
  /* a miss: puff of dust if it was on a hole, and the streak is gone */
  addRing((float)x, (float)y, VIOLET);
  for (int i = 0; i < NHOLE; i++) {
    const Hole &h = holes[i];
    const float dx = (x - h.cx) / h.a, dy = (y - h.cy) / (h.b * 2.2f);
    if (dx * dx + dy * dy < 1.6f) {
      emit(h.wx + (x - h.cx) / h.scale, 0.05f, h.wz, 8, 1.8f, VIOLET, rgb(90, 60, 140), 0.5f, 0.05f,
           PF_GRAV | PF_BOUNCE | PF_DRAG, 1.0f);
      st.combo = 0;
      st.mult = 1;
      break;
    }
  }
}

void render(uint16_t *buf, int stride, int bufX0, int bufY0, int x0, int y0, int x1,
            int y1) {
  /* particles that can touch these rows (shake moves rows by +-shakeY) */
  static uint16_t pidx[MAX_PARTS];
  int pn = 0;
  const int ya = y0 - shakeY - 10, yb = y1 - shakeY + 10;
  for (int i = 0; i < ppN; i++)
    if (pp[i].y + pp[i].r >= ya && pp[i].y - pp[i].r < yb) pidx[pn++] = (uint16_t)i;

  static uint16_t line[W];
  const bool direct = shakeX == 0 && x0 == 0 && x1 == W;
  for (int y = y0; y < y1; y++) {
    uint16_t *dst = buf + (y - bufY0) * stride - bufX0; // indexed by screen x
    uint16_t *row = direct ? dst : line;
    const int sy = clampi(y - shakeY, 0, H - 1);
    renderScene(row, sy, pidx, pn);

    /* glitch: shift and tint random bands */
    if (glitchT > 0) {
      const uint32_t hh = hash3(sy >> 3, frameNo, 77);
      if ((hh & 3) == 0) {
        const int sh = (int)(((int)((hh >> 4) % 41) - 20) * glitchI);
        const int n = W - abs(sh);
        if (sh > 0)
          memmove(row + sh, row, n * 2);
        else if (sh < 0)
          memmove(row, row - sh, n * 2);
        orSpan(row, 0, W, scale565((hh >> 12) & 1 ? CYAN : RED, 70));
      }
    }
    if (flashC) orSpan(row, 0, W, flashC);

    if (!direct)
      for (int x = x0; x < x1; x++) dst[x] = line[clampi(x - shakeX, 0, W - 1)];
  }
}

const Status &status() { return st; }
const Popup *popups() { return pops; }

#ifdef WHACK_SIM
int simMountainRuns() { return mRunsUsed; }
void simSpawn(int i, int type) {
  Mole &m = moles[i];
  m = {};
  m.st = MS_UP;
  m.type = (MoleType)type;
  m.h = 1;
  m.t = 1;
  m.upTime = 1e9f;
  m.blinkT = 5;
}
bool simMoleTarget(int i, int &x, int &y, int &type) {
  const Geo &g = geo[i];
  if (!g.vis || moles[i].st == MS_HIT || moles[i].h < 0.6f) return false;
  x = (int)g.cx;
  y = (int)(g.sphere ? g.sc : (g.top + g.capEnd) * 0.5f);
  type = g.type;
  return true;
}
#endif

} // namespace whack
