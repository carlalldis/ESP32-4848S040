/* STARLANE - screens: dialog, intro, title, crew select, station, shipyard,
 * trade, jobs, cantina, map, menu, death, ending, and the flight frame. */
#include "sl.h"

namespace sl {

Dialog dlg;
float introT;
int scanTarget = -1;
uint8_t mapReturn = MD_FLIGHT;

/* ================================================================ dialog */
static char dynText[12][200];
static int dynK;

void speakerInfo(uint8_t spk, const char **name, int *port, uint8_t *tint) {
  if (spk == SPK_CREWA || spk == SPK_CREWB) {
    int idx = spk == SPK_CREWA ? 0 : 1;
    if (idx < nCrew) {
      int c = crew[idx];
      *name = CREW[c].name;
      *port = CREW[c].sp;
      *tint = CREW[c].tint;
      return;
    }
  }
  const SpeakerDef &S = SPEAKERS[spk];
  *name = S.name;
  *port = S.port;
  *tint = S.tint;
}

void dlgBegin() {
  dlg.n = 0;
  dlg.nOpt = 0;
  dlg.i = 0;
  dlg.chars = 0;
}
void dlgLine(uint8_t spk, const char *t) {
  if (dlg.n >= 28 || !t) return;
  if (spk == SPK_CREWA && nCrew < 1) return;
  if (spk == SPK_CREWB && nCrew < 2) return;
  if (t[0] == '@') {
    int c = crew[spk == SPK_CREWB ? 1 : 0];
    int beat = t[1] - '0';
    if (beat < 0 || beat >= NBEAT) return;
    t = STORYCREW[CREW[c].sp][beat];
    if (!t) return;
  }
  dlg.lines[dlg.n].spk = spk;
  dlg.lines[dlg.n].text = t;
  dlg.n++;
}
void dlgLinef(uint8_t spk, const char *f, ...) {
  char *b = dynText[dynK];
  dynK = (dynK + 1) % 12;
  va_list a;
  va_start(a, f);
  vsnprintf(b, 200, f, a);
  va_end(a);
  dlgLine(spk, b);
}
void dlgLines(const DLine *l, int n) {
  for (int i = 0; i < n; i++) dlgLine(l[i].spk, l[i].text);
}
void dlgOpt(const char *label, int val) {
  if (dlg.nOpt >= 4) return;
  snprintf(dlg.opt[dlg.nOpt], sizeof(dlg.opt[0]), "%s", label);
  dlg.val[dlg.nOpt] = val;
  dlg.nOpt++;
}
void dlgRun(int tag) {
  if (dlg.n == 0) dlgLine(SPK_NARR, "...");
  dlg.tag = tag;
  dlg.on = true;
  dlg.i = 0;
  dlg.chars = 0;
}

void dlgDraw() {
  if (!dlg.on) return;
  DLine &L = dlg.lines[dlg.i];
  bool last = dlg.i == dlg.n - 1;
  bool opts = last && dlg.nOpt > 0;
  int rows = (dlg.nOpt + 1) / 2;
  int y0 = opts ? (rows > 1 ? 104 : 126) : 138;
  panel(0, y0, 240, 240 - y0, C_UIBG, C_UIHI);
  const char *name;
  int port;
  uint8_t tint;
  speakerInfo(L.spk, &name, &port, &tint);
  int tx = 6;
  if (port < SP_NPORT) {
    drawPortrait(6, y0 + 7, port, tint, 2, port == SP_HUSH);
    tx = 44;
  }
  if (*name) text(tx, y0 + 6, name, L.spk == SPK_HUSH ? C_V5 : C_UIHI);
  int wch = (234 - tx) / 6;
  int len = (int)strlen(L.text);
  dlg.chars += 50 * DT;
  uint8_t col = L.spk == SPK_HUSH ? C_V5 : (L.spk == SPK_NARR ? C_G5 : C_WHITE);
  int maxLines = opts ? (rows > 1 ? 8 : 9) : 10;
  wrapText(tx, y0 + (*name ? 17 : 7), wch, L.text, col, (int)dlg.chars, 9, maxLines);
  bool typed = dlg.chars >= len;
  if (opts && typed) {
    for (int k = 0; k < dlg.nOpt; k++) {
      int r = k / 2, c = k % 2;
      bool single = dlg.nOpt == 1 || (k == dlg.nOpt - 1 && (dlg.nOpt & 1));
      int bx = single ? 6 : 6 + c * 116, bw = single ? 228 : 112;
      int by = 240 - (rows - r) * 24 - 2;
      if (button(bx, by, bw, 21, dlg.opt[k], true, C_Y2)) {
        dlg.on = false;
        onDialog(dlg.tag, dlg.val[k]);
        return;
      }
    }
  } else if (!opts && typed && ((frameNo / 10) & 1)) {
    text(222, 230, ">>", C_UIHI);
  }
  if (inTap && !tapEaten) {
    if (!typed) {
      dlg.chars = 1e6f;
      tapEaten = true;
    } else if (!opts) {
      tapEaten = true;
      dlg.i++;
      dlg.chars = 0;
      if (dlg.i >= dlg.n) {
        dlg.on = false;
        onDialog(dlg.tag, -1);
      }
    }
  }
}

/* ================================================================= icons */
void drawIcon(int x, int y, uint8_t m, int scale, const uint8_t *roles, int dir) {
  const char *ic = MODS[m].icon;
  if (!ic) return;
  for (int j = 0; j < 8; j++)
    for (int i = 0; i < 8; i++) {
      int sr = j, sc = i;
      if (dir == 1) { sr = 7 - i; sc = j; }
      else if (dir == 2) { sr = 7 - j; sc = 7 - i; }
      else if (dir == 3) { sr = i; sc = 7 - j; }
      char ch = ic[sr * 8 + sc];
      uint8_t c = (ch >= '1' && ch <= '4') ? roles[ch - '1'] : charColor(ch);
      if (c == TRANSP) continue;
      fillRect(x + i * scale, y + j * scale, scale, scale, c);
    }
}

static void header(const char *title, bool showCredits = true) {
  fillRect(0, 0, LW, 13, C_UIBG2);
  hline(0, LW - 1, 13, C_UILN);
  text(4, 3, title, C_UIHI);
  if (showCredits) textR(236, 3, fmt("%d CR", credits), C_Y2);
}

static void bgStars(float t) {
  clear(C_SPACE);
  drawStarfield(t * 30, t * 8, 0, 0, false);
}

/* ================================================================= intro */
static bool introSceneC = false;

void startIntro() {
  mode = MD_INTRO;
  introT = 0;
  introSceneC = false;
  worldReset();
}

static void introSetupC() {
  introSceneC = true;
  worldReset();
  Design &d = designs[D_PLAYER];
  d.g = designs[D_COURIER].g;
  compileDesign(d, PAINTS[0]);
  Ship &p = ships[PL];
  const StationDef &H = STATIONS[ST_HAVEN];
  p.x = H.x + 1080;
  p.y = H.y + 720;
  p.ang = angTo(H.x - p.x, H.y - p.y);
  p.vx = sinf(p.ang) * 75;
  p.vy = -cosf(p.ang) * 75;
  p.hp = p.hpMax = d.st.hp;
  p.sh = p.shMax = 0;
  p.flags = SF_INVULN;
  camX = p.x;
  camY = p.y;
  for (int k = 0; k < 3; k++) {
    float bx = p.x - sinf(p.ang) * (170 + k * 30) + (k - 1) * 40;
    float by = p.y + cosf(p.ang) * (170 + k * 30) + (k - 1) * 30;
    int i = spawnShip(D_MITE, bx, by, p.ang, SF_HOSTILE | SF_INVULN | SF_MISSION, AI_ATTACK);
    if (i >= 0) {
      ships[i].target = PL;
      spawnPart(bx, by, 1.5f, 0, 16, C_V4, PK_RING);
    }
  }
}

void uiIntro() {
  introT += DT;
  float t = introT;
  if (inTap && t > 0.6f) {
    tapEaten = true;
    worldReset();
    mode = MD_TITLE;
    return;
  }
  if (t < 9.5f) {
    /* scene A: words in the dark */
    bgStars(t);
    for (int k = 0; k < 5; k++) {
      float st = 0.6f + k * 1.6f;
      if (t < st) break;
      int n = (int)((t - st) * 30);
      char buf[48];
      snprintf(buf, sizeof(buf), "%s", INTRO_TEXT[k]);
      if (n < (int)strlen(buf)) buf[n] = 0;
      textC(CX, 70 + k * 16, buf, k < 2 ? C_G5 : C_O5);
    }
    if (t > 8.6f) darken((int)((t - 8.6f) * 18));
  } else if (t < 15.5f) {
    /* scene B: inside a trade lane */
    float tt = t - 9.5f;
    clear(C_SPACE);
    bool fail = tt > 4.4f;
    for (int k = 0; k < 90; k++) {
      uint32_t h = hash2(k, 7, 3);
      float a = (h & 1023) / 1023.0f * TAUF;
      float z = fmodf(((h >> 10) & 1023) / 1023.0f + tt * 0.9f, 1.0f);
      float r0 = 8 + z * z * 180, r1 = r0 + 4 + z * 26;
      line(CX + (int)(cosf(a) * r0), CY + (int)(sinf(a) * r0), CX + (int)(cosf(a) * r1),
           CY + (int)(sinf(a) * r1), z > 0.6f ? C_G5 : C_G3);
    }
    for (int k = 0; k < 6; k++) {
      float z = fmodf(k / 6.0f + tt * 0.55f, 1.0f);
      int r = (int)(6 + z * z * 200);
      uint8_t c = fail ? ((frameNo & 2) ? C_R3 : C_R2) : (z > 0.7f ? C_O4 : C_O2);
      circle(CX, CY, r, c);
      circle(CX, CY, r + 1, z > 0.5f ? c : C_SPACE);
      for (int e = 0; e < 4; e++) {
        float a = e * PIF / 2 + 0.4f;
        fillRect(CX + (int)(cosf(a) * r) - 2, CY + (int)(sinf(a) * r) - 2, 4, 4, fail ? C_R4 : C_Y3);
      }
    }
    fillCircle(CX, CY, 4, fail ? C_R3 : C_WHITE);
    for (int k = 5; k < 8; k++) {
      float st = 0.4f + (k - 5) * 1.3f;
      if (tt < st) break;
      textShC(CX, 176 + (k - 5) * 14, INTRO_TEXT[k], C_WHITE);
    }
    if (fail) {
      for (int k = 0; k < 8; k++) {
        int y = irand(LH - 8), h = 2 + irand(6), off = 1 + irand(8);
        for (int r = y; r < y + h; r++) memmove(&scr[r * LW + off], &scr[r * LW], (size_t)(LW - off));
      }
      if ((frameNo / 4) & 1) textShC(CX, 30, "LANE INTEGRITY FAILURE", C_R4);
    }
    if (tt > 5.6f) clear(tt > 5.85f ? C_SPACE : C_WHITE);
  } else {
    /* scene C: the chase */
    if (!introSceneC) introSetupC();
    float tc = t - 15.5f;
    Ship &p = ships[PL];
    const StationDef &H = STATIONS[ST_HAVEN];
    float a = angTo(H.x - p.x, H.y - p.y);
    p.ang += clampf(wrapAng(a - p.ang), -0.6f * DT, 0.6f * DT);
    p.vx = sinf(p.ang) * 75;
    p.vy = -cosf(p.ang) * 75;
    p.x += p.vx * DT;
    p.y += p.vy * DT;
    p.thr = 0.7f;
    if (tc > 1.0f && tc < 1.0f + DT) {
      /* trailing plasma */
    }
    if (frameNo % 3 == 0) spawnPart(p.x, p.y, frange(-10, 10), frange(-10, 10), 25, C_G3, PK_SMOKE);
    if (tc > 2.5f && tc < 2.5f + DT * 1.5f) {
      for (int k = 0; k < 3; k++) {
        float ox = (p.x + H.x) / 2 + (k - 1) * 40, oy = (p.y + H.y) / 2 + (k - 1) * 30 - 100;
        int i = spawnShip(k == 0 ? D_WARDEN : D_LANCER, ox, oy, angTo(p.x - ox, p.y - oy), SF_MISSION, AI_PATROL);
        if (i >= 0) {
          ships[i].tx = p.x;
          ships[i].ty = p.y;
        }
      }
    }
    if (tc > 7.0f)
      for (int i = FIRST_DYN; i < MAXSHIP; i++)
        if (ships[i].on && ships[i].fac == F_HUSH) ships[i].flags &= (uint16_t)~SF_INVULN;
    if (tc > 3.5f && tc < 3.5f + DT * 1.5f)
      bark(SPK_CTRL, -1, "UNKNOWN COURIER, THIS IS HAVEN SPINDLE. HOLD YOUR COURSE - LANCERS INBOUND.");
    if (tc > 10.5f && tc < 10.5f + DT * 1.5f)
      bark(SPK_CTRL, -1, "YOU'RE LEAKING PLASMA, COURIER. DOCK NOW, BEFORE YOU BECOME A NEW STAR.");
    worldUpdate();
    worldDraw();
    if (tc < 6) textShC(CX, 20, "THE KESTREL REACH", C_O4);
    if (tc < 6) textShC(CX, 30, "212 YEARS AFTER THE SILENCE", C_G4);
    drawComms();
    const char *cap = nullptr;
    if (tc > 1.5f && tc < 6.5f) cap = "YOU ARE A COURIER. THE CASE IN YOUR HOLD IS WORTH MORE THAN YOUR SHIP.";
    else if (tc > 7 && tc < 10) cap = "SOMEONE KNOWS IT.";
    if (cap && bk.t <= 0) {
      panel(10, 168, 220, 30);
      wrapText(16, 173, 34, cap, C_WHITE, (int)((tc - (tc > 7 ? 7 : 1.5f)) * 40));
    }
    if (tc > 15) darken((int)((tc - 15) * 12));
    if (tc > 16.5f) {
      worldReset();
      mode = MD_TITLE;
    }
  }
  if (t > 1.2f && ((frameNo / 15) & 1)) text(186, 231, "TAP: SKIP", C_G2);
}

/* ================================================================= title */
void uiTitle() {
  gameTime += DT;
  const PlanetDef &T = PLANETS[0];
  camX = T.x + sinf(gameTime * 0.05f) * 20;
  camY = T.y - 150;
  clear(C_SPACE);
  drawStarfield(camX + gameTime * 20, camY, 0, 0, false);
  drawPlanetsAndSun(camX, camY);
  drawShipSprite(D_LANCER, 200, 112 + (int)(sinf(gameTime) * 2), 0.3f, 2);
  /* a ship streaking along the lane */
  float lt = fmodf(gameTime * 0.35f, 1.0f);
  int lx = (int)(-40 + lt * 340), ly = 128 - (int)(lt * 30);
  line(lx, ly, lx - 30, ly + 3, C_O3);
  pset(lx, ly, C_WHITE);
  for (int k = 0; k < 4; k++) textC(CX + (k & 1) * 2 - 1, 34 + (k >> 1) * 2 - 1, "STARLANE", C_V2, 4);
  textC(CX, 34, "STARLANE", C_WHITE, 4);
  hline(36, 204, 66, C_O3);
  textC(CX, 72, "THE KESTREL REACH", C_O4);
  if (button(60, 150, 120, 22, "NEW GAME", true, C_Y2)) {
    if (hasSave) {
      dlgBegin();
      dlgLine(SPK_NARR, "START A NEW GAME? YOUR SAVED PROGRESS WILL BE OVERWRITTEN.");
      dlgOpt("NEW GAME", 1);
      dlgOpt("CANCEL", 0);
      dlgRun(DT_CONFIRM_NEW);
    } else {
      mode = MD_CREW;
    }
  }
  if (hasSave && button(60, 178, 120, 22, "CONTINUE")) {
    if (loadGame()) mode = MD_STATION;
  }
  textShC(CX, 228, "A TALE OF LANES AND SILENCE", C_G4);
}

/* ============================================================ crew select */
static int crewSel[2] = {-1, -1}, crewFocus = 0;

void uiCrew() {
  bgStars(gameTime += DT);
  header("CHOOSE TWO CREW", false);
  for (int i = 0; i < 6; i++) {
    int col = i % 2, row = i / 2;
    int x = 4 + col * 118, y = 17 + row * 46;
    bool sel = crewSel[0] == i || crewSel[1] == i;
    panel(x, y, 114, 43, sel ? C_UIBG2 : C_UIBG, sel ? C_Y2 : (crewFocus == i ? C_UIHI : C_UILN));
    const CrewDef &c = CREW[i];
    drawPortrait(x + 5, y + 6, c.sp, c.tint, 2);
    text(x + 41, y + 5, c.name, C_WHITE);
    text(x + 41, y + 15, SPECIES[c.sp].name, C_G4);
    text(x + 41, y + 25, SPECIES[c.sp].role, C_UIHI);
    if (sel) text(x + 104, y + 33, "*", C_Y2);
    if (tapIn(x, y, 114, 43)) {
      crewFocus = i;
      if (crewSel[0] == i) { crewSel[0] = crewSel[1]; crewSel[1] = -1; }
      else if (crewSel[1] == i) crewSel[1] = -1;
      else if (crewSel[0] < 0) crewSel[0] = i;
      else if (crewSel[1] < 0) crewSel[1] = i;
      else { crewSel[0] = crewSel[1]; crewSel[1] = i; }
    }
  }
  const CrewDef &f = CREW[crewFocus];
  panel(4, 157, 232, 56);
  text(8, 160, SPECIES[f.sp].bonus, C_Y2);
  wrapText(8, 170, 37, f.bio, C_G5, 1 << 20, 9, 5);
  bool ok = crewSel[0] >= 0 && crewSel[1] >= 0;
  if (button(60, 216, 120, 22, ok ? "BEGIN" : "PICK TWO", ok, C_Y2)) {
    crew[0] = (uint8_t)crewSel[0];
    crew[1] = (uint8_t)crewSel[1];
    nCrew = 2;
    newGame();
  }
}

/* =============================================================== station */
static float newsX = 0;
static int newsIdx = 0;

void uiStation() {
  gameTime += DT;
  int st = dockedAt < 0 ? 0 : dockedAt;
  const StationDef &S = STATIONS[st];
  Ship &p = ships[PL];
  clear(C_SPACE);
  drawStarfield(S.x + gameTime * 6, S.y, 0, 0, false);
  /* nearest planet in the backdrop */
  for (int i = 0; i < NPLANET; i++) {
    float d = lenf(PLANETS[i].x - S.x, PLANETS[i].y - S.y);
    if (d < 800) drawPlanetsAndSun(S.x - (PLANETS[i].x - S.x) * 0.25f + 70, S.y - (PLANETS[i].y - S.y) * 0.25f + 10);
  }
  bool crippled = ships[stationShip(st)].hp <= 0;
  drawStation(S.style, CX, 58, 2, gameTime, crippled);
  header(S.name);
  text(4, 16, FACS[S.fac].name, S.fac == F_SYND ? C_R4 : (S.fac == F_THREN ? C_GD2 : C_B4));
  /* news ticker */
  fillRect(0, 100, LW, 11, C_UIBG2);
  const char *news = S.news[newsIdx & 3];
  newsX += 0.9f;
  int w = textW(news);
  if (newsX > w + LW) { newsX = 0; newsIdx++; }
  text(LW - (int)newsX, 102, news, C_G5);
  fillRect(0, 100, 30, 11, C_UIBG2);
  text(2, 102, "NEWS", C_O4);

  bool tut = mis == MS_INTRO && mstep >= 1 && gridCount(designs[D_PLAYER].g, M_LASER) < 2;
  int bx = 4, by = 116, bw = 114, bh = 22;
  if (button(bx, by, bw, bh, tut ? "SHIPYARD !" : "SHIPYARD", true, tut ? C_Y2 : C_UIHI)) openShipyard();
  if (button(bx + 118, by, bw, bh, "TRADE")) mode = MD_TRADE;
  if (button(bx, by + 26, bw, bh, job.type ? "JOB BOARD *" : "JOB BOARD")) mode = MD_JOBS;
  if (button(bx + 118, by + 26, bw, bh, "CANTINA")) mode = MD_BAR;
  int cost = (int)((p.hpMax - p.hp) * cm.repairCost + 0.99f);
  bool needRepair = cost > 0;
  if (button(bx, by + 52, bw, bh, needRepair ? fmt("REPAIR %d", cost) : "HULL OK", needRepair && credits > 0)) {
    int afford = cost <= credits ? cost : credits;
    float hpGain = afford / cm.repairCost;
    p.hp = minf(p.hpMax, p.hp + hpGain);
    credits -= afford;
    toast("HULL REPAIRED");
  }
  if (button(bx + 118, by + 52, bw, bh, "SYSTEM MAP")) {
    mapReturn = MD_STATION;
    mode = MD_MAP;
  }
  text(6, 196, fmt("HULL %d/%d", (int)p.hp, (int)p.hpMax), p.hp < p.hpMax ? C_R4 : C_G4);
  textR(234, 196, fmt("CARGO %d/%d", cargoUsed(), cargoCap()), C_G4);
  if (button(4, 210, 232, 26, "LAUNCH", true, C_Y2)) {
    const char *why = nullptr;
    if (storyBlocksLaunch(&why)) toast("%s", why);
    else launchFrom(st);
  }
  if (toastT > 0) {
    toastT -= DT;
    panel(10, 84, 220, 13, C_UIBG2, C_Y2);
    textC(CX, 87, toastText(), C_Y3);
  }
}

/* ============================================================== shipyard */
static uint8_t yardTool = M_LASER; /* module id, or 100 = rotate, 101 = remove */
static bool yardBuy = false;
enum { TOOL_ROT = 100, TOOL_DEL = 101 };

void openShipyard() {
  mode = MD_SHIPYARD;
  yardBuy = false;
  yardTool = inv[M_LASER] ? M_LASER : M_HULL;
}

static bool shopSells(int st, uint8_t m) {
  for (int k = 0; STATIONS[st].shop[k]; k++)
    if (STATIONS[st].shop[k] == m) return true;
  return false;
}

static const char *yardProblem(const Stats &s, const Grid &g) {
  int nc = gridCount(g, M_COCKPIT);
  if (nc == 0) return "NO COCKPIT";
  if (nc > 1) return "ONLY ONE COCKPIT ALLOWED";
  if (!gridConnected(g)) return "MODULES NOT CONNECTED";
  if (s.thrust <= 0) return "NO WORKING THRUSTER";
  if (s.crew < nCrew) return "NOT ENOUGH CREW BERTHS";
  if (cargoUsed() > s.cargo + 0) return "CARGO WON'T FIT";
  return nullptr;
}

void uiShipyard() {
  gameTime += DT;
  Design &d = designs[D_PLAYER];
  Grid &g = d.g;
  const uint8_t *roles = PAINTS[paint & 3];
  clear(C_SPACE);
  header("SHIPYARD");
  int st = dockedAt < 0 ? 0 : dockedAt;
  /* grid */
  const int GX = 4, GY = 23, CS = 18;
  textC(GX + GW * CS / 2, 15, "^ FRONT ^", C_G3);
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++) {
      int cx = GX + x * CS, cy = GY + y * CS;
      fillRect(cx, cy, CS - 1, CS - 1, C_UIBG);
      uint8_t m = g.m[y][x];
      if (m) {
        int dir = modWeapon(m) >= 0 ? g.dir[y][x] : 0;
        drawIcon(cx + 1, cy + 1, m, 2, roles, dir);
        bool bad = (m == M_ENGINE && engineBlocked(g, x, y)) || (modWeapon(m) >= 0 && weaponBlocked(g, x, y));
        if (bad && ((frameNo / 8) & 1)) rect(cx, cy, CS - 1, CS - 1, C_R4);
      } else {
        pset(cx + CS / 2 - 1, cy + CS / 2 - 1, C_UIBG2);
      }
      if (tapIn(cx, cy, CS - 1, CS - 1)) {
        if (yardTool == TOOL_DEL) {
          if (m) { inv[m]++; g.m[y][x] = 0; }
        } else if (yardTool == TOOL_ROT) {
          if (modWeapon(m) >= 0 && m != M_TURRET) g.dir[y][x] = (g.dir[y][x] + 1) & 3;
        } else if (m == yardTool && modWeapon(m) >= 0 && m != M_TURRET) {
          g.dir[y][x] = (g.dir[y][x] + 1) & 3;
        } else if (inv[yardTool] > 0) {
          if (m) inv[m]++;
          g.m[y][x] = yardTool;
          inv[yardTool]--;
          g.dir[y][x] = (uint8_t)autoDir(g, x, y);
        } else {
          toast(yardBuy ? "BUY IT FIRST" : "NONE OWNED - USE THE BUY TAB");
        }
        compileDesign(d, roles);
      }
    }
  rect(GX - 1, GY - 1, GW * CS + 1, GH * CS + 1, C_UILN);
  /* palette */
  const int PX0 = 136;
  if (button(PX0, 15, 49, 12, "OWNED", true, yardBuy ? C_UILN : C_Y2)) yardBuy = false;
  if (button(PX0 + 51, 15, 49, 12, "BUY", true, yardBuy ? C_Y2 : C_UILN)) yardBuy = true;
  for (int m = 1; m < M_COUNT; m++) {
    int k = m - 1, cx = PX0 + (k % 4) * 25, cy = 30 + (k / 4) * 25;
    bool avail = yardBuy ? shopSells(st, (uint8_t)m) : inv[m] > 0;
    fillRect(cx, cy, 24, 24, yardTool == m ? C_UIBG2 : C_UIBG);
    if (yardTool == m) rect(cx, cy, 24, 24, C_Y2);
    drawIcon(cx + 4, cy + 3, (uint8_t)m, 2, roles, 0);
    if (!avail) ditherRect(cx, cy, 24, 24, C_UIBG, 9);
    if (!yardBuy && inv[m]) textR(cx + 23, cy + 17, fmt("%d", inv[m]), C_WHITE);
    if (tapIn(cx, cy, 24, 24)) yardTool = (uint8_t)m;
  }
  if (button(PX0, 131, 49, 14, "ROTATE", true, yardTool == TOOL_ROT ? C_Y2 : C_UILN)) yardTool = TOOL_ROT;
  if (button(PX0 + 51, 131, 49, 14, "REMOVE", true, yardTool == TOOL_DEL ? C_Y2 : C_UILN)) yardTool = TOOL_DEL;
  /* info */
  panel(2, 150, 236, 44);
  if (yardTool == TOOL_ROT) {
    text(6, 153, "ROTATE", C_UIHI);
    wrapText(6, 163, 38, "TAP A GUN TO TURN IT. FORWARD GUNS SUIT NIMBLE SHIPS; BIG SHIPS FIGHT WELL WITH SIDE BATTERIES.", C_G5);
  } else if (yardTool == TOOL_DEL) {
    text(6, 153, "REMOVE", C_UIHI);
    wrapText(6, 163, 38, "TAP A MODULE TO TAKE IT OFF. IT GOES BACK INTO YOUR HOLD.", C_G5);
  } else {
    const ModDef &md = MODS[yardTool];
    text(6, 153, md.name, C_UIHI);
    textR(234, 153, fmt("OWN %d", inv[yardTool]), C_G4);
    wrapText(6, 163, 38, md.desc, C_G5, 1 << 20, 9, 2);
    if (yardBuy) {
      bool sells = shopSells(st, yardTool);
      if (button(6, 181, 110, 11, sells ? fmt("BUY %d CR", md.price) : "NOT SOLD HERE", sells && credits >= md.price, C_Y2)) {
        credits -= md.price;
        inv[yardTool]++;
        toast("BOUGHT %s", md.name);
      }
    }
    if (inv[yardTool] > 0 && button(124, 181, 110, 11, fmt("SELL %d CR", md.price / 2))) {
      inv[yardTool]--;
      credits += md.price / 2;
    }
  }
  /* stats */
  const Stats &s = d.st;
  const char *prob = yardProblem(s, g);
  int gen = s.powGen + cm.power;
  text(4, 197, fmt("HULL %d SHLD %d SPD %d TURN %d", (int)s.hp, (int)s.shield, (int)s.vmax, (int)(s.turn * 57.3f)), C_G5);
  text(4, 206, fmt("PWR %d/%d CARGO %d CREW %d/%d GUNS %d", gen, s.powUse, s.cargo, nCrew, s.crew, s.nMount),
       gen < s.powUse ? C_R4 : C_G5);
  if (toastT > 0) {
    toastT -= DT;
    textShC(CX, 140, toastText(), C_Y3);
  } else if (prob) {
    textShC(CX, 140, prob, (frameNo & 16) ? C_R4 : C_R3);
  } else if (s.blockedEng || s.blockedWeap) {
    textShC(CX, 140, fmt("%d BLOCKED MODULE%s", s.blockedEng + s.blockedWeap, s.blockedEng + s.blockedWeap > 1 ? "S" : ""), C_O4);
  }
  if (button(4, 218, 112, 20, fmt("PAINT: %s", PAINT_NAMES[paint & 3]))) {
    paint = (uint8_t)((paint + 1) & 3);
    compileDesign(d, PAINTS[paint]);
  }
  if (button(124, 218, 112, 20, "DONE", true, C_Y2)) {
    if (prob) toast("%s", prob);
    else {
      playerRefresh(false);
      mode = MD_STATION;
      saveGame();
    }
  }
}

