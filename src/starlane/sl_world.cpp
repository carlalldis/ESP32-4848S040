/* STARLANE - world simulation: ship designs, physics, AI, combat, cruise,
 * trade lanes, autopilot, random encounters and station raids. */
#include "sl.h"

namespace sl {

Design designs[D_COUNT];
Stats stationStats[ST_COUNT];
Ship ships[MAXSHIP];
Shot shots[MAXSHOT];
Part parts[MAXPART];
uint8_t cruise = CR_OFF;
float cruiseT = 0;
LaneRun lr;
Autopilot ap;
float camX, camY, shake;
bool steering;
Raid raid;
float raidCooldown = 150;
bool raidWon;
int hailShip = -1;
bool laneOpen[NLANE] = {true, true, true, false};
int rep[F_COUNT];
CrewMods cm;

static float encT = 30, trafficT = 8, barkCool = 0;
static uint8_t groupSeq = 1;
static float heatWarnT = 0;

/* ================================================================ designs */
static const int DXS[4] = {0, 1, 0, -1}, DYS[4] = {-1, 0, 1, 0};

static bool cellFree(const Grid &g, int x, int y) {
  return x < 0 || y < 0 || x >= GW || y >= GH || g.m[y][x] == M_NONE;
}
bool weaponBlocked(const Grid &g, int x, int y) {
  if (g.m[y][x] == M_TURRET) return false;
  int d = g.dir[y][x] & 3;
  return !cellFree(g, x + DXS[d], y + DYS[d]);
}
bool engineBlocked(const Grid &g, int x, int y) { return !cellFree(g, x, y + 1); }
int autoDir(const Grid &g, int x, int y) {
  if (cellFree(g, x, y - 1)) return 0;
  bool l = cellFree(g, x - 1, y), r = cellFree(g, x + 1, y);
  if (l && r) return x <= GW / 2 ? 3 : 1;
  if (l) return 3;
  if (r) return 1;
  if (cellFree(g, x, y + 1)) return 2;
  return 0;
}
int gridCount(const Grid &g, uint8_t m) {
  int n = 0;
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++) n += g.m[y][x] == m;
  return n;
}
bool gridConnected(const Grid &g) {
  int sx = -1, sy = -1, total = 0;
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++)
      if (g.m[y][x]) {
        total++;
        if (g.m[y][x] == M_COCKPIT) { sx = x; sy = y; }
      }
  if (sx < 0) return false;
  bool seen[GH][GW] = {};
  int qx[GW * GH], qy[GW * GH], qh = 0, qt = 0, reached = 0;
  qx[qt] = sx; qy[qt++] = sy; seen[sy][sx] = true;
  while (qh < qt) {
    int x = qx[qh], y = qy[qh++];
    reached++;
    for (int d = 0; d < 4; d++) {
      int nx = x + DXS[d], ny = y + DYS[d];
      if (nx < 0 || ny < 0 || nx >= GW || ny >= GH || seen[ny][nx] || !g.m[ny][nx]) continue;
      seen[ny][nx] = true;
      qx[qt] = nx; qy[qt++] = ny;
    }
  }
  return reached == total;
}

static uint8_t srcMod(char c, uint8_t &dir, bool &autoD) {
  autoD = false;
  dir = 0;
  switch (c) {
  case 'C': return M_COCKPIT;
  case 'H': return M_HULL;
  case 'A': return M_ARMOR;
  case 'R': return M_REACTOR;
  case 'E': return M_ENGINE;
  case 'S': return M_SHIELD;
  case 'G': return M_CARGO;
  case 'Q': return M_QUARTERS;
  case 'L': autoD = true; return M_LASER;
  case '<': dir = 3; return M_LASER;
  case '>': dir = 1; return M_LASER;
  case 'K': autoD = true; return M_CANNON;
  case '{': dir = 3; return M_CANNON;
  case '}': dir = 1; return M_CANNON;
  case 'M': autoD = true; return M_MISSILE;
  case 'T': return M_TURRET;
  case 'N': return M_SCANNER;
  case 'P': return M_REPAIR;
  case 'U': return M_CRUISE;
  case 'Y': return M_GYRO;
  default: return M_NONE;
  }
}

void gridFromSrc(Grid &g, const DesignSrc &s) {
  memset(&g, 0, sizeof(g));
  int ox = (GW - s.w) / 2, oy = (GH - s.h) / 2;
  bool autoF[GH][GW] = {};
  for (int y = 0; y < s.h; y++)
    for (int x = 0; x < s.w; x++) {
      uint8_t d;
      bool a;
      uint8_t m = srcMod(s.cells[y * s.w + x], d, a);
      g.m[oy + y][ox + x] = m;
      g.dir[oy + y][ox + x] = d;
      autoF[oy + y][ox + x] = a;
    }
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++)
      if (autoF[y][x]) g.dir[y][x] = (uint8_t)autoDir(g, x, y);
}

void compileDesign(Design &d, const uint8_t *roles) {
  Stats &s = d.st;
  memset(&s, 0, sizeof(s));
  const Grid &g = d.g;
  int n = 0;
  float sx = 0, sy = 0;
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++)
      if (g.m[y][x]) {
        n++;
        sx += x * MPX + MPX * 0.5f;
        sy += y * MPX + MPX * 0.5f;
      }
  memset(d.spr, TRANSP, sizeof(d.spr));
  if (!n) {
    s.comX = s.comY = SPR / 2;
    s.mass = 1;
    s.radius = 3;
    s.eff = 1;
    return;
  }
  s.comX = sx / n;
  s.comY = sy / n;
  int gyros = 0, cruises = 0, scanners = 0;
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++) {
      uint8_t m = g.m[y][x];
      if (!m) continue;
      const ModDef &md = MODS[m];
      s.mass += md.mass;
      s.hp += md.hp;
      if (md.power > 0) s.powGen += md.power;
      else s.powUse -= md.power;
      float lx = (x * MPX + 2 - s.comX) * SHIP_SCALE, ly = (y * MPX + 2 - s.comY) * SHIP_SCALE;
      float r = lenf(lx, ly) + 2.8f * SHIP_SCALE;
      if (r > s.radius) s.radius = r;
      switch (m) {
      case M_COCKPIT: s.crew += 2; break;
      case M_ENGINE:
        if (engineBlocked(g, x, y)) s.blockedEng++;
        else {
          s.thrust += 10;
          if (s.nEng < 12) {
            s.engX[s.nEng] = (int8_t)floorf(lx + 0.5f);
            s.engY[s.nEng] = (int8_t)floorf(ly + 2 * SHIP_SCALE + 0.5f);
            s.nEng++;
          }
        }
        break;
      case M_SHIELD: s.shield += 50; s.regen += 5; break;
      case M_CARGO: s.cargo += 10; break;
      case M_QUARTERS: s.crew += 1; break;
      case M_SCANNER: scanners++; break;
      case M_REPAIR: s.repair += 2; break;
      case M_CRUISE: cruises++; break;
      case M_GYRO: gyros++; break;
      default: {
        int w = modWeapon(m);
        if (w < 0) break;
        if (weaponBlocked(g, x, y)) s.blockedWeap++;
        else if (s.nMount < 20) {
          Mount &mt = s.mount[s.nMount++];
          mt.w = (uint8_t)w;
          mt.dir = g.dir[y][x] & 3;
          mt.lx = (int8_t)floorf(lx + 0.5f);
          mt.ly = (int8_t)floorf(ly + 0.5f);
        }
      }
      }
    }
  s.hp *= d.hpMul;
  s.shield *= d.hpMul;
  float r = s.thrust / s.mass;
  s.accel = 150 * r;
  s.vmax = s.thrust > 0 ? 40 + 90 * minf(r, 1.6f) : 6;
  s.turn = clampf((2.4f + 0.6f * gyros) * sqrtf(24.0f / s.mass) * (0.6f + 0.4f * minf(r, 1.5f)), 0.6f, 5.0f);
  if (s.thrust <= 0) s.turn = 0.4f;
  s.eff = s.powUse <= s.powGen ? 1.0f : maxf(0.25f, (float)s.powGen / (float)s.powUse);
  s.cruiseMul = 1 + 0.35f * cruises;
  s.radar = 700.0f + 400.0f * scanners;
  /* sprite */
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++) {
      uint8_t m = g.m[y][x];
      if (!m) continue;
      const char *tex = MODS[m].tex;
      int dir = modWeapon(m) >= 0 ? (g.dir[y][x] & 3) : 0;
      for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
          int sr = j, sc = i;
          if (dir == 1) { sr = 3 - i; sc = j; }
          else if (dir == 2) { sr = 3 - j; sc = 3 - i; }
          else if (dir == 3) { sr = i; sc = 3 - j; }
          char ch = tex[sr * 4 + sc];
          uint8_t c = (ch >= '1' && ch <= '4') ? roles[ch - '1'] : charColor(ch);
          if (c == TRANSP) continue;
          d.spr[(y * MPX + j) * SPR + x * MPX + i] = c;
        }
    }
}

const Stats &statsOf(const Ship &s) {
  return s.kind == K_STATION ? stationStats[s.sta] : designs[s.des].st;
}

