/* STARLANE - drawing primitives, font, sprites and small UI widgets. */
#include "sl.h"

namespace sl {

uint8_t scr[LW * LH];
uint16_t pal565[256];
uint32_t rngState = 0x2545F491u;

bool inDown, inTap, tapEaten, uiLock;
int inX, inY;

/* ------------------------------------------------------------- palette */
static const uint8_t PAL[C_COUNT][3] = {
    {4, 5, 14},     {0, 0, 0},       {28, 30, 40},    {58, 62, 76},
    {98, 104, 120}, {150, 155, 170}, {200, 205, 215}, {245, 246, 250},
    {12, 20, 48},   {22, 46, 104},   {36, 84, 168},   {74, 138, 226},
    {150, 200, 255}, {14, 44, 26},   {30, 92, 46},    {64, 152, 72},
    {140, 214, 112}, {48, 26, 16},   {112, 56, 26},   {184, 98, 42},
    {232, 152, 72}, {250, 206, 142}, {200, 156, 36},  {250, 214, 78},
    {255, 246, 196}, {64, 10, 16},   {136, 26, 32},   {214, 52, 46},
    {255, 118, 96}, {24, 10, 40},    {56, 22, 88},    {102, 42, 156},
    {164, 84, 226}, {218, 164, 255}, {8, 44, 50},     {18, 102, 108},
    {40, 176, 174}, {134, 244, 234}, {164, 122, 32},  {236, 196, 74},
    {255, 120, 200}, {8, 14, 30},    {18, 30, 60},    {44, 74, 130},
    {96, 206, 255}};

static uint8_t charMap[128];

static void initPalette() {
  for (int i = 0; i < 256; i++) pal565[i] = 0;
  for (int i = 0; i < C_COUNT; i++) {
    int r = PAL[i][0], g = PAL[i][1], b = PAL[i][2];
    pal565[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  memset(charMap, TRANSP, sizeof(charMap));
  const char *keys = "kKgGhHwnuUiNflLFoOqpPyYjrRsevVmMztTcadDxb";
  const uint8_t vals[] = {C_BLACK, C_G1, C_G2, C_G3, C_G4, C_G5, C_WHITE, C_B2, C_B3,
                          C_B4, C_B5, C_B1, C_GR2, C_GR3, C_GR4, C_GR1, C_O2, C_O3,
                          C_O1, C_O4, C_O5, C_Y2, C_Y3, C_Y1, C_R2, C_R3, C_R4,
                          C_R1, C_V2, C_V3, C_V4, C_V5, C_V1, C_T2, C_T3, C_T4,
                          C_T1, C_GD1, C_GD2, C_PK, C_SPACE};
  for (int i = 0; keys[i]; i++) charMap[(uint8_t)keys[i]] = vals[i];
}
struct PalInit {
  PalInit() { initPalette(); }
} palInit;

uint8_t charColor(char ch) { return charMap[(uint8_t)ch & 127]; }

const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float vnoise(float x, float y, uint32_t seed) {
  int ix = (int)floorf(x), iy = (int)floorf(y);
  float fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  float a = hashf(ix, iy, seed), b = hashf(ix + 1, iy, seed);
  float c = hashf(ix, iy + 1, seed), d = hashf(ix + 1, iy + 1, seed);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

/* ---------------------------------------------------------- primitives */
void clear(uint8_t c) { memset(scr, c, sizeof(scr)); }

void hline(int x0, int x1, int y, uint8_t c) {
  if ((unsigned)y >= (unsigned)LH) return;
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  if (x0 < 0) x0 = 0;
  if (x1 >= LW) x1 = LW - 1;
  if (x0 > x1) return;
  memset(&scr[y * LW + x0], c, x1 - x0 + 1);
}

void fillRect(int x, int y, int w, int h, uint8_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LW) w = LW - x;
  if (y + h > LH) h = LH - y;
  if (w <= 0 || h <= 0) return;
  for (int j = 0; j < h; j++) memset(&scr[(y + j) * LW + x], c, w);
}

void rect(int x, int y, int w, int h, uint8_t c) {
  if (w <= 0 || h <= 0) return;
  hline(x, x + w - 1, y, c);
  hline(x, x + w - 1, y + h - 1, c);
  for (int j = y + 1; j < y + h - 1; j++) {
    pset(x, j, c);
    pset(x + w - 1, j, c);
  }
}

void line(int x0, int y0, int x1, int y1, uint8_t c) {
  /* reject lines entirely off one side */
  if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= LW && x1 >= LW) ||
      (y0 >= LH && y1 >= LH))
    return;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, guard = 2000;
  for (;;) {
    pset(x0, y0, c);
    if ((x0 == x1 && y0 == y1) || --guard <= 0) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void dline(int x0, int y0, int x1, int y1, uint8_t c, int on, int off) {
  if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= LW && x1 >= LW) ||
      (y0 >= LH && y1 >= LH))
    return;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, n = 0, guard = 4000;
  for (;;) {
    if (n % (on + off) < on) pset(x0, y0, c);
    n++;
    if ((x0 == x1 && y0 == y1) || --guard <= 0) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void fillCircle(int cx, int cy, int r, uint8_t c) {
  if (r < 0 || cx + r < 0 || cx - r >= LW || cy + r < 0 || cy - r >= LH) return;
  int y0 = cy - r < 0 ? -cy : -r, y1 = cy + r >= LH ? LH - 1 - cy : r;
  for (int y = y0; y <= y1; y++) {
    int w = (int)sqrtf((float)(r * r - y * y) + 0.5f);
    hline(cx - w, cx + w, cy + y, c);
  }
}

void circle(int cx, int cy, int r, uint8_t c) {
  if (r <= 0 || cx + r < 0 || cx - r >= LW || cy + r < 0 || cy - r >= LH) {
    if (r == 0) pset(cx, cy, c);
    return;
  }
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    pset(cx + x, cy + y, c); pset(cx - x, cy + y, c);
    pset(cx + x, cy - y, c); pset(cx - x, cy - y, c);
    pset(cx + y, cy + x, c); pset(cx - y, cy + x, c);
    pset(cx + y, cy - x, c); pset(cx - y, cy - x, c);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

void ditherCircle(int cx, int cy, int r, uint8_t c, int level) {
  if (r < 0 || cx + r < 0 || cx - r >= LW || cy + r < 0 || cy - r >= LH) return;
  for (int y = -r; y <= r; y++) {
    int yy = cy + y;
    if ((unsigned)yy >= (unsigned)LH) continue;
    int w = (int)sqrtf((float)(r * r - y * y) + 0.5f);
    int xa = cx - w < 0 ? 0 : cx - w, xb = cx + w >= LW ? LW - 1 : cx + w;
    for (int x = xa; x <= xb; x++)
      if (bayer(x, yy) < level) scr[yy * LW + x] = c;
  }
}

void ditherRect(int x, int y, int w, int h, uint8_t c, int level) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LW) w = LW - x;
  if (y + h > LH) h = LH - y;
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++)
      if (bayer(i, j) < level) scr[j * LW + i] = c;
}

void darken(int level) {
  if (level <= 0) return;
  if (level >= 16) { clear(C_BLACK); return; }
  ditherRect(0, 0, LW, LH, C_BLACK, level);
}

void fillTri(float x0, float y0, float x1, float y1, float x2, float y2, uint8_t c) {
  float t;
  if (y1 < y0) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  if (y2 < y0) { t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
  if (y2 < y1) { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
  if (y2 - y0 < 0.01f) return;
  int ys = (int)ceilf(y0 - 0.5f), ye = (int)ceilf(y2 - 0.5f);
  if (ys < 0) ys = 0;
  if (ye > LH) ye = LH;
  for (int y = ys; y < ye; y++) {
    float py = y + 0.5f;
    float xa = x0 + (x2 - x0) * (py - y0) / (y2 - y0), xb;
    if (py < y1) xb = (y1 - y0) > 0.001f ? x0 + (x1 - x0) * (py - y0) / (y1 - y0) : x1;
    else xb = (y2 - y1) > 0.001f ? x1 + (x2 - x1) * (py - y1) / (y2 - y1) : x1;
    if (xa > xb) { t = xa; xa = xb; xb = t; }
    int ia = (int)ceilf(xa - 0.5f), ib = (int)ceilf(xb - 0.5f) - 1;
    if (ib >= ia) hline(ia, ib, y, c);
  }
}

void fillQuad(float cx, float cy, float hw, float hh, float ang, uint8_t c) {
  float ca = cosf(ang), sa = sinf(ang);
  float ax = ca * hw, ay = sa * hw, bx = -sa * hh, by = ca * hh;
  float x0 = cx - ax - bx, y0 = cy - ay - by, x1 = cx + ax - bx, y1 = cy + ay - by;
  float x2 = cx + ax + bx, y2 = cy + ay + by, x3 = cx - ax + bx, y3 = cy - ay + by;
  fillTri(x0, y0, x1, y1, x2, y2, c);
  fillTri(x0, y0, x2, y2, x3, y3, c);
}

/* -------------------------------------------------------------- sprites */
void drawSprite(int x, int y, int w, int h, const char *px, int scale,
                const uint8_t *roles, uint8_t tint) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      char ch = px[j * w + i];
      uint8_t c;
      if (ch >= '1' && ch <= '4') c = roles ? roles[ch - '1'] : C_G4;
      else if (ch == 'X') c = tint;
      else c = charMap[(uint8_t)ch & 127];
      if (c == TRANSP) continue;
      if (scale == 1) pset(x + i, y + j, c);
      else fillRect(x + i * scale, y + j * scale, scale, scale, c);
    }
}

void blitRot(const uint8_t *spr, int stride, int sw, int sh, float px, float py,
             float sx, float sy, float ang, int R, int flash, float scale) {
  float ca = cosf(ang) / scale, sa = sinf(ang) / scale;
  int cx = (int)floorf(sx), cy = (int)floorf(sy);
  float fx = sx - cx, fy = sy - cy;
  if (cx + R < 0 || cx - R >= LW || cy + R < 0 || cy - R >= LH) return;
  for (int dy = -R; dy <= R; dy++) {
    int yy = cy + dy;
    if ((unsigned)yy >= (unsigned)LH) continue;
    float ry = dy + 0.5f - fy;
    float rx = -R + 0.5f - fx;
    float u = rx * ca + ry * sa + px, v = -rx * sa + ry * ca + py;
    uint8_t *row = &scr[yy * LW];
    for (int dx = -R; dx <= R; dx++, u += ca, v -= sa) {
      int xx = cx + dx;
      if ((unsigned)xx >= (unsigned)LW) continue;
      if (u < 0 || v < 0) continue;
      int iu = (int)u, iv = (int)v;
      if (iu >= sw || iv >= sh) continue;
      uint8_t c = spr[iv * stride + iu];
      if (c == TRANSP) continue;
      row[xx] = flash >= 0 ? (uint8_t)flash : c;
    }
  }
}

void blit(const uint8_t *bmp, int w, int h, int x, int y) {
  int i0 = x < 0 ? -x : 0, i1 = x + w > LW ? LW - x : w;
  int j0 = y < 0 ? -y : 0, j1 = y + h > LH ? LH - y : h;
  for (int j = j0; j < j1; j++) {
    const uint8_t *src = bmp + j * w;
    uint8_t *dst = &scr[(y + j) * LW];
    for (int i = i0; i < i1; i++)
      if (src[i] != TRANSP) dst[x + i] = src[i];
  }
}

/* ----------------------------------------------------------------- font */
/* 5x7, ASCII 32..95; bit 4 is the leftmost column */
static const uint8_t FONT[64][7] = {
    {0, 0, 0, 0, 0, 0, 0},                       // space
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},  // !
    {0x0A, 0x0A, 0, 0, 0, 0, 0},                 // "
    {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0},     // #
    {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},  // $
    {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13},  // %
    {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D},  // &
    {0x04, 0x04, 0x08, 0, 0, 0, 0},              // '
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},  // (
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},  // )
    {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00},  // *
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},  // +
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08},  // ,
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},  // -
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},  // .
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},  // /
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},  // :
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08},  // ;
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},  // <
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},  // =
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},  // >
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},  // ?
    {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E},  // @
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11},  // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // C
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},  // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},  // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // I
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},  // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},  // N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},  // Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},  // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // X
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04},  // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Z
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},  // [
    {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01},  // backslash
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},  // ]
    {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},  // ^
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},  // _
};

