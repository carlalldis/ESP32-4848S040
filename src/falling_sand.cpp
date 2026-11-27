/*******************************************************************************
 * Falling Sand for the ESP32-4848S040 (ESP32-S3, 480x480 ST7701 RGB panel)
 *
 * Performance design:
 *  - No GUI library: cells are written straight into the PSRAM framebuffer.
 *  - Simulation grid is 240x220 cells (2x2 px each), 1 byte per cell, kept in
 *    internal SRAM.
 *  - The grid is split into 16x16 chunks. Only chunks where something moved
 *    last frame are simulated; settled piles cost nothing.
 *  - Rendering compares the grid against a "shown" copy 4 cells at a time and
 *    only touches framebuffer pixels that actually changed, then writes the
 *    cache back per dirty row so the RGB DMA sees the new pixels.
 *  - Touch (GT911 over I2C) is polled in its own task on core 0 so I2C
 *    latency never stalls the simulation loop on core 1.
 *
 * Cell byte layout:  [7] update parity | [6:4] material | [3:0] shade/life
 ******************************************************************************/
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp32s3/rom/cache.h>

#include "display_4848s040.h" /* bus, gfx, panel init (colour fix) */

#include "touch.h"

/* RGB pixel clock (14MHz ~= 49Hz panel refresh, stable on this board). */
#define DISPLAY_PCLK_HZ 14000000

/* Simulation step period. 16ms ~= 60 steps per second. */
#define FRAME_MS 16

/* ---------------------------------------------------------------- layout -- */
#define SCR_W 480
#define SCALE 2
#define GW 240               /* grid width in cells  */
#define GH 220               /* grid height in cells */
#define PLAY_H (GH * SCALE)  /* 440 px of sand      */
#define BAR_Y PLAY_H         /* toolbar at the bottom */
#define BAR_H (480 - PLAY_H) /* 40 px                */
#define NUM_BTN 10
#define BTN_W (SCR_W / NUM_BTN)

#define CHUNK 16
#define CW ((GW + CHUNK - 1) / CHUNK) /* 15 chunk columns */
#define CH ((GH + CHUNK - 1) / CHUNK) /* 14 chunk rows    */

#define MAXFALL 3 /* cells a grain can drop per step through empty space */

/* ------------------------------------------------------------- materials -- */
enum { EMPTY = 0, SAND, WATER, STONE, WOOD, FIRE, OIL, STEAM };

static const uint8_t DENS[8] = {
    /* EMPTY */ 0, /* SAND */ 6,   /* WATER */ 4, /* STONE */ 255,
    /* WOOD */ 255, /* FIRE */ 1,  /* OIL */ 3,   /* STEAM */ 1};

#define TYPE(c) (((c) >> 4) & 7)
#define IS_EMPTY(c) (((c) & 0x70) == 0)
#define MAKE(t, s) ((uint8_t)(((t) << 4) | ((s) & 15)))

/* ----------------------------------------------------------------- state -- */
static uint8_t grid[GW * GH] __attribute__((aligned(4)));
static uint8_t shown[GW * GH] __attribute__((aligned(4)));
static uint8_t rowDirty[GH];
static uint16_t awake[CH];
static uint16_t awakeNext[CH];
static uint8_t parity = 0;
static uint32_t frameNo = 0;

static uint16_t palette[128];
static uint16_t *fb = nullptr;

/* Toolbar */
enum { BTN_SAND = 0, BTN_WATER, BTN_OIL, BTN_STONE, BTN_WOOD, BTN_FIRE,
       BTN_STEAM, BTN_ERASE, BTN_SIZE, BTN_CLEAR };
static const char *BTN_LABEL[NUM_BTN] = {"SAND",  "WATER", "OIL",   "STONE",
                                         "WOOD",  "FIRE",  "STEAM", "ERASE",
                                         "SIZE",  "CLEAR"};
