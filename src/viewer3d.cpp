/*******************************************************************************
 * VIEWER 3D - software 3-D for the ESP32-4848S040 (480x480 RGB565 + GT911).
 *
 * The meshes come from src/stl/*.stl, converted at build time by
 * scripts/stl2h.py (welded, unit-sized, face + vertex normals, edge list).
 *
 * Each frame (tick, LVGL thread, core 1):
 *   transform   rotate + perspective-project every vertex, light faces or
 *               vertices against a key/fill/rim/specular rig into an
 *               intensity, back-face cull, and work out the screen bounds.
 *   invalidate  only the full-width band of rows the model covers this frame
 *               or covered last frame, then render it right away.
 * Rendering happens inside LVGL's DRAW_MAIN, straight into its strip buffer
 * (so LVGL labels still composite on top and the GDMA flush in main.cpp
 * pushes the strips to the panel). Every strip is split between the cores:
 * core 1 draws the even rows, a helper on core 0 the odd ones.
 *   shaded      scanline rasteriser with a strip-sized 16-bit z-buffer;
 *               depth is 1/z, which is affine in screen space so it is
 *               interpolated exactly; intensity indexes a 256-entry colour
 *               LUT per model (gamma-correct ramp into a specular white).
 *   wireframe   anti-aliased (Wu) lines, additive, dimmed with depth.
 *   hidden line the shaded pass plus depth-tested, alpha-blended edges.
 * Everything hot (mesh copy, projected vertices, z-buffer) is in internal
 * SRAM: PSRAM and flash share a bus with the LCD's continuous scan-out.
 ******************************************************************************/
#include "viewer3d.h"
#include "models_gen.h"
#include "touch_points.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>

extern volatile uint32_t g_flush_us;    // main.cpp: CPU time spent copying to the panel
extern volatile uint32_t g_vsync_count; // main.cpp: panel refreshes so far
bool display_wait_vsync(uint32_t timeout_ms);