static void drawChar(int x, int y, char ch, uint8_t c, int s) {
  uint8_t u = (uint8_t)ch;
  if (u >= 'a' && u <= 'z') u -= 32;
  if (u < 32 || u > 95) u = '?';
  if (u == ' ') return;
  const uint8_t *g = FONT[u - 32];
  for (int r = 0; r < 7; r++) {
    uint8_t bits = g[r];
    if (!bits) continue;
    for (int col = 0; col < 5; col++)
      if (bits & (0x10 >> col)) {
        if (s == 1) pset(x + col, y + r, c);
        else fillRect(x + col * s, y + r * s, s, s, c);
      }
  }
}

int textW(const char *t, int s) {
  int n = (int)strlen(t);
  return n ? n * 6 * s - s : 0;
}
void text(int x, int y, const char *t, uint8_t c, int s) {
  for (; *t; t++, x += 6 * s) drawChar(x, y, *t, c, s);
}
void textC(int cx, int y, const char *t, uint8_t c, int s) { text(cx - textW(t, s) / 2, y, t, c, s); }
void textR(int rx, int y, const char *t, uint8_t c, int s) { text(rx - textW(t, s), y, t, c, s); }
void textSh(int x, int y, const char *t, uint8_t c, int s) {
  text(x + 1, y + 1, t, C_BLACK, s);
  text(x, y, t, c, s);
}
void textShC(int cx, int y, const char *t, uint8_t c, int s) {
  textSh(cx - textW(t, s) / 2, y, t, c, s);
}

