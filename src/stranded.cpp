/*******************************************************************************
 * STRANDED - a pixel-art survival/crafting game for the ESP32-4848S040
 *
 * Your escape pod crashed into the ocean. Hook drifting debris, use the pod's
 * fabricator to craft a launch pad and a five-stage rocket, then fly home.
 *
 * Controls (touch):
 *   tap / drag on the raft ... walk
 *   tap debris in the water .. throw your hook (must be in range)
 *   tap the pod .............. open the fabricator (crafting menu)
 *   tap the shark ............ scare it off before it eats your raft
 *
 * Rendering: the game draws a 160x160 "logical" screen (palette indices, in
 * internal SRAM) every frame. Each logical pixel is a 3x3 block on the 480x480
 * panel. Only logical pixels that changed since the last frame are written to
 * the PSRAM framebuffer, so a typical frame touches a few hundred pixels.
 ******************************************************************************/
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp32s3/rom/cache.h>

#include "display_4848s040.h" /* bus, gfx, panel init */
#include "touch.h"

#define DISPLAY_PCLK_HZ 14000000
#define FRAME_MS 33 /* ~30 fps */

/* ---------------------------------------------------------------- screen -- */
#define SCR_W 480
#define LW 160 /* logical width  */
#define LH 160 /* logical height */
#define PX 3   /* screen pixels per logical pixel */

#define TILE 8
#define TW (LW / TILE) /* 20 */
#define TH (LH / TILE) /* 20 */
#define HUD_H 9
#define MSG_Y 151

static uint8_t scr[LW * LH] __attribute__((aligned(4)));
static uint8_t shown[LW * LH] __attribute__((aligned(4)));
static uint16_t pal565[256];
static uint16_t *fb = nullptr;

/* --------------------------------------------------------------- palette -- */
enum : uint8_t {
  C_NONE = 0, C_BLACK, C_WHITE, C_SKIN, C_SHIRT, C_PANTS, C_HAIR, C_WOOD_L,
  C_WOOD_D, C_METAL_L, C_METAL_D, C_ORANGE, C_YELLOW, C_PLASTIC, C_FUEL,
  C_CHIP, C_WATER, C_WATER_MID, C_FOAM, C_SHARK, C_NAVY, C_GLASS, C_RED,
  C_GREY, C_FIRE, C_SMOKE, C_SPACE, C_PANEL, C_PANEL_HI, C_STAR, C_SHADOW,
  C_EARTH_B, C_EARTH_G, C_COUNT
};

static const uint8_t PAL_RGB[C_COUNT][3] = {
    {0, 0, 0},       {20, 16, 24},    {240, 240, 235}, {240, 190, 140},
    {220, 70, 60},   {50, 60, 120},   {90, 55, 30},    {196, 140, 78},
    {128, 84, 44},   {175, 180, 190}, {95, 100, 112},  {240, 140, 40},
    {250, 220, 80},  {80, 215, 205},  {205, 45, 45},   {60, 195, 85},
    {22, 72, 140},   {40, 105, 178},  {175, 220, 245}, {105, 115, 130},
    {12, 16, 36},    {120, 200, 245}, {255, 95, 95},   {140, 140, 155},
    {235, 75, 20},   {205, 205, 205}, {6, 6, 18},      {30, 40, 72},
    {58, 80, 130},   {255, 255, 215}, {14, 50, 104},   {40, 90, 200},
    {60, 160, 80}};