/* =============================================================== helpers */
bool hasSpecies(int sp) {
  for (int i = 0; i < nCrew; i++)
    if (CREW[crew[i]].sp == sp) return true;
  return false;
}
int crewOfSpecies(int sp) {
  for (int i = 0; i < nCrew; i++)
    if (CREW[crew[i]].sp == sp) return crew[i];
  return -1;
}
void computeCrewMods() {
  int n[SP_NPORT] = {};
  for (int i = 0; i < nCrew; i++) n[CREW[crew[i]].sp]++;
  cm.nav = n[SP_VELKU] > 0;
  cm.eng = n[SP_ORNN] > 0;
  cm.envoy = n[SP_SYLITH] > 0;
  cm.gunner = n[SP_GRASK] > 0;
  cm.mech = n[SP_QUIBBIT] > 0;
  cm.seer = n[SP_LUMENI] > 0;
  cm.cruise = 1 + 0.2f * n[SP_VELKU];
  cm.charge = 1 + 0.7f * n[SP_VELKU];
  cm.laneSafe = n[SP_VELKU] ? 0.4f : 1.0f;
  cm.regen = 1 + 0.35f * n[SP_ORNN];
  cm.power = 2 * n[SP_ORNN];
  cm.dmg = 1 + 0.2f * n[SP_GRASK];
  cm.rate = 1 + 0.1f * n[SP_GRASK];
  cm.repair = 1.5f * n[SP_QUIBBIT];
  cm.repairCost = n[SP_QUIBBIT] ? 0.5f : 1.0f;
  cm.radar = 1 + 0.5f * n[SP_LUMENI];
  cm.tradeBuy = n[SP_SYLITH] ? 1.00f : 1.04f;
  cm.tradeSell = n[SP_SYLITH] ? 0.98f : 0.95f;
}

float playerEff() {
  const Stats &st = designs[D_PLAYER].st;
  int gen = st.powGen + cm.power;
  if (st.powUse <= gen) return 1.0f;
  return maxf(0.25f, (float)gen / (float)st.powUse);
}
float cruiseSpeed() {
  return 480.0f * designs[D_PLAYER].st.cruiseMul * cm.cruise;
}
static float chargeTime() {
  float t = 2.6f / cm.charge;
  if (designs[D_PLAYER].st.cruiseMul > 1) t *= 0.8f;
  return t;
}

void playerRefresh(bool heal) {
  Ship &p = ships[PL];
  Design &d = designs[D_PLAYER];
  float oldMax = p.hpMax;
  compileDesign(d, PAINTS[paint & 3]);
  computeCrewMods();
  p.kind = K_SHIP;
  p.des = D_PLAYER;
  p.fac = F_PLAYER;
  p.hpMax = d.st.hp;
  p.shMax = d.st.shield;
  if (heal) {
    p.hp = p.hpMax;
    p.sh = p.shMax;
  } else {
    if (p.hpMax > oldMax) p.hp += p.hpMax - oldMax;
    p.hp = clampf(p.hp, 1, p.hpMax);
    p.sh = clampf(p.sh, 0, p.shMax);
  }
}

float stationDockRange(int st) { return stationStats[st].radius + 60.0f; }

bool stationHostile(int st) {
  if (st == ST_ROOK) {
    if (mis == MS_SABLE || (mis == MS_ECHOES && mstep >= 2)) return false;
    return rep[F_SYND] < 0;
  }
  return rep[STATIONS[st].fac] < -50;
}

int nearestStation(float x, float y, float *d) {
  int best = -1;
  float bd = 1e18f;
  for (int s = 0; s < ST_COUNT; s++) {
    float dd = dist2(x, y, STATIONS[s].x, STATIONS[s].y);
    if (dd < bd) { bd = dd; best = s; }
  }
  if (d) *d = sqrtf(bd);
  return best;
}

int zoneAt(float x, float y) {
  float r = lenf(x, y);
  if (dist2(x, y, NEB_X, NEB_Y) < NEB_R * NEB_R) return Z_NEBULA;
  if (r > BELT_R0 - 200 && r < BELT_R1 + 200) return Z_BELT;
  if (r > BELT_R1 + 200) return Z_DEEP;
  float d;
  int s = nearestStation(x, y, &d);
  if (d < 2600 && s != ST_ROOK) return Z_SAFE;
  return Z_OPEN;
}

int countFlag(uint16_t f) {
  int n = 0;
  for (int i = FIRST_DYN; i < MAXSHIP; i++)
    if (ships[i].on && (ships[i].flags & f)) n++;
  return n;
}
void clearFlag(uint16_t f) {
  for (int i = FIRST_DYN; i < MAXSHIP; i++)
    if (ships[i].on && (ships[i].flags & f)) removeShip(i);
}

/* ============================================================= messaging */
static char toastBuf[64];
float toastT = 0;
const char *toastText() { return toastBuf; }
void toast(const char *f, ...) {
  va_list a;
  va_start(a, f);
  vsnprintf(toastBuf, sizeof(toastBuf), f, a);
  va_end(a);
  toastT = 2.6f;
}

BarkState bk;
void bark(uint8_t spk, int crewIdx, const char *t) {
  bk.spk = spk;
  bk.crew = crewIdx;
  snprintf(bk.text, sizeof(bk.text), "%s", t);
  bk.t = 3.5f + strlen(bk.text) / 25.0f;
}
void crewBark(int b) {
  if (nCrew <= 0 || barkCool > 0) return;
  int c = crew[irand(nCrew)];
  const char *t = BARKS[CREW[c].sp][b];
  if (!t) return;
  bark(SPK_CREWA, c, t);
  barkCool = 10;
}
float barkTime() { return bk.t; }

/* ============================================================= particles */
void spawnPart(float x, float y, float vx, float vy, int life, uint8_t col, uint8_t kind) {
  static int next = 0;
  for (int k = 0; k < MAXPART; k++) {
    next = (next + 1) % MAXPART;
    Part &p = parts[next];
    if (p.life <= 0 || k == MAXPART - 1) {
      p.x = x; p.y = y; p.vx = vx; p.vy = vy;
      p.life = p.max = (int16_t)life;
      p.col = col;
      p.kind = kind;
      return;
    }
  }
}

void explode(float x, float y, float size) {
  spawnPart(x, y, size * 0.35f, 0, 6, C_Y3, PK_FLASH);
  spawnPart(x, y, size * 0.12f, 0, 14, C_O4, PK_RING);
  int n = (int)clampf(size * 1.4f, 8, 40);
  for (int i = 0; i < n; i++) {
    float a = frand() * TAUF, s = frange(20, 90) * (0.5f + size / 30);
    static const uint8_t cols[4] = {C_Y3, C_O4, C_O3, C_R3};
    spawnPart(x, y, cosf(a) * s, sinf(a) * s, 8 + irand(16), cols[irand(4)], PK_SPARK);
  }
  for (int i = 0; i < n / 2; i++) {
    float a = frand() * TAUF, s = frange(5, 30);
    spawnPart(x + cosf(a) * 3, y + sinf(a) * 3, cosf(a) * s, sinf(a) * s, 20 + irand(25),
              irand(2) ? C_G2 : C_G3, PK_SMOKE);
  }
  float d = lenf(x - ships[PL].x, y - ships[PL].y);
  if (d < 300) shake = maxf(shake, size * 0.25f * (1 - d / 300));
}

static void updateParts() {
  for (Part &p : parts) {
    if (p.life <= 0) continue;
    p.life--;
    if (p.kind == PK_RING || p.kind == PK_FLASH) continue;
    p.x += p.vx * DT;
    p.y += p.vy * DT;
    float k = p.kind == PK_SMOKE ? 0.94f : 0.97f;
    p.vx *= k;
    p.vy *= k;
  }
}

/* ================================================================= ships */
int spawnShip(int des, float x, float y, float ang, uint16_t flags, uint8_t ai) {
  for (int i = FIRST_DYN; i < MAXSHIP; i++) {
    if (ships[i].on) continue;
    Ship &s = ships[i];
    memset(&s, 0, sizeof(s));
    s.on = true;
    s.kind = K_SHIP;
    s.des = (uint8_t)des;
    s.fac = designs[des].fac;
    s.x = x; s.y = y; s.ang = ang;
    s.flags = flags;
    s.ai = ai;
    const Stats &st = designs[des].st;
    s.hp = s.hpMax = st.hp;
    s.sh = s.shMax = st.shield;
    s.target = -1;
    s.anchor = -1;
    s.tx = x; s.ty = y;
    for (int k = 0; k < st.nMount; k++) s.cd[k] = frand() * WEAPS[st.mount[k].w].cd + 0.4f;
    return i;
  }
  return -1;
}

void removeShip(int i) {
  if (i < FIRST_DYN) return;
  ships[i].on = false;
  for (int j = 0; j < MAXSHIP; j++)
    if (ships[j].target == i) ships[j].target = -1;
  for (Shot &s : shots)
    if (s.on && s.target == i) s.target = -1;
  if (hailShip == i) hailShip = -1;
}

void killShip(int i) {
  Ship &s = ships[i];
  const Stats &st = statsOf(s);
  explode(s.x, s.y, st.radius);
  if (i == PL) {
    explode(s.x + 6, s.y - 4, 10);
    s.hp = 0;
    cruise = CR_OFF;
    ap.on = false;
    lr.on = false;
    mode = MD_DEAD;
    fade = 0;
    return;
  }
  if (s.kind == K_STATION) return;
  bool byPlayer = s.pdmg > st.hp * 0.15f;
  if (byPlayer && (s.flags & SF_HOSTILE)) {
    kills++;
    int bounty = (int)(st.mass * (s.fac == F_HUSH ? 4 : 3));
    if (s.flags & SF_DRONE) bounty = 0;
    if (bounty > 0) {
      credits += bounty;
      toast("KILL  +%d CR", bounty);
    }
    if (s.fac == F_SYND) {
      rep[F_SYND] -= 2;
      rep[F_AUTH] += 1;
    }
    if (s.fac == F_HUSH) rep[F_AUTH] += 1;
    if (irand(4) == 0) crewBark(BK_WIN);
  }
  storyOnKill(i);
  jobOnKill(i);
  removeShip(i);
}