namespace {

/* ------------------------------------------------------------- constants -- */
constexpr int W = 480, H = 480;
constexpr float CX = W * 0.5f, CY = H * 0.5f - 18;
constexpr float CAM_D = 3.4f;                       // camera distance, unit-sphere model
constexpr float ZNEAR = CAM_D - 1.05f, ZFAR = CAM_D + 1.05f;
constexpr float DEPTH_MAX = 65000.f;
constexpr float DEPTH_K = DEPTH_MAX / (1.f / ZNEAR - 1.f / ZFAR);
constexpr float FOCAL = 178.f * CAM_D;              // px: unit radius -> ~178 px at centre
constexpr float I_MAX = 1.5f;                       // intensity range of the LUT
constexpr float AUTO_SPIN = 0.55f;                  // rad/s
constexpr int HL_BIAS = 700;                        // hidden-line depth bias (depth units)
constexpr uint32_t M565 = 0x07E0F81F;               // RGB565 spread for SWAR maths

enum Mode { M_WIRE, M_FLAT, M_SMOOTH, M_HIDDEN, N_MODES };
const char *const MODE_NAME[N_MODES] = {"WIREFRAME", "FLAT SHADED", "SMOOTH SHADED", "HIDDEN LINE"};

/* ----------------------------------------------------------------- state -- */
struct PV {          // a projected vertex
  float x, y;        // screen position (pixel centres at +0.5)
  float d;           // depth: (1/z - 1/zfar) * K, bigger = nearer, > 0
  float i;           // smooth-mode intensity, LUT index units
};

// mesh, copied into SRAM on load
float *mv;           // nv * 3
int8_t *mvn, *mfn;   // nv * 3, nt * 3
uint16_t *mtri, *medge;
int nv, nt, ne;
uint16_t lut[256];   // intensity -> colour for the current model
uint32_t wireRGB;    // 0xRRGGBB for the wire colour

// per frame
PV *pv;
uint16_t *vis;           // visible (front-facing) triangle indices in two segments
int16_t *visY0, *visY1;  // (one filled by each core), and their row ranges
int visBase[2], visCnt[2];
uint16_t *faceCol;       // flat colour per triangle (indexed by triangle)
struct ES {              // a wireframe edge, set up for integer DDA
  int16_t r0, r1;        // rows it touches
  int16_t s0, s1;        // pixel range along the major axis (s1 < s0: nothing)
  int32_t m0, dm;        // minor coordinate - 0.5 at s0's centre, 16.16, and per pixel
  int32_t d0, dd;        // depth at s0's centre, 24.8, and per pixel
  uint16_t c;            // RGB565
  uint8_t ymaj;          // 1: steps along y
};
ES *edges;
uint16_t *zbuf;          // same layout as LVGL's draw buffer
size_t zbufPx;
uint16_t bgA[H], bgB[H]; // background gradient, two dither phases per row

int mode = M_FLAT, modelIdx = 0;
bool autoSpin = true;
bool vsyncLock = true; // start each frame on a vsync: no tearing, fps = panel rate
float yaw = 0.6f, pitch = 0.32f, yawVel = AUTO_SPIN, pitchVel = 0, zoom = 1.0f;
float idleT = 10, tLive = 0, popT = 1;
int bandY0 = 0, bandY1 = -1;  // rows invalidated last frame

lv_obj_t *view, *lblMode, *lblFps, *lblName, *lblInfo, *lblHint;

// stats
uint32_t statFrames = 0, statXformUs = 0, statRenderUs = 0;

/* ---------------------------------------------------------- colour maths -- */
/* newlib's floorf/ceilf/fminf live in flash, and flash shares the cache and
 * bus with the PSRAM the LCD streams from: never call them per row/pixel. */
inline int ifloor(float x) { const int i = (int)x; return i - (x < (float)i); }
inline int iceil(float x) { const int i = (int)x; return i + (x > (float)i); }
inline float fmn(float a, float b) { return a < b ? a : b; }
inline float fmx(float a, float b) { return a > b ? a : b; }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline uint16_t rgb565(float r, float g, float b) { // 0..1 sRGB
  return (uint16_t)(((int)(clamp01(r) * 31 + 0.5f) << 11) | ((int)(clamp01(g) * 63 + 0.5f) << 5) |
                    (int)(clamp01(b) * 31 + 0.5f));
}
inline uint32_t spread(uint16_t c) { return (c | ((uint32_t)c << 16)) & M565; }
inline uint16_t unspread(uint32_t c) { return (uint16_t)(c | (c >> 16)); }

/* d + c * a/32, per channel, saturating */
inline uint16_t addSat(uint16_t d, uint32_t c32, uint32_t a5) {
  const uint32_t s = ((c32 * a5) >> 5) & M565;
  uint32_t sum = spread(d) + s;
  const uint32_t ovRB = sum & 0x00010020u, ovG = sum & 0x08000000u;
  sum |= (ovRB - (ovRB >> 5)) | (ovG - (ovG >> 6));
  return unspread(sum & M565);
}
/* bg + (fg - bg) * a/32 */
inline uint16_t blend(uint32_t f32, uint16_t bg, uint32_t a5) {
  const uint32_t b = spread(bg);
  return unspread(((((f32 - b) * a5) >> 5) + b) & M565);
}

inline float srgbToLin(float c) { return c * c * (0.7f + 0.3f * c); } // ~gamma 2.2
inline float linToSrgb(float c) { return sqrtf(clamp01(c)) * (1.08f - 0.08f * c); }

void buildLut(uint32_t rgb) {
  const float br = srgbToLin(((rgb >> 16) & 255) / 255.f);
  const float bg = srgbToLin(((rgb >> 8) & 255) / 255.f);
  const float bb = srgbToLin((rgb & 255) / 255.f);
  for (int i = 0; i < 256; i++) {
    const float I = i * (I_MAX / 255.f);
    float r, g, b;
    if (I <= 1.f) {
      const float sh = 1.f - I; // cool tint in the shadows
      r = br * I + 0.010f * sh;
      g = bg * I + 0.014f * sh;
      b = bb * I + 0.030f * sh;
    } else {
      const float t = (I - 1.f) / (I_MAX - 1.f) * 0.85f; // into a warm white highlight
      r = br + (1.00f - br) * t;
      g = bg + (0.97f - bg) * t;
      b = bb + (0.92f - bb) * t;
    }
    lut[i] = rgb565(linToSrgb(r), linToSrgb(g), linToSrgb(b));
  }
}

void buildBackground() {
  for (int y = 0; y < H; y++) {
    const float t = y / (H - 1.f);
    const float glow = expf(-(t - 0.62f) * (t - 0.62f) * 18.f); // a faint horizon band
    const float r = 0.030f + 0.035f * t + 0.030f * glow;
    const float g = 0.036f + 0.020f * t + 0.030f * glow;
    const float b = 0.075f + 0.060f * t + 0.050f * glow;
    // checkerboard dither between colours a quarter step either side
    bgA[y] = rgb565(r - 0.25f / 31, g - 0.25f / 63, b - 0.25f / 31);
    bgB[y] = rgb565(r + 0.25f / 31, g + 0.25f / 63, b + 0.25f / 31);
  }
}

/* -------------------------------------------------------------- lighting -- */
struct V3 { float x, y, z; };
V3 nrm(V3 v) {
  const float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
  return {v.x / l, v.y / l, v.z / l};
}
const V3 L_KEY = nrm({-0.50f, 0.62f, 0.60f});  // upper left, towards us
const V3 L_FILL = nrm({0.75f, -0.20f, 0.25f}); // lower right
const V3 H_KEY = nrm({L_KEY.x, L_KEY.y, L_KEY.z + 1.f}); // Blinn half vector, view = +z

/* view-space normal -> LUT index (0..255) */
inline float shadeIdx(float nx, float ny, float nz) {
  const float dk = nx * L_KEY.x + ny * L_KEY.y + nz * L_KEY.z;
  const float df = nx * L_FILL.x + ny * L_FILL.y + nz * L_FILL.z;
  float h = nx * H_KEY.x + ny * H_KEY.y + nz * H_KEY.z;
  h = h > 0 ? h : 0;
  h *= h; h *= h; h *= h; h *= h; h *= h; // ^32
  float rim = 1.f - fabsf(nz);
  rim = rim * rim * rim;
  float I = 0.10f + 0.80f * (dk > 0 ? dk : 0) + 0.22f * (df > 0 ? df : 0) + 0.28f * rim + 0.50f * h;
  I *= 255.f / I_MAX;
  return I > 255.f ? 255.f : I;
}

/* ---------------------------------------------------------- the rows job -- */
struct Span {
  uint16_t *buf, *zb;  // LVGL draw buffer and matching z-buffer
  int bw, bx1, by1;    // buffer width and origin (screen coords)
  int cx1, cx2;        // clip columns
  int y0, y1, step;    // rows y0, y0+step, ... <= y1
  inline uint16_t *row(int y) const { return buf + (y - by1) * bw - bx1; }
  inline uint16_t *zrow(int y) const { return zb + (y - by1) * bw - bx1; }
  inline bool hasRow(int y) const { return y >= y0 && y <= y1 && ((y - y0) & (step - 1)) == 0; }
  inline int firstRow(int y) const { // first row >= y on this span's grid
    if (y <= y0) return y0;
    return y + ((y - y0) & (step - 1));
  }
};

/* One triangle into the span's rows. Top-left-ish fill: a pixel is in if its
 * centre lies in [left, right) on the row's centre line. */
template <bool SMOOTH>
void IRAM_ATTR rasterTri(const Span &s, int t, uint16_t flatCol) {
  const uint16_t *ix = mtri + 3 * t;
  const PV *a = &pv[ix[0]], *b = &pv[ix[1]], *c = &pv[ix[2]];
  if (a->y > b->y) { const PV *q = a; a = b; b = q; }
  if (b->y > c->y) { const PV *q = b; b = c; c = q; }
  if (a->y > b->y) { const PV *q = a; a = b; b = q; }
  const float e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
  const float area = e1x * e2y - e2x * e1y;
  if (fabsf(area) < 1e-3f || e2y <= 0) return;
  int ys = s.firstRow(iceil(a->y - 0.5f));
  const int ye = min(s.y1, iceil(c->y - 0.5f) - 1);
  if (ys > ye) return;

  const float inv = 1.f / area;
  const float dd1 = b->d - a->d, dd2 = c->d - a->d;
  const float dzdx = (dd1 * e2y - e1y * dd2) * inv, dzdy = (e1x * dd2 - e2x * dd1) * inv;
  float didx = 0, didy = 0;
  if (SMOOTH) {
    const float di1 = b->i - a->i, di2 = c->i - a->i;
    didx = (di1 * e2y - e1y * di2) * inv;
    didy = (e1x * di2 - e2x * di1) * inv;
  }
  const int32_t dz = (int32_t)(dzdx * 256.f);
  const int32_t di = (int32_t)(didx * 65536.f);
  const float sAC = e2x / e2y;
  const float sAB = e1y > 0 ? e1x / e1y : 0;
  const float hBC = c->y - b->y;
  const float sBC = hBC > 0 ? (c->x - b->x) / hBC : 0;

  for (int y = ys; y <= ye; y += s.step) {
    const float yc = y + 0.5f;
    float xl = a->x + (yc - a->y) * sAC;
    float xr = yc < b->y ? a->x + (yc - a->y) * sAB : b->x + (yc - b->y) * sBC;
    if (xl > xr) { const float q = xl; xl = xr; xr = q; }
    int x0 = iceil(xl - 0.5f), x1 = iceil(xr - 0.5f) - 1;
    if (x0 < s.cx1) x0 = s.cx1;
    if (x1 > s.cx2) x1 = s.cx2;
    if (x0 > x1) continue;
    const float fx = x0 + 0.5f - a->x, fy = yc - a->y;
    int32_t z = (int32_t)((a->d + dzdx * fx + dzdy * fy) * 256.f);
    uint16_t *px = s.row(y), *zr = s.zrow(y);
    if (SMOOTH) {
      int32_t i = (int32_t)((a->i + didx * fx + didy * fy) * 65536.f);
      for (int x = x0; x <= x1; x++) {
        const uint16_t zz = (uint16_t)(z >> 8);
        if (zz > zr[x]) {
          zr[x] = zz;
          int li = i >> 16;
          li = li < 0 ? 0 : (li > 255 ? 255 : li);
          px[x] = lut[li];
        }
        z += dz;
        i += di;
      }
    } else {
      for (int x = x0; x <= x1; x++) {
        const uint16_t zz = (uint16_t)(z >> 8);
        if (zz > zr[x]) {
          zr[x] = zz;
          px[x] = flatCol;
        }
        z += dz;
      }
    }
  }
}

/* Anti-aliased (Wu) lines. Each edge is set up once per frame (transform):
 * ordered along its major axis, with the minor coordinate and the depth as
 * 16.16 / 24.8 fixed point at the first pixel centre plus a per-pixel step,
 * so a span only does integer DDA. HIDDEN: depth-tested and alpha-blended
 * over the shaded surface; otherwise additive glow. */
template <bool HIDDEN>
inline void IRAM_ATTR plot(const Span &s, int x, int y, uint32_t c32, uint32_t a5, int32_t d) {
  if (x < s.cx1 || x > s.cx2 || a5 == 0) return;
  uint16_t *p = s.row(y) + x;
  if (HIDDEN) {
    if ((d >> 8) + HL_BIAS < (int32_t)s.zrow(y)[x]) return;
    *p = blend(c32, *p, a5);
  } else {
    *p = addSat(*p, c32, a5);
  }
}

template <bool HIDDEN>
void IRAM_ATTR drawEdge(const Span &s, const ES &e, uint32_t aMax) {
  const uint32_t c32 = spread(e.c);
  if (e.ymaj) { // two pixels per row
    const int ys = s.firstRow(max<int>(e.s0, s.y0)), ye = min<int>(e.s1, s.y1);
    if (ys > ye) return;
    int32_t m = e.m0 + (ys - e.s0) * e.dm, d = e.d0 + (ys - e.s0) * e.dd;
    const int32_t dm = e.dm * s.step, dd = e.dd * s.step;
    for (int y = ys; y <= ye; y += s.step) {
      const int ix = m >> 16;
      const uint32_t a = ((uint32_t)(m & 0xFFFF) * aMax) >> 16;
      plot<HIDDEN>(s, ix, y, c32, aMax - a, d);
      plot<HIDDEN>(s, ix + 1, y, c32, a, d);
      m += dm;
      d += dd;
    }
  } else { // two pixels per column; only the columns whose rows are in this span
    int cs = max<int>(e.s0, s.cx1), ce = min<int>(e.s1, s.cx2);
    if (e.dm != 0) {
      int ca = e.s0 + (((s.y0 - 1) << 16) - e.m0) / e.dm;
      int cb = e.s0 + (((s.y1 + 1) << 16) - e.m0) / e.dm;
      if (ca > cb) { const int t = ca; ca = cb; cb = t; }
      cs = max(cs, ca - 1);
      ce = min(ce, cb + 1);
    }
    if (cs > ce) return;
    int32_t m = e.m0 + (cs - e.s0) * e.dm, d = e.d0 + (cs - e.s0) * e.dd;
    for (int x = cs; x <= ce; x++) {
      const int iy = m >> 16;
      const uint32_t a = ((uint32_t)(m & 0xFFFF) * aMax) >> 16;
      if (s.hasRow(iy)) plot<HIDDEN>(s, x, iy, c32, aMax - a, d);
      if (s.hasRow(iy + 1)) plot<HIDDEN>(s, x, iy + 1, c32, a, d);
      m += e.dm;
      d += e.dd;
    }
  }
}

void IRAM_ATTR drawSpan(const Span &s) {
  if (s.y0 > s.y1) return;
  const int n = s.cx2 - s.cx1 + 1;
  for (int y = s.y0; y <= s.y1; y += s.step) { // background
    uint16_t *row = s.row(y);
    // dither phase by screen parity; pairs of pixels as one 32-bit store
    const uint32_t even = (y & 1) ? bgB[y] : bgA[y], odd = (y & 1) ? bgA[y] : bgB[y];
    int x = s.cx1;
    if ((uintptr_t)(row + x) & 2) { row[x] = (x & 1) ? odd : even; x++; }
    uint32_t *r32 = (uint32_t *)(row + x);
    const uint32_t pair = (x & 1) ? (odd | (even << 16)) : (even | (odd << 16));
    const int pairs = (s.cx2 - x + 1) >> 1;
    for (int k = 0; k < pairs; k++) r32[k] = pair;
    x += pairs * 2;
    if (x <= s.cx2) row[x] = (x & 1) ? odd : even;
  }
  if (mode != M_WIRE) {
    for (int y = s.y0; y <= s.y1; y += s.step) memset(s.zrow(y) + s.cx1, 0, n * 2);
    for (int seg = 0; seg < 2; seg++) {
      const int k0 = visBase[seg], k1 = k0 + visCnt[seg];
      for (int k = k0; k < k1; k++) {
        if (visY0[k] > s.y1 || visY1[k] < s.y0) continue;
        const int t = vis[k];
        if (mode == M_SMOOTH) rasterTri<true>(s, t, 0);
        else rasterTri<false>(s, t, faceCol[t]);
      }
    }
  }
  if (mode == M_WIRE || mode == M_HIDDEN) {
    const uint32_t aMax = mode == M_WIRE ? 32 : 20;
    for (int k = 0; k < ne; k++) {
      const ES &e = edges[k];
      if (e.r0 > s.y1 || e.r1 < s.y0) continue;
      if (mode == M_WIRE) drawEdge<false>(s, e, aMax);
      else drawEdge<true>(s, e, aMax);
    }
  }
}

/* ------------------------------------------------------------- transform -- */
struct Xform {
  float r[9];        // rotation, row major
  float f;           // focal length, px
  bool smooth, faces, edges, depthCue;
  float wr, wg, wb;  // wire colour 0..1
  float bb[2][2];    // per core: min y, max y
} xf;

/* rotate + project vertices [i0, i1); `core` picks the bounds slot */
void IRAM_ATTR xformVerts(int i0, int i1, int core) {
  const float *r = xf.r;
  float miny = 1e9f, maxy = -1e9f;
  for (int i = i0; i < i1; i++) {
    const float x = mv[3 * i], y = mv[3 * i + 1], z = mv[3 * i + 2];
    const float X = r[0] * x + r[1] * y + r[2] * z;
    const float Y = r[3] * x + r[4] * y + r[5] * z;
    const float Z = r[6] * x + r[7] * y + r[8] * z;
    const float iz = 1.f / (CAM_D - Z);
    PV &p = pv[i];
    p.x = CX + xf.f * X * iz;
    p.y = CY - xf.f * Y * iz;
    p.d = (iz - 1.f / ZFAR) * DEPTH_K;
    miny = fmn(miny, p.y);
    maxy = fmx(maxy, p.y);
    if (xf.smooth) {
      const float nx = mvn[3 * i], ny = mvn[3 * i + 1], nz = mvn[3 * i + 2];
      p.i = shadeIdx((r[0] * nx + r[1] * ny + r[2] * nz) * (1 / 127.f),
                     (r[3] * nx + r[4] * ny + r[5] * nz) * (1 / 127.f),
                     (r[6] * nx + r[7] * ny + r[8] * nz) * (1 / 127.f));
    }
  }
  xf.bb[core][0] = miny;
  xf.bb[core][1] = maxy;
}

/* cull + light triangles [t0, t1) into visible-list segment `seg` */
void IRAM_ATTR xformFaces(int t0, int t1, int seg) {
  const float *r = xf.r;
  int n = visBase[seg];
  for (int t = t0; t < t1; t++) {
    const PV &a = pv[mtri[3 * t]], &b = pv[mtri[3 * t + 1]], &c = pv[mtri[3 * t + 2]];
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area >= 0) continue; // y points down: front faces wind clockwise on screen
    vis[n] = t;
    visY0[n] = (int16_t)ifloor(fmn(a.y, fmn(b.y, c.y)));
    visY1[n] = (int16_t)iceil(fmx(a.y, fmx(b.y, c.y)));
    n++;
    if (!xf.smooth) {
      const float nx = mfn[3 * t], ny = mfn[3 * t + 1], nz = mfn[3 * t + 2];
      faceCol[t] = lut[(int)shadeIdx((r[0] * nx + r[1] * ny + r[2] * nz) * (1 / 127.f),
                                     (r[3] * nx + r[4] * ny + r[5] * nz) * (1 / 127.f),
                                     (r[6] * nx + r[7] * ny + r[8] * nz) * (1 / 127.f))];
    }
  }
  visCnt[seg] = n - visBase[seg];
}