static uint16_t rgb565(int r, int g, int b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/* ------------------------------------------------------------------- rng -- */
static uint32_t rngS = 0x9E3779B9;
static inline uint32_t rnd() {
  uint32_t x = rngS;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return rngS = x;
}
static inline float rndf() { return (rnd() >> 8) * (1.0f / 16777216.0f); }
static inline int rndi(int n) { return rnd() % n; }
static inline uint32_t hash2(int x, int y) {
  uint32_t h = x * 374761393u + y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

/* ------------------------------------------------------------ primitives -- */
static inline void pset(int x, int y, uint8_t c) {
  if ((unsigned)x < LW && (unsigned)y < LH)
    scr[y * LW + x] = c;
}

static void fillRect(int x, int y, int w, int h, uint8_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LW) w = LW - x;
  if (y + h > LH) h = LH - y;
  if (w <= 0 || h <= 0)
    return;
  for (int j = 0; j < h; j++)
    memset(&scr[(y + j) * LW + x], c, w);
}

static void rectOutline(int x, int y, int w, int h, uint8_t c) {
  fillRect(x, y, w, 1, c);
  fillRect(x, y + h - 1, w, 1, c);
  fillRect(x, y, 1, h, c);
  fillRect(x + w - 1, y, 1, h, c);
}

static void drawLine(int x0, int y0, int x1, int y1, uint8_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    pset(x0, y0, c);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

/* --------------------------------------------------------------- sprites -- */
struct Sprite {
  uint8_t w, h;
  const char *px;
};

static uint8_t charCol[128];
static void initCharMap() {
  charCol['0'] = C_BLACK;   charCol['1'] = C_WHITE;   charCol['2'] = C_SKIN;
  charCol['3'] = C_SHIRT;   charCol['4'] = C_PANTS;   charCol['5'] = C_HAIR;
  charCol['6'] = C_WOOD_L;  charCol['7'] = C_WOOD_D;  charCol['8'] = C_METAL_L;
  charCol['9'] = C_METAL_D; charCol['a'] = C_ORANGE;  charCol['b'] = C_YELLOW;
  charCol['c'] = C_PLASTIC; charCol['d'] = C_FUEL;    charCol['e'] = C_CHIP;
  charCol['h'] = C_FOAM;    charCol['i'] = C_SHARK;   charCol['k'] = C_FIRE;
  charCol['m'] = C_GLASS;
}

static void drawSprite(int x, int y, const Sprite &s) {
  const char *p = s.px;
  for (int j = 0; j < s.h; j++)
    for (int i = 0; i < s.w; i++) {
      uint8_t c = charCol[(uint8_t)*p++ & 127];
      if (c)
        pset(x + i, y + j, c);
    }
}

static const Sprite SPR_PLAYER = {5, 8,
    ".555."
    ".202."
    ".222."
    "33333"
    "23332"
    ".444."
    ".4.4."
    ".0.0."};

static const Sprite SPR_POD = {8, 8,
    "..8888.."
    ".899998."
    "89mmmm98"
    "89m1mm98"
    "89999998"
    "8aa99aa8"
    ".899998."
    "..0000.."};

static const Sprite SPR_WOOD = {5, 3,
    "66667"
    "76666"
    "77777"};
static const Sprite SPR_SCRAP = {5, 4,
    ".898."
    "89998"
    "98889"
    ".989."};
static const Sprite SPR_PLASTIC = {3, 5,
    ".c."
    "ccc"
    "c1c"
    "ccc"
    "ccc"};
static const Sprite SPR_FUEL = {4, 5,
    "dddd"
    "9999"
    "dddd"
    "9999"
    "dddd"};
static const Sprite SPR_CRATE = {5, 5,
    "77777"
    "76667"
    "76e67"
    "76667"
    "77777"};
static const Sprite SPR_CHIP = {5, 5,
    ".9.9."
    "9eee9"
    ".e0e."
    "9eee9"
    ".9.9."};
static const Sprite SPR_FIN = {5, 4,
    "...i."
    "..ii."
    ".iii."
    "iiiii"};

/* Rocket stages, bottom to top */
static const Sprite SPR_ROCKET[5] = {
    {8, 4, "k999999k" "kk9889kk" "k.9889.k" "..0000.."},           /* engine */
    {8, 5, ".dddddd." ".d1dd1d." ".dddddd." ".d1dd1d." ".dddddd."}, /* tank */
    {8, 6, ".111111." ".11mm11." ".1mmmm1." ".11mm11." ".111111."
           ".888888."},                                             /* hull */
    {8, 3, ".aaaaaa." ".aebeba." ".aaaaaa."},                       /* nav */
    {8, 5, "...kk..." "..kkkk.." "..1111.." ".111111." ".111111."}, /* nose */
};
static const int ROCKET_H = 4 + 5 + 6 + 3 + 5;

/* ------------------------------------------------------------------ font -- */
/* 3x5 pixel font: 15 bits, rows top to bottom, 3 columns each */
struct Glyph {
  char ch;
  const char *bits;
};
static const Glyph GLYPHS[] = {
    {'A', "010101111101101"}, {'B', "110101110101110"}, {'C', "011100100100011"},
    {'D', "110101101101110"}, {'E', "111100110100111"}, {'F', "111100110100100"},
    {'G', "011100101101011"}, {'H', "101101111101101"}, {'I', "111010010010111"},
    {'J', "001001001101010"}, {'K', "101101110101101"}, {'L', "100100100100111"},
    {'M', "101111111101101"}, {'N', "110101101101101"}, {'O', "010101101101010"},
    {'P', "110101110100100"}, {'Q', "010101101110011"}, {'R', "110101110101101"},
    {'S', "011100010001110"}, {'T', "111010010010010"}, {'U', "101101101101111"},
    {'V', "101101101101010"}, {'W', "101101111111101"}, {'X', "101101010101101"},
    {'Y', "101101010010010"}, {'Z', "111001010100111"}, {'0', "111101101101111"},
    {'1', "010110010010111"}, {'2', "110001010100111"}, {'3', "110001010001110"},
    {'4', "101101111001001"}, {'5', "111100110001110"}, {'6', "011100111101111"},
    {'7', "111001010010010"}, {'8', "111101111101111"}, {'9', "111101111001110"},
    {'!', "010010010000010"}, {':', "000010000010000"}, {'-', "000000111000000"},
    {'+', "000010111010000"}, {'.', "000000000000010"}, {'/', "001001010100100"},
    {'?', "110001010000010"}, {',', "000000000010100"}, {'\'', "010010000000000"},
};
static uint16_t font[128];

static void initFont() {
  for (const Glyph &g : GLYPHS) {
    uint16_t v = 0;
    for (int i = 0; i < 15; i++)
      v = (v << 1) | (g.bits[i] == '1');
    font[(uint8_t)g.ch] = v;
  }
}

static void drawChar(int x, int y, char ch, uint8_t c, int s) {
  if (ch >= 'a' && ch <= 'z')
    ch -= 32;
  uint16_t v = font[(uint8_t)ch & 127];
  if (!v)
    return;
  for (int r = 0; r < 5; r++)
    for (int col = 0; col < 3; col++)
      if (v & (1 << (14 - (r * 3 + col))))
        fillRect(x + col * s, y + r * s, s, s, c);
}

static int textW(const char *t, int s = 1) {
  int n = strlen(t);
  return n ? n * 4 * s - s : 0;
}
static void drawText(int x, int y, const char *t, uint8_t c, int s = 1) {
  for (; *t; t++, x += 4 * s)
    drawChar(x, y, *t, c, s);
}
static void drawTextC(int cx, int y, const char *t, uint8_t c, int s = 1) {
  drawText(cx - textW(t, s) / 2, y, t, c, s);
}
static void drawTextShadowC(int cx, int y, const char *t, uint8_t c, int s = 1) {
  drawTextC(cx + 1, y + 1, t, C_BLACK, s);
  drawTextC(cx, y, t, c, s);
}

/* ------------------------------------------------------------ game state -- */
enum GameState { ST_TITLE, ST_PLAY, ST_MENU, ST_PLACE_RAFT, ST_PLACE_PAD,
                 ST_LAUNCH, ST_WIN };
static GameState state = ST_TITLE;
static uint32_t frame = 0, playFrames = 0;

enum { RES_WOOD, RES_SCRAP, RES_PLASTIC, RES_FUEL, RES_CHIP, RES_N };
static int inv[RES_N];
static const Sprite *RES_ICON[RES_N] = {&SPR_WOOD, &SPR_SCRAP, &SPR_PLASTIC,
                                        &SPR_FUEL, &SPR_CHIP};

struct Cost {
  uint8_t res, n;
};

enum { TL_WATER = 0, TL_RAFT, TL_POD, TL_PAD };
static uint8_t tiles[TH][TW];
static uint8_t tileHp[TH][TW];
static int podTX, podTY, padTX, padTY;

static float plX, plY, tgX, tgY;
static bool walking, holdWalk;

static int rocketParts;
static const char *PART_NAME[5] = {"ENGINE", "FUEL TANK", "HULL",
                                   "NAV COMPUTER", "NOSE CONE"};
static const Cost PART_COST[5][2] = {
    {{RES_SCRAP, 5}, {RES_CHIP, 2}},    {{RES_PLASTIC, 4}, {RES_FUEL, 3}},
    {{RES_SCRAP, 6}, {RES_WOOD, 2}},    {{RES_CHIP, 3}, {RES_PLASTIC, 2}},
    {{RES_SCRAP, 3}, {RES_PLASTIC, 2}}};

/* Debris */
enum { D_WOOD, D_SCRAP, D_PLASTIC, D_FUEL, D_CRATE, D_N };
static const Sprite *DEB_SPR[D_N] = {&SPR_WOOD, &SPR_SCRAP, &SPR_PLASTIC,
                                     &SPR_FUEL, &SPR_CRATE};
struct Debris {
  bool on, hooked;
  uint8_t type;
  float x, y, vx, vy, ph;
};
#define MAX_DEB 12
static Debris deb[MAX_DEB];
static int spawnT;

/* Ocean current: slowly changes direction */
static float curAng, curTarget, curX, curY;
static int curTimer;

struct Wave {
  float x, y;
  uint8_t len, ph;
};
#define NUM_WAVES 70
static Wave waves[NUM_WAVES];

/* Hook */
enum { HK_IDLE, HK_OUT, HK_BACK };
static int hookState, hookTarget;
static float hkX, hkY, hookRange;
static bool hookUpgraded;
static int rangeFlash;

/* Shark */
enum { SH_CIRCLE, SH_ATTACK, SH_BITE, SH_FLEE };
static int shState, shTimer, shTX, shTY;
static float shX, shY, shAng, shAX, shAY, shVX, shVY;
static float raftCX = 80, raftCY = 80, raftR = 12;

/* Launch */
static int launchT, winT;
static float rkOff, rkV;

/* Particles and floating "+1" texts */
struct Particle {
  float x, y, vx, vy;
  int16_t life;
  uint8_t col;
};
#define MAX_PART 120
static Particle parts[MAX_PART];

struct FloatText {
  float x, y;
  int life;
  uint8_t res, n;
};
#define MAX_FT 8
static FloatText fts[MAX_FT];

/* Message bar */
static char msg[48];
static int msgT;
static void setMsg(const char *m, int frames = 120) {
  strncpy(msg, m, sizeof(msg) - 1);
  msg[sizeof(msg) - 1] = 0;
  msgT = frames;
}

/* Crafting menu */
enum { A_RAFT, A_HOOK, A_PAD, A_PART, A_LAUNCH, A_CLOSE };
struct MenuRow {
  char name[24];
  Cost cost[2];
  uint8_t nc, action;
  const char *note; /* non-null = cannot build (done / locked) */
};
static MenuRow rows[6];
static int nRows;
static Cost pendingCost[2];
static int pendingNc;

#define MENU_X 8
#define MENU_Y 15
#define MENU_W 144
#define ROW_Y0 29
#define ROW_H 17

/* Touch shared with the core-0 touch task: bit31 pressed, x<<16 | y */
static volatile uint32_t touchState = 0;

/* --------------------------------------------------------------- helpers -- */
static inline int tileAt(float x, float y) {
  int tx = (int)floorf(x / TILE), ty = (int)floorf(y / TILE);
  if (tx < 0 || ty < 0 || tx >= TW || ty >= TH)
    return TL_WATER;
  return tiles[ty][tx];
}

static inline float dist2(float ax, float ay, float bx, float by) {
  return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

static void spawnParticle(float x, float y, float vx, float vy, int life,
                          uint8_t col) {
  for (Particle &p : parts)
    if (p.life <= 0) {
      p = {x, y, vx, vy, (int16_t)life, col};
      return;
    }
}

static void splash(float x, float y, int n) {
  for (int i = 0; i < n; i++) {
    float a = rndf() * 6.2832f, s = 0.3f + rndf() * 0.7f;
    spawnParticle(x, y, cosf(a) * s, sinf(a) * s, 8 + rndi(12),
                  (i & 1) ? C_FOAM : C_WHITE);
  }
}

static void floatText(float x, float y, uint8_t res, int n) {
  for (FloatText &f : fts)
    if (f.life <= 0) {
      f = {x, y, 40, res, (uint8_t)n};
      return;
    }
}

static bool canAfford(const Cost *c, int nc) {
  for (int i = 0; i < nc; i++)
    if (inv[c[i].res] < c[i].n)
      return false;
  return true;
}

static void pay(const Cost *c, int nc, int sign) {
  for (int i = 0; i < nc; i++)
    inv[c[i].res] -= sign * c[i].n;
}

static bool validRaftSpot(int tx, int ty) {
  if (tx < 1 || ty < 2 || tx >= TW - 1 || ty >= TH - 2)
    return false;
  if (tiles[ty][tx] != TL_WATER)
    return false;
  return tiles[ty - 1][tx] || tiles[ty + 1][tx] || tiles[ty][tx - 1] ||
         tiles[ty][tx + 1];
}

static bool validPadSpot(int tx, int ty) {
  if (tx < 0 || ty < 0 || tx >= TW || ty >= TH)
    return false;
  return tiles[ty][tx] == TL_RAFT && ty >= 4; /* room for the rocket above */
}

/* ------------------------------------------------------------- new game -- */
static void spawnDebris(bool onScreen) {
  for (Debris &d : deb) {
    if (d.on)
      continue;
    int r = rndi(100);
    d.type = r < 34 ? D_WOOD : r < 62 ? D_SCRAP : r < 82 ? D_PLASTIC
           : r < 93 ? D_FUEL : D_CRATE;
    float dx = cosf(curAng), dy = sinf(curAng);
    float spd = 0.22f + rndf() * 0.2f;
    d.vx = dx * spd - dy * (rndf() - 0.5f) * 0.08f;
    d.vy = dy * spd + dx * (rndf() - 0.5f) * 0.08f;
    if (onScreen) {
      do {
        d.x = 12 + rndf() * 136;
        d.y = 16 + rndf() * 128;
      } while (tileAt(d.x, d.y) != TL_WATER || dist2(d.x, d.y, 80, 80) < 500);
    } else {
      float s = (rndf() - 0.5f) * 150;
      d.x = 80 - dx * 112 - dy * s;
      d.y = 80 - dy * 112 + dx * s;
    }
    d.ph = rndf() * 6.28f;
    d.hooked = false;
    d.on = true;
    return;
  }
}

static void newGame() {
  memset(tiles, 0, sizeof(tiles));
  memset(tileHp, 3, sizeof(tileHp));
  podTX = 9;
  podTY = 9;
  tiles[9][9] = TL_POD;
  tiles[9][10] = tiles[10][9] = tiles[10][10] = TL_RAFT;
  padTX = padTY = -1;
  rocketParts = 0;
  hookUpgraded = false;
  hookRange = 42;
  hookState = HK_IDLE;
  memset(inv, 0, sizeof(inv));
  plX = 10 * TILE + 4;
  plY = 10 * TILE + 6;
  walking = holdWalk = false;
  memset(deb, 0, sizeof(deb));
  memset(parts, 0, sizeof(parts));
  memset(fts, 0, sizeof(fts));
  curAng = curTarget = rndf() * 6.28f;
  curTimer = 900;
  for (int i = 0; i < 5; i++)
    spawnDebris(true);
  spawnT = 60;
  shState = SH_CIRCLE;
  shAng = rndf() * 6.28f;
  shX = 80 + cosf(shAng) * 40;
  shY = 80 + sinf(shAng) * 40;
  shTimer = 1200; /* first attack after ~40s */
  playFrames = 0;
  setMsg("YOUR POD CRASHED! HOOK SOME DEBRIS", 200);
}

/* ---------------------------------------------------------------- update -- */
static void updateOcean() {
  if (--curTimer <= 0) {
    curTarget = curAng + (rndf() - 0.5f) * 2.4f;
    curTimer = 900 + rndi(900);
  }
  curAng += (curTarget - curAng) * 0.002f;
  curX = cosf(curAng) * 0.3f;
  curY = sinf(curAng) * 0.3f;
  for (Wave &w : waves) {
    w.x += curX * 0.4f;
    w.y += curY * 0.4f;
    if (w.x < -6) w.x += LW + 10;
    if (w.x > LW + 4) w.x -= LW + 10;
    if (w.y < -2) w.y += LH + 4;
    if (w.y > LH + 2) w.y -= LH + 4;
  }
}

static void collect(Debris &d) {
  float fx = plX, fy = plY - 10;
  switch (d.type) {
  case D_WOOD: {
    int n = 1 + (rndi(3) == 0);
    inv[RES_WOOD] += n;
    floatText(fx, fy, RES_WOOD, n);
    break;
  }
  case D_SCRAP: inv[RES_SCRAP]++; floatText(fx, fy, RES_SCRAP, 1); break;
  case D_PLASTIC: inv[RES_PLASTIC]++; floatText(fx, fy, RES_PLASTIC, 1); break;
  case D_FUEL: inv[RES_FUEL]++; floatText(fx, fy, RES_FUEL, 1); break;
  case D_CRATE: {
    int n = 1 + (rndi(3) == 0);
    inv[RES_CHIP] += n;
    floatText(fx - 8, fy, RES_CHIP, n);
    int extra = rndi(3);
    inv[extra]++;
    floatText(fx + 8, fy, extra, 1);
    setMsg("A CRATE! CIRCUIT CHIPS INSIDE", 90);
    break;
  }
  }
  d.on = false;
}

static void updateDebris() {
  if (--spawnT <= 0) {
    spawnDebris(false);
    spawnT = 45 + rndi(45);
  }
  for (Debris &d : deb) {
    if (!d.on || d.hooked)
      continue;
    d.x += d.vx;
    d.y += d.vy;
    d.ph += 0.08f;
    if (dist2(d.x, d.y, 80, 80) > 135 * 135)
      d.on = false;
  }
}

static void updateHook() {
  float ox = plX, oy = plY - 4;
  if (hookState == HK_OUT) {
    Debris &d = deb[hookTarget];
    if (!d.on) {
      hookState = HK_BACK;
      return;
    }
    float dx = d.x - hkX, dy = d.y - hkY, l = sqrtf(dx * dx + dy * dy);
    if (l < 4) {
      d.hooked = true;
      hookState = HK_BACK;
      splash(d.x, d.y, 6);
    } else {
      hkX += dx / l * 4;
      hkY += dy / l * 4;
    }
  } else if (hookState == HK_BACK) {
    float dx = ox - hkX, dy = oy - hkY, l = sqrtf(dx * dx + dy * dy);
    Debris *d = hookTarget >= 0 && deb[hookTarget].on && deb[hookTarget].hooked
                    ? &deb[hookTarget] : nullptr;
    if (l < 3) {
      if (d)
        collect(*d);
      hookState = HK_IDLE;
      hookTarget = -1;
    } else {
      float s = l < 2.5f ? l : 2.5f;
      hkX += dx / l * s;
      hkY += dy / l * s;
      if (d) {
        d->x = hkX;
        d->y = hkY;
        if (rndi(4) == 0 && tileAt(hkX, hkY) == TL_WATER)
          spawnParticle(hkX, hkY, 0, 0, 10, C_FOAM);
      }
    }
  }
}

static void updatePlayer() {
  if (!walking)
    return;
  float dx = tgX - plX, dy = tgY - plY, l = sqrtf(dx * dx + dy * dy);
  if (l < 1) {
    walking = false;
    return;
  }
  float sx = dx / l, sy = dy / l;
  if (tileAt(plX + sx, plY + sy) != TL_WATER) {
    plX += sx;
    plY += sy;
  } else if (fabsf(sx) > 0.1f && tileAt(plX + (sx > 0 ? 1 : -1), plY) != TL_WATER) {
    plX += sx > 0 ? 1 : -1;
  } else if (fabsf(sy) > 0.1f && tileAt(plX, plY + (sy > 0 ? 1 : -1)) != TL_WATER) {
    plY += sy > 0 ? 1 : -1;
  } else {
    walking = false;
  }
}

static void computeRaftShape() {
  float sx = 0, sy = 0;
  int n = 0;
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < TW; x++)
      if (tiles[y][x]) {
        sx += x * TILE + 4;
        sy += y * TILE + 4;
        n++;
      }
  raftCX = sx / n;
  raftCY = sy / n;
  float r = 0;
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < TW; x++)
      if (tiles[y][x])
        r = max(r, sqrtf(dist2(x * TILE + 4, y * TILE + 4, raftCX, raftCY)));
  raftR = r + 6;
}

static void destroyTile(int tx, int ty) {
  tiles[ty][tx] = TL_WATER;
  splash(tx * TILE + 4, ty * TILE + 4, 16);
  for (int i = 0; i < 6; i++)
    spawnParticle(tx * TILE + rndi(8), ty * TILE + rndi(8),
                  (rndf() - 0.5f) * 0.6f, (rndf() - 0.5f) * 0.6f, 20 + rndi(20),
                  C_WOOD_D);
  if (tileAt(plX, plY) == TL_WATER) {
    plX = podTX * TILE + 4;
    plY = podTY * TILE + 6;
    walking = false;
    setMsg("YOU SCRAMBLED BACK TO THE POD!", 120);
  } else {
    setMsg("THE SHARK ATE A RAFT TILE!", 120);
  }
}

static bool pickSharkTarget() {
  static const int NX[4] = {1, -1, 0, 0}, NY[4] = {0, 0, 1, -1};
  int cands = 0;
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < TW; x++) {
      if (tiles[y][x] != TL_RAFT)
        continue;
      for (int k = 0; k < 4; k++) {
        int nx = x + NX[k], ny = y + NY[k];
        if (nx < 0 || ny < 0 || nx >= TW || ny >= TH || tiles[ny][nx])
          continue;
        /* reservoir-pick one (tile, water side) pair uniformly */
        if (rndi(++cands) == 0) {
          shTX = x;
          shTY = y;
          shAX = x * TILE + 4 + NX[k] * 7;
          shAY = y * TILE + 4 + NY[k] * 7;
        }
      }
    }
  return cands > 0;
}