/* ================================================================= trade */
void uiTrade() {
  gameTime += DT;
  int st = dockedAt < 0 ? 0 : dockedAt;
  const StationDef &S = STATIONS[st];
  clear(C_SPACE);
  header(fmt("TRADE - %s", S.shortName));
  text(4, 17, "GOOD", C_G3);
  text(78, 17, "BUY", C_G3);
  text(106, 17, "SELL", C_G3);
  text(136, 17, "HOLD", C_G3);
  int cap = cargoCap(), used = cargoUsed();
  for (int c = 0; c < CM_PARCEL; c++) {
    int y = 28 + c * 25;
    fillRect(2, y, 236, 22, (c & 1) ? C_UIBG : C_UIBG2);
    text(5, y + 8, COMS[c].name, C_WHITE);
    bool contraband = COMS[c].illegal && S.fac == F_AUTH;
    int buy = priceAt(st, c, true), sell = priceAt(st, c, false);
    if (contraband) {
      text(78, y + 8, "CONTRABAND", C_R4);
    } else {
      text(78, y + 8, fmt("%d", buy), C_Y2);
      text(106, y + 8, fmt("%d", sell), C_GR4);
    }
    text(140, y + 8, fmt("%d", cargo[c]), C_G5);
    if (button(166, y + 1, 32, 20, "-", !contraband && cargo[c] > 0)) {
      cargo[c]--;
      credits += sell;
    }
    if (button(202, y + 1, 34, 20, "+", !contraband && credits >= buy && used < cap)) {
      cargo[c]++;
      credits -= buy;
    }
  }
  if (cargo[CM_PARCEL]) text(5, 180, fmt("JOB PARCELS: %d", cargo[CM_PARCEL]), C_O4);
  text(5, 192, fmt("HOLD %d/%d", cargoUsed(), cap), cargoUsed() >= cap ? C_R4 : C_G5);
  textR(235, 192, cm.envoy ? "ENVOY: BETTER PRICES" : "", C_T4);
  wrapText(5, 202, 38, "BUY LOW HERE, SELL HIGH ELSEWHERE. CHECK EACH STATION'S PRICES.", C_G3, 1 << 20, 9, 1);
  if (button(60, 216, 120, 22, "DONE", true, C_Y2)) mode = MD_STATION;
}