/* fixed-point line setup for edges [k0, k1) */
void IRAM_ATTR xformEdges(int k0, int k1) {
  const uint16_t hl = rgb565(0.92f, 0.96f, 1.0f);
  for (int k = k0; k < k1; k++) {
    const PV *p = &pv[medge[2 * k]], *q = &pv[medge[2 * k + 1]];
    ES &e = edges[k];
    float dx = q->x - p->x, dy = q->y - p->y;
    e.ymaj = fabsf(dy) >= fabsf(dx);
    if (e.ymaj) {
      if (dy < 0) { const PV *t = p; p = q; q = t; dx = -dx; dy = -dy; }
      e.s0 = (int16_t)iceil(p->y - 0.5f);
      e.s1 = (int16_t)(iceil(q->y - 0.5f) - 1);
      const float inv = dy > 1e-3f ? 1.f / dy : 0.f;
      const float sm = dx * inv, sd = (q->d - p->d) * inv, t = e.s0 + 0.5f - p->y;
      e.m0 = (int32_t)((p->x + t * sm - 0.5f) * 65536.f);
      e.dm = (int32_t)(sm * 65536.f);
      e.d0 = (int32_t)((p->d + t * sd) * 256.f);
      e.dd = (int32_t)(sd * 256.f);
      e.r0 = e.s0;
      e.r1 = e.s1;
    } else { // dx > |dy| >= 0 here, so dx > 0
      if (dx < 0) { const PV *t = p; p = q; q = t; dx = -dx; dy = -dy; }
      e.s0 = (int16_t)iceil(p->x - 0.5f);
      e.s1 = (int16_t)(iceil(q->x - 0.5f) - 1);
      const float inv = 1.f / dx;
      const float sm = dy * inv, sd = (q->d - p->d) * inv, t = e.s0 + 0.5f - p->x;
      e.m0 = (int32_t)((p->y + t * sm - 0.5f) * 65536.f);
      e.dm = (int32_t)(sm * 65536.f);
      e.d0 = (int32_t)((p->d + t * sd) * 256.f);
      e.dd = (int32_t)(sd * 256.f);
      e.r0 = (int16_t)(ifloor(fmn(p->y, q->y) - 0.5f));
      e.r1 = (int16_t)(ifloor(fmx(p->y, q->y) - 0.5f) + 1);
    }
    if (xf.depthCue) { // far edges fade to a sixth
      float n = (p->d + q->d) * (0.5f / DEPTH_MAX);
      n = 0.16f + 0.84f * n * n;
      e.c = rgb565(xf.wr * n, xf.wg * n, xf.wb * n);
    } else {
      e.c = hl;
    }
  }
}