static void sharkMoveTo(float tx, float ty, float maxSpeed) {
  float dx = tx - shX, dy = ty - shY, l = sqrtf(dx * dx + dy * dy);
  if (l < 0.01f) {
    shVX = shVY = 0;
    return;
  }
  float s = l < maxSpeed ? l : maxSpeed;
  shVX = dx / l * s;
  shVY = dy / l * s;
  shX += shVX;
  shY += shVY;
}

static void updateShark() {
  computeRaftShape();
  switch (shState) {
  case SH_CIRCLE: {
    shAng += 0.008f;
    float r = raftR + 16;
    sharkMoveTo(raftCX + cosf(shAng) * r, raftCY + sinf(shAng) * r, 1.0f);
    if (--shTimer <= 0) {
      if (pickSharkTarget()) {
        shState = SH_ATTACK;
        setMsg("A SHARK IS COMING! TAP IT!", 150);
      } else {
        shTimer = 600;
      }
    }
    break;
  }
  case SH_ATTACK:
    if (tiles[shTY][shTX] != TL_RAFT) {
      shState = SH_CIRCLE;
      shTimer = 400;
      break;
    }
    sharkMoveTo(shAX, shAY, 0.9f);
    if (dist2(shX, shY, shAX, shAY) < 2) {
      shState = SH_BITE;
      shTimer = 0;
    }
    break;
  case SH_BITE:
    shTimer++;
    shVX = shVY = 0;
    if (tiles[shTY][shTX] != TL_RAFT) {
      shState = SH_CIRCLE;
      shTimer = 600 + rndi(400);
      break;
    }
    if (rndi(5) == 0)
      splash(shX, shY, 2);
    if (shTimer % 45 == 0) {
      splash(shX, shY, 8);
      if (--tileHp[shTY][shTX] == 0) {
        destroyTile(shTX, shTY);
        shState = SH_CIRCLE;
        shTimer = 600 + rndi(500);
      }
    }
    break;
  case SH_FLEE: {
    float dx = shX - raftCX, dy = shY - raftCY, l = sqrtf(dx * dx + dy * dy) + 0.01f;
    shVX = dx / l * 2;
    shVY = dy / l * 2;
    shX += shVX;
    shY += shVY;
    if (--shTimer <= 0) {
      shState = SH_CIRCLE;
      shAng = atan2f(shY - raftCY, shX - raftCX);
      shTimer = 700 + rndi(500);
    }
    break;
  }
  }
}

