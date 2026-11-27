/* STARLANE - world rendering and the flight HUD. */
#include "sl.h"

namespace sl {

static uint8_t *planetBmp[NPLANET];
static int planetSz[NPLANET];
static uint8_t *sunBmp;
static int sunSz;
static const int SUN_CORONA = 232;

static float fbm(float x, float y, uint32_t s) {
  return 0.55f * vnoise(x, y, s) + 0.3f * vnoise(x * 2.1f, y * 2.1f, s + 1) +
         0.15f * vnoise(x * 4.3f, y * 4.3f, s + 2);
}

/* ------------------------------------------------------- planet caches */
static void buildPlanet(int i) {
  const PlanetDef &P = PLANETS[i];
  int r = P.r, sz = 2 * r + 2;
  uint8_t *b = (uint8_t *)platformAllocLarge((size_t)sz * sz);
  planetBmp[i] = b;
  planetSz[i] = sz;
  if (!b) return;
  memset(b, TRANSP, (size_t)sz * sz);
  float lx = -P.x, ly = -P.y, ll = lenf(lx, ly);
  lx /= ll;
  ly /= ll;
  const float Lx = lx * 0.85f, Ly = ly * 0.85f, Lz = 0.53f;
  const float c = r + 1.0f;
  static const uint8_t OC[4] = {C_B1, C_B2, C_B3, C_B4}, LA[4] = {C_GR1, C_GR2, C_GR3, C_GR4};
  static const uint8_t IC[4] = {C_G3, C_G4, C_G5, C_WHITE}, DS[4] = {C_O1, C_O2, C_O3, C_O4};
  static const uint8_t GA[4] = {C_O1, C_O2, C_O3, C_O4}, GB[4] = {C_O2, C_O3, C_O4, C_O5};
  static const uint8_t RS[4] = {C_R1, C_R2, C_R3, C_R4};
  static const uint8_t VA[4] = {C_V1, C_V2, C_V3, C_V4}, VB[4] = {C_V2, C_V3, C_V4, C_V5};
  static const uint8_t RK[4] = {C_G1, C_G2, C_G3, C_G4}, AS[4] = {C_R1, C_O1, C_O2, C_O3};
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++) {
      float nx = (x + 0.5f - c) / r, ny = (y + 0.5f - c) / r, d2 = nx * nx + ny * ny;
      if (d2 > 1) continue;
      float nz = sqrtf(1 - d2);
      float lit = clampf((nx * Lx + ny * Ly + nz * Lz) * 1.1f + 0.05f, 0, 1);
      float lon = atan2f(nx, nz), lat = asinf(clampf(ny, -1, 1));
      int li = clampi((int)(lit * 3.999f + bayer(x, y) / 16.0f - 0.5f), 0, 3);
      uint8_t col;
      switch (P.type) {
      case P_TERRAN: {
        float n = fbm(lon * 1.6f + 3, lat * 2.4f + 7, 11);
        float cl = fbm(lon * 2.5f + 9, lat * 5.0f, 23);
        if (fabsf(lat) > 1.15f - n * 0.3f) col = IC[li];
        else if (n > 0.56f) col = n > 0.7f ? DS[li] : LA[li];
        else col = OC[li];
        if (cl > 0.63f) col = IC[li];
        if (lit < 0.06f && n > 0.56f && hash2(x, y, 5) % 19 == 0) col = C_Y2;
        if (d2 > 0.88f && lit > 0.25f && bayer(x, y) < 7) col = C_B5;
        break;
      }
      case P_GAS: {
        float band = sinf(lat * 10 + fbm(lon * 1.5f, lat * 7, 31) * 3.0f);
        col = band > 0 ? GB[li] : GA[li];
        float sx = lon - 0.5f, sy = (lat - 0.35f) * 2.2f;
        if (sx * sx + sy * sy < 0.03f) col = RS[li];
        break;
      }
      case P_ICE: col = fbm(lon * 3, lat * 3, 41) > 0.55f ? VB[li] : VA[li]; break;
      case P_ROCK: col = RK[clampi(li - (fbm(lon * 4, lat * 4, 51) < 0.42f ? 1 : 0), 0, 3)]; break;
      default: col = AS[clampi(li - (fbm(lon * 3, lat * 3, 61) > 0.6f ? 1 : 0), 0, 3)]; break;
      }
      b[y * sz + x] = col;
    }
}

static void buildSun() {
  int R = SUN_CORONA, sz = 2 * R + 2;
  sunSz = sz;
  sunBmp = (uint8_t *)platformAllocLarge((size_t)sz * sz);
  if (!sunBmp) return;
  memset(sunBmp, TRANSP, (size_t)sz * sz);
  const float c = R + 1.0f;
  static const uint8_t SR[5] = {C_O3, C_O4, C_Y2, C_Y3, C_WHITE};
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++) {
      float dx = x + 0.5f - c, dy = y + 0.5f - c, d = lenf(dx, dy);
      if (d <= SUN_R) {
        float t = d / SUN_R;
        float n = fbm(dx * 0.06f, dy * 0.06f, 71);
        float v = 1 - t * t * 0.75f + (n - 0.5f) * 0.4f;
        int li = clampi((int)(v * 4.99f + bayer(x, y) / 16.0f - 0.5f), 0, 4);
        sunBmp[y * sz + x] = SR[li];
      } else if (d < R) {
        float t = (d - SUN_R) / (R - SUN_R);
        float n = fbm(atan2f(dy, dx) * 2.5f + 4, d * 0.02f, 81);
        float dens = (1 - t) * (1 - t) * (0.6f + 0.8f * n);
        if (bayer(x, y) < (int)(dens * 18)) sunBmp[y * sz + x] = dens > 0.5f ? C_O4 : (dens > 0.28f ? C_O3 : C_O2);
      }
    }
}