int wrapText(int x, int y, int wChars, const char *t, uint8_t c, int maxChars, int lineH,
             int maxLines) {
  int ln = 0, col = 0, drawn = 0;
  const char *p = t;
  while (*p) {
    if (*p == '\n') {
      ln++;
      col = 0;
      p++;
      drawn++;
      continue;
    }
    if (*p == ' ') {
      if (col > 0) col++;
      p++;
      drawn++;
      continue;
    }
    int wl = 0;
    while (p[wl] && p[wl] != ' ' && p[wl] != '\n') wl++;
    if (col > 0 && col + wl > wChars) {
      ln++;
      col = 0;
    }
    for (int k = 0; k < wl; k++) {
      if (col >= wChars) {
        ln++;
        col = 0;
      }
      if (drawn < maxChars && ln < maxLines) drawChar(x + col * 6, y + ln * lineH, p[k], c, 1);
      col++;
      drawn++;
    }
    p += wl;
  }
  return ln + 1;
}

const char *fmt(const char *f, ...) {
  static char buf[6][160];
  static int k = 0;
  k = (k + 1) % 6;
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf[k], sizeof(buf[k]), f, ap);
  va_end(ap);
  return buf[k];
}

/* ------------------------------------------------------------------- UI */
bool hitIn(int x, int y, int w, int h) {
  return inX >= x && inX < x + w && inY >= y && inY < y + h;
}
bool tapIn(int x, int y, int w, int h) {
  if (!inTap || tapEaten || uiLock) return false;
  if (!hitIn(x, y, w, h)) return false;
  tapEaten = true;
  return true;
}