static void updateParticles() {
  for (Particle &p : parts) {
    if (p.life <= 0)
      continue;
    p.x += p.vx;
    p.y += p.vy;
    p.vx *= 0.94f;
    p.vy *= 0.94f;
    p.life--;
  }
  for (FloatText &f : fts)
    if (f.life > 0) {
      f.y -= 0.3f;
      f.life--;
    }
}

static void updateLaunch() {
  launchT++;
  float bx = padTX * TILE + 4, by = padTY * TILE + 6;
  if (launchT < 90) {
    if (rndi(2) == 0)
      spawnParticle(bx + (rndf() - 0.5f) * 8, by, (rndf() - 0.5f) * 1.2f,
                    -rndf() * 0.3f, 20 + rndi(20), C_SMOKE);
  } else {
    rkV += 0.05f;
    rkOff += rkV;
    float ey = by - rkOff;
    for (int i = 0; i < 3; i++)
      spawnParticle(bx + (rndf() - 0.5f) * 4, ey + 1, (rndf() - 0.5f) * 0.6f,
                    1 + rndf() * 1.5f, 6 + rndi(8), rndi(2) ? C_FIRE : C_YELLOW);
    if (rndi(2) == 0)
      spawnParticle(bx + (rndf() - 0.5f) * 6, ey + 4, (rndf() - 0.5f) * 0.8f,
                    rndf() * 0.5f, 30 + rndi(30), C_SMOKE);
    if (rkOff > 200 && launchT > 200) {
      state = ST_WIN;
      winT = 0;
    }
  }
}