/* ================================================================== jobs */
void uiJobs() {
  gameTime += DT;
  clear(C_SPACE);
  header("JOB BOARD");
  int y = 17;
  if (job.type) {
    panel(2, y, 236, 50, C_UIBG2, C_Y2);
    text(6, y + 3, "ACTIVE JOB", C_Y2);
    wrapText(6, y + 13, 38, jobText(job), C_WHITE, 1 << 20, 9, 3);
    if (button(166, y + 2, 68, 10, "ABANDON")) {
      if (job.type == JB_DELIVER) cargo[CM_PARCEL] = 0;
      clearFlag(SF_JOB);
      job.type = JB_NONE;
      toast("JOB ABANDONED");
    }
    y += 54;
  }
  for (int k = 0; k < 3; k++) {
    Job &o = offers[k];
    if (!o.type) continue;
    if (y > 166) break;
    panel(2, y, 236, 48);
    wrapText(6, y + 4, 38, jobText(o), C_G5, 1 << 20, 9, 3);
    text(6, y + 36, fmt("PAY %d CR", o.reward), C_Y2);
    bool can = !job.type && (o.type != JB_DELIVER || cargoUsed() + o.count <= cargoCap());
    if (button(150, y + 33, 84, 12, can ? "TAKE JOB" : (job.type ? "BUSY" : "NO ROOM"), can, C_Y2)) {
      job = o;
      o.type = JB_NONE;
      if (job.type == JB_DELIVER) cargo[CM_PARCEL] += job.count;
      toast("JOB ACCEPTED");
    }
    y += 51;
  }
  if (!job.type && !offers[0].type && !offers[1].type && !offers[2].type)
    textC(CX, 100, "NO WORK TODAY. COME BACK LATER.", C_G4);
  if (toastT > 0) {
    toastT -= DT;
    textShC(CX, 204, toastText(), C_Y3);
  }
  if (button(60, 216, 120, 22, "DONE", true, C_Y2)) mode = MD_STATION;
}