/* ------------------------------------------------------- two-core render -- */
enum Job { J_SPAN, J_VERTS, J_FACES_EDGES };
TaskHandle_t helperHandle = nullptr, lvglHandle = nullptr;
volatile int helperJob = J_SPAN;
Span helperSpan;

/* half 0: first half of the faces and edges (core 1), half 1: the rest (core 0) */
void IRAM_ATTR facesEdges(int half) {
  if (xf.faces) xformFaces(half ? nt / 2 : 0, half ? nt : nt / 2, half);
  else visCnt[half] = 0;
  if (xf.edges) xformEdges(half ? ne / 2 : 0, half ? ne : ne / 2);
}

void helperTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    switch (helperJob) {
    case J_SPAN: drawSpan(helperSpan); break;
    case J_VERTS: xformVerts(nv / 2, nv, 1); break;
    case J_FACES_EDGES: facesEdges(1); break;
    }
    xTaskNotifyGive(lvglHandle);
  }
}

inline void helperStart(int job) {
  helperJob = job;
  xTaskNotifyGive(helperHandle);
}
inline void helperWait() { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); }

void render(lv_draw_ctx_t *ctx) {
  const uint32_t t0 = micros();
  const lv_area_t *clip = ctx->clip_area, *ba = ctx->buf_area;
  Span s;
  s.buf = (uint16_t *)ctx->buf;
  s.bw = lv_area_get_width(ba);
  s.bx1 = ba->x1;
  s.by1 = ba->y1;
  // the z-buffer shares the draw buffer's layout; refuse anything bigger
  const int y1 = min<int>(clip->y2, H - 1);
  if ((size_t)(y1 - ba->y1 + 1) * s.bw > zbufPx) return;
  s.zb = zbuf;
  s.cx1 = max<int>(clip->x1, 0);
  s.cx2 = min<int>(clip->x2, W - 1);
  s.y0 = max<int>(clip->y1, 0);
  s.y1 = y1;
  if (s.y1 - s.y0 >= 3) { // interleave rows: even to core 1, odd to core 0
    s.step = 2;
    helperSpan = s;
    helperSpan.y0 = s.y0 + 1;
    helperStart(J_SPAN);
    drawSpan(s);
    helperWait();
  } else {
    s.step = 1;
    drawSpan(s);
  }
  statRenderUs += micros() - t0;
}

void viewEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_COVER_CHECK) {
    ((lv_cover_check_info_t *)lv_event_get_param(e))->res = LV_COVER_RES_COVER;
  } else if (code == LV_EVENT_DRAW_MAIN) {
    render(lv_event_get_draw_ctx(e));
  }
}

void transform() {
  const uint32_t t0 = micros();
  const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
  // R = Rx(pitch) * Ry(yaw)
  const float r[9] = {cy, 0, sy, sp * sy, cp, -sp * cy, -cp * sy, sp, cp * cy};
  memcpy(xf.r, r, sizeof(r));
  // pop-in when a model appears: ease-out-back on the scale
  float pop = 1;
  if (popT < 1) {
    const float t = popT - 1, k = 1.70158f;
    pop = 0.55f + 0.45f * (1 + t * t * ((k + 1) * t + k));
  }
  xf.f = FOCAL * zoom * pop;
  xf.smooth = mode == M_SMOOTH;
  xf.faces = mode != M_WIRE;
  xf.edges = mode == M_WIRE || mode == M_HIDDEN;
  xf.depthCue = mode == M_WIRE;
  xf.wr = ((wireRGB >> 16) & 255) / 255.f;
  xf.wg = ((wireRGB >> 8) & 255) / 255.f;
  xf.wb = (wireRGB & 255) / 255.f;
  visBase[0] = 0;
  visBase[1] = nt / 2;

  helperStart(J_VERTS); // both cores: vertices, then faces + edges
  xformVerts(0, nv / 2, 0);
  helperWait();
  helperStart(J_FACES_EDGES);
  facesEdges(0);
  helperWait();

  // the band of rows to redraw: this frame's bounds plus last frame's
  const float miny = fmn(xf.bb[0][0], xf.bb[1][0]), maxy = fmx(xf.bb[0][1], xf.bb[1][1]);
  const int y0 = max(0, ifloor(miny) - 2), y1 = min(H - 1, iceil(maxy) + 2);
  lv_area_t a = {0, (lv_coord_t)min(y0, bandY0), W - 1, (lv_coord_t)max(y1, bandY1)};
  if (bandY1 < bandY0) a.y1 = y0, a.y2 = y1;
  if (a.y1 <= a.y2) lv_obj_invalidate_area(view, &a);
  bandY0 = y0;
  bandY1 = y1;
  statXformUs += micros() - t0;
}