void buildRenderCaches() {
  for (int i = 0; i < NPLANET; i++) buildPlanet(i);
  buildSun();
}

/* ---------------------------------------------------------- background */
void drawStarfield(float cx, float cy, float vx, float vy, bool streak) {
  static const float PAR[3] = {0.10f, 0.28f, 0.6f};
  static const int CELL[3] = {22, 30, 44};
  static const uint8_t COL[3][4] = {{C_G1, C_G2, C_G2, C_B2},
                                    {C_G2, C_G3, C_B3, C_G3},
                                    {C_G4, C_G5, C_WHITE, C_Y3}};
  for (int L = 0; L < 3; L++) {
    float ox = cx * PAR[L], oy = cy * PAR[L];
    int cs = CELL[L];
    int gx0 = (int)floorf((ox - CX) / cs) - 1, gx1 = (int)floorf((ox + CX) / cs) + 1;
    int gy0 = (int)floorf((oy - CY) / cs) - 1, gy1 = (int)floorf((oy + CY) / cs) + 1;
    float sk = streak ? PAR[L] * 0.045f : 0;
    for (int gy = gy0; gy <= gy1; gy++)
      for (int gx = gx0; gx <= gx1; gx++) {
        uint32_t h = hash2(gx, gy, 11 + L);
        if (L == 2 && (h & 3)) continue;
        int sx = (int)(gx * cs + ((h >> 8) & 255) * cs / 256 - ox) + CX;
        int sy = (int)(gy * cs + ((h >> 16) & 255) * cs / 256 - oy) + CY;
        uint8_t c = COL[L][h >> 30];
        if (streak) {
          int ex = sx - (int)(vx * sk), ey = sy - (int)(vy * sk);
          line(sx, sy, ex, ey, c);
        } else {
          pset(sx, sy, c);
          if (L == 2 && (h & 0x30) == 0x30 && ((frameNo + h) & 63) < 3) {
            pset(sx - 1, sy, C_G3);
            pset(sx + 1, sy, C_G3);
          }
        }
      }
  }
}

static void drawNebula(float cx, float cy) {
  if (lenf(cx - NEB_X, cy - NEB_Y) > NEB_R + 420) return;
  for (int by = 0; by < LH; by += 4)
    for (int bx = 0; bx < LW; bx += 4) {
      float wx = cx * 0.9f + NEB_X * 0.1f + (bx - CX), wy = cy * 0.9f + NEB_Y * 0.1f + (by - CY);
      float dd = lenf(wx - NEB_X, wy - NEB_Y);
      float fall = clampf((NEB_R + 300 - dd) / 700, 0, 1);
      if (fall <= 0) continue;
      float n = fbm(wx * 0.0032f, wy * 0.0032f, 91) * (0.4f + 0.6f * fall);
      if (n < 0.3f) continue;
      float tn = vnoise(wx * 0.002f + 5, wy * 0.002f, 93);
      uint8_t lo, hi;
      float f;
      if (n < 0.42f) { lo = C_SPACE; hi = tn > 0.6f ? C_T1 : C_V1; f = (n - 0.3f) / 0.12f; }
      else if (n < 0.55f) { lo = tn > 0.6f ? C_T1 : C_V1; hi = C_V2; f = (n - 0.42f) / 0.13f; }
      else { lo = C_V2; hi = C_V3; f = clampf((n - 0.55f) / 0.2f, 0, 1); }
      int lvl = (int)(f * 16);
      for (int y = by; y < by + 4; y++)
        for (int x = bx; x < bx + 4; x++) scr[y * LW + x] = bayer(x, y) < lvl ? hi : lo;
    }
}

void drawPlanetsAndSun(float cx, float cy) {
  if (sunBmp) {
    int sx = (int)(-cx) + CX - (SUN_CORONA + 1), sy = (int)(-cy) + CY - (SUN_CORONA + 1);
    if (sx < LW && sy < LH && sx + sunSz > 0 && sy + sunSz > 0) {
      blit(sunBmp, sunSz, sunSz, sx, sy);
      int scx = (int)(-cx) + CX, scy = (int)(-cy) + CY;
      for (int k = 0; k < 50; k++) {
        float a = frand() * TAUF, r = SUN_R + frand() * frand() * 30;
        pset(scx + (int)(cosf(a) * r), scy + (int)(sinf(a) * r), irand(2) ? C_Y2 : C_O4);
      }
    }
  }
  for (int i = 0; i < NPLANET; i++) {
    if (!planetBmp[i]) continue;
    const PlanetDef &P = PLANETS[i];
    int sx = (int)(P.x - cx) + CX - (P.r + 1), sy = (int)(P.y - cy) + CY - (P.r + 1);
    if (sx >= LW || sy >= LH || sx + planetSz[i] <= 0 || sy + planetSz[i] <= 0) continue;
    blit(planetBmp[i], planetSz[i], planetSz[i], sx, sy);
  }
}