void damageShip(int i, float dmg, int src) {
  Ship &s = ships[i];
  if (!s.on || dmg <= 0) return;
  if (s.flags & SF_INVULN) {
    s.shT = 0.25f;
    return;
  }
  if (i == PL) {
    if (cruise != CR_OFF) dropCruise("CRUISE DISRUPTED");
    if (ap.on) { ap.on = false; toast("AUTOPILOT DISENGAGED"); }
    shake = maxf(shake, 2.0f);
    if (s.hp < s.hpMax * 0.3f && irand(30) == 0) crewBark(BK_HULL);
  }
  if (src == PL) {
    s.pdmg += dmg;
    if (raid.on && (s.flags & SF_RAID)) raid.pdmg += dmg;
  }
  if (s.sh > 0) {
    float a = minf(s.sh, dmg);
    s.sh -= a;
    dmg -= a;
    s.shT = 0.25f;
  }
  s.regenT = 2.5f;
  if (dmg > 0) {
    s.hp -= dmg;
    s.hitT = 0.07f;
  }
  /* NPCs fight back */
  if (i != PL && s.kind == K_SHIP && src >= 0 && src < MAXSHIP && ships[src].on) {
    if (src == PL && !(s.flags & SF_HOSTILE)) {
      /* never happens via auto-fire, but stay safe */
    } else if (s.ai == AI_PATROL || s.ai == AI_TRAVEL || s.ai == AI_DEFEND || s.ai == AI_HOLD) {
      if (statsOf(s).nMount > 0 && isEnemy(s, ships[src])) {
        s.target = (int8_t)src;
        s.ai = AI_ATTACK;
      }
    }
  }
  if (s.kind == K_STATION) {
    if (s.hp <= 0) {
      if (s.hp > -1000) {
        explode(s.x, s.y, 20);
        if (i == stationShip(raid.sta) && raid.on) toast("%s CRIPPLED!", STATIONS[s.sta].shortName);
      }
      s.hp = -1000;
    }
    return;
  }
  if (s.hp <= 0) killShip(i);
}

/* ================================================================ hostility */
static bool facEnemy(uint8_t fa, uint16_t fla, uint8_t fb, uint16_t flb) {
  if (fa == fb) return false;
  if (fa == F_PLAYER || fb == F_PLAYER) return false;
  if (fa == F_HUSH || fb == F_HUSH) return ending != 2;
  if ((fa == F_AUTH && fb == F_SYND) || (fa == F_SYND && fb == F_AUTH)) return true;
  if ((fa == F_THREN && fb == F_SYND) || (fa == F_SYND && fb == F_THREN))
    return ((fla | flb) & SF_ATTACKER) != 0;
  return false;
}
static bool targetable(const Ship &b) {
  return b.on && b.kind != K_WRECK && !(b.flags & SF_LANE);
}
bool isEnemy(const Ship &a, const Ship &b) {
  int ai = (int)(&a - ships), bi = (int)(&b - ships);
  if (ai == bi || !targetable(b) || (a.flags & SF_LANE)) return false;
  if (b.kind == K_STATION && b.hp <= 0) return false;
  if (ai == PL) return (b.flags & SF_HOSTILE) != 0;
  if (bi == PL) return (a.flags & SF_HOSTILE) != 0;
  return facEnemy(a.fac, a.flags, b.fac, b.flags);
}
static bool shotHits(const Shot &sh, const Ship &t, int j) {
  if (!targetable(t)) return false;
  if (t.kind == K_STATION && t.hp <= 0) return false;
  if (sh.fromPlayer) return (t.flags & SF_HOSTILE) != 0;
  if (j == PL) return (sh.oflags & SF_HOSTILE) != 0;
  return facEnemy(sh.fac, sh.oflags, t.fac, t.flags);
}

static int findTarget(int si, float range) {
  Ship &s = ships[si];
  float best = range * range;
  int bi = -1;
  for (int j = 0; j < MAXSHIP; j++) {
    if (!isEnemy(s, ships[j])) continue;
    float d = dist2(s.x, s.y, ships[j].x, ships[j].y);
    /* raiders prefer the station they came for, defenders prefer raiders */
    if (ships[j].kind == K_STATION) d *= (s.flags & SF_ATTACKER) ? 0.5f : 1.5f;
    if (d < best) { best = d; bi = j; }
  }
  return bi;
}

/* ================================================================ weapons */
static float npcDmgMul(const Ship &s) {
  if (s.kind == K_STATION) return 1.0f;
  if (s.flags & SF_BOSS) return 0.45f;
  switch (s.fac) {
  case F_SYND: return 0.3f;
  case F_HUSH: return 0.3f + 0.025f * (float)mis;
  default: return 0.35f;
  }
}

static void spawnShot(int owner, int w, float x, float y, float ang, float bvx, float bvy,
                      float dmg, int target) {
  for (Shot &s : shots) {
    if (s.on) continue;
    const WeapDef &wd = WEAPS[w];
    float sp = w == W_MISSILE ? wd.speed * 0.55f : wd.speed;
    s.on = true;
    s.fromPlayer = owner == PL;
    s.w = (uint8_t)w;
    s.fac = ships[owner].fac;
    s.oflags = ships[owner].flags;
    s.owner = (int8_t)owner;
    s.target = (int8_t)target;
    s.x = x; s.y = y;
    s.vx = sinf(ang) * sp + bvx;
    s.vy = -cosf(ang) * sp + bvy;
    s.life = w == W_MISSILE ? 2.8f : wd.range / wd.speed * 1.15f;
    s.dmg = dmg;
    return;
  }
}

static int enemyMissileNear(const Ship &s, float x, float y, float range) {
  float best = range * range;
  int bi = -1;
  bool isPl = &s == &ships[PL];
  for (int k = 0; k < MAXSHOT; k++) {
    const Shot &m = shots[k];
    if (!m.on || m.w != W_MISSILE) continue;
    bool hostile = isPl ? !m.fromPlayer && (m.oflags & SF_HOSTILE)
                        : (m.fromPlayer ? (s.flags & SF_HOSTILE) != 0
                                        : facEnemy(m.fac, m.oflags, s.fac, s.flags));
    if (!hostile) continue;
    float d = dist2(x, y, m.x, m.y);
    if (d < best) { best = d; bi = k; }
  }
  return bi;
}

static void fireWeapons(int si) {
  Ship &s = ships[si];
  if (s.kind == K_WRECK || (s.flags & SF_LANE)) return;
  if (s.kind == K_STATION && s.hp <= 0) return;
  if (si == PL && (cruise == CR_ON || lr.on)) return;
  const Stats &st = statsOf(s);
  if (!st.nMount) return;
  float eff = si == PL ? playerEff() : 1.0f;
  float rate = si == PL ? cm.rate : (s.kind == K_STATION ? 1.0f : 0.6f);
  float dmgMul = si == PL ? cm.dmg : npcDmgMul(s);
  float ca = cosf(s.ang), sa = sinf(s.ang);
  for (int k = 0; k < st.nMount; k++) {
    s.cd[k] -= DT * eff * rate;
    if (s.cd[k] > 0) continue;
    const Mount &m = st.mount[k];
    const WeapDef &w = WEAPS[m.w];
    float mx = s.x + ca * m.lx - sa * m.ly, my = s.y + sa * m.lx + ca * m.ly;
    float mang = s.ang + m.dir * (PIF / 2);
    if (m.w == W_TURRET) {
      int mi = enemyMissileNear(s, mx, my, w.range * 0.8f);
      if (mi >= 0) {
        Shot &ms = shots[mi];
        float aim = angTo(ms.x - mx, ms.y - my);
        spawnShot(si, W_TURRET, mx, my, aim, 0, 0, 0, -1);
        if (irand(100) < 40) {
          ms.on = false;
          spawnPart(ms.x, ms.y, 4, 0, 5, C_Y3, PK_FLASH);
        }
        s.cd[k] = w.cd;
        continue;
      }
    }
    /* target: NPCs keep their chosen target if it is in this mount's arc */
    int t = -1;
    float best = w.range * w.range;
    if (si != PL && s.target >= 0 && ships[s.target].on && isEnemy(s, ships[s.target])) {
      Ship &T = ships[s.target];
      float d = dist2(mx, my, T.x, T.y);
      if (d < best && (w.arc >= PIF || fabsf(wrapAng(angTo(T.x - mx, T.y - my) - mang)) < w.arc + 0.25f))
        t = s.target;
    }
    if (t < 0) {
      for (int j = 0; j < MAXSHIP; j++) {
        if (!isEnemy(s, ships[j])) continue;
        Ship &T = ships[j];
        float dx = T.x - mx, dy = T.y - my;
        if (fabsf(dx) > w.range || fabsf(dy) > w.range) continue;
        float d = dx * dx + dy * dy;
        if (d > best) continue;
        if (w.arc < PIF && fabsf(wrapAng(angTo(dx, dy) - mang)) > w.arc + 0.25f) continue;
        best = d;
        t = j;
      }
    }
    if (t < 0) {
      s.cd[k] = 0.06f;
      continue;
    }
    Ship &T = ships[t];
    float dx = T.x - mx, dy = T.y - my;
    float tt = lenf(dx, dy) / w.speed;
    float ax = dx + (T.vx - s.vx) * tt, ay = dy + (T.vy - s.vy) * tt;
    float aim = angTo(ax, ay);
    if (w.arc < PIF) aim = mang + clampf(wrapAng(aim - mang), -w.arc, w.arc);
    if (si != PL) aim += (frand() - 0.5f) * 0.14f;
    spawnShot(si, m.w, mx, my, aim, s.vx, s.vy, w.dmg * dmgMul, t);
    s.cd[k] = w.cd * frange(0.9f, 1.1f);
  }
}