/* ------------------------------------------------------------------ menu -- */
static void addRow(const char *name, uint8_t action, const char *note,
                   Cost a = {0, 0}, Cost b = {0, 0}) {
  MenuRow &r = rows[nRows++];
  snprintf(r.name, sizeof(r.name), "%s", name);
  r.action = action;
  r.note = note;
  r.nc = 0;
  if (a.n) r.cost[r.nc++] = a;
  if (b.n) r.cost[r.nc++] = b;
}

static void buildMenu() {
  nRows = 0;
  addRow("RAFT TILE", A_RAFT, nullptr, {RES_WOOD, 3});
  addRow("BETTER HOOK", A_HOOK, hookUpgraded ? "DONE" : nullptr,
         {RES_SCRAP, 3}, {RES_PLASTIC, 2});
  addRow("LAUNCH PAD", A_PAD, padTX >= 0 ? "DONE" : nullptr, {RES_WOOD, 6},
         {RES_SCRAP, 4});
  if (rocketParts < 5) {
    char name[24];
    snprintf(name, sizeof(name), "ROCKET %s", PART_NAME[rocketParts]);
    addRow(name, A_PART, padTX < 0 ? "NEEDS PAD" : nullptr,
           PART_COST[rocketParts][0], PART_COST[rocketParts][1]);
  } else {
    addRow("FUEL UP + LAUNCH!", A_LAUNCH, nullptr, {RES_FUEL, 4});
  }
  addRow("CLOSE", A_CLOSE, nullptr);
}

static void menuTap(int lx, int ly) {
  if (lx < MENU_X || lx >= MENU_X + MENU_W || ly < ROW_Y0 ||
      ly >= ROW_Y0 + nRows * ROW_H) {
    state = ST_PLAY;
    return;
  }
  MenuRow &r = rows[(ly - ROW_Y0) / ROW_H];
  if (r.action == A_CLOSE) {
    state = ST_PLAY;
    return;
  }
  if (r.note) {
    setMsg(r.action == A_PART ? "BUILD A LAUNCH PAD FIRST" : "ALREADY BUILT", 90);
    return;
  }
  if (!canAfford(r.cost, r.nc)) {
    setMsg("NOT ENOUGH MATERIALS", 90);
    return;
  }
  pay(r.cost, r.nc, 1);
  switch (r.action) {
  case A_RAFT:
  case A_PAD:
    memcpy(pendingCost, r.cost, sizeof(pendingCost));
    pendingNc = r.nc;
    state = r.action == A_RAFT ? ST_PLACE_RAFT : ST_PLACE_PAD;
    setMsg(r.action == A_RAFT ? "TAP WATER NEXT TO THE RAFT"
                              : "TAP A RAFT TILE FOR THE PAD", 600);
    return;
  case A_HOOK:
    hookUpgraded = true;
    hookRange = 70;
    setMsg("HOOK RANGE INCREASED!", 120);
    break;
  case A_PART: {
    char m[48];
    snprintf(m, sizeof(m), "%s BUILT! (%d/5)", PART_NAME[rocketParts],
             rocketParts + 1);
    rocketParts++;
    setMsg(m, 150);
    splash(padTX * TILE + 4, padTY * TILE + 4, 10);
    break;
  }
  case A_LAUNCH:
    state = ST_LAUNCH;
    launchT = 0;
    rkOff = rkV = 0;
    hookState = HK_IDLE;
    setMsg("ALL SYSTEMS GO...", 300);
    return;
  }
  buildMenu();
}