static void drawBelt(float cx, float cy) {
  float r = lenf(cx, cy);
  if (r < BELT_R0 - 450 || r > BELT_R1 + 450) return;
  int x0 = (int)floorf((cx - CX - 30) / 90), x1 = (int)floorf((cx + CX + 30) / 90);
  int y0 = (int)floorf((cy - CY - 30) / 90), y1 = (int)floorf((cy + CY + 30) / 90);
  for (int gy = y0; gy <= y1; gy++)
    for (int gx = x0; gx <= x1; gx++) {
      uint32_t h = hash2(gx, gy, 77);
      float ax = gx * 90 + (h & 63) + 13, ay = gy * 90 + ((h >> 6) & 63) + 13;
      float ar = lenf(ax, ay);
      float dens = 1 - fabsf(ar - (BELT_R0 + BELT_R1) / 2) / ((BELT_R1 - BELT_R0) / 2);
      int sx = (int)(ax - cx) + CX, sy = (int)(ay - cy) + CY;
      /* pebbles */
      uint32_t h2 = hash2(gx, gy, 78);
      if (dens > -0.3f)
        for (int k = 0; k < 3; k++) {
          int px = sx - 40 + (int)((h2 >> (k * 8)) & 63), py = sy - 30 + (int)((h2 >> (k * 8 + 4)) & 63);
          pset(px, py, k ? C_O1 : C_G2);
        }
      if (dens <= 0 || ((h >> 12) & 255) > dens * 110) continue;
      int rad = 4 + (int)((h >> 20) % 9);
      if (sx < -rad || sy < -rad || sx > LW + rad || sy > LH + rad) continue;
      bool grey = (h >> 29) & 1;
      float lx = -ax / ar, ly = -ay / ar;
      fillCircle(sx, sy, rad, grey ? C_G1 : C_O1);
      fillCircle(sx + (int)(lx * rad * 0.3f), sy + (int)(ly * rad * 0.3f), rad * 7 / 10, grey ? C_G2 : C_O2);
      fillCircle(sx + (int)(lx * rad * 0.5f), sy + (int)(ly * rad * 0.5f), rad / 3, grey ? C_G3 : C_O3);
      pset(sx - (int)(lx * rad * 0.3f) + 1, sy - (int)(ly * rad * 0.3f), C_BLACK);
    }
}

static void drawLanes(float cx, float cy) {
  for (int k = 0; k < NLANE; k++) {
    const LaneDef &L = LANES[k];
    float dx = L.bx - L.ax, dy = L.by - L.ay, len = lenf(dx, dy);
    float ux = dx / len, uy = dy / len, px = -uy, py = ux;
    float t = (cx - L.ax) * ux + (cy - L.ay) * uy;
    float off = (cx - L.ax) * px + (cy - L.ay) * py;
    if (fabsf(off) > 260 || t < -260 || t > len + 260) continue;
    bool open = laneOpen[k];
    bool active = lr.on && lr.lane == k;
    /* track */
    if (open) {
      float t0 = maxf(0, t - 240), t1 = minf(len, t + 240);
      float ph = fmodf(gameTime * 220.0f, 24.0f);
      for (float s = floorf(t0 / 24) * 24 + ph; s < t1; s += 24) {
        int sx = (int)(L.ax + ux * s - cx) + CX, sy = (int)(L.ay + uy * s - cy) + CY;
        pset(sx, sy, active ? C_O4 : C_O2);
        if (active) pset(sx + (int)(ux * 2), sy + (int)(uy * 2), C_O3);
      }
    }
    int n = (int)(len / RING_GAP + 0.5f);
    float gap = len / n;
    int r0 = clampi((int)floorf((t - 260) / gap), 0, n), r1 = clampi((int)ceilf((t + 260) / gap), 0, n);
    float ang = atan2f(py, px);
    for (int ri = r0; ri <= r1; ri++) {
      float s = ri * gap;
      float wx = L.ax + ux * s, wy = L.ay + uy * s;
      int sx = (int)(wx - cx) + CX, sy = (int)(wy - cy) + CY;
      if (sx < -40 || sy < -40 || sx > LW + 40 || sy > LH + 40) continue;
      bool lit = active && fabsf(lr.pos - s) < 160;
      fillQuad((float)sx, (float)sy, 30, 2.5f, ang, lit ? C_O3 : C_G2);
      fillQuad((float)sx + ux * 3, (float)sy + uy * 3, 27, 1.0f, ang, open ? C_O2 : C_G1);
      fillQuad((float)sx - ux * 3, (float)sy - uy * 3, 27, 1.0f, ang, C_G1);
      for (int e = -1; e <= 1; e += 2) {
        int qx = sx + (int)(px * 30 * e), qy = sy + (int)(py * 30 * e);
        fillRect(qx - 4, qy - 4, 8, 8, C_G3);
        fillRect(qx - 3, qy - 3, 6, 2, C_G5);
        fillRect(qx - 4, qy + 2, 8, 2, C_G2);
        bool on = open && (((int)(gameTime * 3) + ri) & 1);
        fillRect(qx - 1, qy - 1, 3, 3, open ? (on ? C_Y3 : C_O3) : C_R2);
      }
      if (open && (ri == 0 || ri == n)) {
        /* end rings carry a beacon */
        if (((int)(gameTime * 2)) & 1) circle(sx, sy, 38, C_O2);
      }
    }
  }
}