static void updateShots() {
  for (Shot &s : shots) {
    if (!s.on) continue;
    s.life -= DT;
    if (s.life <= 0) {
      s.on = false;
      if (s.w == W_MISSILE) spawnPart(s.x, s.y, 3, 0, 5, C_O4, PK_FLASH);
      continue;
    }
    if (s.w == W_MISSILE) {
      float cur = angTo(s.vx, s.vy), sp = lenf(s.vx, s.vy);
      if (s.target >= 0 && ships[s.target].on) {
        Ship &T = ships[s.target];
        float des = angTo(T.x - s.x, T.y - s.y);
        cur += clampf(wrapAng(des - cur), -3.4f * DT, 3.4f * DT);
      }
      sp = minf(sp + 300 * DT, 240);
      s.vx = sinf(cur) * sp;
      s.vy = -cosf(cur) * sp;
      if (frameNo & 1) spawnPart(s.x, s.y, frange(-5, 5), frange(-5, 5), 14, C_G3, PK_SMOKE);
    }
    s.x += s.vx * DT;
    s.y += s.vy * DT;
    if (s.dmg <= 0) continue; /* tracer */
    for (int j = 0; j < MAXSHIP; j++) {
      Ship &t = ships[j];
      if (!t.on || j == s.owner) continue;
      float dx = t.x - s.x, dy = t.y - s.y;
      if (fabsf(dx) > 40 || fabsf(dy) > 40) continue;
      float r = statsOf(t).radius;
      if (dx * dx + dy * dy > r * r) continue;
      if (!shotHits(s, t, j)) continue;
      uint8_t c = s.w == W_MISSILE ? C_O4 : FACS[s.fac].shot;
      for (int k = 0; k < 4; k++)
        spawnPart(s.x, s.y, frange(-40, 40) - s.vx * 0.1f, frange(-40, 40) - s.vy * 0.1f,
                  5 + irand(5), c, PK_SPARK);
      if (s.w == W_MISSILE) explode(s.x, s.y, 7);
      s.on = false;
      damageShip(j, s.dmg, s.owner);
      break;
    }
  }
}

/* ===================================================================== AI */
static void steer(Ship &s, const Stats &st, float des, float thr) {
  float d = wrapAng(des - s.ang), mt = st.turn * DT;
  s.ang = wrapAng(s.ang + clampf(d, -mt, mt));
  if (thr > 0 && st.thrust > 0) {
    s.vx += sinf(s.ang) * st.accel * thr * DT;
    s.vy -= cosf(s.ang) * st.accel * thr * DT;
  }
  s.thr = thr;
}
static void moveShip(Ship &s, const Stats &st, float drag) {
  float sp = lenf(s.vx, s.vy);
  if (sp > st.vmax && sp > 0) {
    float k = maxf(st.vmax / sp, 1 - 2.0f * DT);
    s.vx *= k;
    s.vy *= k;
  }
  s.vx *= 1 - drag * DT;
  s.vy *= 1 - drag * DT;
  s.x += s.vx * DT;
  s.y += s.vy * DT;
}
static void goTo(Ship &s, const Stats &st, float x, float y, float thr) {
  float dx = x - s.x, dy = y - s.y, d = lenf(dx, dy);
  if (d < 1) {
    s.thr = 0;
    return;
  }
  float des = angTo(dx, dy);
  float aligned = fabsf(wrapAng(des - s.ang));
  float t = aligned > 1.2f ? 0.2f : thr;
  if (d < 80) t *= d / 80;
  steer(s, st, des, t);
}

static int sideMounts(const Stats &st) {
  int n = 0;
  for (int k = 0; k < st.nMount; k++)
    n += st.mount[k].w != W_TURRET && (st.mount[k].dir & 1);
  return n;
}
static int fwdMounts(const Stats &st) {
  int n = 0;
  for (int k = 0; k < st.nMount; k++)
    n += st.mount[k].w != W_TURRET && st.mount[k].dir == 0;
  return n;
}

static void aiShip(int i) {
  Ship &s = ships[i];
  const Stats &st = statsOf(s);
  Ship &p = ships[PL];
  float drag = 0.3f;
  switch (s.ai) {
  case AI_ATTACK: {
    if (s.target < 0 || !ships[s.target].on || !isEnemy(s, ships[s.target]) ||
        dist2(s.x, s.y, ships[s.target].x, ships[s.target].y) > 1500.0f * 1500.0f) {
      s.target = (int8_t)findTarget(i, 950);
      s.sub = 0;
    }
    if ((s.flags & SF_DEFENDER) && s.anchor >= 0 &&
        dist2(s.x, s.y, ships[s.anchor].x, ships[s.anchor].y) > 1200.0f * 1200.0f) {
      s.target = -1;
    }
    if (s.target < 0) {
      s.ai = (s.flags & SF_DEFENDER) ? AI_DEFEND : AI_PATROL;
      s.tx = s.x;
      s.ty = s.y;
      break;
    }
    Ship &t = ships[s.target];
    float dx = t.x - s.x, dy = t.y - s.y, d = lenf(dx, dy);
    float tr = statsOf(t).radius;
    if (sideMounts(st) > fwdMounts(st)) {
      float a = angTo(dx, dy), side = (i & 1) ? 1.0f : -1.0f, want = 120 + tr;
      float off = d > want + 50 ? 0.45f : (d < want - 40 ? 2.3f : PIF / 2);
      steer(s, st, a + side * off, 0.85f);
    } else if (s.sub == 0) {
      float lead = d / 420;
      steer(s, st, angTo(dx + (t.vx - s.vx) * lead, dy + (t.vy - s.vy) * lead), d > 90 ? 1.0f : 0.5f);
      if (d < 70 + tr) {
        s.sub = 1;
        s.aiT = frange(0.9f, 1.7f);
        s.tx = s.ang + ((i & 1) ? 1 : -1) * frange(1.5f, 2.4f);
      }
    } else {
      steer(s, st, s.tx, 1.0f);
      s.aiT -= DT;
      if (s.aiT <= 0) s.sub = 0;
    }
    if (s.fac == F_SYND && !(s.flags & (SF_MISSION | SF_RAID)) && s.hp < s.hpMax * 0.2f && (i % 3) == 0) {
      s.ai = AI_FLEE;
      if (dist2(s.x, s.y, p.x, p.y) < 400 * 400) toast("RAIDER IS FLEEING");
    }
    break;
  }
  case AI_DEFEND: {
    Ship &a = ships[s.anchor >= 0 ? s.anchor : PL];
    int t = findTarget(i, 750);
    if (t >= 0 && dist2(ships[t].x, ships[t].y, a.x, a.y) < 1000.0f * 1000.0f) {
      s.target = (int8_t)t;
      s.ai = AI_ATTACK;
      s.sub = 0;
      break;
    }
    float ang = gameTime * 0.35f + i * 1.3f;
    goTo(s, st, a.x + cosf(ang) * 150, a.y + sinf(ang) * 150, 0.6f);
    break;
  }
  case AI_PATROL: {
    int t = findTarget(i, 520);
    if (t >= 0 && st.nMount > 0) {
      s.target = (int8_t)t;
      s.ai = AI_ATTACK;
      s.sub = 0;
      break;
    }
    s.aiT -= DT;
    if (s.aiT <= 0 || dist2(s.x, s.y, s.tx, s.ty) < 40 * 40) {
      s.aiT = frange(3, 7);
      s.tx = s.x + frange(-250, 250);
      s.ty = s.y + frange(-250, 250);
    }
    goTo(s, st, s.tx, s.ty, 0.5f);
    break;
  }
  case AI_TRAVEL:
    goTo(s, st, s.tx, s.ty, 0.8f);
    if (dist2(s.x, s.y, s.tx, s.ty) < 60 * 60) {
      if (s.flags & (SF_TRADER | SF_DEFENDER | SF_ENC)) {
        removeShip(i);
        return;
      }
      s.ai = AI_PATROL;
    }
    break;
  case AI_HAIL: {
    float dx = p.x - s.x, dy = p.y - s.y, d = lenf(dx, dy);
    if (!(s.flags & SF_HAIL)) {
      /* follower: tag along behind the group leader */
      int lead = -1;
      for (int j = FIRST_DYN; j < MAXSHIP; j++)
        if (ships[j].on && ships[j].group == s.group && (ships[j].flags & SF_HAIL)) lead = j;
      if (lead < 0) {
        s.ai = AI_HOLD;
        break;
      }
      Ship &L = ships[lead];
      float side = (i & 1) ? 1.0f : -1.0f;
      goTo(s, st, L.x + cosf(L.ang) * 26 * side - sinf(L.ang) * 14, L.y + sinf(L.ang) * 26 * side + cosf(L.ang) * 14, 0.9f);
      break;
    }
    if (d > 130) steer(s, st, angTo(dx, dy), 1.0f);
    else {
      steer(s, st, angTo(dx, dy), 0);
      drag = 1.5f;
    }
    if (d < 190 && !(s.flags & SF_HAILED) && !dlg.on && mode == MD_FLIGHT && !lr.on) {
      s.flags |= SF_HAILED;
      startHail(i);
    }
    break;
  }
  case AI_HOLD:
    s.thr = 0;
    drag = 1.2f;
    break;
  case AI_FLEE: {
    float dx = s.x - p.x, dy = s.y - p.y;
    steer(s, st, angTo(dx, dy), 1.0f);
    if (dx * dx + dy * dy > 1400.0f * 1400.0f) {
      removeShip(i);
      return;
    }
    break;
  }
  case AI_DRIFT:
    s.ang = wrapAng(s.ang + 0.15f * DT);
    s.thr = 0;
    drag = 0.05f;
    break;
  case AI_LANE: {
    const LaneDef &L = LANES[s.sub];
    float lx = L.bx - L.ax, ly = L.by - L.ay, len = lenf(lx, ly);
    float ux = lx / len, uy = ly / len;
    s.ty += s.tx * LANE_SPEED * 0.8f * DT;
    s.x = L.ax + ux * s.ty;
    s.y = L.ay + uy * s.ty;
    s.vx = ux * s.tx * LANE_SPEED * 0.8f;
    s.vy = uy * s.tx * LANE_SPEED * 0.8f;
    s.ang = angTo(s.vx, s.vy);
    s.thr = 1;
    if (s.ty < 0 || s.ty > len) removeShip(i);
    return;
  }
  case AI_INTRO:
    goTo(s, st, s.tx, s.ty, 1.0f);
    break;
  default:
    break;
  }
  moveShip(s, st, drag);
}

