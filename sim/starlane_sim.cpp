/* Desktop simulator for STARLANE (src/starlane).
 *
 * Plays a scripted session with a simple bot (intro, crew pick, shipyard,
 * shakedown, lanes, autopilot, combat), jumps to later story beats, and runs
 * a random "monkey" pass that taps everywhere. Selected frames are written
 * as 480x480 PPM images; sim/starlane_png.py turns them into PNGs.
 *
 * Build + run:  wsl sh sim/run_starlane.sh                                   */
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "../src/starlane/sl.h"

using namespace sl;

/* ------------------------------------------------------------ platform */
static std::string outDir = ".";
static std::vector<uint8_t> saveBuf;
bool sl::platformLoad(void *buf, size_t len) {
  if (saveBuf.size() != len) return false;
  memcpy(buf, saveBuf.data(), len);
  return true;
}
void sl::platformSave(const void *buf, size_t len) {
  saveBuf.assign((const uint8_t *)buf, (const uint8_t *)buf + len);
}
void *sl::platformAllocLarge(size_t len) { return malloc(len); }

/* ------------------------------------------------------------- helpers */
static int nShots = 0;
static double tickUs = 0, worstUs = 0;
static long ticks = 0;
static int failures = 0;

static void shot(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%02d_%s.ppm", outDir.c_str(), nShots++, name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n480 480\n255\n");
  static unsigned char row[480 * 3];
  for (int y = 0; y < 480; y++) {
    for (int x = 0; x < 480; x++) {
      uint16_t c = pal565[scr[(y / 2) * LW + x / 2]];
      row[x * 3] = (unsigned char)((c >> 11) * 255 / 31);
      row[x * 3 + 1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
      row[x * 3 + 2] = (unsigned char)((c & 31) * 255 / 31);
    }
    fwrite(row, 1, sizeof(row), f);
  }
  fclose(f);
}

static double modeUs[16], modeMax[16];
static long modeN[16];
static void step(bool down, int x, int y) {
  int m = mode;
  auto t0 = std::chrono::steady_clock::now();
  tick(down, x, y);
  double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  tickUs += us;
  if (us > worstUs) worstUs = us;
  ticks++;
  modeUs[m & 15] += us;
  modeN[m & 15]++;
  if (us > modeMax[m & 15]) modeMax[m & 15] = us;
}
static void run(int n, bool down = false, int x = 0, int y = 0) {
  for (int i = 0; i < n; i++) step(down, x, y);
}
static void tapAt(int x, int y) {
  step(true, x, y);
  step(false, x, y);
}
static void finishDialog(int maxTaps = 200) {
  for (int k = 0; k < maxTaps && dlg.on; k++) {
    if (dlg.i == dlg.n - 1 && dlg.nOpt > 0) {
      dlg.chars = 1e6f;
      return;
    }
    tapAt(120, 60);
    run(2);
  }
}
static void expect(bool ok, const char *what) {
  printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
  if (!ok) failures++;
}