/* --------------------------------------------------------------- stations */
void drawStation(int style, int sx, int sy, int sc, float t, bool crippled) {
  auto R = [&](int x, int y, int w, int h, uint8_t c) { fillRect(sx + x * sc, sy + y * sc, w * sc, h * sc, c); };
  auto Cf = [&](float x, float y, int r, uint8_t c) { fillCircle(sx + (int)(x * sc), sy + (int)(y * sc), r * sc, c); };
  auto Co = [&](float x, float y, int r, uint8_t c) {
    circle(sx + (int)(x * sc), sy + (int)(y * sc), r * sc, c);
    if (sc > 1) circle(sx + (int)(x * sc), sy + (int)(y * sc), r * sc - 1, c);
  };
  auto Ln = [&](float x0, float y0, float x1, float y1, uint8_t c) {
    line(sx + (int)(x0 * sc), sy + (int)(y0 * sc), sx + (int)(x1 * sc), sy + (int)(y1 * sc), c);
  };
  auto P = [&](float x, float y, uint8_t c) {
    if (sc == 1) pset(sx + (int)x, sy + (int)y, c);
    else fillRect(sx + (int)(x * sc), sy + (int)(y * sc), sc, sc, c);
  };
  bool blink = ((int)(t * 2)) & 1;
  switch (style) {
  case 0: { /* Haven Spindle */
    Co(0, 0, 22, C_G3);
    Co(0, 0, 21, C_G4);
    Co(0, 0, 20, C_G2);
    for (int k = 0; k < 4; k++) {
      float a = t * 0.5f + k * PIF / 2;
      Ln(cosf(a) * 6, sinf(a) * 6, cosf(a) * 20, sinf(a) * 20, C_G3);
    }
    for (int k = 0; k < 16; k++) {
      float a = t * 0.5f + k * TAUF / 16;
      P(cosf(a) * 21, sinf(a) * 21, (k & 1) ? C_Y2 : C_G5);
    }
    R(-3, -32, 6, 64, C_G4);
    R(-2, -32, 1, 64, C_G5);
    R(-6, -9, 12, 18, C_G3);
    R(-5, -8, 10, 16, C_G4);
    Cf(0, 0, 6, C_G5);
    Cf(0, 0, 3, C_B4);
    R(-3, -35, 6, 3, C_B3);
    R(-3, 32, 6, 3, C_B3);
    P(0, -36, blink ? C_R4 : C_R2);
    P(0, 35, blink ? C_B5 : C_B3);
    R(-4, 9, 8, 2, C_UIHI);
    break;
  }
  case 1: { /* Dockyard Tolle */
    R(-28, -26, 14, 9, C_G4);
    for (int i = 0; i < 4; i++) P(-26 + i * 3, -23, C_Y2);
    R(-26, -16, 52, 4, C_G3);
    R(-26, 12, 52, 4, C_G3);
    R(-26, -16, 4, 32, C_G3);
    R(22, -16, 4, 32, C_G3);
    for (int x = -22; x < 22; x += 6) {
      Ln(x, -12, x + 6, -6, C_G2);
      Ln(x, 12, x + 6, 6, C_G2);
    }
    R(-11, -6, 22, 12, C_O2);
    R(-9, -4, 18, 8, C_O3);
    R(-3, -9, 6, 3, C_O2);
    float a = sinf(t * 0.4f) * 0.7f;
    Ln(20, -16, 20 + sinf(a) * 18, -16 - cosf(a) * 18, C_O4);
    P(20 + sinf(a) * 18, -16 - cosf(a) * 18, C_Y3);
    P(-26, -16, blink ? C_O4 : C_O2);
    P(25, 15, blink ? C_O2 : C_O4);
    R(-4, 16, 8, 2, C_UIHI);
    break;
  }
  case 2: { /* Meridian Exchange */
    for (int k = 0; k < 6; k++) {
      float a = t * 0.15f + k * TAUF / 6;
      fillQuad(sx + cosf(a) * 17 * sc, sy + sinf(a) * 17 * sc, 9.0f * sc, 2.5f * sc, a, C_GD1);
      Cf(cosf(a) * 26, sinf(a) * 26, 4, C_T2);
      Cf(cosf(a) * 26 - 1, sinf(a) * 26 - 1, 2, C_T3);
      if (blink == (k & 1)) P(cosf(a) * 26, sinf(a) * 26, C_T4);
    }
    Co(0, 0, 15, C_GD1);
    Cf(0, 0, 13, C_GD1);
    Cf(-2, -2, 10, C_GD2);
    Cf(-4, -4, 4, C_Y3);
    R(-3, 13, 6, 2, C_UIHI);
    break;
  }
  case 3: { /* Lantern Abbey */
    int lvl = 5 + (int)(sinf(t * 1.7f) * 3);
    ditherCircle(sx, sy, 34 * sc, C_V1, lvl);
    float vx[7], vy[7];
    for (int k = 0; k <= 6; k++) {
      vx[k] = cosf(k * TAUF / 6 + 0.52f) * 18;
      vy[k] = sinf(k * TAUF / 6 + 0.52f) * 18;
    }
    for (int k = 0; k < 6; k++) {
      fillTri(sx, sy, sx + vx[k] * sc, sy + vy[k] * sc, sx + vx[k + 1] * sc, sy + vy[k + 1] * sc,
              (k & 1) ? C_G2 : C_G3);
      Ln(vx[k], vy[k], vx[k + 1], vy[k + 1], C_G4);
      Ln(vx[k], vy[k], vx[k] * 1.5f, vy[k] * 1.5f, C_G3);
      P(vx[k] * 1.5f, vy[k] * 1.5f, blink ? C_Y3 : C_Y1);
    }
    int pr = 7 + (int)(sinf(t * 2.3f) * 1.5f);
    Cf(0, 0, pr, C_Y1);
    Cf(0, 0, pr - 2, C_Y2);
    Cf(0, 0, pr - 4, C_Y3);
    Cf(0, 0, 1, C_WHITE);
    break;
  }
  case 4: { /* Rook's Nest */
    Cf(0, 0, 22, C_O1);
    Cf(-8, -6, 14, C_O2);
    Cf(9, 5, 13, C_O1);
    Cf(4, -11, 10, C_O2);
    Cf(-6, 11, 9, C_O1);
    Cf(-10, -9, 5, C_O3);
    Cf(6, -13, 3, C_O3);
    for (int k = 0; k < 14; k++) {
      uint32_t h = hash2(k, 3, 9);
      float a = (h & 1023) / 1023.0f * TAUF, r = 4 + ((h >> 10) % 15);
      P(cosf(a) * r, sinf(a) * r, ((h >> 20) & 3) ? C_Y2 : C_R3);
    }
    R(-28, -2, 9, 3, C_G3);
    R(19, 5, 9, 3, C_G3);
    R(-3, -30, 3, 9, C_G3);
    P(-28, -1, blink ? C_R4 : C_R2);
    P(27, 6, blink ? C_R2 : C_R4);
    R(-5, 17, 10, 3, blink ? C_R3 : C_R2);
    break;
  }
  case 5: { /* Farpoint Listening Post */
    R(-36, -6, 13, 12, C_B2);
    R(23, -6, 13, 12, C_B2);
    for (int i = 0; i < 3; i++) {
      Ln(-36 + i * 4, -6, -36 + i * 4, 5, C_B3);
      Ln(23 + i * 4 + 2, -6, 23 + i * 4 + 2, 5, C_B3);
    }
    R(-23, -1, 46, 3, C_G3);
    R(-2, -22, 4, 40, C_G3);
    Cf(0, 4, 8, C_G4);
    Cf(0, 4, 5, C_G3);
    float a = -0.7f + sinf(t * 0.2f) * 0.15f;
    for (float b = -1.1f; b <= 1.1f; b += 0.08f) {
      float cx2 = sinf(a) * -6, cy2 = -cosf(a) * -6 - 14;
      P(cx2 + sinf(a + b) * 14, cy2 - cosf(a + b) * 14, C_G5);
      P(cx2 + sinf(a + b) * 13, cy2 - cosf(a + b) * 13, C_G4);
    }
    Ln(0, -14, sinf(a) * 10, -14 - cosf(a) * 10, C_G4);
    P(sinf(a) * 10, -14 - cosf(a) * 10, blink ? C_R4 : C_R2);
    R(-3, 18, 6, 2, C_UIHI);
    break;
  }
  }
  if (crippled) {
    ditherCircle(sx, sy, 26 * sc, C_BLACK, 7);
    if (irand(3) == 0) P(frange(-20, 20), frange(-20, 20), C_O4);
  }
}