/* =============================================================== cantina */
static int barTalk = -1;

void uiBar() {
  gameTime += DT;
  int st = dockedAt < 0 ? 0 : dockedAt;
  const StationDef &S = STATIONS[st];
  clear(C_SPACE);
  for (int y = 14; y < LH; y += 3) hline(0, LW - 1, y, C_UIBG);
  header(fmt("CANTINA - %s", S.shortName));
  panel(2, 16, 236, 52);
  const char *talk = barTalk < 0 ? S.barkeep : S.news[barTalk & 3];
  wrapText(6, 20, 38, talk, barTalk < 0 ? C_G5 : C_O5, 1 << 20, 9, 5);
  if (button(170, 55, 66, 11, "LISTEN")) barTalk = barTalk < 0 ? 0 : barTalk + 1;
  int cap = designs[D_PLAYER].st.crew;
  int y = 72;
  for (int k = 0; k < 2; k++) {
    int c = S.recruit[k];
    bool hired = false;
    for (int i = 0; i < nCrew; i++) hired |= crew[i] == c;
    if (hired) continue;
    const CrewDef &cd = CREW[c];
    panel(2, y, 236, 46);
    drawPortrait(7, y + 7, cd.sp, cd.tint, 2);
    text(44, y + 4, cd.name, C_WHITE);
    text(44, y + 13, fmt("%s %s", SPECIES[cd.sp].name, SPECIES[cd.sp].role), C_UIHI);
    wrapText(44, y + 22, 32, SPECIES[cd.sp].bonus, C_Y2, 1 << 20, 9, 2);
    bool room = nCrew < cap && nCrew < 6;
    if (button(170, y + 3, 64, 11, room ? fmt("HIRE %d", cd.hire) : "NO BUNK", room && credits >= cd.hire, C_Y2)) {
      crew[nCrew++] = (uint8_t)c;
      credits -= cd.hire;
      computeCrewMods();
      dlgBegin();
      dlgLine(SPK_NARR, cd.bio);
      dlgLinef(SPK_NARR, "%s JOINS YOUR CREW.", cd.name);
      dlgRun(DT_INFO);
    }
    y += 49;
  }
  /* roster */
  text(4, 172, fmt("YOUR CREW %d/%d%s", nCrew, cap, nCrew >= cap && nCrew < 6 ? " - BUNKS ADD BERTHS" : "  (TAP TO DISMISS)"), C_G4);
  for (int i = 0; i < nCrew; i++) {
    int x = 4 + i * 39;
    drawPortrait(x + 1, 183, CREW[crew[i]].sp, CREW[crew[i]].tint, 2);
    if (tapIn(x, 182, 36, 34) && nCrew > 1) {
      dlgBegin();
      dlgLinef(SPK_NARR, "DISMISS %s? THEY WILL WAIT FOR WORK AT THEIR HOME STATION.", CREW[crew[i]].name);
      dlgOpt("DISMISS", 10 + i);
      dlgOpt("KEEP", 0);
      dlgRun(DT_DISMISS);
    }
  }
  if (button(60, 218, 120, 20, "DONE", true, C_Y2)) {
    mode = MD_STATION;
    barTalk = -1;
  }
}