/* ========================================================== player flight */
void dropCruise(const char *why) {
  if (cruise == CR_OFF) return;
  cruise = CR_OFF;
  if (why) toast("%s", why);
}

static float laneLen(int k) {
  const LaneDef &L = LANES[k];
  return lenf(L.bx - L.ax, L.by - L.ay);
}

int nearRing(float x, float y, float *pos) {
  for (int k = 0; k < NLANE; k++) {
    if (!laneOpen[k]) continue;
    const LaneDef &L = LANES[k];
    float len = laneLen(k), ux = (L.bx - L.ax) / len, uy = (L.by - L.ay) / len;
    float t = (x - L.ax) * ux + (y - L.ay) * uy;
    int n = (int)(len / RING_GAP + 0.5f);
    float gap = len / n;
    int ri = (int)floorf(t / gap + 0.5f);
    if (ri < 0 || ri > n) continue;
    float rp = ri * gap;
    if (dist2(x, y, L.ax + ux * rp, L.ay + uy * rp) < 90 * 90) {
      if (pos) *pos = rp;
      return k;
    }
  }
  return -1;
}

void enterLane(int k, float pos, float dir) {
  Ship &p = ships[PL];
  lr.on = true;
  lr.lane = (int8_t)k;
  lr.len = laneLen(k);
  lr.pos = pos;
  lr.dir = dir;
  lr.spd = maxf(lenf(p.vx, p.vy), 60);
  lr.dropped = false;
  lr.dropAt = -1;
  cruise = CR_OFF;
  if (mis > MS_LANE && frand() < 0.08f * cm.laneSafe * (k == 3 ? 2.0f : 1.0f))
    lr.dropAt = frange(0.3f, 0.7f);
  storyOnLane(k, -1);
  toast("ENTERING %s", LANES[k].name);
  if (irand(2) == 0) crewBark(BK_LANE);
}

static void exitLane(bool disrupted) {
  Ship &p = ships[PL];
  const LaneDef &L = LANES[lr.lane];
  float ux = (L.bx - L.ax) / lr.len, uy = (L.by - L.ay) / lr.len;
  lr.on = false;
  p.vx = ux * lr.dir * 110;
  p.vy = uy * lr.dir * 110;
  if (disrupted) {
    ap.on = false;
    shake = 7;
    spawnPart(p.x, p.y, 3, 0, 18, C_O4, PK_RING);
    toast("LANE DISRUPTED!");
    if (!lr.dropped) spawnGroup(ENC_AMBUSH, p.x, p.y);
  } else {
    toast("LANE EXIT");
    if (ap.on && ap.i < ap.n) {
      /* autopilot continues from here */
    }
  }
}

void leaveLane() {
  if (!lr.on) return;
  ap.on = false;
  exitLane(false);
}

/* autopilot route: Dijkstra over start, goal and the lane endpoints */
bool planRoute(float gx, float gy, int destSta) {
  Ship &p = ships[PL];
  const int N = 2 + 2 * NLANE;
  float nx[N], ny[N];
  nx[0] = p.x; ny[0] = p.y;
  nx[1] = gx; ny[1] = gy;
  for (int k = 0; k < NLANE; k++) {
    nx[2 + 2 * k] = LANES[k].ax; ny[2 + 2 * k] = LANES[k].ay;
    nx[3 + 2 * k] = LANES[k].bx; ny[3 + 2 * k] = LANES[k].by;
  }
  float cs = cruiseSpeed(), ch = chargeTime();
  auto flyCost = [&](int a, int b) {
    float d = lenf(nx[a] - nx[b], ny[a] - ny[b]);
    return d < 700 ? d / 90.0f : d / cs + ch + 2.0f;
  };
  float dist[N];
  int prev[N];
  bool done[N] = {};
  for (int i = 0; i < N; i++) { dist[i] = 1e30f; prev[i] = -1; }
  dist[0] = 0;
  for (int it = 0; it < N; it++) {
    int u = -1;
    for (int i = 0; i < N; i++)
      if (!done[i] && (u < 0 || dist[i] < dist[u])) u = i;
    if (u < 0 || dist[u] >= 1e29f) break;
    done[u] = true;
    for (int v = 0; v < N; v++) {
      if (v == u || done[v]) continue;
      if (v >= 2 && !laneOpen[(v - 2) / 2] ) continue;
      if (u >= 2 && !laneOpen[(u - 2) / 2] ) continue;
      float c = flyCost(u, v);
      if (u >= 2 && v >= 2 && (u - 2) / 2 == (v - 2) / 2)
        c = laneLen((u - 2) / 2) / (LANE_SPEED * 0.8f) + 3.0f;
      if (dist[u] + c < dist[v]) { dist[v] = dist[u] + c; prev[v] = u; }
    }
  }
  if (prev[1] < 0) return false;
  int path[N + 1], np = 0;
  for (int v = 1; v != -1 && np < N; v = prev[v]) path[np++] = v;
  /* reverse: path[np-1] == 0 */
  ap.n = 0;
  for (int k = np - 2; k >= 0 && ap.n < 8; k--) {
    int v = path[k];
    int next = k > 0 ? path[k - 1] : -1;
    int lane = -1;
    if (v >= 2 && next >= 2 && (v - 2) / 2 == (next - 2) / 2) lane = (v - 2) / 2;
    /* sun avoidance for the flight leg into this node */
    int from = path[k + 1];
    bool laneLeg = from >= 2 && v >= 2 && (from - 2) / 2 == (v - 2) / 2;
    if (!laneLeg && ap.n < 7) {
      float ax = nx[from], ay = ny[from], bx = nx[v], by = ny[v];
      float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
      float t = l2 > 0 ? clampf(-(ax * dx + ay * dy) / l2, 0, 1) : 0;
      float cx = ax + dx * t, cy = ay + dy * t, cd = lenf(cx, cy);
      if (cd < 450) {
        float ox = cd > 1 ? cx / cd : -dy, oy = cd > 1 ? cy / cd : dx;
        float ol = lenf(ox, oy);
        ap.px[ap.n] = ox / ol * 750;
        ap.py[ap.n] = oy / ol * 750;
        ap.lane[ap.n] = -1;
        ap.n++;
      }
    }
    ap.px[ap.n] = nx[v];
    ap.py[ap.n] = ny[v];
    ap.lane[ap.n] = (int8_t)lane;
    ap.n++;
  }
  ap.i = 0;
  ap.dest = destSta;
  ap.on = ap.n > 0;
  return ap.on;
}

static bool hostilesNear(float r) {
  Ship &p = ships[PL];
  for (int j = 1; j < MAXSHIP; j++)
    if (ships[j].on && (ships[j].flags & SF_HOSTILE) && dist2(p.x, p.y, ships[j].x, ships[j].y) < r * r)
      return true;
  return false;
}

static void autopilotSteer(float &des, float &thr) {
  Ship &p = ships[PL];
  if (ap.i >= ap.n) {
    ap.on = false;
    return;
  }
  float tx = ap.px[ap.i], ty = ap.py[ap.i];
  float dx = tx - p.x, dy = ty - p.y, d = lenf(dx, dy);
  bool last = ap.i == ap.n - 1;
  float arrive = ap.lane[ap.i] >= 0 ? 60.0f : (last && ap.dest >= 0 ? stationDockRange(ap.dest) * 0.8f : 70.0f);
  des = angTo(dx, dy);
  thr = 1;
  bool aligned = fabsf(wrapAng(des - p.ang)) < 0.3f;
  if (d > 900 && cruise == CR_OFF && aligned && !hostilesNear(700)) {
    cruise = CR_CHARGE;
    cruiseT = 0;
  }
  if (cruise != CR_OFF && d < 320 + lenf(p.vx, p.vy) * 0.7f) cruise = CR_OFF;
  if (d < 160) thr = clampf(d / 160, 0.35f, 1);
  if (d < arrive) {
    if (ap.lane[ap.i] >= 0) {
      int k = ap.lane[ap.i];
      bool atA = dist2(p.x, p.y, LANES[k].ax, LANES[k].ay) < dist2(p.x, p.y, LANES[k].bx, LANES[k].by);
      ap.i++;
      enterLane(k, atA ? 0 : laneLen(k), atA ? 1.0f : -1.0f);
    } else if (last) {
      ap.on = false;
      if (ap.dest >= 0) dockAt(ap.dest);
      else toast("ARRIVED AT WAYPOINT");
    } else {
      ap.i++;
    }
  }
}