/* ----------------------------------------------------------------- input -- */
static void touchTask(void *) {
  for (;;) {
    uint32_t s = 0;
    if (touch_touched()) {
      int x = constrain(touch_last_x, 0, SCR_W - 1);
      int y = constrain(touch_last_y, 0, SCR_W - 1);
      s = 0x80000000u | ((uint32_t)x << 16) | (uint32_t)y;
    }
    touchState = s;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void playTap(int lx, int ly) {
  /* 1. shark */
  if ((shState == SH_ATTACK || shState == SH_BITE) &&
      dist2(lx, ly, shX, shY) < 12 * 12) {
    shState = SH_FLEE;
    shTimer = 70;
    splash(shX, shY, 12);
    setMsg("SHOO! THE SHARK SWAM OFF", 120);
    return;
  }
  /* 2. pod */
  int tx = lx / TILE, ty = ly / TILE;
  if (tx == podTX && ty == podTY) {
    buildMenu();
    state = ST_MENU;
    walking = false;
    return;
  }
  /* 3. debris (only ones visible in the water) */
  int best = -1;
  float bd = 9 * 9;
  for (int i = 0; i < MAX_DEB; i++) {
    Debris &d = deb[i];
    if (!d.on || d.hooked || tileAt(d.x, d.y) != TL_WATER)
      continue;
    float dd = dist2(lx, ly, d.x, d.y);
    if (dd < bd) {
      bd = dd;
      best = i;
    }
  }
  if (best >= 0) {
    if (hookState != HK_IDLE) {
      setMsg("YOUR HOOK IS BUSY", 45);
    } else if (dist2(plX, plY - 4, deb[best].x, deb[best].y) >
               hookRange * hookRange) {
      setMsg("TOO FAR - WALK CLOSER", 90);
      rangeFlash = 45;
    } else {
      hookState = HK_OUT;
      hookTarget = best;
      hkX = plX;
      hkY = plY - 4;
    }
    return;
  }
  /* 4. walk */
  if (tileAt(lx, ly) != TL_WATER) {
    tgX = lx;
    tgY = ly;
    walking = true;
    holdWalk = true;
    return;
  }
  setMsg("TAP DEBRIS TO THROW YOUR HOOK", 90);
  rangeFlash = 45;
}

static void handleInput() {
  static bool wasDown = false;
  uint32_t s = touchState;
  bool down = s >> 31;
  int lx = ((s >> 16) & 0x7FFF) / PX, ly = (s & 0xFFFF) / PX;
  bool pressed = down && !wasDown;
  wasDown = down;
  if (!down)
    holdWalk = false;

  switch (state) {
  case ST_TITLE:
    if (pressed) {
      newGame();
      state = ST_PLAY;
    }
    break;
  case ST_WIN:
    if (pressed && winT > 60)
      state = ST_TITLE;
    break;
  case ST_MENU:
    if (pressed)
      menuTap(lx, ly);
    break;
  case ST_PLACE_RAFT:
  case ST_PLACE_PAD:
    if (pressed) {
      int tx = lx / TILE, ty = ly / TILE;
      if (state == ST_PLACE_RAFT && validRaftSpot(tx, ty)) {
        tiles[ty][tx] = TL_RAFT;
        tileHp[ty][tx] = 3;
        splash(tx * TILE + 4, ty * TILE + 4, 10);
        setMsg("RAFT EXTENDED", 90);
        state = ST_PLAY;
      } else if (state == ST_PLACE_PAD && validPadSpot(tx, ty)) {
        tiles[ty][tx] = TL_PAD;
        padTX = tx;
        padTY = ty;
        setMsg("LAUNCH PAD READY - BUILD THE ROCKET", 150);
        state = ST_PLAY;
      } else {
        pay(pendingCost, pendingNc, -1); /* refund */
        setMsg("CANCELLED", 60);
        state = ST_PLAY;
      }
    }
    break;
  case ST_PLAY:
    if (pressed)
      playTap(lx, ly);
    else if (down && holdWalk && tileAt(lx, ly) != TL_WATER) {
      tgX = lx;
      tgY = ly;
      walking = true;
    }
    break;
  default:
    break;
  }
}

/* ---------------------------------------------------------------- render -- */
static void drawRaftTile(int tx, int ty) {
  int x0 = tx * TILE, y0 = ty * TILE;
  int hp = tileHp[ty][tx];
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      uint8_t c = (y % 3 == 2) ? C_WOOD_D : C_WOOD_L;
      if (x == ((y / 3) & 1 ? 2 : 5))
        c = C_WOOD_D;
      if (hp < 3 && (hash2(x0 + x, y0 + y) & 7) < (uint32_t)(3 - hp) * 2)
        c = C_WATER;
      pset(x0 + x, y0 + y, c);
    }
}

static void drawPadTile(int tx, int ty) {
  int x0 = tx * TILE, y0 = ty * TILE;
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      bool edge = x == 0 || y == 0 || x == 7 || y == 7;
      uint8_t c = edge ? (((x + y) >> 1) & 1 ? C_YELLOW : C_BLACK) : C_METAL_L;
      pset(x0 + x, y0 + y, c);
    }
  pset(x0 + 2, y0 + 2, C_METAL_D);
  pset(x0 + 5, y0 + 2, C_METAL_D);
  pset(x0 + 2, y0 + 5, C_METAL_D);
  pset(x0 + 5, y0 + 5, C_METAL_D);
}

static void drawRocket(int cx, int baseY) {
  int y = baseY;
  for (int i = 0; i < rocketParts; i++) {
    y -= SPR_ROCKET[i].h;
    drawSprite(cx - 4, y, SPR_ROCKET[i]);
  }
}