static const uint8_t BTN_MATERIAL[8] = {SAND, WATER, OIL, STONE,
                                        WOOD, FIRE,  STEAM, EMPTY};
static const uint8_t BRUSH_SIZES[] = {2, 4, 7, 11};
static uint8_t tool = BTN_SAND;
static uint8_t brushIdx = 1;

/* Touch state shared with the core-0 touch task:
 * bit31 = pressed, bits 30..16 = x, bits 15..0 = y */
static volatile uint32_t touchState = 0;

/* ------------------------------------------------------------------ rng -- */
static uint32_t rngState = 0x12345678;
static inline uint32_t rnd() {
  uint32_t x = rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return rngState = x;
}

/* ---------------------------------------------------------------- colors -- */
static uint16_t rgb565(int r, int g, int b) {
  r = constrain(r, 0, 255);
  g = constrain(g, 0, 255);
  b = constrain(b, 0, 255);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void buildPalette() {
  for (int s = 0; s < 16; s++) {
    int v = s - 8; /* -8..7 brightness jitter */
    palette[MAKE(EMPTY, s)] = rgb565(10, 10, 18);
    palette[MAKE(SAND, s)] = rgb565(222 + v * 3, 188 + v * 3, 110 + v * 2);
    palette[MAKE(WATER, s)] = rgb565(35 + v, 95 + v * 2, 215 + v * 2);
    palette[MAKE(STONE, s)] = rgb565(112 + v * 4, 112 + v * 4, 118 + v * 4);
    palette[MAKE(WOOD, s)] = rgb565(112 + v * 3, 70 + v * 2, 35 + v);
    /* Fire: shade is remaining life, 15 = hot yellow, 0 = dim red */
    palette[MAKE(FIRE, s)] = rgb565(130 + s * 8, 10 + s * 14, s > 11 ? (s - 11) * 30 : 0);
    palette[MAKE(OIL, s)] = rgb565(78 + v * 2, 62 + v * 2, 28 + v);
    palette[MAKE(STEAM, s)] = rgb565(175 + v * 3, 185 + v * 3, 205 + v * 2);
  }
}

/* ------------------------------------------------------------ wake/dirty -- */
/* Mark the chunk holding (x,y) active next step, plus the neighbouring chunk
 * when the cell sits on a chunk edge (its neighbours may now be able to move). */
static inline void wake(int x, int y) {
  int cx = x >> 4, cy = y >> 4;
  int lx = x & 15, ly = y & 15;
  uint16_t m = 1u << cx;
  if (lx == 0 && cx > 0)
    m |= m >> 1;
  else if (lx == 15 && cx < CW - 1)
    m |= m << 1;
  awakeNext[cy] |= m;
  if (ly == 0 && cy > 0)
    awakeNext[cy - 1] |= m;
  else if (ly == 15 && cy < CH - 1)
    awakeNext[cy + 1] |= m;
}

static inline void touchCell(int x, int y) {
  wake(x, y);
  rowDirty[y] = 1;
}

/* Swap two cells, stamp both as updated this step. */
static inline void swapCells(int i, int x, int y, int j, int nx, int ny) {
  uint8_t a = grid[i], b = grid[j];
  grid[j] = (a & 0x7F) | parity;
  grid[i] = (b & 0x7F) | parity;
  touchCell(x, y);
  touchCell(nx, ny);
}

/* ------------------------------------------------------------ simulation -- */
/* Falling: straight down (several cells through empty space), sink through
 * lighter materials, else slide diagonally. Returns true if it moved. */
static inline bool fall(int i, int x, int y, uint8_t dens) {
  if (y >= GH - 1)
    return false;
  int j = i + GW;
  uint8_t b = grid[j];
  if (IS_EMPTY(b)) {
    int ny = y + 1;
    while (ny < GH - 1 && ny - y < MAXFALL && IS_EMPTY(grid[j + GW])) {
      j += GW;
      ny++;
    }
    swapCells(i, x, y, j, x, ny);
    return true;
  }
  if (DENS[TYPE(b)] < dens && (rnd() & 1)) {
    swapCells(i, x, y, j, x, y + 1);
    return true;
  }
  int d = (rnd() & 1) ? 1 : -1;
  for (int k = 0; k < 2; k++, d = -d) {
    int nx = x + d;
    if (nx < 0 || nx >= GW)
      continue;
    uint8_t bb = grid[j + d];
    if (IS_EMPTY(bb) || (DENS[TYPE(bb)] < dens && (rnd() & 1))) {
      swapCells(i, x, y, j + d, nx, y + 1);
      return true;
    }
  }
  return false;
}

/* Liquids spread sideways up to `disp` cells, stopping early over a drop. */
static inline bool flow(int i, int x, int y, int disp) {
  int d = (rnd() & 1) ? 1 : -1;
  const uint8_t *row = &grid[y * GW];
  bool hasBelow = y < GH - 1;
  for (int k = 0; k < 2; k++, d = -d) {
    int best = -1;
    for (int s = 1; s <= disp; s++) {
      int nx = x + d * s;
      if (nx < 0 || nx >= GW || !IS_EMPTY(row[nx]))
        break;
      best = nx;
      if (hasBelow && IS_EMPTY(grid[i + GW + d * s]))
        break;
    }
    if (best >= 0) {
      swapCells(i, x, y, y * GW + best, best, y);
      return true;
    }
  }
  return false;
}

static inline void ignite(int n, int nx, int ny) {
  grid[n] = MAKE(FIRE, 12 + (rnd() & 3)) | parity;
  touchCell(nx, ny);
}

static inline void updateFire(int i, int x, int y, uint8_t c) {
  /* Interact with the four neighbours */
  int nb[4], nbx[4], nby[4], cnt = 0;
  if (y > 0)      { nb[cnt] = i - GW; nbx[cnt] = x; nby[cnt++] = y - 1; }
  if (y < GH - 1) { nb[cnt] = i + GW; nbx[cnt] = x; nby[cnt++] = y + 1; }
  if (x > 0)      { nb[cnt] = i - 1;  nbx[cnt] = x - 1; nby[cnt++] = y; }
  if (x < GW - 1) { nb[cnt] = i + 1;  nbx[cnt] = x + 1; nby[cnt++] = y; }
  for (int k = 0; k < cnt; k++) {
    uint8_t t = TYPE(grid[nb[k]]);
    if (t == WATER) { /* doused: water flashes to steam */
      grid[nb[k]] = MAKE(STEAM, rnd()) | parity;
      grid[i] = EMPTY;
      touchCell(nbx[k], nby[k]);
      touchCell(x, y);
      return;
    }
    if (t == WOOD && (rnd() & 31) == 0)
      ignite(nb[k], nbx[k], nby[k]);
    else if (t == OIL && (rnd() & 3) == 0)
      ignite(nb[k], nbx[k], nby[k]);
  }

  int life = c & 15;
  if (rnd() & 1)
    life--;
  if (life <= 0) {
    grid[i] = EMPTY;
    touchCell(x, y);
    return;
  }
  uint8_t nc = MAKE(FIRE, life) | parity;

  /* Flicker upward */
  uint32_t r = rnd();
  if (y > 0 && (r & 3)) {
    int nx = x + (int)((r >> 2) % 3) - 1;
    if (nx >= 0 && nx < GW) {
      int j = i - GW + (nx - x);
      if (IS_EMPTY(grid[j])) {
        grid[j] = nc;
        grid[i] = EMPTY;
        touchCell(x, y);
        touchCell(nx, y - 1);
        return;
      }
    }
  }
  grid[i] = nc;
  touchCell(x, y);
}

static inline void updateSteam(int i, int x, int y, uint8_t c) {
  /* Condense back into water now and then (more often at the ceiling) */
  uint32_t r = rnd();
  if ((r & 1023) < 2 || (y == 0 && (r & 63) == 0)) {
    grid[i] = MAKE(WATER, r >> 12) | parity;
    touchCell(x, y);
    return;
  }
  if (y > 0) {
    int j = i - GW;
    uint8_t b = grid[j];
    uint8_t t = TYPE(b);
    if (IS_EMPTY(b) || ((t == WATER || t == OIL) && (r & 0x30000) == 0)) {
      swapCells(i, x, y, j, x, y - 1);
      return;
    }
    int d = (r & 0x100) ? 1 : -1;
    for (int k = 0; k < 2; k++, d = -d) {
      int nx = x + d;
      if (nx >= 0 && nx < GW && IS_EMPTY(grid[j + d])) {
        swapCells(i, x, y, j + d, nx, y - 1);
        return;
      }
    }
  }
  int nx = x + ((r & 0x200) ? 1 : -1);
  if (nx >= 0 && nx < GW && IS_EMPTY(grid[y * GW + nx])) {
    swapCells(i, x, y, y * GW + nx, nx, y);
    return;
  }
  grid[i] = (c & 0x7F) | parity;
  wake(x, y); /* stay alive so it can condense later */
}

static inline void updateCell(int i, int x, int y) {
  uint8_t c = grid[i];
  uint8_t t = TYPE(c);
  if (t == EMPTY || t == STONE || t == WOOD)
    return;
  if ((c & 0x80) == parity)
    return; /* already moved this step */

  switch (t) {
  case SAND:
    if (fall(i, x, y, DENS[SAND]))
      return;
    break;
  case WATER:
    if (fall(i, x, y, DENS[WATER]) || flow(i, x, y, 4))
      return;
    break;
  case OIL:
    if (fall(i, x, y, DENS[OIL]) || flow(i, x, y, 3))
      return;
    break;
  case FIRE:
    updateFire(i, x, y, c);
    return;
  case STEAM:
    updateSteam(i, x, y, c);
    return;
  }
  grid[i] = (c & 0x7F) | parity; /* didn't move: no wake, chunk may sleep */
}

static void IRAM_ATTR simulate() {
  parity ^= 0x80;
  frameNo++;
  memcpy(awake, awakeNext, sizeof(awake));
  memset(awakeNext, 0, sizeof(awakeNext));

  /* Bottom-up so falling grains are not processed twice; alternate the
   * horizontal direction to avoid a left/right bias. */
  for (int y = GH - 1; y >= 0; y--) {
    uint16_t m = awake[y >> 4];
    if (!m)
      continue;
    int base = y * GW;
    if ((y ^ frameNo) & 1) {
      for (int cx = 0; cx < CW; cx++) {
        if (!(m & (1u << cx)))
          continue;
        int x0 = cx * CHUNK, x1 = min(x0 + CHUNK, GW);
        for (int x = x0; x < x1; x++)
          updateCell(base + x, x, y);
      }
    } else {
      for (int cx = CW - 1; cx >= 0; cx--) {
        if (!(m & (1u << cx)))
          continue;
        int x0 = cx * CHUNK, x1 = min(x0 + CHUNK, GW);
        for (int x = x1 - 1; x >= x0; x--)
          updateCell(base + x, x, y);
      }
    }
  }
}

/* --------------------------------------------------------------- render -- */
static void IRAM_ATTR render() {
  for (int y = 0; y < GH; y++) {
    if (!rowDirty[y])
      continue;
    rowDirty[y] = 0;
    uint32_t *a = (uint32_t *)&grid[y * GW];
    uint32_t *b = (uint32_t *)&shown[y * GW];
    /* One uint32 = two 16-bit pixels = one cell's width */
    uint32_t *p0 = (uint32_t *)(fb + (y * SCALE) * SCR_W);
    uint32_t *p1 = p0 + SCR_W / 2;
    bool wrote = false;
    for (int k = 0; k < GW / 4; k++) {
      uint32_t diff = (a[k] ^ b[k]) & 0x7F7F7F7F;
      if (!diff)
        continue;
      uint32_t cur = a[k];
      for (int n = 0; n < 4; n++) {
        if (diff & (0x7Fu << (n * 8))) {
          uint32_t col = palette[(cur >> (n * 8)) & 0x7F];
          col |= col << 16;
          int x = k * 4 + n;
          p0[x] = col;
          p1[x] = col;
        }
      }
      b[k] = cur;
      wrote = true;
    }
    if (wrote)
      Cache_WriteBack_Addr((uint32_t)p0, SCR_W * SCALE * 2);
  }
}

/* ---------------------------------------------------------------- brush -- */
static void stamp(int cx, int cy) {
  int r = BRUSH_SIZES[brushIdx];
  uint8_t mat = tool < 8 ? BTN_MATERIAL[tool] : SAND;
  bool erase = tool == BTN_ERASE;
  bool solid = mat == STONE || mat == WOOD;
  /* Fill probability out of 256: sparse for grains so they look like grains */
  uint32_t prob = erase || solid ? 256 : mat == SAND ? 90 : mat == FIRE ? 70
                : mat == STEAM ? 60 : 140;
  int r2 = r * r + r;
  for (int dy = -r; dy <= r; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= GH)
      continue;
    for (int dx = -r; dx <= r; dx++) {
      int x = cx + dx;
      if (x < 0 || x >= GW || dx * dx + dy * dy > r2)
        continue;
      int i = y * GW + x;
      uint8_t c = grid[i];
      if (erase) {
        if (IS_EMPTY(c))
          continue;
        grid[i] = EMPTY;
      } else {
        if (!solid && !IS_EMPTY(c))
          continue;
        uint32_t rv = rnd();
        if ((rv & 255) >= prob)
          continue;
        uint8_t shade = mat == FIRE ? 10 + ((rv >> 8) % 6) : (rv >> 8);
        grid[i] = MAKE(mat, shade) | parity; /* differs after next flip */
      }
      touchCell(x, y);
    }
  }
}