/* ----------------------------------------------------------------- model -- */
void loadModel(int idx) {
  modelIdx = (idx % N_MODELS + N_MODELS) % N_MODELS;
  const Model &m = MODELS[modelIdx];
  nv = m.nv;
  nt = m.nt;
  ne = m.ne;
  memcpy(mv, m.v, nv * 3 * sizeof(float));
  memcpy(mvn, m.vn, nv * 3);
  memcpy(mfn, m.fn, nt * 3);
  memcpy(mtri, m.tri, nt * 3 * sizeof(uint16_t));
  memcpy(medge, m.edge, ne * 2 * sizeof(uint16_t));
  buildLut(m.color);
  // wire colour: the material pushed towards a bright neon
  const float r = ((m.color >> 16) & 255) / 255.f, g = ((m.color >> 8) & 255) / 255.f,
              b = (m.color & 255) / 255.f;
  const float mx = fmaxf(r, fmaxf(g, b));
  wireRGB = ((uint32_t)(255 * (0.35f + 0.65f * r / mx)) << 16) |
            ((uint32_t)(255 * (0.35f + 0.65f * g / mx)) << 8) |
            (uint32_t)(255 * (0.35f + 0.65f * b / mx));
  popT = 0;
  lv_label_set_text_fmt(lblName, LV_SYMBOL_LEFT "      %s      " LV_SYMBOL_RIGHT, m.name);
  lv_label_set_text_fmt(lblInfo, "%d TRIANGLES  " LV_SYMBOL_BULLET "  %d VERTICES  " LV_SYMBOL_BULLET
                                 "  %d/%d", nt, nv, modelIdx + 1, N_MODELS);
  Serial.printf("[viewer3d] model %s: %d verts, %d tris, %d edges\n", m.name, nv, nt, ne);
}