static void drawWorld() {
  memset(scr, C_WATER, sizeof(scr));

  for (int i = 0; i < NUM_WAVES; i++) {
    Wave &w = waves[i];
    if (((frame + w.ph * 7) / 20) % 6 == 0)
      continue;
    uint8_t c = (i % 5 == 0) ? C_FOAM : C_WATER_MID;
    fillRect((int)w.x, (int)w.y, w.len, 1, c);
  }

  for (Debris &d : deb) {
    if (!d.on)
      continue;
    const Sprite &s = *DEB_SPR[d.type];
    int bob = d.hooked ? 0 : (int)roundf(sinf(d.ph) * 0.7f);
    drawSprite((int)d.x - s.w / 2, (int)d.y - s.h / 2 + bob, s);
    if (!d.hooked && ((frame + (int)(d.ph * 10)) & 31) == 0)
      spawnParticle(d.x + s.w / 2 + 1, d.y + s.h / 2, 0, 0, 12, C_FOAM);
  }

  /* raft shadow, then tiles */
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < TW; x++)
      if (tiles[y][x])
        fillRect(x * TILE + 1, y * TILE + 2, TILE, TILE, C_SHADOW);
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < TW; x++) {
      switch (tiles[y][x]) {
      case TL_RAFT: drawRaftTile(x, y); break;
      case TL_POD:
        drawRaftTile(x, y);
        drawSprite(x * TILE, y * TILE, SPR_POD);
        /* blinking beacon so players notice the pod is interactive */
        if (state == ST_PLAY && (frame & 31) < 16)
          pset(x * TILE + 3, y * TILE, C_RED);
        break;
      case TL_PAD: drawPadTile(x, y); break;
      }
    }

  /* shark */
  if (state != ST_WIN) {
    int sx = (int)shX, sy = (int)shY;
    if (shState == SH_BITE)
      sx += (frame & 2) ? 1 : -1;
    drawSprite(sx - 2, sy - 3, SPR_FIN);
    if (fabsf(shVX) + fabsf(shVY) > 0.2f) {
      pset((int)(shX - shVX * 4), (int)(shY - shVY * 4) + 1, C_FOAM);
      pset((int)(shX - shVX * 7), (int)(shY - shVY * 7) + 1, C_FOAM);
    }
    fillRect(sx - 2, sy + 1, 5, 1, C_FOAM);
  }

  /* rocket */
  if (padTX >= 0) {
    int cx = padTX * TILE + 4, by = padTY * TILE + 6;
    if (state == ST_LAUNCH) {
      by -= (int)rkOff;
      if (launchT < 90 || rkOff < 10)
        cx += (frame & 1) ? 1 : 0; /* rumble */
    }
    drawRocket(cx, by);
  }

  /* hook line */
  if (hookState != HK_IDLE) {
    drawLine((int)plX, (int)plY - 4, (int)hkX, (int)hkY, C_GREY);
    fillRect((int)hkX - 1, (int)hkY - 1, 2, 2, C_METAL_L);
  }

  /* player (inside the rocket after launch) */
  if (state != ST_LAUNCH) {
    int bob = walking && ((frame / 4) & 1) ? 1 : 0;
    drawSprite((int)plX - 2, (int)plY - 7 - bob, SPR_PLAYER);
  }

  /* hook range ring */
  if (rangeFlash > 0) {
    for (int a = 0; a < 48; a++) {
      float t = a * 6.2832f / 48;
      pset((int)(plX + cosf(t) * hookRange), (int)(plY - 4 + sinf(t) * hookRange),
           (a & 1) ? C_YELLOW : C_WHITE);
    }
  }

  for (Particle &p : parts)
    if (p.life > 0)
      pset((int)p.x, (int)p.y, p.col);

  /* placement highlights */
  if ((state == ST_PLACE_RAFT || state == ST_PLACE_PAD) && ((frame >> 3) & 1)) {
    for (int y = 0; y < TH; y++)
      for (int x = 0; x < TW; x++)
        if (state == ST_PLACE_RAFT ? validRaftSpot(x, y) : validPadSpot(x, y))
          rectOutline(x * TILE, y * TILE, TILE, TILE, C_YELLOW);
  }

  for (FloatText &f : fts) {
    if (f.life <= 0)
      continue;
    char t[6];
    snprintf(t, sizeof(t), "+%d", f.n);
    int x = (int)f.x - 6, y = (int)f.y;
    drawText(x + 1, y + 1, t, C_BLACK);
    drawText(x, y, t, C_WHITE);
    drawSprite(x + 9, y, *RES_ICON[f.res]);
  }
}

static void drawHud() {
  fillRect(0, 0, LW, HUD_H, C_NAVY);
  char t[12];
  for (int r = 0; r < RES_N; r++) {
    int x = 2 + r * 21;
    const Sprite &s = *RES_ICON[r];
    drawSprite(x + (5 - s.w) / 2, 2 + (5 - s.h) / 2, s);
    snprintf(t, sizeof(t), "%d", inv[r]);
    drawText(x + 7, 2, t, C_WHITE);
  }
  snprintf(t, sizeof(t), "ROCKET %d/5", rocketParts);
  drawText(LW - textW(t) - 2, 2, t, rocketParts == 5 ? C_YELLOW : C_ORANGE);

  fillRect(0, MSG_Y, LW, LH - MSG_Y, C_NAVY);
  const char *m = msgT > 0 ? msg : nullptr;
  if (!m) {
    switch (state) {
    case ST_PLAY:
      m = rocketParts == 5 ? "ROCKET READY! FUEL UP AT THE POD"
        : padTX < 0        ? "TAP THE POD TO CRAFT"
                           : "COLLECT PARTS FOR THE ROCKET";
      break;
    default: m = ""; break;
    }
  }
  drawTextC(LW / 2, MSG_Y + 2, m, C_WHITE);
}

static void drawCost(int x, int y, const Cost &c, bool ok) {
  const Sprite &s = *RES_ICON[c.res];
  drawSprite(x + (5 - s.w) / 2, y + (5 - s.h) / 2, s);
  char t[4];
  snprintf(t, sizeof(t), "%d", c.n);
  drawText(x + 7, y, t, ok ? C_WHITE : C_RED);
}

static void drawMenu() {
  fillRect(MENU_X, MENU_Y, MENU_W, ROW_Y0 - MENU_Y + nRows * ROW_H + 4, C_NAVY);
  rectOutline(MENU_X, MENU_Y, MENU_W, ROW_Y0 - MENU_Y + nRows * ROW_H + 4,
              C_ORANGE);
  drawTextC(LW / 2, MENU_Y + 5, "POD FABRICATOR", C_ORANGE);
  for (int i = 0; i < nRows; i++) {
    MenuRow &r = rows[i];
    int y = ROW_Y0 + i * ROW_H;
    bool ok = !r.note && canAfford(r.cost, r.nc);
    fillRect(MENU_X + 3, y + 1, MENU_W - 6, ROW_H - 2,
             r.action == A_CLOSE ? C_PANEL : ok ? C_PANEL_HI : C_PANEL);
    uint8_t nameCol = r.action == A_CLOSE ? C_WHITE
                    : r.note             ? C_GREY
                    : ok                 ? C_WHITE : C_GREY;
    if (r.action == A_CLOSE)
      drawTextC(LW / 2, y + 6, r.name, nameCol);
    else
      drawText(MENU_X + 7, y + 6, r.name, nameCol);
    int rx = MENU_X + MENU_W - 6;
    if (r.note) {
      drawText(rx - textW(r.note), y + 6, r.note, C_GREY);
    } else {
      for (int k = r.nc - 1; k >= 0; k--) {
        rx -= 15;
        drawCost(rx, y + 6, r.cost[k], inv[r.cost[k].res] >= r.cost[k].n);
      }
    }
  }
}

static void drawTitle() {
  fillRect(12, 22, LW - 24, 112, C_NAVY);
  rectOutline(12, 22, LW - 24, 112, C_ORANGE);
  drawTextShadowC(LW / 2, 30, "STRANDED", C_ORANGE, 3);
  drawTextC(LW / 2, 52, "BUILD A ROCKET. GO HOME.", C_WHITE);
  drawTextC(LW / 2, 68, "TAP DEBRIS TO HOOK IT", C_FOAM);
  drawTextC(LW / 2, 77, "TAP THE RAFT TO WALK", C_FOAM);
  drawTextC(LW / 2, 86, "TAP THE POD TO CRAFT", C_FOAM);
  drawTextC(LW / 2, 95, "TAP SHARKS TO SCARE THEM", C_FOAM);
  if ((frame >> 4) & 1)
    drawTextC(LW / 2, 115, "TAP TO START", C_YELLOW);
}

