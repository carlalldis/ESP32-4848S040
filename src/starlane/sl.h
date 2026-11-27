/* STARLANE - internal shared header (game core only). */
#pragma once
#include "sl_api.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sl {

constexpr float DT = 1.0f / 30.0f;
constexpr float PIF = 3.14159265f;
constexpr float TAUF = 6.28318531f;
constexpr uint8_t TRANSP = 255;
constexpr int CX = LW / 2, CY = LH / 2;

/* ================================================================ palette */
enum : uint8_t {
  C_SPACE, C_BLACK, C_G1, C_G2, C_G3, C_G4, C_G5, C_WHITE,
  C_B1, C_B2, C_B3, C_B4, C_B5,
  C_GR1, C_GR2, C_GR3, C_GR4,
  C_O1, C_O2, C_O3, C_O4, C_O5,
  C_Y1, C_Y2, C_Y3,
  C_R1, C_R2, C_R3, C_R4,
  C_V1, C_V2, C_V3, C_V4, C_V5,
  C_T1, C_T2, C_T3, C_T4,
  C_GD1, C_GD2, C_PK,
  C_UIBG, C_UIBG2, C_UILN, C_UIHI,
  C_COUNT
};

/* ============================================================ math / rng */
extern uint32_t rngState;
inline uint32_t rnd() {
  uint32_t x = rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return rngState = x;
}
inline float frand() { return (rnd() >> 8) * (1.0f / 16777216.0f); }
inline float frange(float a, float b) { return a + (b - a) * frand(); }
inline int irand(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }
inline uint32_t hash2(int x, int y, uint32_t s = 0) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
inline float hashf(int x, int y, uint32_t s = 0) { return (hash2(x, y, s) >> 8) * (1.0f / 16777216.0f); }
inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }
inline float wrapAng(float a) {
  while (a > PIF) a -= TAUF;
  while (a < -PIF) a += TAUF;
  return a;
}
inline float lenf(float x, float y) { return sqrtf(x * x + y * y); }
inline float dist2(float ax, float ay, float bx, float by) {
  float dx = ax - bx, dy = ay - by;
  return dx * dx + dy * dy;
}
/* heading convention: angle 0 points up the screen (-y), clockwise positive */
inline float angTo(float dx, float dy) { return atan2f(dx, -dy); }
float vnoise(float x, float y, uint32_t seed);

/* ================================================================= gfx */
void clear(uint8_t c);
inline void pset(int x, int y, uint8_t c) {
  if ((unsigned)x < (unsigned)LW && (unsigned)y < (unsigned)LH) scr[y * LW + x] = c;
}
void hline(int x0, int x1, int y, uint8_t c);
void fillRect(int x, int y, int w, int h, uint8_t c);
void rect(int x, int y, int w, int h, uint8_t c);
void line(int x0, int y0, int x1, int y1, uint8_t c);
void dline(int x0, int y0, int x1, int y1, uint8_t c, int on, int off); /* dashed */
void fillCircle(int cx, int cy, int r, uint8_t c);
void circle(int cx, int cy, int r, uint8_t c);
void ditherCircle(int cx, int cy, int r, uint8_t c, int level); /* level 0..16 */
void ditherRect(int x, int y, int w, int h, uint8_t c, int level);
void fillTri(float x0, float y0, float x1, float y1, float x2, float y2, uint8_t c);
void fillQuad(float cx, float cy, float hw, float hh, float ang, uint8_t c);
extern const uint8_t BAYER[16];
inline int bayer(int x, int y) { return BAYER[(y & 3) * 4 + (x & 3)]; }
void darken(int level); /* full screen dither fade, 0..16 */

uint8_t charColor(char ch);
void drawSprite(int x, int y, int w, int h, const char *px, int scale = 1,
                const uint8_t *roles = nullptr, uint8_t tint = C_WHITE);
void blitRot(const uint8_t *spr, int stride, int sw, int sh, float px, float py,
             float sx, float sy, float ang, int R, int flash, float scale = 1.0f);
void blit(const uint8_t *bmp, int w, int h, int x, int y);

int textW(const char *t, int s = 1);
void text(int x, int y, const char *t, uint8_t c, int s = 1);
void textC(int cx, int y, const char *t, uint8_t c, int s = 1);
void textR(int rx, int y, const char *t, uint8_t c, int s = 1);
void textSh(int x, int y, const char *t, uint8_t c, int s = 1);
void textShC(int cx, int y, const char *t, uint8_t c, int s = 1);
int wrapText(int x, int y, int wChars, const char *t, uint8_t c, int maxChars = 1 << 20,
             int lineH = 9, int maxLines = 99);