/* bot: hold a finger where we want to fly */
static void steerToward(float wx, float wy, float thr = 1.0f) {
  Ship &p = ships[PL];
  float sx = p.x - camX + CX, sy = p.y - camY + CY;
  float dx = wx - p.x, dy = wy - p.y, d = lenf(dx, dy);
  float r = 20 + 50 * thr;
  if (d > 0.1f) {
    dx /= d;
    dy /= d;
  }
  int tx = clampi((int)(sx + dx * r), 2, LW - 3), ty = clampi((int)(sy + dy * r), 42, 200);
  step(true, tx, ty);
}
static int nearestHostile() {
  Ship &p = ships[PL];
  int b = -1;
  float bd = 1e18f;
  for (int i = 1; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on || !(s.flags & SF_HOSTILE) || s.kind == K_STATION) continue;
    float d = dist2(s.x, s.y, p.x, p.y);
    if (d < bd) { bd = d; b = i; }
  }
  return b;
}
/* fight until no hostiles are near or timeout; returns seconds used */
static float fight(float maxSec, const char *snapName = nullptr) {
  int n = (int)(maxSec * 30), k = 0;
  bool snapped = false;
  for (; k < n; k++) {
    if (dlg.on) finishDialog();
    if (dlg.on && dlg.nOpt) { tapAt(60, 216); run(1); continue; }
    if (mode != MD_FLIGHT) break;
    int h = nearestHostile();
    if (h < 0) break;
    Ship &t = ships[h];
    Ship &p = ships[PL];
    float d = lenf(t.x - p.x, t.y - p.y);
    float lead = d / 470.0f;
    float ax = t.x + (t.vx - p.vx) * lead, ay = t.y + (t.vy - p.vy) * lead;
    float close = 60 + statsOf(t).radius;
    if (d > close) steerToward(ax, ay, d > 140 ? 1.0f : 0.35f);
    else steerToward(p.x + (t.y - p.y), p.y - (t.x - p.x), 0.8f);
    if (snapName && !snapped && d < 120 && k > 30) {
      shot(snapName);
      snapped = true;
    }
  }
  step(false, 0, 0);
  return k / 30.0f;
}
static bool waitFor(bool (*cond)(), float maxSec) {
  for (int k = 0; k < (int)(maxSec * 30); k++) {
    if (cond()) return true;
    if (dlg.on) finishDialog();
    run(1);
  }
  return cond();
}
static void teleport(float x, float y) {
  Ship &p = ships[PL];
  p.x = x; p.y = y; p.vx = p.vy = 0;
  camX = x; camY = y;
}
static void flyTo(float x, float y, float arrive, float maxSec) {
  for (int k = 0; k < (int)(maxSec * 30); k++) {
    if (dlg.on) { finishDialog(); if (dlg.on && dlg.nOpt) { tapAt(60, 216); } continue; }
    if (mode != MD_FLIGHT) return;
    Ship &p = ships[PL];
    if (lenf(x - p.x, y - p.y) < arrive) break;
    steerToward(x, y);
  }
  step(false, 0, 0);
}

static void autopilotTo(int station) {
  /* open the map, pick a station, engage */
  tapAt(149, 226);
  run(2);
  int sx = 120 + (int)(STATIONS[station].x * 0.0083f), sy = 100 + (int)(STATIONS[station].y * 0.0083f);
  tapAt(sx, sy);
  run(2);
  tapAt(60, 228);
  run(2);
}

/* ------------------------------------------------------------- scenario */
static bool docked() { return mode == MD_STATION; }

/* travel by autopilot (lanes and cruise), fighting anything that jumps us */
static bool travelTo(int station, float maxSec) {
  if (mode == MD_STATION) { tapAt(120, 223); run(3); }
  autopilotTo(station);
  for (int k = 0; k < (int)(maxSec * 30); k++) {
    if (mode == MD_STATION && dockedAt == station) return true;
    if (mode == MD_DEAD) { run(100); tapAt(120, 202); run(3); return false; }
    if (dlg.on) {
      finishDialog();
      if (dlg.on && dlg.nOpt) tapAt(60, 216); /* first option: FIGHT / buy / ... */
      run(1);
      continue;
    }
    if (mode != MD_FLIGHT) { mode = MD_FLIGHT; continue; }
    if (nearestHostile() >= 0 && dist2(ships[nearestHostile()].x, ships[nearestHostile()].y, ships[PL].x, ships[PL].y) < 500 * 500) {
      fight(60);
      autopilotTo(station);
      continue;
    }
    if (!ap.on && !lr.on) autopilotTo(station);
    run(1);
  }
  return mode == MD_STATION && dockedAt == station;
}