void panel(int x, int y, int w, int h, uint8_t bg, uint8_t ln) {
  fillRect(x, y, w, h, bg);
  rect(x, y, w, h, ln);
  /* corner notches */
  pset(x, y, C_SPACE);
  pset(x + w - 1, y, C_SPACE);
  pset(x, y + h - 1, C_SPACE);
  pset(x + w - 1, y + h - 1, C_SPACE);
}

bool button(int x, int y, int w, int h, const char *label, bool en, uint8_t acc) {
  bool held = en && inDown && !uiLock && hitIn(x, y, w, h);
  fillRect(x, y, w, h, held ? C_UIBG2 : C_UIBG);
  rect(x, y, w, h, en ? (held ? C_WHITE : acc) : C_G2);
  if (en) {
    hline(x + 2, x + 5, y + 2, acc);
    pset(x + 2, y + 3, acc);
  }
  textC(x + w / 2, y + (h - 7) / 2, label, en ? C_WHITE : C_G3);
  return en && tapIn(x, y, w, h);
}

void bar(int x, int y, int w, int h, float f, uint8_t fg, uint8_t bg) {
  f = clampf(f, 0, 1);
  fillRect(x, y, w, h, bg);
  fillRect(x, y, (int)(w * f + 0.5f), h, fg);
}

void drawPortrait(int x, int y, int port, uint8_t tint, int scale, bool glitch) {
  if (port < 0 || port >= SP_NPORT) return;
  int s = 16 * scale;
  fillRect(x, y, s, s, C_UIBG2);
  for (int j = 0; j < s; j += 2 * scale) hline(x, x + s - 1, y + j, C_UIBG);
  if (!glitch) {
    drawSprite(x, y, 16, 16, PORTRAITS[port], scale, nullptr, tint);
  } else {
    const char *px = PORTRAITS[port];
    for (int j = 0; j < 16; j++) {
      int off = (irand(5) == 0) ? irand(5) - 2 : 0;
      drawSprite(x + off * scale, y + j * scale, 16, 1, px + j * 16, scale, nullptr, tint);
    }
  }
  rect(x - 1, y - 1, s + 2, s + 2, C_UILN);
}

} // namespace sl