const char *fmt(const char *f, ...);

/* immediate-mode input */
extern bool inDown, inTap;
extern int inX, inY;
extern bool tapEaten, uiLock;
bool tapIn(int x, int y, int w, int h);
bool hitIn(int x, int y, int w, int h);
void panel(int x, int y, int w, int h, uint8_t bg = C_UIBG, uint8_t ln = C_UILN);
bool button(int x, int y, int w, int h, const char *label, bool enabled = true,
            uint8_t accent = C_UIHI);
void bar(int x, int y, int w, int h, float f, uint8_t fg, uint8_t bg);
void drawPortrait(int x, int y, int port, uint8_t tint, int scale, bool glitch = false);

/* ============================================================== modules */
enum : uint8_t {
  M_NONE, M_COCKPIT, M_HULL, M_ARMOR, M_REACTOR, M_ENGINE, M_SHIELD, M_CARGO,
  M_QUARTERS, M_LASER, M_CANNON, M_MISSILE, M_TURRET, M_SCANNER, M_REPAIR,
  M_CRUISE, M_GYRO, M_COUNT
};
struct ModDef {
  const char *name, *desc;
  uint16_t price;
  uint8_t mass, hp;
  int8_t power;
  const char *icon; /* 8x8 */
  const char *tex;  /* 4x4, barrel/front up */
};
extern const ModDef MODS[M_COUNT];

enum : uint8_t { W_LASER, W_CANNON, W_MISSILE, W_TURRET, W_STATION, W_COUNT };
struct WeapDef {
  float dmg, cd, range, speed, arc;
};
extern const WeapDef WEAPS[W_COUNT];
int modWeapon(uint8_t m);

constexpr int GW = 7, GH = 7, MPX = 4, SPR = GW * MPX;
constexpr float SHIP_SCALE = 2.0f; /* ships are drawn at 2x their sprite */
struct Grid {
  uint8_t m[GH][GW];
  uint8_t dir[GH][GW]; /* weapons: 0 fwd, 1 right, 2 back, 3 left */
};
struct Mount {
  uint8_t w, dir;
  int8_t lx, ly; /* px from centre of mass; +x right, +y back */
};
struct Stats {
  float mass, thrust, accel, vmax, turn;
  float hp, shield, regen, repair, cruiseMul, radar, eff, radius;
  int powGen, powUse;
  int cargo, crew, nMount, nEng;
  int blockedEng, blockedWeap;
  Mount mount[20];
  int8_t engX[12], engY[12];
  float comX, comY;
};
enum : uint8_t {
  D_LANCER, D_WARDEN, D_RAIDER, D_CORSAIR, D_HAULER, D_WASP, D_MITE, D_SHARD,
  D_HWARDEN, D_CHOIR, D_DRONE, D_COURIER, D_PLAYER, D_COUNT
};
struct Design {
  Grid g;
  Stats st;
  uint8_t fac;
  const char *name;
  float hpMul;
  uint8_t spr[SPR * SPR];
};
struct DesignSrc {
  const char *name;
  uint8_t fac, w, h;
  const char *cells;
  float hpMul;
};
extern const DesignSrc DSRC[D_PLAYER];
extern Design designs[D_COUNT];
void gridFromSrc(Grid &g, const DesignSrc &s);
void compileDesign(Design &d, const uint8_t *roles);
bool weaponBlocked(const Grid &g, int x, int y);
bool engineBlocked(const Grid &g, int x, int y);
int autoDir(const Grid &g, int x, int y);
bool gridConnected(const Grid &g);
int gridCount(const Grid &g, uint8_t m);

/* ============================================================== factions */
enum : uint8_t { F_PLAYER, F_AUTH, F_SYND, F_THREN, F_HUSH, F_COUNT };
struct FacDef {
  const char *name;
  uint8_t role[4];
  uint8_t shot;
};
extern const FacDef FACS[F_COUNT];
extern const uint8_t PAINTS[4][4];
extern const char *const PAINT_NAMES[4];
extern int rep[F_COUNT];

/* =========================================================== commodities */
constexpr int NCOM = 7;
enum : uint8_t { CM_ICE, CM_ORE, CM_ALLOY, CM_MED, CM_SPICE, CM_RELIC, CM_PARCEL };
struct ComDef {
  const char *name;
  uint16_t base;
  bool illegal;
};
extern const ComDef COMS[NCOM];