void setMode(int m) {
  mode = (m % N_MODES + N_MODES) % N_MODES;
  lv_label_set_text(lblMode, MODE_NAME[mode]);
}

/* ----------------------------------------------------------------- input -- */
struct Gesture {
  bool down = false, dragging = false;
  int startX = 0, startY = 0, lastX = 0, lastY = 0, fingers = 0;
  uint32_t startMs = 0;
  float pinchDist = 0;
} g;

void onTap(int x, int y) {
  if (y > H - 110 && x < W / 3) loadModel(modelIdx - 1);
  else if (y > H - 110 && x > 2 * W / 3) loadModel(modelIdx + 1);
  else setMode(mode + 1);
  lv_obj_add_flag(lblHint, LV_OBJ_FLAG_HIDDEN);
}

void processTouch(float dt) {
  const uint32_t now = millis();
  const int n = touch_count;
  if (n > 0) {
    idleT = 0;
    const int x = touch_pts_x[0], y = touch_pts_y[0];
    if (!g.down) {
      g.down = true;
      g.dragging = false;
      g.startX = g.lastX = x;
      g.startY = g.lastY = y;
      g.startMs = now;
      g.fingers = n;
      g.pinchDist = 0;
    }
    if (n > g.fingers) g.fingers = n;
    if (n >= 2) { // pinch zoom
      const float dx = touch_pts_x[1] - touch_pts_x[0], dy = touch_pts_y[1] - touch_pts_y[0];
      const float d = sqrtf(dx * dx + dy * dy);
      if (g.pinchDist > 20 && d > 20) zoom = fminf(2.2f, fmaxf(0.45f, zoom * d / g.pinchDist));
      g.pinchDist = d;
      g.dragging = true;
      yawVel *= 0.8f;
      pitchVel *= 0.8f;
    } else {
      g.pinchDist = 0;
      if (abs(x - g.startX) + abs(y - g.startY) > 12) g.dragging = true;
      if (g.dragging && g.fingers == 1 && dt > 0) {
        const float ky = 0.0105f, kp = 0.0085f; // rad per pixel
        const float dyaw = (x - g.lastX) * ky, dpitch = (y - g.lastY) * kp;
        yaw += dyaw;
        pitch = fminf(1.3f, fmaxf(-1.3f, pitch + dpitch));
        // a smoothed velocity, carried on as inertia when the finger lifts
        const float iv = fminf(1.f, dt * 12.f);
        yawVel += (dyaw / dt - yawVel) * iv;
        pitchVel += (dpitch / dt - pitchVel) * iv;
      }
    }
    g.lastX = x;
    g.lastY = y;
  } else if (g.down) {
    g.down = false;
    if (!g.dragging && g.fingers == 1 && now - g.startMs < 400) onTap(g.startX, g.startY);
    yawVel = fminf(9.f, fmaxf(-9.f, yawVel));
    pitchVel = fminf(4.f, fmaxf(-4.f, pitchVel));
  }
}

void animate(float dt) {
  tLive += dt;
  idleT += dt;
  if (popT < 1) popT = fminf(1.f, popT + dt * 2.4f);
  if (g.down) return; // the finger owns the rotation
  // inertia decays into the auto spin (in whichever direction it was going)
  const float target = autoSpin ? (yawVel < -0.05f ? -AUTO_SPIN : AUTO_SPIN) : 0;
  const float k = 1.f - expf(-dt * 1.6f);
  yawVel += (target - yawVel) * k;
  pitchVel -= pitchVel * k * 1.5f;
  yaw += yawVel * dt;
  pitch += pitchVel * dt;
  if (idleT > 2.5f) { // drift back to a gently bobbing three-quarter view
    const float rest = 0.30f + 0.10f * sinf(tLive * 0.45f);
    pitch += (rest - pitch) * (1.f - expf(-dt * 0.8f));
  }
  pitch = fminf(1.3f, fmaxf(-1.3f, pitch));
  if (yaw > 6.2831853f) yaw -= 6.2831853f;
  if (yaw < 0) yaw += 6.2831853f;
}

/* ------------------------------------------------------------------ tick -- */
void logStats() {
  static uint32_t last = millis(), lFrames = 0, lX = 0, lR = 0, lF = 0, lV = 0;
  const uint32_t now = millis();
  if (now - last < 1000) return;
  const uint32_t fr = statFrames - lFrames;
  const float s = (now - last) / 1000.f;
  const float fps = fr / s;
  const float xms = fr ? (statXformUs - lX) / 1000.f / fr : 0;
  const float rms = fr ? (statRenderUs - lR) / 1000.f / fr : 0;
  lv_label_set_text_fmt(lblFps, "%d FPS", (int)(fps + 0.5f));
  Serial.printf("[viewer3d] %.1f fps | xform %.2f ms  raster %.2f ms  cpu-flush %.2f ms/frame | "
                "%d/%d tris visible, band %d rows, %s, panel %.1f Hz%s\n",
                fps, xms, rms, fr ? (g_flush_us - lF) / 1000.f / fr : 0, visCnt[0] + visCnt[1], nt,
                bandY1 - bandY0 + 1, MODE_NAME[mode], (g_vsync_count - lV) / s,
                vsyncLock ? " (vsync lock)" : "");
  lV = g_vsync_count;
  last = now;
  lFrames = statFrames;
  lX = statXformUs;
  lR = statRenderUs;
  lF = g_flush_us;
}