static void strokeLine(int x0, int y0, int x1, int y1) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = max(abs(dx), abs(dy));
  int spacing = max(1, BRUSH_SIZES[brushIdx] / 2);
  int n = steps / spacing;
  for (int k = 1; k <= n; k++)
    stamp(x0 + dx * k * spacing / steps, y0 + dy * k * spacing / steps);
  stamp(x1, y1);
}

/* -------------------------------------------------------------- toolbar -- */
static void drawToolbar() {
  for (int b = 0; b < NUM_BTN; b++) {
    int x = b * BTN_W;
    uint16_t bg, fg;
    if (b < 8 && b != BTN_ERASE) {
      bg = palette[MAKE(BTN_MATERIAL[b], 8 + (b == BTN_FIRE ? 7 : 0))];
    } else if (b == BTN_ERASE) {
      bg = rgb565(150, 40, 50);
    } else {
      bg = rgb565(45, 45, 60);
    }
    /* Pick black/white text by rough luminance */
    int lum = ((bg >> 11) & 31) * 2 * 3 + ((bg >> 5) & 63) * 6 + (bg & 31) * 2;
    fg = lum > 300 ? BLACK : WHITE;

    gfx->fillRect(x, BAR_Y, BTN_W, BAR_H, bg);
    if (b == tool) {
      gfx->drawRect(x, BAR_Y, BTN_W, BAR_H, WHITE);
      gfx->drawRect(x + 1, BAR_Y + 1, BTN_W - 2, BAR_H - 2, WHITE);
      gfx->drawRect(x + 2, BAR_Y + 2, BTN_W - 4, BAR_H - 4, BLACK);
    } else {
      gfx->drawRect(x, BAR_Y, BTN_W, BAR_H, BLACK);
    }

    char label[8];
    if (b == BTN_SIZE)
      snprintf(label, sizeof(label), "SZ %d", BRUSH_SIZES[brushIdx]);
    else
      snprintf(label, sizeof(label), "%s", BTN_LABEL[b]);
    int len = strlen(label);
    gfx->setTextSize(1);
    gfx->setTextColor(fg);
    gfx->setCursor(x + (BTN_W - len * 6) / 2, BAR_Y + (BAR_H - 8) / 2);
    gfx->print(label);
  }
}