static void drawGate(float cx, float cy) {
  int sx = (int)(GATE_X - cx) + CX, sy = (int)(GATE_Y - cy) + CY;
  if (sx < -180 || sy < -180 || sx > LW + 180 || sy > LH + 180) return;
  bool awake = (mis >= MS_FARPOINT && ending == 0) || ending == 2;
  if (awake) {
    for (int r = 104; r > 4; r -= 8) {
      float n = vnoise(r * 0.1f, gameTime * 0.7f, 5);
      uint8_t c = ending == 2 ? (r < 40 ? C_WHITE : (r < 70 ? C_Y3 : C_V4))
                              : (r < 30 ? C_V4 : (r < 70 ? C_V2 : C_V1));
      ditherCircle(sx, sy, r, c, 3 + (int)(n * 8));
    }
    for (int k = 0; k < 30; k++) {
      float a = frand() * TAUF, r = frand() * 100;
      pset(sx + (int)(cosf(a + gameTime) * r), sy + (int)(sinf(a + gameTime) * r), ending == 2 ? C_WHITE : C_V5);
    }
  } else if (ending == 1) {
    fillCircle(sx, sy, 6, C_G1);
  }
  for (int k = 0; k < 16; k++) {
    float a = gameTime * 0.05f + k * TAUF / 16;
    float px = sx + cosf(a) * GATE_R, py = sy + sinf(a) * GATE_R;
    fillQuad(px, py, 21, 5, a + PIF / 2, C_G2);
    fillQuad(px, py, 19, 3, a + PIF / 2, C_G3);
    pset((int)px, (int)py, awake ? ((k + (int)(gameTime * 4)) % 4 ? C_V3 : C_V5) : C_G1);
  }
  for (int k = 0; k < 10; k++) {
    float a = -gameTime * 0.08f + k * TAUF / 10;
    float px = sx + cosf(a) * 92, py = sy + sinf(a) * 92;
    fillQuad(px, py, 20, 3, a + PIF / 2, C_G1);
    pset((int)(px + cosf(a + PIF / 2) * 8), (int)(py + sinf(a + PIF / 2) * 8), C_T3);
    pset((int)(px - cosf(a + PIF / 2) * 6), (int)(py - sinf(a + PIF / 2) * 6), C_T2);
  }
}

static void drawNest(float cx, float cy) {
  if (!(known & KN_NEST)) return;
  int sx = (int)(NEST_X - cx) + CX, sy = (int)(NEST_Y - cy) + CY;
  if (sx < -80 || sy < -80 || sx > LW + 80 || sy > LH + 80) return;
  for (int k = 0; k < 7; k++) {
    uint32_t h = hash2(k, 1, 33);
    int ox = (int)(h % 90) - 45, oy = (int)((h >> 8) % 90) - 45, r = 6 + (int)((h >> 16) % 10);
    fillCircle(sx + ox, sy + oy, r, C_G1);
    fillCircle(sx + ox - 1, sy + oy - 1, r - 2, C_BLACK);
    if (mis <= MS_SABLE) pset(sx + ox, sy + oy, ((frameNo / 8 + k) & 3) ? C_V2 : C_V5);
  }
}