static void drawWin() {
  memset(scr, C_SPACE, sizeof(scr));
  for (int i = 0; i < 70; i++) {
    uint32_t h = hash2(i, 77);
    int x = h % LW, y = (h >> 8) % LH;
    if (((frame >> 3) + i) % 9 != 0)
      pset(x, y, (i % 4) ? C_GREY : C_STAR);
  }
  /* home planet */
  const int ex = 80, ey = 205, er = 72;
  for (int y = ey - er; y < LH; y++)
    for (int x = 0; x < LW; x++) {
      int dx = x - ex, dy = y - ey;
      int d = dx * dx + dy * dy;
      if (d > er * er)
        continue;
      uint8_t c = (hash2(x / 5 + 3, y / 4) % 3 == 0) ? C_EARTH_G : C_EARTH_B;
      if (d > (er - 2) * (er - 2))
        c = C_GLASS; /* atmosphere rim */
      pset(x, y, c);
    }
  /* rocket gliding toward home */
  int ry = 82 + min(winT, 120) / 6;
  int saved = rocketParts;
  rocketParts = 5;
  drawRocket(80, ry);
  rocketParts = saved;
  if (frame & 1)
    fillRect(78, ry, 4, 2 + rndi(3), (frame & 2) ? C_FIRE : C_YELLOW);

  drawTextShadowC(LW / 2, 14, "YOU MADE IT HOME!", C_YELLOW, 2);
  char t[24];
  uint32_t secs = playFrames * FRAME_MS / 1000;
  snprintf(t, sizeof(t), "TIME %lu:%02lu", secs / 60, secs % 60);
  drawTextC(LW / 2, 34, t, C_WHITE);
  if (winT > 60 && ((frame >> 4) & 1))
    drawTextC(LW / 2, 44, "TAP TO PLAY AGAIN", C_FOAM);
}

static void drawCountdown() {
  if (launchT < 90) {
    char t[2] = {(char)('3' - launchT / 30), 0};
    drawTextShadowC(LW / 2, 50, t, C_YELLOW, 5);
  } else if (launchT < 130) {
    drawTextShadowC(LW / 2, 50, "LIFT OFF!", C_YELLOW, 2);
  }
}

/* --------------------------------------------------------------- present -- */
/* Push changed logical pixels to the panel as 3x3 blocks. */
static void IRAM_ATTR present() {
  for (int y = 0; y < LH; y++) {
    uint32_t *a = (uint32_t *)&scr[y * LW];
    uint32_t *b = (uint32_t *)&shown[y * LW];
    uint16_t *row = fb + (y * PX) * SCR_W;
    bool wrote = false;
    for (int k = 0; k < LW / 4; k++) {
      uint32_t ca = a[k], cb = b[k];
      if (ca == cb)
        continue;
      for (int n = 0; n < 4; n++) {
        uint8_t ia = ca >> (n * 8);
        if (ia == (uint8_t)(cb >> (n * 8)))
          continue;
        uint16_t col = pal565[ia];
        uint16_t *p = row + (k * 4 + n) * PX;
        p[0] = p[1] = p[2] = col;
        p[SCR_W] = p[SCR_W + 1] = p[SCR_W + 2] = col;
        p[2 * SCR_W] = p[2 * SCR_W + 1] = p[2 * SCR_W + 2] = col;
      }
      b[k] = ca;
      wrote = true;
    }
    if (wrote)
      Cache_WriteBack_Addr((uint32_t)row, SCR_W * PX * 2);
  }
}

/* ----------------------------------------------------------------- setup -- */
void setup() {
  Serial.begin(115200);
  esp_task_wdt_init(120, true);
  esp_task_wdt_add(NULL);

  touch_init();
  gfx->begin(DISPLAY_PCLK_HZ);
  fb = gfx->getFramebuffer();
  gfx->fillScreen(BLACK);

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  for (int i = 0; i < C_COUNT; i++)
    pal565[i] = rgb565(PAL_RGB[i][0], PAL_RGB[i][1], PAL_RGB[i][2]);
  initCharMap();
  initFont();
  rngS ^= esp_random();

  for (Wave &w : waves) {
    w.x = rndf() * LW;
    w.y = rndf() * LH;
    w.len = 2 + rndi(4);
    w.ph = rndi(256);
  }
  memset(shown, 0xFF, sizeof(shown)); /* force a full first frame */
  newGame();                          /* world behind the title screen */
  msgT = 0;

  xTaskCreatePinnedToCore(touchTask, "touch", 4096, NULL, 5, NULL, 0);
  Serial.println("STRANDED ready");
}

void loop() {
  static TickType_t lastWake = xTaskGetTickCount();
  static uint32_t statFrames = 0, statDraw = 0, statPush = 0, statT0 = 0;
  esp_task_wdt_reset();
  frame++;

  handleInput();

  /* update */
  updateOcean();
  switch (state) {
  case ST_PLAY:
  case ST_PLACE_RAFT:
  case ST_PLACE_PAD:
    playFrames++;
    updateDebris();
    updateHook();
    updatePlayer();
    updateShark();
    break;
  case ST_TITLE:
    updateDebris();
    computeRaftShape();
    shAng += 0.008f;
    sharkMoveTo(raftCX + cosf(shAng) * (raftR + 16),
                raftCY + sinf(shAng) * (raftR + 16), 1.0f);
    break;
  case ST_LAUNCH:
    updateDebris();
    updateLaunch();
    break;
  case ST_WIN:
    winT++;
    break;
  default:
    break;
  }
  updateParticles();
  if (msgT > 0)
    msgT--;
  if (rangeFlash > 0)
    rangeFlash--;

  /* draw */
  uint32_t t0 = micros();
  if (state == ST_WIN) {
    drawWin();
  } else {
    drawWorld();
    if (state != ST_TITLE)
      drawHud();
    if (state == ST_MENU)
      drawMenu();
    else if (state == ST_TITLE)
      drawTitle();
    else if (state == ST_LAUNCH)
      drawCountdown();
  }
  uint32_t t1 = micros();
  present();
  uint32_t t2 = micros();

  statDraw += t1 - t0;
  statPush += t2 - t1;
  if (++statFrames >= 150) {
    Serial.printf("fps %.1f  draw %luus  push %luus\n",
                  statFrames * 1000.0f / (millis() - statT0),
                  statDraw / statFrames, statPush / statFrames);
    statFrames = statDraw = statPush = 0;
    statT0 = millis();
  }

  vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(FRAME_MS));
}