/* =================================================================== map */
static const float MAPK = 0.0083f;
static const int MCX = 120, MCY = 100;
static int mapSel = -1; /* 0..5 stations, 10 gate, 11 wreck, 12 nest, 20 custom */
static float mapSelX, mapSelY;

static void mapXY(float wx, float wy, int &x, int &y) {
  x = MCX + (int)(wx * MAPK);
  y = MCY + (int)(wy * MAPK);
}

static bool mapFeature(int id, float &x, float &y, const char *&name) {
  if (id >= 0 && id < ST_COUNT) {
    if (!(known & (1 << id))) return false;
    x = STATIONS[id].x; y = STATIONS[id].y; name = STATIONS[id].name;
    return true;
  }
  if (id == 10 && knownGate) { x = GATE_X; y = GATE_Y; name = "ARCHITECT GATE"; return true; }
  if (id == 11 && (known & KN_WRECK)) { x = WRECK_X; y = WRECK_Y; name = "WRECK OF THE SAINT IVO"; return true; }
  if (id == 12 && (known & KN_NEST)) { x = NEST_X; y = NEST_Y; name = "KELL'S GRAVE"; return true; }
  if (id == 20) { x = mapSelX; y = mapSelY; name = "CUSTOM WAYPOINT"; return true; }
  return false;
}