/* ------------------------------------------------------------------ ships */
static int shX, shY;

static void drawOneShip(int i) {
  Ship &s = ships[i];
  int sx = (int)(s.x - camX) + CX + shX, sy = (int)(s.y - camY) + CY + shY;
  if (s.kind == K_STATION) {
    if (sx < -90 || sy < -90 || sx > LW + 90 || sy > LH + 90) return;
    drawStation(STATIONS[s.sta].style, sx, sy, 2, gameTime, s.hp <= 0);
    if (s.shT > 0) {
      circle(sx, sy, 58, C_T3);
      circle(sx, sy, 59, C_T2);
    }
    return;
  }
  const Design &d = designs[s.des];
  const Stats &st = d.st;
  int R = (int)st.radius + 2;
  if (sx < -R - 12 || sy < -R - 12 || sx > LW + R + 12 || sy > LH + R + 12) return;
  float ca = cosf(s.ang), sa = sinf(s.ang);
  if (s.thr > 0.05f && s.kind == K_SHIP) {
    bool fast = (i == PL && (cruise == CR_ON || lr.on)) || s.ai == AI_LANE;
    int len = fast ? 16 + irand(6) : 2 + (int)(s.thr * 7) + irand(3);
    for (int e = 0; e < st.nEng; e++) {
      float ex = s.x + ca * st.engX[e] - sa * st.engY[e], ey = s.y + sa * st.engX[e] + ca * st.engY[e];
      for (int k = 0; k < len; k++) {
        float fx = ex - sa * k, fy = ey + ca * k;
        uint8_t c = k < 1 ? C_WHITE : (k < len / 2 + 1 ? C_Y3 : (k < len - 1 ? C_O4 : C_R3));
        if (fast && k > 2) c = (k & 1) ? C_B5 : C_B4;
        int px = (int)(fx - camX) + CX + shX, py = (int)(fy - camY) + CY + shY;
        pset(px, py, c);
        if (k < len - 2) pset(px + 1, py, c);
      }
    }
  }
  int flash = s.hitT > 0 ? C_WHITE : -1;
  if (s.kind == K_WRECK) flash = C_G2;
  blitRot(d.spr, SPR, SPR, SPR, st.comX, st.comY, (float)sx + (s.x - floorf(s.x)),
          (float)sy + (s.y - floorf(s.y)), s.ang, R, flash, SHIP_SCALE);
  if (s.kind == K_WRECK) {
    if ((frameNo / 10) & 1) pset(sx + 1, sy - 2, C_R3);
    if (s.scan > 0) {
      int n = (int)(s.scan * 40);
      for (int k = 0; k < n; k++) {
        float a = k * TAUF / 40 - PIF / 2;
        pset(sx + (int)(cosf(a) * (R + 6)), sy + (int)(sinf(a) * (R + 6)), C_UIHI);
      }
    }
  }
  if (s.shT > 0 && s.sh > 0) {
    int r = R + 1;
    for (int k = 0; k < 24; k++) {
      if ((k + frameNo) & 1) continue;
      float a = k * TAUF / 24;
      pset(sx + (int)(cosf(a) * r), sy + (int)(sinf(a) * r), C_T4);
    }
  }
}

static void drawShots() {
  for (const Shot &s : shots) {
    if (!s.on) continue;
    int sx = (int)(s.x - camX) + CX + shX, sy = (int)(s.y - camY) + CY + shY;
    if ((unsigned)sx >= LW || (unsigned)sy >= LH) continue;
    uint8_t c = s.fromPlayer ? C_O5 : FACS[s.fac].shot;
    switch (s.w) {
    case W_LASER:
    case W_STATION:
      line(sx, sy, sx - (int)(s.vx * 0.01f), sy - (int)(s.vy * 0.01f), c);
      pset(sx, sy, C_WHITE);
      break;
    case W_CANNON:
      fillRect(sx - 1, sy - 1, 2, 2, C_Y3);
      pset(sx - (int)(s.vx * 0.012f), sy - (int)(s.vy * 0.012f), C_O4);
      break;
    case W_MISSILE:
      pset(sx, sy, C_WHITE);
      pset(sx - (int)(s.vx * 0.01f), sy - (int)(s.vy * 0.01f), C_R3);
      break;
    default:
      line(sx, sy, sx - (int)(s.vx * 0.006f), sy - (int)(s.vy * 0.006f), C_Y2);
      break;
    }
  }
}

static void drawParts() {
  for (const Part &p : parts) {
    if (p.life <= 0) continue;
    int sx = (int)(p.x - camX) + CX + shX, sy = (int)(p.y - camY) + CY + shY;
    switch (p.kind) {
    case PK_DOT: pset(sx, sy, p.col); break;
    case PK_SPARK:
      line(sx, sy, sx - (int)(p.vx * 0.03f), sy - (int)(p.vy * 0.03f), p.life < 4 ? C_R2 : p.col);
      break;
    case PK_SMOKE:
      if (p.life > 4 || (frameNo & 1)) pset(sx, sy, p.col);
      break;
    case PK_RING: circle(sx, sy, (int)((p.max - p.life) * p.vx * 2.0f), p.life < 5 ? C_R2 : p.col); break;
    case PK_FLASH: fillCircle(sx, sy, (int)(p.life * p.vx * 0.35f) + 1, p.life > 3 ? C_WHITE : p.col); break;
    }
  }
}