static void clearWorld() {
  memset(grid, 0, sizeof(grid));
  memset(rowDirty, 1, sizeof(rowDirty));
}

static void pressButton(int b) {
  if (b < 0 || b >= NUM_BTN)
    return;
  if (b == BTN_SIZE)
    brushIdx = (brushIdx + 1) % sizeof(BRUSH_SIZES);
  else if (b == BTN_CLEAR)
    clearWorld();
  else
    tool = b;
  drawToolbar();
}

/* ---------------------------------------------------------------- input -- */
static void touchTask(void *) {
  for (;;) {
    uint32_t s = 0;
    if (touch_touched()) {
      int x = constrain(touch_last_x, 0, SCR_W - 1);
      int y = constrain(touch_last_y, 0, 479);
      s = 0x80000000u | ((uint32_t)x << 16) | (uint32_t)y;
    }
    touchState = s;
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

static void handleInput() {
  static bool wasDown = false, stroking = false;
  static int lastX = 0, lastY = 0;
  uint32_t s = touchState;
  bool down = s >> 31;
  int tx = (s >> 16) & 0x7FFF, ty = s & 0xFFFF;

  if (down) {
    int gx = tx / SCALE, gy = min(ty / SCALE, GH - 1);
    if (!wasDown) {
      if (ty >= BAR_Y) {
        pressButton(tx / BTN_W);
        stroking = false;
      } else {
        stroking = true;
        stamp(gx, gy);
        lastX = gx;
        lastY = gy;
      }
    } else if (stroking) {
      strokeLine(lastX, lastY, gx, gy); /* held still = keeps pouring */
      lastX = gx;
      lastY = gy;
    }
  } else {
    stroking = false;
  }
  wasDown = down;
}

/* ---------------------------------------------------------------- setup -- */
void setup() {
  Serial.begin(115200);
  esp_task_wdt_init(120, true);
  esp_task_wdt_add(NULL);

  touch_init();
  gfx->begin(DISPLAY_PCLK_HZ);
  fb = gfx->getFramebuffer();

  buildPalette();
  rngState ^= esp_random();

  /* Paint the play area background once, then the toolbar */
  uint16_t bg = palette[EMPTY];
  for (int i = 0; i < SCR_W * PLAY_H; i++)
    fb[i] = bg;
  Cache_WriteBack_Addr((uint32_t)fb, SCR_W * PLAY_H * 2);
  drawToolbar();

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  xTaskCreatePinnedToCore(touchTask, "touch", 4096, NULL, 5, NULL, 0);
  Serial.printf("Falling sand ready: %dx%d cells, free SRAM %u\n", GW, GH,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void loop() {
  static TickType_t lastWake = xTaskGetTickCount();
  static uint32_t statFrames = 0, statSim = 0, statRender = 0, statT0 = 0;

  esp_task_wdt_reset();
  handleInput();

  uint32_t t0 = micros();
  simulate();
  uint32_t t1 = micros();
  render();
  uint32_t t2 = micros();

  statSim += t1 - t0;
  statRender += t2 - t1;
  statFrames++;
  if (millis() - statT0 >= 2000) {
    int chunks = 0;
    for (int i = 0; i < CH; i++)
      chunks += __builtin_popcount(awake[i]);
    Serial.printf("fps %.1f  sim %luus  render %luus  active chunks %d/%d\n",
                  statFrames * 1000.0f / (millis() - statT0),
                  statSim / statFrames, statRender / statFrames, chunks,
                  CW * CH);
    statFrames = statSim = statRender = 0;
    statT0 = millis();
  }

  vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(FRAME_MS));
}