void uiMap() {
  gameTime += DT;
  Ship &p = ships[PL];
  clear(C_SPACE);
  for (int y = 14; y < 196; y += 12) dline(0, y, LW - 1, y, C_UIBG, 1, 3);
  for (int x = 0; x < LW; x += 12) dline(x, 14, x, 195, C_UIBG, 1, 3);
  header("SYSTEM MAP");
  int x, y;
  /* nebula */
  mapXY(NEB_X, NEB_Y, x, y);
  ditherCircle(x, y, (int)(NEB_R * MAPK), C_V1, 7);
  ditherCircle(x, y, (int)(NEB_R * MAPK * 0.6f), C_V2, 4);
  text(x - 15, y + 12, "VEIL", C_V3);
  /* belt */
  for (int k = 0; k < 220; k++) {
    float a = k * TAUF / 220, r = BELT_R0 + (hash2(k, 0, 4) % 1000) * (BELT_R1 - BELT_R0) / 1000;
    mapXY(cosf(a) * r, sinf(a) * r, x, y);
    pset(x, y, C_O1);
  }
  /* sun, planets */
  fillCircle(MCX, MCY, 3, C_Y3);
  circle(MCX, MCY, 5, C_O3);
  for (int i = 0; i < NPLANET; i++) {
    mapXY(PLANETS[i].x, PLANETS[i].y, x, y);
    uint8_t c = PLANETS[i].type == P_TERRAN ? C_B4 : (PLANETS[i].type == P_GAS ? C_O3 : (PLANETS[i].type == P_ICE ? C_V3 : C_G3));
    fillCircle(x, y, PLANETS[i].r > 100 ? 3 : 2, c);
  }
  /* lanes */
  for (int k = 0; k < NLANE; k++) {
    int x0, y0, x1, y1;
    mapXY(LANES[k].ax, LANES[k].ay, x0, y0);
    mapXY(LANES[k].bx, LANES[k].by, x1, y1);
    if (laneOpen[k]) line(x0, y0, x1, y1, C_O3);
    else dline(x0, y0, x1, y1, C_G2, 2, 2);
  }
  /* features */
  for (int id = 0; id <= 20; id++) {
    float fx, fy;
    const char *nm;
    if (id == 20 && mapSel != 20) continue;
    if (!mapFeature(id, fx, fy, nm)) continue;
    mapXY(fx, fy, x, y);
    uint8_t c = C_WHITE;
    if (id < ST_COUNT) {
      c = stationHostile(id) ? C_R4 : (STATIONS[id].fac == F_THREN ? C_GD2 : (STATIONS[id].fac == F_SYND ? C_R3 : C_B5));
      fillRect(x - 2, y - 2, 5, 5, c);
      if (storyMarker(id) && ((frameNo / 8) & 1)) text(x + 4, y - 10, "!", C_Y2);
      const char *sn = STATIONS[id].shortName;
      int lx = clampi(x - textW(sn) / 2, 1, LW - 1 - textW(sn));
      text(lx, y + 4, sn, C_G4);
      if (raid.on && raid.sta == id) circle(x, y, 5 + (int)(frameNo / 3 % 6), C_R4);
    } else if (id == 10) {
      circle(x, y, 4, C_V4);
      text(x - 23, y + 6, "GATE", C_V4);
    } else if (id == 20) {
      line(x - 3, y, x + 3, y, C_UIHI);
      line(x, y - 3, x, y + 3, C_UIHI);
    } else {
      pset(x, y, C_O5);
      circle(x, y, 2, id == 12 ? C_V3 : C_G4);
    }
    if (mapSel == id) circle(x, y, 7, (frameNo & 8) ? C_Y2 : C_Y1);
  }
  /* waypoint & player */
  float wx, wy;
  if (waypoint(wx, wy)) {
    mapXY(wx, wy, x, y);
    for (int k = -3; k <= 3; k++) { pset(x + k, y - 3 + abs(k), C_UIHI); pset(x + k, y + 3 - abs(k), C_UIHI); }
  }
  if (ap.on)
    for (int k = ap.i; k < ap.n; k++) {
      int x1, y1;
      mapXY(ap.px[k], ap.py[k], x1, y1);
      pset(x1, y1, C_UIHI);
    }
  mapXY(p.x, p.y, x, y);
  float a = p.ang;
  fillTri(x + sinf(a) * 5, y - cosf(a) * 5, x + sinf(a + 2.5f) * 4, y - cosf(a + 2.5f) * 4,
          x + sinf(a - 2.5f) * 4, y - cosf(a - 2.5f) * 4, (frameNo & 8) ? C_WHITE : C_Y3);
  /* selection */
  if (inTap && !tapEaten && inY > 14 && inY < 196) {
    tapEaten = true;
    int best = -1, bd = 14 * 14;
    for (int id = 0; id <= 12; id++) {
      float fx, fy;
      const char *nm;
      if (!mapFeature(id, fx, fy, nm)) continue;
      int sx, sy;
      mapXY(fx, fy, sx, sy);
      int d = (sx - inX) * (sx - inX) + (sy - inY) * (sy - inY);
      if (d < bd) { bd = d; best = id; }
    }
    if (best >= 0) mapSel = best;
    else {
      mapSel = 20;
      mapSelX = (inX - MCX) / MAPK;
      mapSelY = (inY - MCY) / MAPK;
    }
  }
  panel(0, 196, 240, 44);
  float fx, fy;
  const char *nm;
  bool haveSel = mapFeature(mapSel, fx, fy, nm);
  if (haveSel) {
    text(4, 199, nm, C_UIHI);
    text(4, 208, fmt("DISTANCE %d", (int)lenf(fx - p.x, fy - p.y)), C_G4);
  } else {
    text(4, 199, "TAP A STATION OR ANYWHERE TO PLOT", C_G4);
    text(4, 208, "A COURSE. AUTOPILOT USES LANES.", C_G4);
  }
  bool canAuto = haveSel && (mapReturn == MD_FLIGHT || dockedAt >= 0);
  if (button(4, 218, 112, 20, "AUTOPILOT", canAuto, C_Y2)) {
    int dest = mapSel < ST_COUNT ? mapSel : -1;
    if (dest >= 0 && dest == dockedAt) {
      toast("YOU ARE ALREADY HERE");
      mode = mapReturn;
    } else {
      if (mapReturn == MD_STATION && dockedAt >= 0) {
        const char *why = nullptr;
        if (storyBlocksLaunch(&why)) {
          toast("%s", why);
          mode = MD_STATION;
          return;
        }
        launchFrom(dockedAt);
      }
      if (dest >= 0 && stationHostile(dest)) dest = -1;
      planRoute(fx, fy, dest);
      mode = MD_FLIGHT;
      toast("AUTOPILOT ENGAGED");
    }
  }
  if (button(124, 218, 112, 20, "CLOSE")) mode = mapReturn;
}