void drawShipSprite(int des, int x, int y, float ang, int scale) {
  const Design &d = designs[des];
  if (scale <= 1) {
    blitRot(d.spr, SPR, SPR, SPR, d.st.comX, d.st.comY, (float)x, (float)y, ang, (int)d.st.radius + 2, -1);
    return;
  }
  for (int v = 0; v < SPR; v++)
    for (int u = 0; u < SPR; u++) {
      uint8_t c = d.spr[v * SPR + u];
      if (c == TRANSP) continue;
      fillRect(x + (int)((u - d.st.comX) * scale), y + (int)((v - d.st.comY) * scale), scale, scale, c);
    }
}

void worldDraw() {
  Ship &p = ships[PL];
  shX = shake > 0 ? irand((int)(shake * 2) + 1) - (int)shake : 0;
  shY = shake > 0 ? irand((int)(shake * 2) + 1) - (int)shake : 0;
  float cx = camX - shX, cy = camY - shY;
  clear(C_SPACE);
  drawNebula(cx, cy);
  bool streak = lr.on || (cruise == CR_ON && mode == MD_FLIGHT);
  drawStarfield(cx, cy, p.vx, p.vy, streak);
  drawPlanetsAndSun(cx, cy);
  drawBelt(cx, cy);
  drawGate(cx, cy);
  drawNest(cx, cy);
  drawLanes(cx, cy);
  if (lr.on) {
    float sp = lenf(p.vx, p.vy);
    float ux = p.vx / sp, uy = p.vy / sp;
    for (int k = 0; k < 14; k++) {
      int x0 = irand(LW), y0 = irand(LH), l = 20 + irand(30);
      line(x0, y0, x0 - (int)(ux * l), y0 - (int)(uy * l), (k & 1) ? C_O2 : C_B2);
    }
  }
  for (int i = 1; i < MAXSHIP; i++)
    if (ships[i].on && ships[i].kind == K_STATION) drawOneShip(i);
  for (int i = 1; i < MAXSHIP; i++)
    if (ships[i].on && ships[i].kind != K_STATION) drawOneShip(i);
  if (p.on && p.hp > 0) drawOneShip(PL);
  drawShots();
  drawParts();
}

/* -------------------------------------------------------------------- HUD */
static void bracket(int x, int y, int r, uint8_t c) {
  int l = 3;
  hline(x - r, x - r + l, y - r, c); hline(x + r - l, x + r, y - r, c);
  hline(x - r, x - r + l, y + r, c); hline(x + r - l, x + r, y + r, c);
  for (int k = 0; k <= l; k++) {
    pset(x - r, y - r + k, c); pset(x + r, y - r + k, c);
    pset(x - r, y + r - k, c); pset(x + r, y + r - k, c);
  }
}

static void edgeArrow(float dx, float dy, uint8_t c, const char *label) {
  float a = atan2f(dy, dx);
  float ca = cosf(a), sa = sinf(a);
  float t = minf((CX - 10) / maxf(fabsf(ca), 0.001f), (CY - 46) / maxf(fabsf(sa), 0.001f));
  float ex = CX + ca * t, ey = CY + 6 + sa * t;
  fillTri(ex + ca * 6, ey + sa * 6, ex - sa * 4, ey + ca * 4, ex + sa * 4, ey - ca * 4, c);
  if (label) {
    int tx = (int)(ex - ca * 14) - textW(label) / 2, ty = (int)(ey - sa * 12) - 3;
    textSh(clampi(tx, 2, LW - 2 - textW(label)), clampi(ty, 40, 196), label, c);
  }
}

void drawComms() {
  if (bk.t <= 0) return;
  const char *name;
  int port;
  uint8_t tint;
  if (bk.crew >= 0) {
    name = CREW[bk.crew].name;
    port = CREW[bk.crew].sp;
    tint = CREW[bk.crew].tint;
  } else {
    speakerInfo(bk.spk, &name, &port, &tint);
  }
  panel(2, 151, 236, 50);
  if (port < SP_NPORT) drawPortrait(6, 157, port, tint, 2, port == SP_HUSH);
  text(42, 154, name, C_UIHI);
  wrapText(42, 164, 32, bk.text, bk.spk == SPK_HUSH ? C_V5 : C_WHITE, 1 << 20, 9, 4);
}

static const char *distText(float d) {
  if (d < 1000) return fmt("%d", (int)d);
  return fmt("%d.%dK", (int)(d / 1000), ((int)d % 1000) / 100);
}