/* ============================================================== places */
enum : uint8_t { ST_HAVEN, ST_TOLLE, ST_MERIDIAN, ST_LANTERN, ST_ROOK, ST_FARPOINT, ST_COUNT };
struct StationDef {
  const char *name, *shortName;
  float x, y;
  uint8_t fac, style;
  int8_t price[NCOM]; /* percent modifier on base price */
  uint8_t shop[12];   /* module ids, 0 terminated */
  const char *blurb, *barkeep;
  const char *news[4];
  uint8_t recruit[2];
  float hp;
};
extern const StationDef STATIONS[ST_COUNT];

enum : uint8_t { P_TERRAN, P_GAS, P_ICE, P_ROCK, P_ASH };
struct PlanetDef {
  const char *name;
  float x, y;
  int r;
  uint8_t type;
};
constexpr int NPLANET = 5;
extern const PlanetDef PLANETS[NPLANET];

constexpr int NLANE = 4;
struct LaneDef {
  const char *name;
  float ax, ay, bx, by;
  uint8_t sa, sb;
};
extern const LaneDef LANES[NLANE];
extern bool laneOpen[NLANE];
constexpr float RING_GAP = 700.0f;
constexpr float LANE_SPEED = 2000.0f;

constexpr float SUN_R = 150.0f;
constexpr float BELT_R0 = 6800.0f, BELT_R1 = 7900.0f;
constexpr float NEB_X = -6500.0f, NEB_Y = -5000.0f, NEB_R = 2300.0f;
constexpr float GATE_X = 10400.0f, GATE_Y = 7900.0f, GATE_R = 130.0f;
constexpr float WRECK_X = -7000.0f, WRECK_Y = 1500.0f;
constexpr float NEST_X = 7000.0f, NEST_Y = -2100.0f;

/* ============================================================ species / crew */
enum : uint8_t {
  SP_HUMAN, SP_VELKU, SP_ORNN, SP_SYLITH, SP_GRASK, SP_QUIBBIT, SP_LUMENI,
  SP_THRENNIC, SP_HUSH, SP_NPORT
};
struct SpeciesDef {
  const char *name, *role, *bonus, *lore;
};
extern const SpeciesDef SPECIES[SP_NPORT];
extern const char *const PORTRAITS[SP_NPORT];
constexpr int NCREW = 12;
struct CrewDef {
  const char *name;
  uint8_t sp, tint;
  uint16_t hire;
  const char *bio;
};
extern const CrewDef CREW[NCREW];
enum : uint8_t { BK_COMBAT, BK_LANE, BK_HULL, BK_WIN, BK_HUSH, BK_DOCK, BK_COUNT };
extern const char *const BARKS[SP_NPORT][BK_COUNT];
constexpr int NBEAT = 9;
extern const char *const STORYCREW[SP_NPORT][NBEAT];

enum : uint8_t {
  SPK_NARR, SPK_YOU, SPK_VARROW, SPK_BRAKKA, SPK_SABLE, SPK_OSSK, SPK_NINEFOLD,
  SPK_HUSH, SPK_CTRL, SPK_PIRATE, SPK_TRADER, SPK_PATROL, SPK_CREWA, SPK_CREWB,
  SPK_COUNT
};
struct SpeakerDef {
  const char *name;
  uint8_t port, tint;
};
extern const SpeakerDef SPEAKERS[SPK_COUNT];
struct DLine {
  uint8_t spk;
  const char *text;
};
extern const char *const LORE_LOGS[8];
extern const char *const PIRATE_HAILS[4];
extern const char *const TRADER_HAILS[3];
extern const char *const INTRO_TEXT[];

/* ================================================================ state */
enum : uint8_t {
  MD_INTRO, MD_TITLE, MD_CREW, MD_FLIGHT, MD_STATION, MD_SHIPYARD, MD_TRADE,
  MD_JOBS, MD_BAR, MD_MAP, MD_MENU, MD_DEAD, MD_ENDING
};
extern uint8_t mode;
extern uint32_t frameNo;
extern float playTime, gameTime;

extern int credits;
extern int cargo[NCOM];
extern uint8_t inv[M_COUNT];
extern uint8_t paint;
extern uint8_t crew[6];
extern int nCrew;
extern int dockedAt, lastStation;
extern uint8_t known; /* bit per station, plus extras */
enum : uint8_t { KN_WRECK = 1 << 6, KN_NEST = 1 << 7 };
extern bool knownGate;
extern int kills;
extern float fade;