/* ================================================================== menu */
static int menuTab = 0;

void uiMenu() {
  worldDraw();
  darken(11);
  header("CAPTAIN'S LOG");
  static const char *tabs[3] = {"SHIP", "CREW", "LOG"};
  for (int k = 0; k < 3; k++)
    if (button(4 + k * 78, 16, 74, 14, tabs[k], true, menuTab == k ? C_Y2 : C_UILN)) menuTab = k;
  Ship &p = ships[PL];
  const Stats &s = designs[D_PLAYER].st;
  panel(2, 33, 236, 180);
  if (menuTab == 0) {
    drawShipSprite(D_PLAYER, 58, 110, 0, 3);
    int y = 40;
    auto row = [&](const char *k, const char *v, uint8_t c) {
      text(124, y, k, C_G4);
      textR(234, y, v, c);
      y += 10;
    };
    row("HULL", fmt("%d/%d", (int)p.hp, (int)p.hpMax), C_WHITE);
    row("SHIELD", fmt("%d", (int)s.shield), C_T4);
    row("SPEED", fmt("%d", (int)s.vmax), C_WHITE);
    row("CRUISE", fmt("%d", (int)cruiseSpeed()), C_B5);
    row("TURN", fmt("%d DEG/S", (int)(s.turn * 57.3f)), C_WHITE);
    row("POWER", fmt("%d/%d", s.powGen + cm.power, s.powUse), playerEff() < 1 ? C_R4 : C_GR4);
    row("GUNS", fmt("%d", s.nMount), C_WHITE);
    row("CARGO", fmt("%d/%d", cargoUsed(), s.cargo), C_WHITE);
    row("RADAR", fmt("%d", (int)(s.radar * cm.radar)), C_WHITE);
    row("MASS", fmt("%d", (int)s.mass), C_WHITE);
    row("KILLS", fmt("%d", kills), C_Y2);
    wrapText(8, 172, 37, "REBUILD YOUR SHIP AT ANY STATION SHIPYARD. THE GAME SAVES EVERY TIME YOU DOCK.", C_G3);
  } else if (menuTab == 1) {
    for (int i = 0; i < nCrew; i++) {
      const CrewDef &c = CREW[crew[i]];
      int y = 37 + i * 29;
      drawPortrait(6, y + 2, c.sp, c.tint, 1);
      text(26, y + 1, fmt("%s - %s %s", c.name, SPECIES[c.sp].name, SPECIES[c.sp].role), C_WHITE);
      wrapText(26, y + 10, 35, SPECIES[c.sp].bonus, C_Y2, 1 << 20, 9, 2);
    }
    if (!nCrew) textC(CX, 100, "NO CREW", C_G4);
  } else {
    text(6, 37, missionTitle(), C_Y2);
    wrapText(6, 48, 38, missionLog(), C_G5, 1 << 20, 9, 9);
    const char *o = objective();
    if (o && *o) wrapText(6, 134, 38, fmt("NOW: %s", o), C_UIHI, 1 << 20, 9, 2);
    if (job.type) wrapText(6, 154, 38, fmt("JOB: %s", jobObjective()), C_O4, 1 << 20, 9, 2);
    static const char *fn[3] = {"AUTHORITY", "SYNDICATE", "THRENNIC"};
    for (int k = 0; k < 3; k++) {
      int v = rep[F_AUTH + k];
      text(6 + k * 78, 177, fn[k], C_G4);
      bar(6 + k * 78, 187, 70, 4, (v + 100) / 200.0f, v < 0 ? C_R3 : C_GR3, C_G1);
      text(6 + k * 78, 194, fmt("%d", v), v < 0 ? C_R4 : C_GR4);
    }
  }
  if (button(60, 216, 120, 22, "RESUME", true, C_Y2)) mode = MD_FLIGHT;
}