void hudDraw() {
  Ship &p = ships[PL];
  const Stats &st = designs[D_PLAYER].st;
  /* markers on ships */
  for (int i = 1; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on || (s.flags & SF_LANE)) continue;
    int sx = (int)(s.x - camX) + CX, sy = (int)(s.y - camY) + CY;
    bool on = sx > -10 && sy > -80 && sx < LW + 10 && sy < LH + 10;
    if (s.kind == K_STATION) {
      if (on) textShC(sx, sy + 72, STATIONS[s.sta].shortName, stationHostile(s.sta) ? C_R4 : C_G4);
      continue;
    }
    int r = (int)statsOf(s).radius + 4;
    uint8_t c = 0;
    if (s.flags & SF_HOSTILE) c = (s.fac == F_HUSH) ? C_V5 : C_R4;
    else if (s.flags & SF_MISSION) c = C_Y2;
    else if (s.flags & (SF_HAIL | SF_DISTRESS)) c = C_GR4;
    else if (s.kind == K_WRECK) c = C_G4;
    if (!c) continue;
    if (on) {
      bracket(sx, sy, r, c);
      if ((s.flags & SF_HOSTILE) && s.hp < s.hpMax) bar(sx - 8, sy - r - 4, 16, 2, s.hp / s.hpMax, c, C_G1);
      if (s.flags & SF_BOSS) textShC(sx, sy - r - 12, designs[s.des].name, C_V5);
    } else if ((s.flags & SF_HOSTILE) && dist2(s.x, s.y, p.x, p.y) < 700.0f * 700.0f) {
      edgeArrow(s.x - p.x, s.y - p.y, c, nullptr);
    }
  }
  /* waypoint */
  float wx, wy;
  bool hasWp = waypoint(wx, wy);
  if (hasWp) {
    float dx = wx - p.x, dy = wy - p.y, d = lenf(dx, dy);
    int sx = (int)(wx - camX) + CX, sy = (int)(wy - camY) + CY;
    if (sx > 8 && sy > 40 && sx < LW - 8 && sy < 196) {
      int b = (frameNo / 6) & 1;
      for (int k = -5; k <= 5; k++) {
        pset(sx + k, sy - 5 + abs(k), C_UIHI);
        pset(sx + k, sy + 5 - abs(k), C_UIHI);
      }
      if (b) pset(sx, sy, C_WHITE);
    } else {
      edgeArrow(dx, dy, C_UIHI, distText(d));
    }
  }
  /* status, top left */
  text(3, 3, "HULL", C_G4);
  bar(29, 4, 56, 4, p.hp / maxf(p.hpMax, 1), p.hp < p.hpMax * 0.3f && (frameNo & 8) ? C_R4 : C_GR3, C_G1);
  text(3, 12, "SHLD", C_G4);
  bar(29, 13, 56, 4, p.sh / maxf(p.shMax, 1), C_T3, C_G1);
  text(3, 21, fmt("%d CR", credits), C_Y2);
  float spd = lenf(p.vx, p.vy);
  if (lr.on) text(3, 30, fmt("LANE %d", (int)spd), C_O4);
  else if (cruise == CR_ON) text(3, 30, fmt("CRUISE %d", (int)spd), C_B5);
  else text(3, 30, fmt("SPD %d", (int)spd), C_G4);
  if (ap.on) text(3, 39, "AUTOPILOT", (frameNo & 16) ? C_UIHI : C_B3);
  if (playerEff() < 1) text(3, ap.on ? 48 : 39, "LOW POWER", C_R4);

  /* radar */
  int rx = 213, ry = 26, rr = 24;
  fillCircle(rx, ry, rr, C_UIBG);
  circle(rx, ry, rr, C_UILN);
  circle(rx, ry, rr / 2, C_UIBG2);
  float range = st.radar * cm.radar * (zoneAt(p.x, p.y) == Z_NEBULA ? 0.6f : 1.0f);
  float k = rr / range;
  for (int s = 0; s < ST_COUNT; s++) {
    float dx = STATIONS[s].x - p.x, dy = STATIONS[s].y - p.y;
    if (dx * dx + dy * dy > range * range) continue;
    fillRect(rx + (int)(dx * k) - 1, ry + (int)(dy * k) - 1, 3, 3, stationHostile(s) ? C_R3 : C_WHITE);
  }
  for (int i = FIRST_DYN; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on || (s.flags & SF_LANE)) continue;
    float dx = s.x - p.x, dy = s.y - p.y;
    if (dx * dx + dy * dy > range * range) continue;
    uint8_t c = C_G5;
    if (s.flags & SF_HOSTILE) c = s.fac == F_HUSH ? C_V5 : C_R4;
    else if (s.flags & SF_MISSION) c = C_Y2;
    else if (s.flags & SF_DEFENDER) c = C_GR4;
    else if (s.kind == K_WRECK) c = C_G3;
    pset(rx + (int)(dx * k), ry + (int)(dy * k), c);
    pset(rx + (int)(dx * k) + 1, ry + (int)(dy * k), c);
  }
  if (hasWp) {
    float dx = wx - p.x, dy = wy - p.y, d = lenf(dx, dy);
    if (d > range) { dx *= range / d; dy *= range / d; }
    int qx = rx + (int)(dx * k), qy = ry + (int)(dy * k);
    pset(qx, qy - 1, C_UIHI); pset(qx, qy + 1, C_UIHI); pset(qx - 1, qy, C_UIHI); pset(qx + 1, qy, C_UIHI);
  }
  pset(rx, ry, C_WHITE);
  pset(rx + (int)(sinf(p.ang) * 4), ry - (int)(cosf(p.ang) * 4), C_WHITE);
  pset(rx + (int)(sinf(p.ang) * 3), ry - (int)(cosf(p.ang) * 3), C_G5);

  /* cruise spool */
  if (cruise == CR_CHARGE) {
    textShC(CX, 62, "CRUISE CHARGING", C_B5);
    float ct = 2.6f / cm.charge * (st.cruiseMul > 1 ? 0.8f : 1.0f);
    bar(CX - 34, 72, 68, 3, cruiseT / ct, C_B5, C_G1);
  }
  if (toastT > 0 && (toastT > 0.5f || (frameNo & 2))) textShC(CX, 52, toastText(), C_Y3);

  drawComms();
  /* objective */
  const char *obj = objective();
  if (obj && *obj) {
    const char *o = fmt("> %s", obj);
    if (hasWp) {
      const char *ow = fmt("> %s  %s", obj, distText(lenf(wx - p.x, wy - p.y)));
      if (textW(ow) <= 236) o = ow;
    }
    if (textW(o) > 236) {
      static char cut[40];
      snprintf(cut, sizeof(cut), "%s", o);
      cut[37] = 0;
      o = cut;
    }
    textShC(CX, 204, o, C_Y2);
  }
}

} // namespace sl