static void laneUpdate() {
  Ship &p = ships[PL];
  const LaneDef &L = LANES[lr.lane];
  float ux = (L.bx - L.ax) / lr.len, uy = (L.by - L.ay) / lr.len;
  float remain = lr.dir > 0 ? lr.len - lr.pos : lr.pos;
  const float decel = 1500;
  if (remain <= lr.spd * lr.spd / (2 * decel) + 5) lr.spd = maxf(90.0f, lr.spd - decel * DT);
  else lr.spd = minf(LANE_SPEED, lr.spd + 1700 * DT);
  lr.pos += lr.dir * lr.spd * DT;
  p.x = L.ax + ux * lr.pos;
  p.y = L.ay + uy * lr.pos;
  p.vx = ux * lr.dir * lr.spd;
  p.vy = uy * lr.dir * lr.spd;
  p.ang = angTo(p.vx, p.vy);
  p.thr = 1;
  float frac = lr.pos / lr.len;
  if (lr.dir < 0) frac = 1 - frac;
  storyOnLane(lr.lane, frac);
  if (!lr.on) return;
  if (lr.dropAt > 0 && frac >= lr.dropAt) {
    exitLane(true);
    return;
  }
  if ((lr.dir > 0 && lr.pos >= lr.len) || (lr.dir < 0 && lr.pos <= 0)) {
    lr.pos = clampf(lr.pos, 0, lr.len);
    exitLane(false);
  }
}

static void playerUpdate() {
  Ship &p = ships[PL];
  const Stats &st = designs[D_PLAYER].st;
  if (lr.on) {
    laneUpdate();
    return;
  }
  float des = p.ang, thr = 0;
  bool brake = false;
  if (steering) {
    if (ap.on) {
      ap.on = false;
      toast("AUTOPILOT OFF");
    }
    float psx = p.x - camX + CX, psy = p.y - camY + CY;
    float dx = inX - psx, dy = inY - psy, d = lenf(dx, dy);
    if (d < 14) brake = true;
    else {
      des = angTo(dx, dy);
      thr = clampf((d - 14) / 55.0f, 0.3f, 1.0f);
    }
  } else if (ap.on) {
    autopilotSteer(des, thr);
    if (lr.on) return;
  }
  if (cruise == CR_CHARGE) {
    cruiseT += DT;
    if (cruiseT >= chargeTime()) {
      cruise = CR_ON;
      toast("CRUISE ENGAGED");
    }
  }
  float turn = st.turn * (cruise == CR_ON ? 0.5f : 1.0f);
  float da = wrapAng(des - p.ang), mt = turn * DT;
  p.ang = wrapAng(p.ang + clampf(da, -mt, mt));
  float fx = sinf(p.ang), fy = -cosf(p.ang);
  if (cruise == CR_ON) {
    if (brake) cruise = CR_OFF;
    float cs = cruiseSpeed();
    p.vx += (fx * cs - p.vx) * 1.6f * DT;
    p.vy += (fy * cs - p.vy) * 1.6f * DT;
    p.thr = 1;
  } else {
    if (thr > 0 && st.thrust > 0) {
      p.vx += fx * st.accel * thr * DT;
      p.vy += fy * st.accel * thr * DT;
    }
    p.thr = thr;
    float drag = brake ? 3.5f : (thr > 0 ? 0.25f : 0.8f);
    p.vx *= 1 - drag * DT;
    p.vy *= 1 - drag * DT;
    float sp = lenf(p.vx, p.vy);
    if (sp > st.vmax && sp > 0) {
      float k = maxf(st.vmax / sp, 1 - 1.4f * DT);
      p.vx *= k;
      p.vy *= k;
    }
  }
  p.x += p.vx * DT;
  p.y += p.vy * DT;

  /* the sun is hot */
  float r = lenf(p.x, p.y);
  if (r < SUN_R + 130) {
    damageShip(PL, 22 * DT, -1);
    if (heatWarnT <= 0) {
      toast("HULL TEMPERATURE CRITICAL");
      heatWarnT = 3;
    }
  }
  /* asteroid bumps */
  if (r > BELT_R0 - 100 && r < BELT_R1 + 100) {
    int cx0 = (int)floorf(p.x / 90) - 1, cy0 = (int)floorf(p.y / 90) - 1;
    for (int cy = cy0; cy < cy0 + 3; cy++)
      for (int cx = cx0; cx < cx0 + 3; cx++) {
        uint32_t h = hash2(cx, cy, 77);
        float ax = cx * 90 + (h & 63) + 13, ay = cy * 90 + ((h >> 6) & 63) + 13;
        float ar = lenf(ax, ay);
        float dens = 1 - fabsf(ar - (BELT_R0 + BELT_R1) / 2) / ((BELT_R1 - BELT_R0) / 2);
        if (dens <= 0 || ((h >> 12) & 255) > dens * 110) continue;
        float rad = 4 + ((h >> 20) % 9);
        float dx = p.x - ax, dy = p.y - ay, d = lenf(dx, dy), md = rad + st.radius * 0.7f;
        if (d < md && d > 0.01f) {
          float nxx = dx / d, nyy = dy / d;
          float vn = p.vx * nxx + p.vy * nyy;
          if (vn < 0) {
            p.vx -= 1.6f * vn * nxx;
            p.vy -= 1.6f * vn * nyy;
            if (vn < -60) damageShip(PL, -vn * 0.08f, -1);
            for (int k = 0; k < 5; k++) spawnPart(ax + nxx * rad, ay + nyy * rad, frange(-30, 30), frange(-30, 30), 10, C_O3, PK_SPARK);
          }
          p.x = ax + nxx * md;
          p.y = ay + nyy * md;
        }
      }
  }
  if (r > 14500) {
    float k = 1 - 2 * DT;
    p.vx = p.vx * k - p.x / r * 20 * DT;
    p.vy = p.vy * k - p.y / r * 20 * DT;
    if (heatWarnT <= 0) {
      toast("EDGE OF THE REACH - TURN BACK");
      heatWarnT = 3;
    }
  }
}

/* ============================================================= docking */
void dockAt(int st) {
  Ship &p = ships[PL];
  cruise = CR_OFF;
  ap.on = false;
  lr.on = false;
  steering = false;
  mode = MD_STATION;
  dockedAt = st;
  lastStation = st;
  known |= (uint8_t)(1 << st);
  p.vx = p.vy = 0;
  p.sh = p.shMax;
  /* encounter traffic goes away while we are inside */
  for (int i = FIRST_DYN; i < MAXSHIP; i++)
    if (ships[i].on && !(ships[i].flags & SF_MISSION)) removeShip(i);
  for (Shot &s : shots) s.on = false;
  if (raid.on && !raid.story) raid.on = false;
  hailShip = -1;
  /* customs */
  if (STATIONS[st].fac == F_AUTH && cargo[CM_RELIC] > 0 && irand(100) < 40) {
    int n = cargo[CM_RELIC];
    cargo[CM_RELIC] = 0;
    int fine = 60 * n;
    credits = credits > fine ? credits - fine : 0;
    toast("CUSTOMS SEIZED %d RELICS, FINE %d", n, fine);
    rep[F_AUTH] -= 3;
  }
  genJobs(st);
  bool talk = jobOnDock(st);
  if (!storyOnDock(st) && !talk && irand(3) == 0) crewBark(BK_DOCK);
  saveGame();
}

void launchFrom(int st) {
  Ship &p = ships[PL];
  const StationDef &S = STATIONS[st];
  float ox = S.x, oy = S.y;
  /* launch away from the station's planet / the sun */
  float ax = S.x, ay = S.y;
  float al = lenf(ax, ay);
  float ang = al > 1 ? angTo(ax / al, ay / al) + 0.6f : 0;
  float r = stationStats[st].radius + 30;
  p.x = ox + sinf(ang) * r;
  p.y = oy - cosf(ang) * r;
  p.ang = ang;
  p.vx = sinf(ang) * 50;
  p.vy = -cosf(ang) * 50;
  p.on = true;
  camX = p.x;
  camY = p.y;
  mode = MD_FLIGHT;
  dockedAt = -1;
  cruise = CR_OFF;
  encT = maxf(encT, 25);
  storyOnLaunch();
  if (!raid.on && mis > MS_LANE && irand(100) < 10 && st != ST_ROOK && raidCooldown <= 0 &&
      countFlag(SF_MISSION) == 0)
    startRaid(st, 2 + irand(2), (mis >= MS_ECHOES && ending == 0 && irand(100) < 55) ? F_HUSH : F_SYND, false);
}

/* ================================================================ raids */
static void raidWave() {
  const StationDef &S = STATIONS[raid.sta];
  int n = 2 + raid.wave + (raid.story ? 1 : 0);
  float a = frand() * TAUF;
  float cx = S.x + sinf(a) * 620, cy = S.y - cosf(a) * 620;
  uint16_t fl = SF_RAID | SF_ATTACKER | SF_HOSTILE | (raid.story ? SF_MISSION : 0);
  for (int k = 0; k < n; k++) {
    int des;
    if (raid.fac == F_HUSH) {
      des = D_MITE;
      if (raid.wave >= 1 && k % 2 == 0) des = D_SHARD;
      if (raid.wave >= 2 && k == 0) des = D_HWARDEN;
    } else {
      des = (raid.wave >= 1 && k == 0) ? D_CORSAIR : D_RAIDER;
    }
    float ox = frange(-70, 70), oy = frange(-70, 70);
    int i = spawnShip(des, cx + ox, cy + oy, angTo(S.x - cx, S.y - cy), fl, AI_ATTACK);
    if (i >= 0) {
      ships[i].target = (int8_t)stationShip(raid.sta);
      if (raid.fac == F_HUSH) spawnPart(ships[i].x, ships[i].y, 1.5f, 0, 16, C_V4, PK_RING);
    }
  }
  raid.wave++;
  if (raid.waves > 1) toast("WAVE %d OF %d", raid.wave, raid.waves);
}