static int storyRun() {
  printf("=== story run ===\n");
  init(99);
  run(5);
  mode = MD_CREW;
  tapAt(60, 40); run(1);
  tapAt(178, 40); run(1);
  tapAt(120, 226); run(3);
  finishDialog();
  /* a well-armed ship so the bot can finish fights */
  Grid &g = designs[D_PLAYER].g;
  const char *rows[7] = {".L.L.L.", ".LHCHL.", ".SRRRS.", "TAHQHAT", ".SGGGS.", ".E.E.E.", "......."};
  memset(&g, 0, sizeof(g));
  for (int y = 0; y < 7; y++)
    for (int x = 0; x < 7; x++) {
      char c = rows[y][x];
      uint8_t m = c == 'L' ? M_LASER : c == 'H' ? M_HULL : c == 'C' ? M_COCKPIT : c == 'S' ? M_SHIELD
                : c == 'R' ? M_REACTOR : c == 'T' ? M_TURRET : c == 'A' ? M_ARMOR : c == 'Q' ? M_QUARTERS
                : c == 'G' ? M_CARGO : c == 'E' ? M_ENGINE : 0;
      g.m[y][x] = m;
      g.dir[y][x] = (uint8_t)autoDir(g, x, y);
    }
  playerRefresh(true);
  const Stats &s = designs[D_PLAYER].st;
  printf("  test ship: hp %.0f sh %.0f vmax %.0f guns %d power %d/%d cargo %d\n", s.hp, s.shield, s.vmax, s.nMount, s.powGen, s.powUse, s.cargo);
  tapAt(120, 223); run(3);
  expect(mis == MS_SHAKEDOWN, "M1 shakedown starts on launch");
  fight(60);
  run(3);
  finishDialog();
  expect(mis == MS_LANE, "M2 lane runner");
  bool ok = travelTo(ST_TOLLE, 240);
  if (!ok && mode != MD_STATION) ok = travelTo(ST_TOLLE, 240);
  finishDialog();
  expect(mis == MS_ECHOES, "M3 echoes after Brakka");
  tapAt(120, 223); run(3);
  /* the wreck: fly there */
  ships[PL].flags |= SF_INVULN;
  teleport(WRECK_X + 500, WRECK_Y);
  flyTo(WRECK_X, WRECK_Y, 60, 30);
  scanTarget = -1;
  for (int k = 0; k < 30 * 5 && !dlg.on; k++) {
    if (scanTarget < 0) tapAt(89, 226);
    run(1);
  }
  finishDialog();
  expect(mstep == 1, "M3 hush ambush after scan");
  fight(120);
  run(3);
  finishDialog();
  expect(mstep == 2, "M3 fragment recovered");
  ok = travelTo(ST_TOLLE, 300);
  finishDialog();
  expect(mis == MS_SABLE && (known & (1 << ST_ROOK)), "M4 old friends, Rook's Nest revealed");
  ok = travelTo(ST_ROOK, 300);
  expect(ok, "docked at Rook's Nest");
  run(20); shot("story_sable");
  finishDialog();
  expect(mstep == 1, "M4 nest job given");
  tapAt(120, 223); run(3);
  teleport(NEST_X - 800, NEST_Y);
  flyTo(NEST_X, NEST_Y, 500, 30);
  for (int w = 0; w < 6 && mstep == 2; w++) { fight(120); run(30 * 4); }
  run(10);
  shot("story_nest_cleared");
  expect(mstep == 3, "M4 nest destroyed");
  ok = travelTo(ST_ROOK, 300);
  finishDialog();
  expect(mis == MS_MERIDIAN && rep[F_SYND] >= 40, "M5 meridian job, syndicate friendly");
  ok = travelTo(ST_MERIDIAN, 400);
  expect(ok, "docked at Meridian");
  finishDialog();
  if (dlg.on && dlg.nOpt) tapAt(60, 228);
  run(5);
  expect(mode == MD_FLIGHT && raid.on && mstep == 2, "M5 siege of Meridian starts");
  run(30 * 3); shot("story_meridian_siege");
  for (int k = 0; k < 10 && raid.on; k++) { fight(60); run(30 * 4); }
  expect(mstep == 3, "M5 siege broken");
  ok = travelTo(ST_MERIDIAN, 120);
  finishDialog();
  expect(mis == MS_ABBEY, "M6 abbey");
  ok = travelTo(ST_LANTERN, 400);
  expect(ok, "docked at Lantern Abbey");
  run(20); shot("story_ninefold");
  finishDialog();
  expect(mstep == 1 && knownGate, "M6 truth revealed, gate on map");
  ok = travelTo(ST_TOLLE, 400);
  finishDialog();
  expect(mis == MS_FARPOINT && laneOpen[3], "M7 farpoint lane relit");
  if (mode == MD_STATION) { tapAt(120, 223); run(3); }
  autopilotTo(ST_FARPOINT);
  for (int k = 0; k < 30 * 200 && mstep == 0; k++) {
    if (dlg.on) { finishDialog(); if (dlg.on && dlg.nOpt) tapAt(60, 216); }
    if (!ap.on && !lr.on && mode == MD_FLIGHT) autopilotTo(ST_FARPOINT);
    if (mode == MD_STATION) { tapAt(120, 223); run(3); }
    run(1);
  }
  expect(mstep == 1 && raid.on, "M7 farpoint siege");
  run(30 * 3); shot("story_farpoint_siege");
  for (int k = 0; k < 14 && raid.on; k++) { fight(60); run(30 * 4); }
  expect(mstep == 2, "M7 siege broken");
  ok = travelTo(ST_FARPOINT, 200);
  finishDialog();
  expect(mis == MS_MAW, "M8 the maw");
  tapAt(120, 223); run(3);
  teleport(GATE_X - 1300, GATE_Y - 300);
  flyTo(GATE_X, GATE_Y, 1100, 20);
  run(30 * 2); shot("story_choir");
  for (int k = 0; k < 10 && mstep == 1; k++) { fight(90); run(30 * 2); }
  expect(mstep == 2, "M8 choir destroyed");
  for (int k = 0; k < 30 * 60 && !dlg.on; k++) steerToward(GATE_X, GATE_Y, 0.6f);
  step(false, 0, 0);
  for (int k = 0; k < 40 && dlg.on && !(dlg.i == dlg.n - 1); k++) { tapAt(120, 60); run(1); }
  run(40);
  shot("story_gate");
  expect(dlg.on && dlg.nOpt == 2, "final choice at the gate");
  dlg.chars = 1e6f;
  run(1);
  tapAt(60, 228); /* SEAL */
  run(3);
  finishDialog();
  run(5);
  expect(mode == MD_ENDING && ending == 1, "sealed ending");
  shot("story_ending");
  tapAt(120, 217); run(3);
  expect(mode == MD_FLIGHT && mis == MS_FREE, "free flight after the ending");
  printf("  play time %.0fs, kills %d, credits %d\n", playTime, kills, credits);
  return failures;
}