/* crew-derived player bonuses */
struct CrewMods {
  float cruise, charge, laneSafe, regen, dmg, rate, repair, repairCost, radar, tradeBuy,
      tradeSell;
  int power;
  bool nav, eng, envoy, gunner, mech, seer;
};
extern CrewMods cm;
void computeCrewMods();
bool hasSpecies(int sp);
int crewOfSpecies(int sp);

/* ================================================================ ships */
enum : uint8_t { K_SHIP, K_STATION, K_WRECK };
enum : uint16_t {
  SF_HOSTILE = 1, SF_MISSION = 2, SF_INVULN = 4, SF_DEFENDER = 8, SF_ATTACKER = 16,
  SF_HAIL = 32, SF_BOSS = 64, SF_TRADER = 128, SF_DISTRESS = 256, SF_SCANNED = 512,
  SF_DRONE = 1024, SF_LANE = 2048, SF_ENC = 4096, SF_RAID = 8192, SF_HAILED = 16384,
  SF_JOB = 32768
};
enum : uint8_t {
  AI_NONE, AI_PATROL, AI_ATTACK, AI_DEFEND, AI_TRAVEL, AI_HAIL, AI_FLEE, AI_DRIFT,
  AI_LANE, AI_INTRO, AI_HOLD
};
struct Ship {
  bool on;
  uint8_t kind, des, fac, ai, sta, group, sub;
  uint16_t flags;
  int8_t target, anchor;
  float x, y, vx, vy, ang;
  float hp, hpMax, sh, shMax;
  float cd[20];
  float thr, hitT, shT, aiT, tx, ty, regenT, pdmg, scan;
};
constexpr int MAXSHIP = 40, PL = 0, FIRST_DYN = 1 + ST_COUNT;
extern Ship ships[MAXSHIP];
extern Stats stationStats[ST_COUNT];
const Stats &statsOf(const Ship &s);
inline int stationShip(int st) { return 1 + st; }

struct Shot {
  bool on, fromPlayer;
  uint8_t w, fac;
  int8_t owner, target;
  uint16_t oflags;
  float x, y, vx, vy, life, dmg;
};
constexpr int MAXSHOT = 160;
extern Shot shots[MAXSHOT];

enum : uint8_t { PK_DOT, PK_SPARK, PK_SMOKE, PK_RING, PK_FLASH };
struct Part {
  float x, y, vx, vy;
  int16_t life, max;
  uint8_t col, kind;
};
constexpr int MAXPART = 320;
extern Part parts[MAXPART];

/* flight */
enum : uint8_t { CR_OFF, CR_CHARGE, CR_ON };
extern uint8_t cruise;
extern float cruiseT;
struct LaneRun {
  bool on;
  int8_t lane;
  float pos, len, dir, spd, dropAt;
  bool dropped;
};
extern LaneRun lr;
struct Autopilot {
  bool on;
  int n, i, dest;
  float px[8], py[8];
  int8_t lane[8];
};
extern Autopilot ap;
extern float camX, camY, shake;
extern bool steering;

struct Raid {
  bool on, story;
  uint8_t sta, fac, wave, waves;
  float t, pdmg, next;
};
extern Raid raid;
extern float raidCooldown;
extern bool raidWon;

/* ================================================================ world */
void worldInit();
void worldReset();
int spawnShip(int des, float x, float y, float ang, uint16_t flags, uint8_t ai);
void removeShip(int i);
void killShip(int i);
bool isEnemy(const Ship &a, const Ship &b);
void damageShip(int i, float dmg, int src);
void explode(float x, float y, float size);
void spawnPart(float x, float y, float vx, float vy, int life, uint8_t col, uint8_t kind);
void worldUpdate();
void worldDraw();
void hudDraw();
void flightInput();
void playerRefresh(bool heal);
float playerEff();
float cruiseSpeed();
void launchFrom(int st);
void dockAt(int st);
int nearestStation(float x, float y, float *d);
float stationDockRange(int st);
bool stationHostile(int st);
void startRaid(int st, int waves, uint8_t fac, bool story);
void raidTick();
void encounterTick();
void spawnGroup(int type, float x, float y);
int countFlag(uint16_t f);
void clearFlag(uint16_t f);
bool planRoute(float gx, float gy, int destSta);
void enterLane(int lane, float pos, float dir);
void leaveLane();
void drawComms();
void scanTick();
void startHail(int si);
extern int scanTarget;
extern uint8_t mapReturn;
void drawIcon(int x, int y, uint8_t m, int scale, const uint8_t *roles, int dir);
int nearRing(float x, float y, float *pos);
void toast(const char *f, ...);
const char *toastText();
extern float toastT;
struct BarkState {
  float t;
  uint8_t spk;
  int crew;
  char text[180];
};
extern BarkState bk;
void bark(uint8_t spk, int crewIdx, const char *text);
void crewBark(int bk);
void dropCruise(const char *why);
void drawStation(int style, int sx, int sy, int sc, float t, bool crippled);
void drawStarfield(float cx, float cy, float vx, float vy, bool streak);
void drawPlanetsAndSun(float cx, float cy);
int zoneAt(float x, float y);
enum : uint8_t { Z_SAFE, Z_OPEN, Z_BELT, Z_NEBULA, Z_DEEP };
void drawShipSprite(int des, int sx, int sy, float ang, int scale);
enum : uint8_t {
  ENC_PIRATE, ENC_TRADER, ENC_PATROL, ENC_HUSH, ENC_DERELICT, ENC_DISTRESS, ENC_AMBUSH,
  ENC_COUNT
};
extern int hailShip;