void startRaid(int st, int waves, uint8_t fac, bool story) {
  if (raid.on) return;
  raid.on = true;
  raid.story = story;
  raid.sta = (uint8_t)st;
  raid.fac = fac;
  raid.wave = 0;
  raid.waves = (uint8_t)waves;
  raid.pdmg = 0;
  raid.next = 2.5f;
  raid.t = 0;
  raidCooldown = 240;
  const StationDef &S = STATIONS[st];
  int ndef = story ? 3 : 2;
  for (int k = 0; k < ndef; k++) {
    int des = S.fac == F_THREN ? D_WASP : (S.fac == F_SYND ? D_RAIDER : (k == 0 ? D_WARDEN : D_LANCER));
    float a = frand() * TAUF;
    int i = spawnShip(des, S.x + cosf(a) * 70, S.y + sinf(a) * 70, a,
                      SF_DEFENDER | SF_RAID | (story ? SF_MISSION : 0), AI_DEFEND);
    if (i >= 0) ships[i].anchor = (int8_t)stationShip(st);
  }
  bark(SPK_CTRL, -1, fmt("ALL PILOTS, ALL PILOTS: %s IS UNDER ATTACK BY %s! DEFENDERS LAUNCHING.",
                         S.name, fac == F_HUSH ? "UNKNOWN SHIPS" : "SYNDICATE RAIDERS"));
  if (fac == F_HUSH) crewBark(BK_HUSH);
}

static void endRaid(bool won) {
  bool story = raid.story;
  raid.on = false;
  raidWon = won;
  for (int i = FIRST_DYN; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on || !(s.flags & SF_RAID)) continue;
    if (s.flags & SF_DEFENDER) {
      s.ai = AI_TRAVEL;
      s.tx = STATIONS[raid.sta].x;
      s.ty = STATIONS[raid.sta].y;
      s.flags &= (uint16_t)~SF_RAID;
    } else {
      removeShip(i);
    }
  }
  if (won && !story && raid.pdmg > 40) {
    int reward = 220 * raid.waves;
    credits += reward;
    rep[STATIONS[raid.sta].fac] += 6;
    bark(SPK_CTRL, -1, fmt("ALL HOSTILES DOWN. %s THANKS YOU, CAPTAIN. %d CREDITS TRANSFERRED.",
                           STATIONS[raid.sta].shortName, reward));
  } else if (won && !story) {
    bark(SPK_CTRL, -1, "ALL HOSTILES DOWN. STAND DOWN, DEFENDERS.");
  }
  Ship &st = ships[stationShip(raid.sta)];
  if (st.hp <= 0) st.hp = 1;
  if (story) storyOnRaidEnd(won);
}

void raidTick() {
  raidCooldown -= DT;
  Ship &p = ships[PL];
  if (!raid.on) {
    /* random raids near friendly stations */
    if (mis <= MS_LANE || raidCooldown > 0 || lr.on) return;
    if ((frameNo % 300) != 0) return;
    float d;
    int s = nearestStation(p.x, p.y, &d);
    if (d > 1800 || s == ST_ROOK || stationHostile(s)) return;
    if (irand(100) < 22) startRaid(s, 2 + irand(2), (mis >= MS_ECHOES && ending == 0 && irand(100) < 55) ? F_HUSH : F_SYND, false);
    else raidCooldown = 30;
    return;
  }
  raid.t += DT;
  const StationDef &S = STATIONS[raid.sta];
  if (!raid.story && dist2(p.x, p.y, S.x, S.y) > 3500.0f * 3500.0f) {
    endRaid(false);
    return;
  }
  int alive = 0;
  for (int i = FIRST_DYN; i < MAXSHIP; i++)
    if (ships[i].on && (ships[i].flags & SF_RAID) && (ships[i].flags & SF_ATTACKER)) alive++;
  if (alive == 0) {
    raid.next -= DT;
    if (raid.next <= 0) {
      if (raid.wave < raid.waves) {
        raidWave();
        raid.next = 3.5f;
      } else {
        endRaid(true);
      }
    }
  }
}

/* ============================================================ encounters */
void spawnGroup(int type, float x, float y) {
  Ship &p = ships[PL];
  uint8_t g = groupSeq++;
  if (!groupSeq) groupSeq = 1;
  auto put = [&](int des, float ox, float oy, uint16_t fl, uint8_t ai) {
    int i = spawnShip(des, x + ox, y + oy, angTo(p.x - x, p.y - y), fl | SF_ENC, ai);
    if (i >= 0) {
      ships[i].group = g;
      if (ai == AI_ATTACK) ships[i].target = PL;
    }
    return i;
  };
  switch (type) {
  case ENC_PIRATE: {
    bool friendly = rep[F_SYND] >= 40;
    bool ambush = !friendly && irand(100) < 35;
    int n = 1 + irand(2) + (mis >= MS_SABLE ? irand(2) : 0);
    for (int k = 0; k < n; k++) {
      int des = (k == 0 && mis >= MS_SABLE && irand(100) < 40) ? D_CORSAIR : D_RAIDER;
      uint16_t fl = ambush ? (SF_HOSTILE | SF_ATTACKER) : (k == 0 ? SF_HAIL : 0);
      put(des, k * 30.0f, k * 20.0f, fl, ambush ? AI_ATTACK : AI_HAIL);
    }
    if (ambush) {
      toast("SYNDICATE RAIDERS!");
      crewBark(BK_COMBAT);
    }
    break;
  }
  case ENC_TRADER:
    put(D_HAULER, 0, 0, SF_HAIL | SF_TRADER, AI_HAIL);
    if (irand(2)) put(D_WASP, 30, 10, SF_TRADER, AI_HAIL);
    break;
  case ENC_PATROL: {
    bool hostile = rep[F_AUTH] < -30;
    put(D_LANCER, 0, 0, hostile ? SF_HOSTILE : SF_HAIL, hostile ? AI_ATTACK : AI_HAIL);
    put(mis >= MS_SABLE && irand(2) ? D_WARDEN : D_LANCER, 30, 0, hostile ? SF_HOSTILE : 0,
        hostile ? AI_ATTACK : AI_HAIL);
    break;
  }
  case ENC_HUSH: {
    if (ending == 1) break;
    int n = 2 + irand(2) + (mis >= MS_MERIDIAN ? 1 : 0);
    for (int k = 0; k < n; k++) {
      int des = D_MITE;
      if (mis >= MS_SABLE && k % 2) des = D_SHARD;
      if (mis >= MS_ABBEY && k == 0 && irand(100) < 35) des = D_HWARDEN;
      float a = k * 1.3f;
      int i = put(des, cosf(a) * 40, sinf(a) * 40, ending == 2 ? 0 : SF_HOSTILE,
                  ending == 2 ? AI_PATROL : AI_ATTACK);
      if (i >= 0) spawnPart(ships[i].x, ships[i].y, 1.5f, 0, 16, C_V4, PK_RING);
    }
    if (ending != 2) {
      toast("CONTACT: UNKNOWN SIGNATURES");
      crewBark(BK_HUSH);
    }
    break;
  }
  case ENC_DERELICT: {
    static const uint8_t wr[4] = {D_HAULER, D_CORSAIR, D_LANCER, D_WARDEN};
    int i = put(wr[irand(4)], 0, 0, 0, AI_DRIFT);
    if (i >= 0) {
      ships[i].kind = K_WRECK;
      ships[i].ang = frand() * TAUF;
      ships[i].vx = frange(-5, 5);
      ships[i].vy = frange(-5, 5);
    }
    toast("DERELICT ON SCOPE");
    break;
  }
  case ENC_DISTRESS: {
    int h = put(D_HAULER, 0, 0, SF_DISTRESS, AI_TRAVEL);
    if (h >= 0) {
      float d;
      int s = nearestStation(x, y, &d);
      ships[h].tx = STATIONS[s].x;
      ships[h].ty = STATIONS[s].y;
    }
    for (int k = 0; k < 2; k++) {
      int i = put(D_RAIDER, 70 + k * 30.0f, -40, SF_HOSTILE | SF_ATTACKER, AI_ATTACK);
      if (i >= 0) ships[i].target = (int8_t)h;
    }
    bark(SPK_TRADER, -1, "MAYDAY! MAYDAY! THRENNIC HAULER UNDER ATTACK! ANY CAPTAIN, PLEASE!");
    break;
  }
  case ENC_AMBUSH: {
    bool hush = mis >= MS_ECHOES && irand(100) < 40 && ending == 0;
    int n = 2 + irand(2);
    for (int k = 0; k < n; k++) {
      float a = frand() * TAUF;
      float ox = cosf(a) * 230 - (x - p.x), oy = sinf(a) * 230 - (y - p.y);
      int i = put(hush ? (k ? D_MITE : D_SHARD) : D_RAIDER, ox, oy,
                  SF_HOSTILE | SF_ATTACKER, AI_ATTACK);
      if (i >= 0 && hush) spawnPart(ships[i].x, ships[i].y, 1.5f, 0, 16, C_V4, PK_RING);
    }
    crewBark(hush ? BK_HUSH : BK_COMBAT);
    break;
  }
  }
}

static float hushWarnT = -1;