static int jobsRun() {
  printf("=== jobs + hails ===\n");
  init(7);
  mode = MD_CREW;
  tapAt(60, 40); run(1);
  tapAt(60, 86); run(1); /* kirri + imara (envoy) */
  tapAt(120, 226); run(3);
  finishDialog();
  mis = MS_FREE;
  ships[PL].flags |= SF_INVULN;
  inv[M_LASER] = 0;
  Grid &g = designs[D_PLAYER].g;
  g.m[2][2] = M_LASER; g.m[2][4] = M_LASER;
  playerRefresh(true);
  dockAt(ST_HAVEN);
  finishDialog();
  int done = 0;
  for (int t = JB_BOUNTY; t <= JB_DEFEND; t++) {
    memset(&job, 0, sizeof(job));
    genJobs(ST_HAVEN);
    Job j = offers[0];
    j.type = (uint8_t)t;
    j.from = ST_HAVEN;
    j.count = 3;
    j.reward = 500;
    j.fac = F_SYND;
    j.to = t == JB_DELIVER ? ST_MERIDIAN : ST_HAVEN;
    job = j;
    if (t == JB_DELIVER) cargo[CM_PARCEL] = 3;
    int c0 = credits;
    if (mode == MD_STATION) { tapAt(120, 223); run(3); }
    raid.on = false;
    raidCooldown = 0;
    if (t == JB_BOUNTY || t == JB_SALVAGE) {
      teleport(job.x - 400, job.y);
      flyTo(job.x, job.y, 80, 20);
      if (t == JB_SALVAGE) {
        for (int k = 0; k < 30 * 8 && job.type; k++) {
          for (int i = FIRST_DYN; i < MAXSHIP; i++)
            if (ships[i].on && ships[i].kind == K_WRECK && (ships[i].flags & SF_JOB)) {
              flyTo(ships[i].x, ships[i].y, 50, 5);
              scanTarget = i;
            }
          run(1);
        }
      } else {
        fight(90);
      }
      run(5);
    } else if (t == JB_DELIVER) {
      travelTo(ST_MERIDIAN, 300);
    } else {
      teleport(STATIONS[ST_HAVEN].x + 900, STATIONS[ST_HAVEN].y + 600);
      run(5);
      for (int k = 0; k < 12 && (job.type || raid.on); k++) { fight(60); run(30 * 5); }
    }
    finishDialog();
    bool paid = credits > c0 && job.type == JB_NONE;
    printf("  job type %d: credits %d -> %d\n", t, c0, credits);
    expect(paid, "job pays out");
    done += paid;
  }
  /* hails */
  if (mode == MD_STATION) { tapAt(120, 223); run(3); }
  Ship &p = ships[PL];
  teleport(0, -3000);
  static const int encs[3] = {ENC_PIRATE, ENC_TRADER, ENC_PATROL};
  static const char *names[3] = {"hail_pirate", "hail_trader", "hail_patrol"};
  for (int e = 0; e < 3; e++) {
    clearFlag(SF_ENC);
    rep[F_SYND] = -30;
    int c0 = credits;
    spawnGroup(encs[e] == ENC_PIRATE ? 99 : encs[e], p.x + 300, p.y);
    if (encs[e] == ENC_PIRATE) {
      int i = spawnShip(D_RAIDER, p.x + 300, p.y, 0, SF_HAIL | SF_ENC, AI_HAIL);
      ships[i].group = 77;
    }
    bool hailed = false;
    for (int k = 0; k < 30 * 15 && !hailed; k++) { run(1); hailed = dlg.on; }
    expect(hailed, names[e]);
    run(40);
    shot(names[e]);
    dlg.chars = 1e6f;
    run(1);
    tapAt(178, 228); /* second option */
    run(5);
    finishDialog();
    printf("  %s: options %d, credits %d -> %d, hostile %d\n", names[e], dlg.nOpt, c0, credits, countFlag(SF_HOSTILE));
  }
  return failures;
}