/* ================================================================ dialog */
struct Dialog {
  bool on;
  DLine lines[28];
  int n, i, tag, nOpt, val[4];
  float chars;
  char opt[4][28];
};
extern Dialog dlg;
void dlgBegin();
void dlgLine(uint8_t spk, const char *t);
void dlgLinef(uint8_t spk, const char *f, ...);
void dlgLines(const DLine *l, int n);
void dlgOpt(const char *label, int val);
void dlgRun(int tag);
void dlgDraw();
void speakerInfo(uint8_t spk, const char **name, int *port, uint8_t *tint);
enum : uint8_t {
  DT_NONE, DT_M0, DT_M1, DT_M2_AMBUSH, DT_M2_DONE, DT_M3_SCAN, DT_M3_DONE, DT_M4_MEET,
  DT_M4_DONE, DT_M5_MEET, DT_M5_DONE, DT_M6, DT_M6B, DT_M7, DT_M8, DT_END_SEAL,
  DT_END_OPEN, DT_HAIL_PIRATE, DT_HAIL_TRADER, DT_HAIL_PATROL, DT_DISTRESS_DONE,
  DT_WRECK, DT_DISMISS, DT_INFO, DT_JOBDONE, DT_RAID_DONE, DT_CONFIRM_NEW
};

/* ================================================================ story */
enum : uint8_t {
  MS_INTRO, MS_SHAKEDOWN, MS_LANE, MS_ECHOES, MS_SABLE, MS_MERIDIAN, MS_ABBEY,
  MS_FARPOINT, MS_MAW, MS_FREE
};
extern int mis, mstep;
extern float mT;
extern uint8_t ending; /* 0 none, 1 sealed, 2 opened */
void storyNewGame();
void storyTick();
bool storyOnDock(int st);
void storyOnLaunch();
void storyOnKill(int si);
void storyOnScan(int si);
void storyOnLane(int lane, float frac);
void storyOnRaidEnd(bool won);
void storyRevert();
const char *objective();
bool waypoint(float &x, float &y);
bool storyBlocksLaunch(const char **why);
bool storyMarker(int st); /* station has story for you */
void onDialog(int tag, int choice);
const char *missionTitle();
const char *missionLog();

/* jobs */
enum : uint8_t { JB_NONE, JB_BOUNTY, JB_DELIVER, JB_SALVAGE, JB_DEFEND };
struct Job {
  uint8_t type, from, to, count, stage;
  uint16_t reward;
  float x, y;
  uint8_t fac;
};
extern Job job, offers[3];
void genJobs(int st);
void jobTick();
bool jobOnDock(int st);
void jobOnKill(int si);
void jobOnScan(int si);
const char *jobText(const Job &j);
const char *jobObjective();

/* economy */
int priceAt(int st, int com, bool buy);
int cargoUsed();
int cargoCap();

/* save */
void saveGame();
bool loadGame();
extern bool hasSave;
void newGame();
void respawn();

/* screens (sl_ui.cpp) */
void uiIntro();
void uiTitle();
void uiCrew();
void uiStation();
void uiShipyard();
void uiTrade();
void uiJobs();
void uiBar();
void uiMap();
void uiMenu();
void uiDead();
void uiEnding();
void flightFrame();
extern float introT;
void startIntro();
void openShipyard();

} // namespace sl