void encounterTick() {
  Ship &p = ships[PL];
  if (barkCool > 0) barkCool -= DT;
  if (heatWarnT > 0) heatWarnT -= DT;
  /* despawn far traffic */
  for (int i = FIRST_DYN; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on || (s.flags & (SF_MISSION | SF_RAID | SF_JOB))) continue;
    if (dist2(s.x, s.y, p.x, p.y) > 2600.0f * 2600.0f) removeShip(i);
  }
  /* distress resolution */
  for (int i = FIRST_DYN; i < MAXSHIP; i++) {
    Ship &h = ships[i];
    if (!h.on || !(h.flags & SF_DISTRESS)) continue;
    int foes = 0;
    for (int j = FIRST_DYN; j < MAXSHIP; j++)
      if (ships[j].on && ships[j].group == h.group && (ships[j].flags & SF_ATTACKER)) foes++;
    if (foes == 0 && !dlg.on && mode == MD_FLIGHT) {
      h.flags &= (uint16_t)~SF_DISTRESS;
      h.flags |= SF_TRADER;
      hailShip = i;
      dlgBegin();
      dlgLine(SPK_TRADER, "THE GUILDS WILL REMEMBER THIS, CAPTAIN. TAKE THIS - IT IS NOT A GIFT, IT IS A DEBT REPAID.");
      dlgOpt("ACCEPT 250 CR", 1);
      dlgRun(DT_DISTRESS_DONE);
    }
  }
  if (hushWarnT > 0) {
    hushWarnT -= DT;
    if (hushWarnT <= 0) {
      float a = p.ang + frange(-1, 1);
      spawnGroup(ENC_HUSH, p.x + sinf(a) * 380, p.y - cosf(a) * 380);
    }
  }
  if (lr.on || raid.on || mode != MD_FLIGHT) return;
  float d;
  nearestStation(p.x, p.y, &d);
  if (d < 1100) {
    encT = maxf(encT, 12);
    return;
  }
  encT -= DT * (cruise == CR_ON ? 1.6f : 1.0f);
  if (encT > 0) return;
  encT = frange(32, 65);
  if (countFlag(SF_ENC) > 3 || countFlag(SF_MISSION) > 0 || countFlag(SF_JOB) > 0) return;
  int z = zoneAt(p.x, p.y);
  /* weights: pirate trader patrol hush derelict distress */
  static const uint8_t W[5][6] = {
      {15, 40, 35, 0, 5, 5},   /* safe */
      {30, 25, 15, 10, 10, 10}, /* open */
      {40, 5, 0, 15, 30, 10},  /* belt */
      {20, 20, 0, 30, 30, 0},  /* nebula */
      {20, 0, 5, 45, 30, 0},   /* deep */
  };
  int w[6];
  int tot = 0;
  for (int k = 0; k < 6; k++) {
    w[k] = W[z][k];
    if (k == 3 && (mis < MS_ECHOES || ending == 1)) w[k] = 0;
    tot += w[k];
  }
  int r = irand(tot), type = 0;
  for (; type < 5; type++) {
    if (r < w[type]) break;
    r -= w[type];
  }
  static const uint8_t MAP[6] = {ENC_PIRATE, ENC_TRADER, ENC_PATROL, ENC_HUSH, ENC_DERELICT, ENC_DISTRESS};
  int enc = MAP[type];
  if (enc == ENC_HUSH && cm.seer && ending != 2) {
    int c = crewOfSpecies(SP_LUMENI);
    bark(SPK_CREWA, c, "CAPTAIN... SOMETHING IS COMING. A HOLE IN THE LIGHT, DEAD AHEAD.");
    hushWarnT = 3.5f;
    return;
  }
  float a = p.ang + frange(-1.1f, 1.1f);
  float dist = enc == ENC_DERELICT ? 420.0f : 540.0f;
  spawnGroup(enc, p.x + sinf(a) * dist, p.y - cosf(a) * dist);
}

/* ambient trade lane traffic: freighters streaking along lanes */
static void laneTraffic() {
  trafficT -= DT;
  if (trafficT > 0) return;
  trafficT = frange(6, 14);
  Ship &p = ships[PL];
  for (int k = 0; k < NLANE; k++) {
    if (!laneOpen[k]) continue;
    const LaneDef &L = LANES[k];
    float len = laneLen(k), ux = (L.bx - L.ax) / len, uy = (L.by - L.ay) / len;
    float t = (p.x - L.ax) * ux + (p.y - L.ay) * uy;
    float cx = L.ax + ux * t, cy = L.ay + uy * t;
    if (t < 0 || t > len || dist2(p.x, p.y, cx, cy) > 900.0f * 900.0f) continue;
    float dir = irand(2) ? 1.0f : -1.0f;
    float start = clampf(t - dir * 1400, 0, len);
    int i = spawnShip(irand(3) ? D_HAULER : D_LANCER, L.ax + ux * start, L.ay + uy * start, 0, SF_LANE, AI_LANE);
    if (i >= 0) {
      ships[i].sub = (uint8_t)k;
      ships[i].tx = dir;
      ships[i].ty = start;
    }
    return;
  }
}

/* ================================================================ update */
void worldUpdate() {
  gameTime += DT;
  Ship &p = ships[PL];
  if (mode == MD_FLIGHT) playerUpdate();
  for (int i = 1; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on) continue;
    if (s.kind == K_STATION) {
      s.flags = stationHostile(s.sta) ? SF_HOSTILE : 0;
      continue;
    }
    aiShip(i);
  }
  for (int i = 0; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on) continue;
    const Stats &st = statsOf(s);
    if (s.hitT > 0) s.hitT -= DT;
    if (s.shT > 0) s.shT -= DT;
    if (s.regenT > 0) s.regenT -= DT;
    else if (s.sh < s.shMax) {
      float rg = st.regen * (i == PL ? playerEff() * cm.regen : 1.0f);
      s.sh = minf(s.shMax, s.sh + rg * DT);
    }
    if (i == PL) {
      float rp = st.repair * playerEff() + cm.repair;
      if (rp > 0 && s.hp < s.hpMax && s.hp > 0) s.hp = minf(s.hpMax, s.hp + rp * DT);
    }
    if (s.kind == K_STATION) {
      if (s.hp <= 0) {
        if (s.hp < -999) s.hp = 0;
        if (!raid.on || raid.sta != s.sta) s.hp += 25 * DT;
        if (frameNo % 6 == 0) spawnPart(s.x + frange(-20, 20), s.y + frange(-20, 20), frange(-8, 8), frange(-8, 8), 30, C_G3, PK_SMOKE);
      } else if (s.hp < s.hpMax) {
        s.hp = minf(s.hpMax, s.hp + 4 * DT);
      }
    }
    fireWeapons(i);
  }
  updateShots();
  updateParts();
  if (mode == MD_FLIGHT) {
    encounterTick();
    raidTick();
    laneTraffic();
  }
  /* camera */
  float look = lr.on ? 0.06f : 0.35f;
  float lx = clampf(p.vx * look, -55, 55), ly = clampf(p.vy * look, -55, 55);
  float k = minf(1.0f, (lr.on ? 10.0f : 5.0f) * DT);
  camX += (p.x + lx - camX) * k;
  camY += (p.y + ly - camY) * k;
  if (fabsf(p.x - camX) > 200 || fabsf(p.y - camY) > 200) {
    camX = p.x;
    camY = p.y;
  }
  shake *= 0.86f;
  if (shake < 0.2f) shake = 0;
  if (toastT > 0) toastT -= DT;
  if (bk.t > 0) bk.t -= DT;
}

void worldReset() {
  for (int i = 0; i < MAXSHIP; i++) ships[i].on = false;
  for (Shot &s : shots) s.on = false;
  for (Part &p : parts) p.life = 0;
  for (int s = 0; s < ST_COUNT; s++) {
    Ship &sh = ships[stationShip(s)];
    memset(&sh, 0, sizeof(sh));
    sh.on = true;
    sh.kind = K_STATION;
    sh.sta = (uint8_t)s;
    sh.fac = STATIONS[s].fac;
    sh.x = STATIONS[s].x;
    sh.y = STATIONS[s].y;
    sh.hp = sh.hpMax = stationStats[s].hp;
    sh.sh = sh.shMax = stationStats[s].shield;
    sh.target = -1;
    sh.anchor = -1;
  }
  Ship &p = ships[PL];
  p.on = true;
  p.kind = K_SHIP;
  p.des = D_PLAYER;
  p.fac = F_PLAYER;
  p.target = -1;
  raid.on = false;
  lr.on = false;
  ap.on = false;
  cruise = CR_OFF;
  hailShip = -1;
  bk.t = 0;
  toastT = 0;
  hushWarnT = -1;
}

void buildRenderCaches();

void worldInit() {
  for (int d = 0; d < D_PLAYER; d++) {
    designs[d].fac = DSRC[d].fac;
    designs[d].name = DSRC[d].name;
    designs[d].hpMul = DSRC[d].hpMul;
    gridFromSrc(designs[d].g, DSRC[d]);
    compileDesign(designs[d], FACS[DSRC[d].fac].role);
  }
  designs[D_PLAYER].fac = F_PLAYER;
  designs[D_PLAYER].name = "YOUR SHIP";
  designs[D_PLAYER].hpMul = 1.0f;
  for (int s = 0; s < ST_COUNT; s++) {
    Stats &st = stationStats[s];
    memset(&st, 0, sizeof(st));
    st.hp = STATIONS[s].hp;
    st.shield = 600;
    st.regen = 15;
    st.radius = 50;
    st.mass = 1000;
    st.eff = 1;
    st.radar = 1000;
    int n = s == ST_ROOK ? 8 : (s == ST_LANTERN ? 4 : 6);
    for (int k = 0; k < n; k++) {
      float a = k * TAUF / n;
      Mount &m = st.mount[st.nMount++];
      m.w = (k & 1) ? W_TURRET : W_STATION;
      m.dir = 0;
      m.lx = (int8_t)(cosf(a) * 36);
      m.ly = (int8_t)(sinf(a) * 36);
    }
  }
  buildRenderCaches();
  worldReset();
}

} // namespace sl