void tick(lv_timer_t *) {
  static uint32_t lastUs = micros();
  const uint32_t nowUs = micros();
  float dt = (nowUs - lastUs) * 1e-6f;
  lastUs = nowUs;
  if (dt > 0.1f) dt = 0.1f;
  processTouch(dt);
  animate(dt);
  if (idleT > 6 && !lv_obj_has_flag(lblHint, LV_OBJ_FLAG_HIDDEN) && tLive > 8)
    lv_obj_add_flag(lblHint, LV_OBJ_FLAG_HIDDEN);
  transform();
  // Draw top to bottom right behind the vsync, so the scan-out never
  // overtakes the rows being rewritten. (No hook found -> just draw.)
  if (vsyncLock && !display_wait_vsync(100)) vsyncLock = false;
  lv_refr_now(NULL);
  statFrames++;
  logStats();
}

/* -------------------------------------------------------------------- UI -- */
lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t hex, int space, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
  lv_obj_set_style_text_letter_space(l, space, 0);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(l, txt);
  return l;
}

void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  view = lv_obj_create(scr);
  lv_obj_remove_style_all(view);
  lv_obj_set_size(view, W, H);
  lv_obj_set_pos(view, 0, 0);
  lv_obj_clear_flag(view, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(view, viewEvent, LV_EVENT_ALL, NULL);

  lblMode = mkLabel(scr, &lv_font_montserrat_16, 0xE8F0FF, 4, "");
  lv_obj_align(lblMode, LV_ALIGN_TOP_MID, 0, 22);
  lblFps = mkLabel(scr, &lv_font_montserrat_12, 0x7F8CA8, 1, "");
  lv_obj_align(lblFps, LV_ALIGN_TOP_RIGHT, -22, 25);
  lblName = mkLabel(scr, &lv_font_montserrat_20, 0xF2F6FF, 5, "");
  lv_obj_align(lblName, LV_ALIGN_BOTTOM_MID, 0, -46);
  lblInfo = mkLabel(scr, &lv_font_montserrat_12, 0x8A97B4, 2, "");
  lv_obj_align(lblInfo, LV_ALIGN_BOTTOM_MID, 0, -24);
  lblHint = mkLabel(scr, &lv_font_montserrat_12, 0x6F7C98, 2,
                    "TAP: MODE  " LV_SYMBOL_BULLET "  DRAG: SPIN  " LV_SYMBOL_BULLET "  PINCH: ZOOM");
  lv_obj_align(lblHint, LV_ALIGN_TOP_MID, 0, 48);
}

template <typename T>
T *alloc(size_t n, const char *what) {
  T *p = (T *)heap_caps_calloc(n, sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) {
    Serial.printf("[viewer3d] %s: no SRAM left, falling back to PSRAM (slower)\n", what);
    p = (T *)heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM);
  }
  if (!p) Serial.printf("[viewer3d] %s: allocation failed!\n", what);
  return p;
}

} // namespace

void viewer3d_command(int c) {
  switch (c) {
  case 'm': setMode(mode + 1); break;
  case 'n': loadModel(modelIdx + 1); break;
  case 'p': loadModel(modelIdx - 1); break;
  case 'a': autoSpin = !autoSpin; break;
  case 'v': vsyncLock = !vsyncLock; break;
  default: return;
  }
  Serial.printf("[viewer3d] '%c' -> %s, %s, auto spin %s\n", c, MODELS[modelIdx].name,
                MODE_NAME[mode], autoSpin ? "on" : "off");
}

void viewer3d_start() {
  int maxV = 0, maxT = 0, maxE = 0;
  for (int i = 0; i < N_MODELS; i++) {
    maxV = max<int>(maxV, MODELS[i].nv);
    maxT = max<int>(maxT, MODELS[i].nt);
    maxE = max<int>(maxE, MODELS[i].ne);
  }
  Serial.printf("[viewer3d] %d models; SRAM free before alloc: %u\n", N_MODELS,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  // the z-buffer mirrors LVGL's draw buffer (same pixel count, same layout)
  lv_disp_t *disp = lv_disp_get_default();
  zbufPx = disp->driver->draw_buf->size;
  zbuf = alloc<uint16_t>(zbufPx, "z-buffer");
  mv = alloc<float>(maxV * 3, "vertices");
  mvn = alloc<int8_t>(maxV * 3, "vertex normals");
  mfn = alloc<int8_t>(maxT * 3, "face normals");
  mtri = alloc<uint16_t>(maxT * 3, "triangles");
  medge = alloc<uint16_t>(maxE * 2, "edges");
  pv = alloc<PV>(maxV, "projected vertices");
  vis = alloc<uint16_t>(maxT, "visible list");
  visY0 = alloc<int16_t>(maxT, "visible rows");
  visY1 = alloc<int16_t>(maxT, "visible rows");
  faceCol = alloc<uint16_t>(maxT, "face colours");
  edges = alloc<ES>(maxE, "edge setup");
  Serial.printf("[viewer3d] SRAM free after alloc: %u\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  buildBackground();
  buildUI();
  setMode(M_FLAT);
  loadModel(0);
  popT = 1;

  lvglHandle = xTaskGetCurrentTaskHandle();
  xTaskCreatePinnedToCore(helperTask, "raster+", 4096, NULL, configMAX_PRIORITIES - 3, &helperHandle, 0);
  lv_timer_create(tick, 1, NULL);
}