/* ============================================================ death / end */
static float deadT = 0;

void uiDead() {
  deadT += DT;
  if (deadT < 2.5f) {
    worldUpdate();
    worldDraw();
    darken((int)(deadT * 6));
    return;
  }
  clear(C_BLACK);
  drawStarfield(gameTime * 4, 0, 0, 0, false);
  gameTime += DT;
  textShC(CX, 50, "YOUR SHIP IS GONE", C_R4, 2);
  int st = lastStation < 0 ? 0 : lastStation;
  wrapText(14, 84, 36, fmt("THE ESCAPE POD DRIFTS FOR NINE HOURS BEFORE A TUG FROM %s FINDS YOU. "
                           "YOUR INSURER GRUMBLES AND REBUILDS THE SHIP. THE CARGO IS LOST.",
                           STATIONS[st].name),
           C_G5);
  if (button(60, 190, 120, 24, "CONTINUE", deadT > 3.2f, C_Y2)) {
    deadT = 0;
    respawn();
  }
}

void uiEnding() {
  gameTime += DT;
  clear(C_SPACE);
  drawStarfield(gameTime * 10, gameTime * 3, 0, 0, false);
  textShC(CX, 22, ending == 1 ? "THE GATE IS SEALED" : "THE GATE IS OPEN", ending == 1 ? C_B5 : C_Y3, 1);
  textShC(CX, 36, "THE END", C_WHITE, 3);
  const char *epi = ending == 1
      ? "THE HUSH DRIFT DARK AND EMPTY. THE LANES STILL SING. VARROW PINS A MEDAL ON YOU AND NEVER SAYS WHAT SHE THINKS ABOUT THE DOOR. SOME NIGHTS, THE SYLITH SAY THEY HEAR KNOCKING."
      : "THE HUSH ARE GONE HOME. SHIPS OF LIGHT NOW DRIFT THROUGH THE GATE, CURIOUS AS CHILDREN. THRENNIC BROKERS ARE ALREADY TRYING TO SELL THEM THINGS. THE REACH WILL NEVER BE THE SAME.";
  panel(8, 64, 224, 92);
  wrapText(14, 69, 35, epi, C_G5);
  int secs = (int)playTime;
  text(14, 162, fmt("TIME FLOWN  %dH %02dM", secs / 3600, (secs / 60) % 60), C_G4);
  text(14, 172, fmt("SHIPS DOWN  %d", kills), C_G4);
  text(14, 182, fmt("CREDITS     %d", credits), C_G4);
  if (button(30, 204, 180, 26, "KEEP FLYING THE REACH", true, C_Y2)) {
    mode = MD_FLIGHT;
    saveGame();
  }
}

/* ================================================================ flight */
void flightFrame() {
  Ship &p = ships[PL];
  steering = !dlg.on && inDown && inY < 212;
  if (!dlg.on) {
    worldUpdate();
    if (mode != MD_FLIGHT) return;
    storyTick();
    jobTick();
    scanTick();
    playTime += DT;
  }
  worldDraw();
  hudDraw();
  if (mode != MD_FLIGHT) return;
  const int by = 214, bh = 24, bw = 57;
  if (button(2, by, bw, bh, cruise == CR_OFF ? "CRUISE" : "STOP", !lr.on, C_B5)) {
    if (cruise == CR_OFF) {
      cruise = CR_CHARGE;
      cruiseT = 0;
    } else {
      cruise = CR_OFF;
    }
  }
  /* context action */
  const char *act = "---";
  int actKind = 0, actArg = -1;
  float lanePos = 0;
  float d;
  int s = nearestStation(p.x, p.y, &d);
  if (lr.on) { act = "EXIT"; actKind = 1; }
  else if (d < stationDockRange(s)) {
    act = "DOCK";
    actKind = 2;
    actArg = s;
  } else {
    int k = nearRing(p.x, p.y, &lanePos);
    if (k >= 0) { act = "LANE"; actKind = 3; actArg = k; }
    else {
      for (int i = FIRST_DYN; i < MAXSHIP; i++) {
        Ship &w = ships[i];
        if (!w.on) continue;
        float dd = dist2(w.x, w.y, p.x, p.y);
        if (w.kind == K_WRECK && !(w.flags & SF_SCANNED) && dd < 85 * 85) { act = scanTarget == i ? "SCANNING" : "SCAN"; actKind = 4; actArg = i; break; }
        if ((w.flags & SF_HAIL) && !(w.flags & SF_HAILED) && dd < 220 * 220) { act = "HAIL"; actKind = 5; actArg = i; }
      }
    }
  }
  if (button(61, by, bw, bh, act, actKind != 0, C_Y2)) {
    switch (actKind) {
    case 1: leaveLane(); break;
    case 2:
      if (stationHostile(actArg)) toast("DOCKING REFUSED");
      else if (raid.on && raid.sta == actArg) toast("BAY SEALED - UNDER ATTACK");
      else if (ships[stationShip(actArg)].hp <= 0) toast("DOCKING BAY DAMAGED");
      else dockAt(actArg);
      break;
    case 3: {
      const LaneDef &L = LANES[actArg];
      float len = lenf(L.bx - L.ax, L.by - L.ay);
      float dir;
      if (lanePos < 1) dir = 1;
      else if (lanePos > len - 1) dir = -1;
      else dir = (sinf(p.ang) * (L.bx - L.ax) - cosf(p.ang) * (L.by - L.ay)) >= 0 ? 1.0f : -1.0f;
      ap.on = false;
      enterLane(actArg, lanePos, dir);
      break;
    }
    case 4: scanTarget = actArg; break;
    case 5:
      ships[actArg].flags |= SF_HAILED;
      startHail(actArg);
      break;
    }
  }
  if (button(120, by, bw, bh, "MAP")) {
    mapReturn = MD_FLIGHT;
    mode = MD_MAP;
  }
  if (button(179, by, bw + 2, bh, "MENU")) mode = MD_MENU;
}

} // namespace sl