int main(int argc, char **argv) {
  if (argc > 1) outDir = argv[1];
  if (argc > 2 && !strcmp(argv[2], "story")) return storyRun();
  if (argc > 2 && !strcmp(argv[2], "jobs")) return jobsRun();
  init(1234);
  printf("intro\n");
  run(30 * 3); shot("intro_words");
  run(30 * 7); shot("intro_lane");
  run(30 * 4 + 20); shot("intro_lane_fail");
  run(30 * 4); shot("intro_chase");
  run(30 * 6); shot("intro_lancers");
  run(30 * 8);
  expect(mode == MD_TITLE, "intro ends on the title screen");
  run(40); shot("title");

  printf("new game\n");
  tapAt(120, 160); run(3);
  expect(mode == MD_CREW, "crew select opens");
  tapAt(60, 40); run(2);
  tapAt(178, 86); run(2);
  shot("crew_select");
  tapAt(120, 226); run(3);
  expect(mode == MD_STATION && nCrew == 2, "docked at Haven with 2 crew");
  run(40); shot("arrival_dialog");
  finishDialog();
  expect(!dlg.on && mstep == 1, "arrival dialog done");
  run(10); shot("station_haven");

  printf("shipyard\n");
  tapAt(60, 127); run(3);
  expect(mode == MD_SHIPYARD, "shipyard opens");
  tapAt(148, 92); run(2); /* laser in palette */
  tapAt(4 + 2 * 18 + 8, 23 + 2 * 18 + 8); run(2);
  tapAt(4 + 4 * 18 + 8, 23 + 2 * 18 + 8); run(2);
  shot("shipyard");
  expect(gridCount(designs[D_PLAYER].g, M_LASER) == 2, "two lasers fitted");
  tapAt(180, 228); run(3);
  expect(mode == MD_STATION, "back to station");
  tapAt(120, 223); run(3);
  expect(mode == MD_FLIGHT && mis == MS_SHAKEDOWN, "launched into shakedown");
  run(30); shot("flight_launch");

  printf("shakedown\n");
  float t = fight(60, "shakedown_combat");
  printf("  drones cleared in %.1fs\n", t);
  run(5);
  expect(dlg.on || mis == MS_LANE, "shakedown done dialog");
  shot("shakedown_done");
  finishDialog();
  expect(mis == MS_LANE, "mission 2 (lane runner) active");

  printf("lane + autopilot\n");
  autopilotTo(ST_TOLLE);
  expect(ap.on && mode == MD_FLIGHT, "autopilot engaged");
  bool sawCruise = false, sawLane = false;
  for (int k = 0; k < 30 * 60 && !dlg.on && mode == MD_FLIGHT; k++) {
    run(1);
    if (cruise == CR_ON && !sawCruise) { sawCruise = true; shot("cruise"); }
    if (lr.on && !sawLane && lr.spd > 1500) { sawLane = true; shot("trade_lane"); }
  }
  expect(sawLane, "autopilot entered the trade lane");
  expect(mstep == 1 && dlg.on, "lane disrupted by raiders (story)");
  shot("lane_ambush_dialog");
  finishDialog();
  run(20); shot("lane_ambush");
  t = fight(90, "raider_fight");
  printf("  raiders fought for %.1fs, hp %.0f/%.0f\n", t, ships[PL].hp, ships[PL].hpMax);
  if (mode == MD_DEAD) {
    printf("  (player died, respawning)\n");
    run(120); tapAt(120, 202); run(5);
  }
  run(30);
  if (mode == MD_FLIGHT) {
    autopilotTo(ST_TOLLE);
    waitFor(docked, 120);
  }
  expect(mode == MD_STATION && dockedAt == ST_TOLLE, "docked at Tolle");
  run(20); shot("tolle_brakka");
  finishDialog();
  expect(mis == MS_ECHOES, "mission 3 (echoes) active");
  run(5); shot("station_tolle");

  /* side screens */
  tapAt(178, 127); run(3); shot("trade"); tapAt(120, 227); run(2);
  tapAt(60, 153); run(3); shot("jobs"); tapAt(120, 227); run(2);
  tapAt(178, 153); run(3); shot("cantina"); tapAt(120, 228); run(2);
  tapAt(178, 179); run(3); shot("map_from_station"); tapAt(180, 228); run(2);
  expect(mode == MD_STATION, "side screens return to station");

  printf("echoes: derelict\n");
  tapAt(120, 223); run(10);
  teleport(WRECK_X + 300, WRECK_Y + 200);
  flyTo(WRECK_X, WRECK_Y, 60, 30);
  run(5); shot("belt_wreck");
  tapAt(89, 226); /* SCAN */
  waitFor([] { return dlg.on; }, 8);
  expect(dlg.on, "scan of the Saint Ivo starts the story dialog");
  run(40); shot("hush_voice");
  finishDialog();
  run(20);
  t = fight(80, "hush_fight");
  printf("  hush fought for %.1fs, mstep %d\n", t, mstep);
  finishDialog();

  printf("jump: nebula, rook's nest, gate\n");
  if (mode == MD_DEAD) { run(120); tapAt(120, 202); run(5); tapAt(120, 223); run(5); }
  if (mode == MD_STATION) { tapAt(120, 223); run(5); }
  teleport(NEB_X + 500, NEB_Y + 300); run(20); shot("nebula");
  teleport(STATIONS[ST_LANTERN].x + 80, STATIONS[ST_LANTERN].y + 60); run(20); shot("lantern_abbey");
  teleport(STATIONS[ST_MERIDIAN].x + 90, STATIONS[ST_MERIDIAN].y + 80); run(20); shot("meridian_vey");
  teleport(4400, 1500); run(20); shot("ormaz_gas_giant");
  teleport(260, 200); run(10); shot("sun");
  mis = MS_SABLE; mstep = 0;
  teleport(STATIONS[ST_ROOK].x + 70, STATIONS[ST_ROOK].y + 60); run(20); shot("rooks_nest");
  tapAt(89, 226); run(3);
  expect(mode == MD_STATION && dockedAt == ST_ROOK, "docked at Rook's Nest during M4");
  run(30); shot("sable");
  finishDialog();
  expect(mstep == 1, "sable gives the nest job");

  printf("raid at haven\n");
  mis = MS_ABBEY; mstep = 0;
  tapAt(120, 223); run(5);
  teleport(STATIONS[ST_HAVEN].x + 250, STATIONS[ST_HAVEN].y + 150);
  run(10);
  startRaid(ST_HAVEN, 2, F_SYND, false);
  run(30 * 4);
  shot("raid_haven");
  t = fight(90, "raid_fight");
  run(30 * 5);
  printf("  raid fought for %.1fs, raid.on=%d\n", t, raid.on);

  printf("boss\n");
  if (mode == MD_DEAD) { run(120); tapAt(120, 202); run(5); }
  if (mode == MD_STATION) { tapAt(120, 223); run(5); }
  mis = MS_MAW; mstep = 0; knownGate = true;
  ships[PL].flags |= SF_INVULN;
  teleport(GATE_X - 900, GATE_Y - 500);
  flyTo(GATE_X, GATE_Y, 1000, 10);
  run(30 * 3); shot("choir");
  t = fight(240, "choir_fight");
  printf("  choir fight %.1fs, mstep %d\n", t, mstep);
  ships[PL].flags &= (uint16_t)~SF_INVULN;
  if (mstep == 2) {
    flyTo(GATE_X, GATE_Y, 100, 40);
    run(5);
    shot("gate_choice");
    expect(dlg.on && dlg.nOpt == 2, "final choice offered");
    finishDialog();
    tapAt(178, 228); /* OPEN */
    run(3);
    finishDialog();
    run(5);
    expect(mode == MD_ENDING, "ending screen");
    shot("ending");
  }

  printf("menu + map in flight\n");
  mode = MD_FLIGHT; run(5);
  tapAt(208, 226); run(3); shot("menu_ship");
  tapAt(120, 22); run(2); shot("menu_crew");
  tapAt(198, 22); run(2); shot("menu_log");
  tapAt(120, 227); run(3);
  tapAt(149, 226); run(3); shot("map_flight");
  tapAt(180, 228); run(2);

  printf("save / load\n");
  mode = MD_STATION; dockedAt = ST_TOLLE; saveGame();
  int c0 = credits;
  credits = 5;
  expect(loadGame() && credits == c0, "save round-trips");

  printf("monkey test\n");
  long mt0 = ticks;
  for (int k = 0; k < 30 * 600; k++) {
    if (mode == MD_TITLE || mode == MD_INTRO) { newGame(); }
    bool down = (rnd() % 5) != 0;
    int x = rnd() % LW, y = rnd() % LH;
    int hold = 1 + rnd() % 20;
    for (int h = 0; h < hold; h++) step(down, x, y);
    if (rnd() % 400 == 0) mode = MD_FLIGHT;
    if (rnd() % 900 == 0) { teleport(frange(-11000, 11000), frange(-9000, 9000)); }
    if (rnd() % 1500 == 0) { mis = irand(MS_FREE + 1); mstep = 0; }
  }
  printf("  monkey ran %ld ticks, mode %d\n", ticks - mt0, mode);

  printf("\naverage tick %.0f us, worst %.0f us over %ld ticks\n", tickUs / ticks, worstUs, ticks);
  static const char *mn[13] = {"intro", "title", "crew", "flight", "station", "shipyard", "trade", "jobs", "bar", "map", "menu", "dead", "ending"};
  for (int m = 0; m < 13; m++)
    if (modeN[m]) printf("  %-9s avg %6.0f us  max %6.0f us  (%ld ticks)\n", mn[m], modeUs[m] / modeN[m], modeMax[m], modeN[m]);
  printf("%d shots, %d failures\n", nShots, failures);
  return failures ? 1 : 0;
}
