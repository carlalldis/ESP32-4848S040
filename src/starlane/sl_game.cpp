/* STARLANE - game state, story missions, hails, jobs, economy, saving, and
 * the per-frame entry point. */
#include "sl.h"

namespace sl {

uint8_t mode = MD_INTRO;
uint32_t frameNo;
float playTime, gameTime;
int credits;
int cargo[NCOM];
uint8_t inv[M_COUNT];
uint8_t paint;
uint8_t crew[6];
int nCrew;
int dockedAt = -1, lastStation = 0;
uint8_t known;
bool knownGate;
int kills;
float fade;
int mis, mstep;
float mT;
uint8_t ending;
Job job, offers[3];
bool hasSave;

static bool prevDown;
static bool m2Dropped;
static int waveN;
static float waveT;

/* ============================================================== dialogue */
#define N(a) (int)(sizeof(a) / sizeof(a[0]))

static const DLine D_M0[] = {
    {SPK_CTRL, "DOCKING CLAMPS ENGAGED. WELCOME TO HAVEN SPINDLE, COURIER. TRY NOT TO BLEED PLASMA ON MY DECK."},
    {SPK_VARROW, "COMMANDER ILSA VARROW, REACH AUTHORITY. MY LANCERS JUST PULLED YOU OUT OF A VERY STRANGE FIRE."},
    {SPK_VARROW, "THOSE BLACK SHIPS DIDN'T HAIL. DIDN'T SHOW ON LONG SCAN. THEY JUST ARRIVED, LIKE THE DARK HAD GROWN TEETH."},
    {SPK_YOU, "THEY CAME OUT OF THE LANE WITH ME. THE RINGS WENT RED, AND THEN THEY WERE JUST... THERE."},
    {SPK_VARROW, "WHAT ARE YOU CARRYING, COURIER?"},
    {SPK_YOU, "A SEALED CASE. PICKUP AT FARPOINT, DROP AT DOCKYARD TOLLE. THE CLIENT PAID TRIPLE AND LEFT NO NAME."},
    {SPK_VARROW, "FARPOINT WENT DARK SIX HOURS AGO. YOU MIGHT BE THE LAST THING THAT CAME OUT OF THERE ALIVE."},
    {SPK_CREWA, "@0"},
    {SPK_CREWB, "@0"},
    {SPK_VARROW, "YOUR SHIP IS SCRAP. I'M LENDING YOU A FRAME FROM OUR YARD AND TWO LASERS. BOLT THEM ON IN THE SHIPYARD."},
    {SPK_VARROW, "THEN MEET ME OUTSIDE. I WANT TO SEE YOU SHOOT BEFORE I TRUST YOU WITH ANYTHING."},
    {SPK_NARR, "TIP: IN THE SHIPYARD, PICK THE PULSE LASER AND TAP AN EMPTY CELL BESIDE YOUR COCKPIT. A GUN NEEDS A CLEAR CELL IN FRONT OF IT."},
};
static const DLine D_M1[] = {
    {SPK_VARROW, "NOT BAD, COURIER. YOUR GUNS FIRE ON THEIR OWN - KEEP THEM POINTED THE RIGHT WAY AND KEEP MOVING."},
    {SPK_VARROW, "NOW. THAT CASE WAS GOING TO TOLLE, SO THAT IS WHERE IT GOES. ENGINEER BRAKKA WILL KNOW WHAT IT IS."},
    {SPK_VARROW, "TAKE THE TRADE LANE. FLY TO THE NEAREST RING AND TAP LANE. THE ARCHITECTS DID THE HARD PART TWO CENTURIES AGO."},
    {SPK_CREWA, "@1"},
    {SPK_NARR, "TIP: OPEN THE MAP AND PICK A STATION TO LET THE AUTOPILOT FLY. IT RIDES THE LANES FOR YOU."},
};
static const DLine D_M2_AMBUSH[] = {
    {SPK_PIRATE, "WELL, WELL. THE COURIER FROM FARPOINT. SOMEBODY'S PAYING GOOD MONEY FOR THAT CASE, FRIEND."},
    {SPK_PIRATE, "WE PULLED THE PLUG ON YOUR LANE. HAND IT OVER. OR DON'T. WE GET PAID EITHER WAY."},
};
static const DLine D_M2_DONE[] = {
    {SPK_BRAKKA, "SMALL CAPTAIN. VARROW SAID YOU WOULD COME. VARROW SAID YOU WOULD BE ON FIRE. YOU ARE ONLY A LITTLE ON FIRE. GOOD."},
    {SPK_YOU, "THIS IS FOR YOU, I THINK. NO NAME ON IT."},
    {SPK_BRAKKA, "...OH. THIS IS OLD. OLDER THAN MY GRANDMOTHER'S GRANDMOTHER, AND SHE WAS A MOUNTAIN."},
    {SPK_BRAKKA, "A PIECE OF A LANE KEY. ARCHITECT WORK. THE KEYS TELL THE RINGS WHEN TO OPEN. AND WHEN TO CLOSE."},
    {SPK_BRAKKA, "IT IS SINGING. VERY QUIET. SINGING TO ANOTHER PIECE OF ITSELF, OUT IN THE CINDER BELT."},
    {SPK_BRAKKA, "THERE IS A DEAD SHIP OUT THERE. THE SAINT IVO. RELIC HUNTERS. NOBODY CAME BACK."},
    {SPK_BRAKKA, "TAKE THIS SHIELD GENERATOR AND 600 CREDITS. BOLT THE SHIELD ON. GO CAREFUL. CRUISE IS FASTER THAN YOU THINK."},
    {SPK_CREWB, "@1"},
};
static const DLine D_M3_SCAN[] = {
    {SPK_NARR, "THE SAINT IVO'S HOLD IS TORN OPEN FROM THE INSIDE. THE CREW ARE GONE. THE LOG HAS ONE ENTRY, REPEATED TEN THOUSAND TIMES:"},
    {SPK_NARR, "'IT ISN'T DEAD. IT'S LISTENING.'"},
    {SPK_CREWA, "@2"},
    {SPK_NARR, "IN THE WRECKAGE, A SHARD OF WARM BLACK METAL HUMS IN TIME WITH YOUR CASE. YOU TAKE IT."},
    {SPK_HUSH, "W E   R E M E M B E R   T H E   S H A P E   O F   Y O U"},
};
static const DLine D_M3_CLEAR[] = {
    {SPK_CREWB, "@3"},
    {SPK_NARR, "TWO FRAGMENTS NOW, HUMMING TOGETHER. BRAKKA WILL WANT TO HEAR THIS. RETURN TO DOCKYARD TOLLE."},
};
static const DLine D_M3_DONE[] = {
    {SPK_BRAKKA, "TWO PIECES. SINGING LOUDER NOW. I DO NOT LIKE THE SONG."},
    {SPK_BRAKKA, "THE BLACK SHIPS SPOKE TO YOU? NOBODY HEARS THEM SPEAK. IN THE OLD STORIES THEY ARE CALLED THE HUSH. THE QUIET THAT COMES BEFORE."},
    {SPK_BRAKKA, "THE THIRD PIECE WAS SOLD. RELICS ALWAYS GO THROUGH ROOK'S NEST, IN THE BELT. THE VASHTI SYNDICATE. MOTHER SABLE."},
    {SPK_BRAKKA, "SHE WILL SHOOT YOU. PROBABLY. BUT SHE WILL TALK FIRST IF YOU ARE INTERESTING. BE INTERESTING."},
    {SPK_NARR, "BRAKKA PAYS YOU 700 CREDITS. ROOK'S NEST IS NOW ON YOUR MAP. ITS GUNS WILL HOLD FIRE - SABLE IS EXPECTING YOU."},
};
static const DLine D_M4_MEET[] = {
    {SPK_NARR, "ROOK'S NEST IS A HOLLOWED ASTEROID FULL OF GUNS, MUSIC AND PEOPLE WHO DON'T USE THEIR REAL NAMES."},
    {SPK_SABLE, "THE FARPOINT COURIER. SIT, SIT. ALL FOUR OF MY HANDS ARE WHERE YOU CAN SEE THEM. THAT SHOULD WORRY YOU."},
    {SPK_SABLE, "YES, I SOLD THE THIRD FRAGMENT. TO A THRENNIC BROKER CALLED OSSK. GOOD PRICE. I REGRET NOTHING."},
    {SPK_SABLE, "BUT. SOMETHING HAS BEEN EATING MY SHIPS. BLACK SHIPS. QUIET. THEY NEST IN THE ROCKS AT KELL'S GRAVE."},
    {SPK_SABLE, "BURN THE NEST AND I'LL TELL OSSK YOU'RE FAMILY. THE THRENNIC RESPECT FAMILY. MOSTLY THEY RESPECT MONEY."},
    {SPK_CREWA, "@4"},
};
static const DLine D_M4_DONE[] = {
    {SPK_SABLE, "YOU'RE EITHER VERY BRAVE OR VERY STUPID. I EMPLOY BOTH. HERE - A MISSILE RACK FROM MY OWN COLLECTION, AND 900 CREDITS."},
    {SPK_SABLE, "OSSK IS AT MERIDIAN EXCHANGE. I'VE SENT WORD. MY PEOPLE WON'T BOTHER YOU ON THE ROADS NOW, EITHER."},
    {SPK_SABLE, "AND COURIER - THE HUSH DIDN'T SHOW UP UNTIL WE STARTED PULLING RELICS OUT OF THE OLD RINGS."},
    {SPK_SABLE, "I'M NOT SAYING IT'S OUR FAULT. I'M SAYING DON'T TELL VARROW."},
};
static const DLine D_M5_MEET[] = {
    {SPK_OSSK, "WARM VACUUM, COURIER. YOU ARE FAMILY NOW, SABLE SAYS. FAMILY GETS A FOUR PERCENT DISCOUNT."},
    {SPK_OSSK, "THE FRAGMENT? YES, YES. IT IS IN MY VAULT. IT HAS BEEN... HUMMING. MY ACCOUNTANTS ARE NERVOUS. ACCOUNTANTS ARE ALWAYS NERVOUS."},
    {SPK_OSSK, "I WILL GIVE IT TO YOU FOR A VERY REASONABLE-"},
    {SPK_CTRL, "ALERT. ALERT. MULTIPLE UNKNOWN CONTACTS DROPPING OUT OF NOWHERE. ALL PILOTS TO DEFENSIVE STATIONS."},
    {SPK_OSSK, "...FOR FREE. IT IS FREE. IF THERE IS STILL A STATION TOMORROW. PLEASE GO OUTSIDE AND SHOOT THINGS."},
    {SPK_CREWA, "@5"},
};
static const DLine D_M5_DONE[] = {
    {SPK_OSSK, "THE BOOKS BALANCE! THE FRAGMENT IS YOURS. NO CHARGE. ALSO NO CHARGE: THIS TURRET, WHICH FELL OFF A FREIGHTER. FELL VERY PRECISELY."},
    {SPK_OSSK, "THREE PIECES NOW. THEY WANT TO BE TOGETHER. MY GRANDMOTHER SAID THE SYLITH OF LANTERN ABBEY CAN HEAR WHAT OLD THINGS WANT."},
    {SPK_OSSK, "TAKE THE LANE TO LANTERN. AND COURIER - WHATEVER THE THREE PIECES OPEN... MAYBE ASK FIRST WHAT IS ON THE OTHER SIDE."},
    {SPK_NARR, "OSSK ALSO TRANSFERS 800 CREDITS, 'FOR THE INCONVENIENCE OF ALMOST DYING'."},
};
static const DLine D_M6[] = {
    {SPK_NARR, "LANTERN ABBEY GLOWS INSIDE THE VEIL LIKE A CANDLE IN A FOGGED WINDOW. NOBODY SPEAKS ABOVE A WHISPER HERE."},
    {SPK_NINEFOLD, "WE HAVE BEEN WAITING FOR YOU, COURIER. NOT YOU EXACTLY. FOR WHOEVER WOULD CARRY THE KEY. YOU WILL DO."},
    {SPK_NINEFOLD, "PUT THE PIECES DOWN. ...LISTEN. DO YOU HEAR IT? NO. OF COURSE NOT. WE WILL TELL YOU WHAT IT SAYS."},
    {SPK_NINEFOLD, "THE ARCHITECTS DID NOT VANISH. THEY STEPPED THROUGH THE GREAT GATE AT THE EDGE OF THE REACH, AND LOCKED IT BEHIND THEM."},
    {SPK_NINEFOLD, "THE HUSH ARE WHAT THEY LEFT TO GUARD THE DOOR. A WATCHFUL SILENCE. EVERY RELIC STRIPPED FROM A RING IS A KNOCK ON THAT DOOR."},
    {SPK_NINEFOLD, "SOMEONE HAS BEEN KNOCKING VERY HARD. NOW THE GATE IS WAKING, AND THE HUSH POUR OUT TO SEE WHO IS THERE."},
    {SPK_NINEFOLD, "THE KEY CAN SEAL THE GATE FOREVER. OR IT CAN OPEN IT. WE DO NOT KNOW WHAT WAITS ON THE OTHER SIDE. NEITHER DO THE HUSH, WE THINK."},
    {SPK_CREWA, "@6"},
    {SPK_CREWB, "@6"},
    {SPK_NINEFOLD, "THE GATE LIES PAST FARPOINT, AND THE FARPOINT LANE IS DARK. GO TO BRAKKA AT TOLLE. ONLY ORNN HANDS CAN RELIGHT A DEAD LANE."},
    {SPK_NARR, "THE ABBEY GIVES YOU A NANO-REPAIR MODULE AND 500 CREDITS. THE ARCHITECT GATE IS NOW ON YOUR MAP."},
};
static const DLine D_M6B[] = {
    {SPK_BRAKKA, "LANTERN SENT A WHISPER AHEAD OF YOU. THE SYLITH ARE TERRIBLE AT SECRETS. THEY JUST TELL EVERYBODY QUIETLY."},
    {SPK_BRAKKA, "I WILL WAKE THE FARPOINT LANE. IT WILL TAKE FOUR MINUTES OR FOUR DAYS. ...DONE. IT WAS FOUR MINUTES."},
    {SPK_VARROW, "COURIER, VARROW HERE. FARPOINT IS UNDER SIEGE. WHATEVER YOU'RE DOING, DO IT THERE. I'M BRINGING EVERY LANCER I HAVE."},
};
static const DLine D_M7[] = {
    {SPK_VARROW, "FARPOINT STANDS. BARELY. I'VE SEEN THE GATE ON LONG SCAN, COURIER. IT'S OPEN A CRACK, AND SOMETHING HUGE IS SITTING IN THE CRACK."},
    {SPK_VARROW, "THE HUSH CALL IT THE CHOIR. EVERY BLACK SHIP WE'VE FOUGHT SINGS BACK TO IT. KILL THE CHOIR AND THE REST GO QUIET."},
    {SPK_VARROW, "THEN YOU USE THAT KEY. I WON'T TELL YOU WHICH WAY TO TURN IT. I'M NOT SURE I KNOW."},
    {SPK_CREWA, "@7"},
    {SPK_CREWB, "@7"},
    {SPK_NARR, "FARPOINT'S CREW PATCH YOUR HULL AND LOAD TWO ARMOR BLOCKS AND 1000 CREDITS. FIT THEM WELL - THE CHOIR IS BIG."},
};
static const DLine D_M8[] = {
    {SPK_NARR, "THE GATE FILLS THE SKY. ITS RINGS TURN SLOWLY, ONE INSIDE THE OTHER, LIKE A VAST AND PATIENT EYE."},
    {SPK_HUSH, "K E Y B E A R E R .   C H O O S E ."},
    {SPK_NARR, "THE THREE FRAGMENTS LOCK TOGETHER IN YOUR HANDS. THEY ARE WARM. THE KEY WILL TURN EITHER WAY."},
};
static const DLine D_END_SEAL[] = {
    {SPK_NARR, "YOU TURN THE KEY. THE RINGS SLOW. STOP. THE LIGHT AT THE GATE'S HEART SHRINKS TO A POINT, AND GOES OUT."},
    {SPK_NARR, "ACROSS THE REACH, EVERY BLACK SHIP STOPS AT ONCE, AS IF LISTENING. THEN THEY DRIFT, DARK AND EMPTY, AND DO NOT WAKE AGAIN."},
    {SPK_CREWA, "@8"},
    {SPK_NARR, "THE LANES STILL SING. THE REACH IS SAFE. WHATEVER THE ARCHITECTS BECAME, THEY ARE ALONE BEHIND A DOOR THAT WILL NEVER OPEN."},
};
static const DLine D_END_OPEN[] = {
    {SPK_NARR, "YOU TURN THE KEY. THE RINGS SPIN FASTER, AND FASTER, AND THE GATE'S HEART BLOOMS WHITE."},
    {SPK_NARR, "THE HUSH TURN, ALL OF THEM AT ONCE, AND FLY INTO THE LIGHT LIKE BIRDS GOING HOME."},
    {SPK_HUSH, "T H A N K   Y O U .   W E   W E R E   S O   T I R E D   O F   W A I T I N G ."},
    {SPK_CREWA, "@8"},
    {SPK_NARR, "SOMETHING VAST LOOKS OUT THROUGH THE GATE. IT IS NOT HOSTILE. IT IS NOT FRIENDLY. IT IS CURIOUS."},
};

static const char *const TITLES[] = {
    "PROLOGUE: DEAD LANE", "1. SHAKEDOWN", "2. THE LANE RUNNER", "3. ECHOES", "4. OLD FRIENDS",
    "5. THE MERIDIAN JOB", "6. LANTERN", "7. FARPOINT", "8. THE MAW", "EPILOGUE: FREE FLIGHT"};
static const char *const LOGS[] = {
    "THE LANE FAILED AND BLACK SHIPS CAME OUT OF IT WITH YOU. HAVEN'S LANCERS SAVED YOUR LIFE. COMMANDER VARROW HAS LENT YOU A SHIP FRAME AND TWO LASERS. FIT THEM AT THE SHIPYARD.",
    "VARROW WANTS TO SEE YOU SHOOT BEFORE SHE TRUSTS YOU. YOUR GUNS FIRE BY THEMSELVES - JUST STEER THEM TOWARDS THE DRONES. HOLD A FINGER WHERE YOU WANT TO FLY.",
    "THE CASE WAS BOUND FOR DOCKYARD TOLLE. TAKE THE TRADE LANE: FLY INTO A RING AND TAP LANE. BRAKKA, THE ORNN ENGINEER, WILL KNOW WHAT IT IS.",
    "THE CASE HOLDS A FRAGMENT OF AN ARCHITECT LANE KEY. IT IS SINGING TO ANOTHER PIECE ABOARD THE DERELICT SAINT IVO, OUT IN THE CINDER BELT. USE CRUISE FOR THE LONG HAUL.",
    "THE BLACK SHIPS ARE THE HUSH. THE THIRD FRAGMENT WAS SOLD THROUGH ROOK'S NEST, THE SYNDICATE'S ROCK IN THE BELT. MOTHER SABLE WILL TALK - IF YOU ARE INTERESTING.",
    "SABLE SOLD THE THIRD PIECE TO OSSK, A THRENNIC BROKER AT MERIDIAN EXCHANGE. SHE HAS SENT WORD THAT YOU ARE 'FAMILY'.",
    "THREE FRAGMENTS. THE SYLITH OF LANTERN ABBEY, DEEP IN THE VEIL NEBULA, CAN HEAR WHAT OLD THINGS WANT. TAKE THE LANE FROM MERIDIAN.",
    "THE ARCHITECTS NEVER VANISHED - THEY WENT THROUGH THE GATE AND LOCKED IT. THE HUSH GUARD THE DOOR. BRAKKA HAS RELIT THE FARPOINT LANE, AND FARPOINT IS UNDER SIEGE.",
    "SOMETHING HUGE SITS IN THE CRACK OF THE GATE: THE CHOIR. KILL IT, THEN DECIDE WHAT TO DO WITH THE KEY.",
    "THE STORY IS DONE, BUT THE REACH IS STILL YOURS. TAKE JOBS, TRADE BETWEEN STATIONS, AND BUILD THE SHIP YOU ALWAYS WANTED.",
};
const char *missionTitle() { return TITLES[clampi(mis, 0, MS_FREE)]; }
const char *missionLog() { return LOGS[clampi(mis, 0, MS_FREE)]; }

/* ================================================================ helpers */
static int countMissionHostiles() {
  int n = 0;
  for (int i = FIRST_DYN; i < MAXSHIP; i++)
    if (ships[i].on && (ships[i].flags & SF_MISSION) && (ships[i].flags & SF_HOSTILE)) n++;
  return n;
}
static int nearestMissionHostile() {
  Ship &p = ships[PL];
  int b = -1;
  float bd = 1e18f;
  for (int i = FIRST_DYN; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on || !(s.flags & SF_HOSTILE) || !(s.flags & (SF_MISSION | SF_JOB))) continue;
    float d = dist2(s.x, s.y, p.x, p.y);
    if (d < bd) { bd = d; b = i; }
  }
  return b;
}
static void spawnRing(int des, float cx, float cy, float r, int n, uint16_t fl) {
  Ship &p = ships[PL];
  for (int k = 0; k < n; k++) {
    float a = frand() * TAUF;
    float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
    int i = spawnShip(des, x, y, angTo(p.x - x, p.y - y), fl, AI_ATTACK);
    if (i >= 0) {
      ships[i].target = PL;
      if (designs[des].fac == F_HUSH) spawnPart(x, y, 1.5f, 0, 16, C_V4, PK_RING);
    }
  }
}
static float distTo(float x, float y) { return lenf(ships[PL].x - x, ships[PL].y - y); }
static float distSt(int st) { return distTo(STATIONS[st].x, STATIONS[st].y); }

/* ================================================================== story */
void storyNewGame() {
  mis = MS_INTRO;
  mstep = 0;
  mT = 0;
  ending = 0;
  m2Dropped = false;
  dlgBegin();
  dlgLines(D_M0, N(D_M0));
  dlgRun(DT_M0);
}

static void spawnWreckIvo() {
  for (int i = FIRST_DYN; i < MAXSHIP; i++)
    if (ships[i].on && ships[i].kind == K_WRECK && (ships[i].flags & SF_MISSION)) return;
  int i = spawnShip(D_HAULER, WRECK_X, WRECK_Y, 0.7f, SF_MISSION, AI_DRIFT);
  if (i >= 0) ships[i].kind = K_WRECK;
}

static void nestWave() {
  uint16_t fl = SF_MISSION | SF_HOSTILE | SF_ATTACKER;
  if (waveN == 0) spawnRing(D_MITE, NEST_X, NEST_Y, 120, 3, fl);
  else if (waveN == 1) {
    spawnRing(D_MITE, NEST_X, NEST_Y, 140, 2, fl);
    spawnRing(D_SHARD, NEST_X, NEST_Y, 120, 2, fl);
  } else {
    spawnRing(D_HWARDEN, NEST_X, NEST_Y, 80, 1, fl);
    spawnRing(D_SHARD, NEST_X, NEST_Y, 140, 2, fl);
  }
  toast("THE NEST STIRS - WAVE %d OF 3", waveN + 1);
}

void storyTick() {
  mT += DT;
  Ship &p = ships[PL];
  switch (mis) {
  case MS_SHAKEDOWN:
    if (mstep == 0 && countMissionHostiles() == 0 && mT > 1) {
      mstep = 1;
      dlgBegin();
      dlgLines(D_M1, N(D_M1));
      dlgRun(DT_M1);
    }
    break;
  case MS_LANE:
    if (mstep == 1 && countMissionHostiles() == 0) {
      mstep = 2;
      crewBark(BK_WIN);
      toast("RE-ENTER THE LANE OR CRUISE TO TOLLE");
    }
    break;
  case MS_ECHOES:
    if (mstep == 0 && distTo(WRECK_X, WRECK_Y) < 3000) spawnWreckIvo();
    if (mstep == 1 && countMissionHostiles() == 0) {
      mstep = 2;
      toast("LANE KEY FRAGMENT RECOVERED");
      dlgBegin();
      dlgLines(D_M3_CLEAR, N(D_M3_CLEAR));
      dlgRun(DT_INFO);
    }
    break;
  case MS_SABLE:
    if (mstep == 1 && distTo(NEST_X, NEST_Y) < 650) {
      mstep = 2;
      waveN = 0;
      nestWave();
      crewBark(BK_HUSH);
    } else if (mstep == 2 && countMissionHostiles() == 0) {
      waveT += DT;
      if (waveT > 3) {
        waveT = 0;
        waveN++;
        if (waveN < 3) nestWave();
        else {
          mstep = 3;
          bark(SPK_SABLE, -1, "HA! I SAW THAT FROM HERE, COURIER. COME COLLECT YOUR PAY.");
        }
      }
    }
    break;
  case MS_MERIDIAN:
    if (mstep == 1 && !raid.on && distSt(ST_MERIDIAN) < 1500) {
      startRaid(ST_MERIDIAN, 3, F_HUSH, true);
      mstep = 2;
    }
    break;
  case MS_FARPOINT:
    if (mstep == 0 && !raid.on && distSt(ST_FARPOINT) < 1700 && !lr.on) {
      startRaid(ST_FARPOINT, 4, F_HUSH, true);
      const StationDef &F = STATIONS[ST_FARPOINT];
      for (int k = 0; k < 3; k++) {
        int i = spawnShip(k < 2 ? D_WARDEN : D_LANCER, p.x - 120 + k * 50, p.y + 80, p.ang,
                          SF_DEFENDER | SF_RAID | SF_MISSION, AI_DEFEND);
        if (i >= 0) ships[i].anchor = (int8_t)stationShip(ST_FARPOINT);
      }
      (void)F;
      bark(SPK_VARROW, -1, "LANCERS, WEAPONS FREE! COURIER, ON MY WING - LET'S BREAK THIS SIEGE!");
      mstep = 1;
    }
    break;
  case MS_MAW:
    if (mstep == 0 && distTo(GATE_X, GATE_Y) < 1150) {
      float a = angTo(p.x - GATE_X, p.y - GATE_Y);
      float cx = GATE_X + sinf(a) * 60, cy = GATE_Y - cosf(a) * 60;
      int i = spawnShip(D_CHOIR, cx, cy, a, SF_MISSION | SF_HOSTILE | SF_BOSS | SF_ATTACKER, AI_ATTACK);
      if (i >= 0) {
        ships[i].target = PL;
        spawnPart(cx, cy, 3, 0, 22, C_V5, PK_RING);
      }
      spawnRing(D_SHARD, cx, cy, 90, 3, SF_MISSION | SF_HOSTILE | SF_ATTACKER);
      bark(SPK_HUSH, -1, "Y O U   C A R R Y   T H E   K E Y .   G I V E   I T   T O   U S .");
      mstep = 1;
      mT = 0;
    } else if (mstep == 1) {
      if (countFlag(SF_BOSS) == 0) {
        mstep = 2;
        toast("THE CHOIR FALLS SILENT");
        for (int i = FIRST_DYN; i < MAXSHIP; i++)
          if (ships[i].on && ships[i].fac == F_HUSH) {
            explode(ships[i].x, ships[i].y, 8);
            removeShip(i);
          }
        crewBark(BK_WIN);
      } else if (mT > 22) {
        mT = 0;
        for (int i = FIRST_DYN; i < MAXSHIP; i++)
          if (ships[i].on && (ships[i].flags & SF_BOSS)) {
            spawnRing(D_MITE, ships[i].x, ships[i].y, 70, 2, SF_MISSION | SF_HOSTILE | SF_ATTACKER);
            break;
          }
      }
    } else if (mstep == 2 && distTo(GATE_X, GATE_Y) < 170) {
      mstep = 3;
      dlgBegin();
      dlgLines(D_M8, N(D_M8));
      dlgOpt("SEAL THE GATE", 1);
      dlgOpt("OPEN THE GATE", 2);
      dlgRun(DT_M8);
    }
    break;
  default:
    break;
  }
}

bool storyOnDock(int st) {
  const DLine *l = nullptr;
  int n = 0, tag = 0;
  switch (mis) {
  case MS_LANE:
    if (st == ST_TOLLE) { l = D_M2_DONE; n = N(D_M2_DONE); tag = DT_M2_DONE; }
    break;
  case MS_ECHOES:
    if (st == ST_TOLLE && mstep == 2) { l = D_M3_DONE; n = N(D_M3_DONE); tag = DT_M3_DONE; }
    break;
  case MS_SABLE:
    if (st == ST_ROOK && mstep == 0) { l = D_M4_MEET; n = N(D_M4_MEET); tag = DT_M4_MEET; }
    if (st == ST_ROOK && mstep == 3) { l = D_M4_DONE; n = N(D_M4_DONE); tag = DT_M4_DONE; }
    break;
  case MS_MERIDIAN:
    if (st == ST_MERIDIAN && mstep == 0) { l = D_M5_MEET; n = N(D_M5_MEET); tag = DT_M5_MEET; }
    if (st == ST_MERIDIAN && mstep == 3) { l = D_M5_DONE; n = N(D_M5_DONE); tag = DT_M5_DONE; }
    break;
  case MS_ABBEY:
    if (st == ST_LANTERN && mstep == 0) { l = D_M6; n = N(D_M6); tag = DT_M6; }
    if (st == ST_TOLLE && mstep == 1) { l = D_M6B; n = N(D_M6B); tag = DT_M6B; }
    break;
  case MS_FARPOINT:
    if (st == ST_FARPOINT && mstep == 2) { l = D_M7; n = N(D_M7); tag = DT_M7; }
    break;
  default:
    break;
  }
  if (!l) return false;
  dlgBegin();
  dlgLines(l, n);
  if (tag == DT_M5_MEET) dlgOpt("LAUNCH!", 1);
  dlgRun(tag);
  return true;
}

void storyOnLaunch() {
  Ship &p = ships[PL];
  if (mis == MS_INTRO && mstep >= 1) {
    mis = MS_SHAKEDOWN;
    mstep = 0;
    mT = 0;
    for (int k = 0; k < 3; k++) {
      float a = p.ang + (k - 1) * 0.5f;
      float r = 260 + k * 60;
      int i = spawnShip(D_DRONE, p.x + sinf(a) * r, p.y - cosf(a) * r, frand() * TAUF,
                        SF_MISSION | SF_HOSTILE | SF_DRONE, AI_DRIFT);
      if (i >= 0) ships[i].vx = frange(-8, 8);
    }
    bark(SPK_VARROW, -1, "THREE TARGET DRONES OFF YOUR BOW. HOLD A FINGER WHERE YOU WANT TO FLY - YOUR GUNS WILL DO THE REST.");
  }
  if (mis == MS_MERIDIAN && mstep == 1 && !raid.on) {
    startRaid(ST_MERIDIAN, 3, F_HUSH, true);
    mstep = 2;
  }
}

void storyOnKill(int si) {
  Ship &s = ships[si];
  if ((s.flags & SF_BOSS)) shake = 14;
}

void storyOnScan(int si) {
  ships[si].flags |= SF_SCANNED;
  if (mis == MS_ECHOES && mstep == 0) {
    dlgBegin();
    dlgLines(D_M3_SCAN, N(D_M3_SCAN));
    dlgRun(DT_M3_SCAN);
  }
}

void storyOnLane(int lane, float frac) {
  if (mis != MS_LANE || mstep != 0 || lane != 0) return;
  if (frac < 0) {
    if (!m2Dropped) {
      lr.dropAt = 0.45f;
      lr.dropped = true;
    }
    return;
  }
  if (!m2Dropped && frac >= 0.45f) {
    m2Dropped = true;
    mstep = 1;
    Ship &p = ships[PL];
    spawnRing(D_RAIDER, p.x, p.y, 230, 2, SF_MISSION | SF_HOSTILE | SF_ATTACKER);
    dlgBegin();
    dlgLines(D_M2_AMBUSH, N(D_M2_AMBUSH));
    dlgRun(DT_M2_AMBUSH);
  }
}

void storyOnRaidEnd(bool won) {
  if (!won) return;
  if (mis == MS_MERIDIAN && mstep == 2) {
    mstep = 3;
    bark(SPK_OSSK, -1, "THE BOOKS BALANCE! COME INSIDE, COURIER. COME INSIDE QUICKLY, BEFORE I CHANGE MY MIND ABOUT 'FREE'.");
  } else if (mis == MS_FARPOINT && mstep == 1) {
    mstep = 2;
    bark(SPK_VARROW, -1, "THAT'S THE LAST OF THEM. GOOD FLYING, COURIER. DOCK AT FARPOINT - WE NEED TO TALK.");
  }
}

void storyRevert() {
  clearFlag(SF_MISSION);
  raid.on = false;
  switch (mis) {
  case MS_SHAKEDOWN: mis = MS_INTRO; mstep = 1; break;
  case MS_LANE: if (mstep == 1) mstep = 0; m2Dropped = false; break;
  case MS_ECHOES: if (mstep == 1) mstep = 0; break;
  case MS_SABLE: if (mstep == 2) mstep = 1; break;
  case MS_MERIDIAN: if (mstep == 2) mstep = 1; break;
  case MS_FARPOINT: if (mstep == 1) mstep = 0; break;
  case MS_MAW: if (mstep == 1) mstep = 0; if (mstep == 3) mstep = 2; break;
  default: break;
  }
}

bool storyBlocksLaunch(const char **why) {
  if (mis == MS_INTRO && gridCount(designs[D_PLAYER].g, M_LASER) < 2) {
    *why = "VARROW: FIT BOTH LASERS FIRST";
    return true;
  }
  return false;
}

bool storyMarker(int st) {
  switch (mis) {
  case MS_LANE: return st == ST_TOLLE;
  case MS_ECHOES: return mstep == 2 && st == ST_TOLLE;
  case MS_SABLE: return (mstep == 0 || mstep == 3) && st == ST_ROOK;
  case MS_MERIDIAN: return (mstep == 0 || mstep == 3) && st == ST_MERIDIAN;
  case MS_ABBEY: return mstep == 0 ? st == ST_LANTERN : st == ST_TOLLE;
  case MS_FARPOINT: return st == ST_FARPOINT;
  default: return false;
  }
}

const char *objective() {
  switch (mis) {
  case MS_INTRO: return mstep >= 1 ? "FIT 2 LASERS IN THE SHIPYARD, THEN LAUNCH" : "";
  case MS_SHAKEDOWN: return fmt("SHOOT THE TARGET DRONES (%d LEFT)", countMissionHostiles());
  case MS_LANE:
    if (mstep == 1) return "DESTROY THE RAIDERS";
    if (mstep == 0 && !lr.on && distSt(ST_TOLLE) > 4000) return "TAKE THE LANE TO TOLLE";
    return "DOCK AT DOCKYARD TOLLE";
  case MS_ECHOES:
    if (mstep == 0) return "SCAN THE WRECK OF THE SAINT IVO";
    if (mstep == 1) return "SURVIVE THE HUSH";
    return "RETURN TO DOCKYARD TOLLE";
  case MS_SABLE:
    if (mstep == 0) return "FIND MOTHER SABLE AT ROOK'S NEST";
    if (mstep == 1) return "BURN THE NEST AT KELL'S GRAVE";
    if (mstep == 2) return fmt("DESTROY THE NEST (WAVE %d/3)", waveN + 1);
    return "RETURN TO ROOK'S NEST";
  case MS_MERIDIAN:
    if (mstep == 0) return "MEET OSSK AT MERIDIAN EXCHANGE";
    if (mstep == 1) return "DEFEND MERIDIAN EXCHANGE";
    if (mstep == 2) return fmt("DEFEND MERIDIAN (WAVE %d/%d)", raid.wave, raid.waves);
    return "DOCK AT MERIDIAN EXCHANGE";
  case MS_ABBEY: return mstep == 0 ? "TAKE THE FRAGMENTS TO LANTERN ABBEY" : "RETURN TO BRAKKA AT TOLLE";
  case MS_FARPOINT:
    if (mstep == 0) return "TAKE THE FARPOINT LANE";
    if (mstep == 1) return fmt("BREAK THE SIEGE (WAVE %d/%d)", raid.wave, raid.waves);
    return "DOCK AT FARPOINT";
  case MS_MAW:
    if (mstep <= 1) return "DESTROY THE CHOIR AT THE GATE";
    return "FLY INTO THE HEART OF THE GATE";
  default:
    if (job.type) return jobObjective();
    return "";
  }
}

static bool jobWaypoint(float &x, float &y) {
  switch (job.type) {
  case JB_BOUNTY:
  case JB_SALVAGE:
    x = job.x;
    y = job.y;
    return true;
  case JB_DELIVER:
  case JB_DEFEND:
    x = STATIONS[job.to].x;
    y = STATIONS[job.to].y;
    return true;
  default: return false;
  }
}

bool waypoint(float &x, float &y) {
  auto st = [&](int s) {
    x = STATIONS[s].x;
    y = STATIONS[s].y;
    return true;
  };
  int h;
  switch (mis) {
  case MS_SHAKEDOWN:
    h = nearestMissionHostile();
    if (h < 0) return false;
    x = ships[h].x; y = ships[h].y;
    return true;
  case MS_LANE:
    if (mstep == 1 && (h = nearestMissionHostile()) >= 0) { x = ships[h].x; y = ships[h].y; return true; }
    if (mstep == 0 && !lr.on && distTo(LANES[0].ax, LANES[0].ay) < distSt(ST_TOLLE) - 3000) {
      x = LANES[0].ax; y = LANES[0].ay;
      return true;
    }
    return st(ST_TOLLE);
  case MS_ECHOES:
    if (mstep == 0) { x = WRECK_X; y = WRECK_Y; return true; }
    if (mstep == 1 && (h = nearestMissionHostile()) >= 0) { x = ships[h].x; y = ships[h].y; return true; }
    return st(ST_TOLLE);
  case MS_SABLE:
    if (mstep == 1 || mstep == 2) { x = NEST_X; y = NEST_Y; return true; }
    return st(ST_ROOK);
  case MS_MERIDIAN: return st(ST_MERIDIAN);
  case MS_ABBEY: return st(mstep == 0 ? ST_LANTERN : ST_TOLLE);
  case MS_FARPOINT: return st(ST_FARPOINT);
  case MS_MAW: x = GATE_X; y = GATE_Y; return true;
  case MS_INTRO: return false;
  default: return jobWaypoint(x, y);
  }
}

/* ================================================================= hails */
static int offerType, offerArg, offerPrice, tollAmt;

static void groupDo(int lead, uint8_t ai, bool hostile) {
  uint8_t g = ships[lead].group;
  Ship &p = ships[PL];
  for (int i = FIRST_DYN; i < MAXSHIP; i++) {
    Ship &s = ships[i];
    if (!s.on || s.group != g) continue;
    s.flags &= (uint16_t)~SF_HAIL;
    s.flags |= SF_HAILED;
    if (hostile) {
      s.flags |= SF_HOSTILE | SF_ATTACKER;
      s.target = PL;
    }
    s.ai = ai;
    if (ai == AI_TRAVEL) {
      int st = irand(ST_COUNT);
      if (st == ST_ROOK) st = ST_MERIDIAN;
      s.tx = STATIONS[st].x;
      s.ty = STATIONS[st].y;
    }
    if (ai == AI_PATROL) {
      s.tx = s.x + (s.x - p.x);
      s.ty = s.y + (s.y - p.y);
      s.aiT = 6;
    }
  }
}

void startHail(int si) {
  Ship &s = ships[si];
  hailShip = si;
  dlgBegin();
  if (s.fac == F_SYND) {
    if (rep[F_SYND] >= 40) {
      dlgLine(SPK_PIRATE, "SABLE'S FRIEND? FLY ON, THEN. TELL HER WE SAID HELLO. AND THAT WE'RE BEHAVING.");
      dlgOpt("FLY ON", 6);
    } else {
      tollAmt = 100 + mis * 40;
      dlgLine(SPK_PIRATE, PIRATE_HAILS[irand(4)]);
      dlgOpt("FIGHT", 1);
      if (credits >= tollAmt) dlgOpt(fmt("PAY %d CR", tollAmt), 2);
      if (cargoUsed() - cargo[CM_PARCEL] > 0) dlgOpt("DUMP CARGO", 3);
      if (cm.envoy) dlgOpt("BLUFF (ENVOY)", 4);
      else if (cm.gunner) dlgOpt("THREATEN (GUNNER)", 5);
    }
    dlgRun(DT_HAIL_PIRATE);
  } else if (s.fac == F_THREN) {
    dlgLine(SPK_TRADER, TRADER_HAILS[irand(3)]);
    offerType = irand(3);
    if (offerType == 2 && cargo[CM_RELIC] == 0) offerType = 1;
    if (offerType == 0) {
      static const uint8_t mods[10] = {M_SHIELD, M_LASER, M_CANNON, M_MISSILE, M_TURRET, M_REACTOR, M_ARMOR, M_CRUISE, M_GYRO, M_SCANNER};
      offerArg = mods[irand(10)];
      offerPrice = MODS[offerArg].price * 7 / 10;
      dlgLinef(SPK_TRADER, "TODAY WE OFFER ONE %s. SALVAGED, BARELY SCORCHED. %d CREDITS, AND WE THROW IN OUR BLESSING.", MODS[offerArg].name, offerPrice);
      if (credits >= offerPrice) dlgOpt(fmt("BUY %d CR", offerPrice), 1);
    } else if (offerType == 1) {
      static const uint8_t goods[3] = {CM_SPICE, CM_MED, CM_ALLOY};
      offerArg = goods[irand(3)];
      offerPrice = COMS[offerArg].base * 5 * 3 / 4;
      dlgLinef(SPK_TRADER, "FIVE CRATES OF %s FOR %d CREDITS. A STEAL. FOR YOU, NOT FOR US.", COMS[offerArg].name, offerPrice);
      if (credits >= offerPrice && cargoUsed() + 5 <= cargoCap()) dlgOpt(fmt("BUY 5 FOR %d", offerPrice), 1);
    } else {
      offerPrice = COMS[CM_RELIC].base * 13 / 10;
      dlgLinef(SPK_TRADER, "YOUR HOLD SINGS, CAPTAIN. RELICS? WE PAY %d EACH. NO QUESTIONS. QUESTIONS ARE BAD FOR BUSINESS.", offerPrice);
      dlgOpt(fmt("SELL %d RELICS", cargo[CM_RELIC]), 1);
    }
    dlgOpt("NO THANKS", 0);
    dlgRun(DT_HAIL_TRADER);
  } else {
    if (cargo[CM_RELIC] > 0) {
      tollAmt = 100 * cargo[CM_RELIC];
      dlgLine(SPK_PATROL, "REACH AUTHORITY. ROUTINE SCAN... THAT'S ARCHITECT SALVAGE IN YOUR HOLD, CAPTAIN. HAND IT OVER OR PAY THE FINE.");
      if (credits >= tollAmt) dlgOpt(fmt("PAY FINE %d", tollAmt), 1);
      dlgOpt("HAND OVER RELICS", 2);
      dlgOpt("RUN FOR IT", 3);
    } else {
      static const char *const lines[3] = {
          "REACH AUTHORITY PATROL. ROUTINE SCAN... YOU'RE CLEAN. FLY SAFE, CAPTAIN.",
          "PATROL HERE. SYNDICATE ACTIVITY REPORTED IN THIS SECTOR. KEEP YOUR SHIELDS UP.",
          "EVENING, CAPTAIN. VARROW SAYS TO KEEP AN EYE OUT FOR BLACK SHIPS. WE ALL ARE."};
      dlgLine(SPK_PATROL, lines[irand(3)]);
      dlgOpt("CARRY ON", 0);
    }
    dlgRun(DT_HAIL_PATROL);
  }
}

/* ================================================================== loot */
static void wreckLoot(int i) {
  ships[i].flags |= SF_SCANNED;
  dlgBegin();
  int r = irand(100);
  if (cm.seer && r >= 85 && irand(2)) r = irand(85);
  if (r < 35) {
    int c = 80 + irand(220);
    if (cm.seer) c = c * 3 / 2;
    credits += c;
    dlgLinef(SPK_NARR, "THE WRECK'S STRONGBOX IS STILL SEALED. YOU CRACK IT AND FIND %d CREDITS IN CHITS.", c);
  } else if (r < 55) {
    static const uint8_t mods[8] = {M_HULL, M_ARMOR, M_LASER, M_SHIELD, M_ENGINE, M_CARGO, M_CANNON, M_GYRO};
    uint8_t m = mods[irand(8)];
    inv[m]++;
    dlgLinef(SPK_NARR, "ONE MODULE SURVIVED INTACT: A %s. YOUR CREW CUT IT FREE. FIT IT AT ANY SHIPYARD.", MODS[m].name);
  } else if (r < 70) {
    int c = irand(2) ? CM_RELIC : CM_ORE, n = 2 + irand(3);
    int room = cargoCap() - cargoUsed();
    if (n > room) n = room;
    if (n > 0) {
      cargo[c] += n;
      dlgLinef(SPK_NARR, "THE HOLD STILL HAS CARGO. YOU TAKE %d CRATES OF %s.", n, COMS[c].name);
    } else {
      dlgLine(SPK_NARR, "THE HOLD IS FULL OF CARGO, BUT YOUR OWN HOLD HAS NO ROOM.");
    }
  } else if (r < 85) {
    int c = 40 + irand(60);
    credits += c;
    dlgLine(SPK_NARR, LORE_LOGS[irand(8)]);
    dlgLinef(SPK_NARR, "THE DATA CORE SELLS FOR %d CREDITS.", c);
  } else {
    dlgLine(SPK_NARR, "THE WRECK'S BEACON WAS BAIT. CONTACTS DROPPING IN ALL AROUND YOU!");
    spawnGroup(ENC_AMBUSH, ships[PL].x, ships[PL].y);
  }
  dlgRun(DT_WRECK);
}

void scanTick() {
  if (scanTarget < 0) return;
  Ship &w = ships[scanTarget];
  Ship &p = ships[PL];
  if (!w.on || w.kind != K_WRECK || (w.flags & SF_SCANNED) || dist2(w.x, w.y, p.x, p.y) > 100.0f * 100.0f) {
    if (w.on) w.scan = 0;
    scanTarget = -1;
    return;
  }
  w.scan += DT / 3.0f;
  if (w.scan >= 1) {
    w.scan = 0;
    int i = scanTarget;
    scanTarget = -1;
    if (w.flags & SF_MISSION) storyOnScan(i);
    else if (w.flags & SF_JOB) jobOnScan(i);
    else wreckLoot(i);
  }
}

/* ================================================================== jobs */
static const char *bearing(float dx, float dy) {
  static const char *const D[8] = {"NORTH", "NORTH-EAST", "EAST", "SOUTH-EAST", "SOUTH", "SOUTH-WEST", "WEST", "NORTH-WEST"};
  float a = angTo(dx, dy);
  int k = ((int)floorf((a + PIF / 8) / (PIF / 4)) % 8 + 8) % 8;
  return D[k];
}

void genJobs(int st) {
  const StationDef &S = STATIONS[st];
  for (int k = 0; k < 3; k++) {
    Job &o = offers[k];
    memset(&o, 0, sizeof(o));
    if (mis < MS_LANE) continue;
    int t = 1 + irand(4);
    if (t == JB_DEFEND && (S.fac == F_SYND || mis < MS_ECHOES)) t = JB_BOUNTY;
    o.type = (uint8_t)t;
    o.from = (uint8_t)st;
    float a = frand() * TAUF, d = frange(1500, 3200);
    o.x = S.x + sinf(a) * d;
    o.y = S.y - cosf(a) * d;
    if (lenf(o.x, o.y) < 700) { o.x *= 2; o.y *= 2; }
    switch (t) {
    case JB_BOUNTY:
      o.fac = (mis >= MS_ECHOES && ending == 0 && irand(2)) ? F_HUSH : F_SYND;
      if (S.fac == F_SYND && ending == 0) o.fac = F_HUSH;
      o.count = (uint8_t)(2 + irand(2) + mis / 3);
      o.reward = (uint16_t)(250 + o.count * 90 + mis * 30);
      break;
    case JB_DELIVER: {
      int to;
      int tries = 0;
      do {
        to = irand(ST_COUNT);
      } while ((to == st || !(known & (1 << to)) || stationHostile(to) || (to == ST_FARPOINT && !laneOpen[3])) && ++tries < 20);
      if (tries >= 20) { o.type = JB_BOUNTY; o.fac = F_SYND; o.count = 2; o.reward = 300; break; }
      o.to = (uint8_t)to;
      o.count = (uint8_t)(3 + irand(4));
      o.reward = (uint16_t)(90 + lenf(STATIONS[to].x - S.x, STATIONS[to].y - S.y) * 0.05f + o.count * 18);
      break;
    }
    case JB_SALVAGE:
      o.reward = (uint16_t)(220 + irand(200));
      break;
    case JB_DEFEND: {
      o.to = (uint8_t)st;
      o.count = (uint8_t)(2 + irand(2));
      o.fac = (mis >= MS_ECHOES && ending == 0 && irand(2)) ? F_HUSH : F_SYND;
      o.reward = (uint16_t)(380 + o.count * 140);
      break;
    }
    }
  }
}

const char *jobText(const Job &j) {
  const StationDef &F = STATIONS[j.from];
  switch (j.type) {
  case JB_BOUNTY:
    return fmt("BOUNTY: %d %s SPOTTED %s OF %s. DESTROY THEM ALL.", j.count,
               j.fac == F_HUSH ? "HUSH SHIPS" : "SYNDICATE RAIDERS", bearing(j.x - F.x, j.y - F.y), F.shortName);
  case JB_DELIVER:
    return fmt("DELIVERY: CARRY %d PARCELS TO %s. NEEDS %d CARGO SPACE.", j.count, STATIONS[j.to].name, j.count);
  case JB_SALVAGE:
    return fmt("SALVAGE: RECOVER A BLACK BOX FROM A WRECK %s OF %s. SCAN IT.", bearing(j.x - F.x, j.y - F.y), F.shortName);
  case JB_DEFEND:
    return fmt("DEFENCE: %s ATTACK EXPECTED NEAR %s. LAUNCH AND HOLD THE LINE.", j.fac == F_HUSH ? "HUSH" : "SYNDICATE", STATIONS[j.to].shortName);
  default: return "";
  }
}

const char *jobObjective() {
  switch (job.type) {
  case JB_BOUNTY: return job.stage ? "JOB: DESTROY THE BOUNTY TARGETS" : "JOB: HUNT THE BOUNTY TARGETS";
  case JB_DELIVER: return fmt("JOB: DELIVER PARCELS TO %s", STATIONS[job.to].shortName);
  case JB_SALVAGE: return "JOB: SCAN THE WRECK";
  case JB_DEFEND: return fmt("JOB: DEFEND %s", STATIONS[job.to].shortName);
  default: return "";
  }
}

static void jobPay(const char *what) {
  credits += job.reward;
  rep[STATIONS[job.from].fac] += 3;
  bark(SPK_CTRL, -1, fmt("%s COMPLETE. %d CREDITS TRANSFERRED FROM %s.", what, job.reward, STATIONS[job.from].shortName));
  job.type = JB_NONE;
}

void jobTick() {
  Ship &p = ships[PL];
  switch (job.type) {
  case JB_BOUNTY:
    if (job.stage == 0 && distTo(job.x, job.y) < 700) {
      job.stage = 1;
      int n = job.count;
      for (int k = 0; k < n; k++) {
        int des = job.fac == F_HUSH ? (k == 0 && mis >= MS_SABLE ? D_SHARD : D_MITE) : (k == 0 && mis >= MS_SABLE ? D_CORSAIR : D_RAIDER);
        float a = frand() * TAUF;
        int i = spawnShip(des, job.x + cosf(a) * 90, job.y + sinf(a) * 90, angTo(p.x - job.x, p.y - job.y),
                          SF_JOB | SF_HOSTILE | SF_ATTACKER, AI_ATTACK);
        if (i >= 0) ships[i].target = PL;
      }
      crewBark(job.fac == F_HUSH ? BK_HUSH : BK_COMBAT);
    } else if (job.stage == 1 && countFlag(SF_JOB) == 0) {
      jobPay("BOUNTY");
    }
    break;
  case JB_SALVAGE:
    if (job.stage == 0 && distTo(job.x, job.y) < 900) {
      job.stage = 1;
      int i = spawnShip(irand(2) ? D_CORSAIR : D_HAULER, job.x, job.y, frand() * TAUF, SF_JOB, AI_DRIFT);
      if (i >= 0) ships[i].kind = K_WRECK;
    }
    break;
  case JB_DEFEND:
    if (job.stage == 0 && !raid.on && distSt(job.to) < 1300) {
      job.stage = 1;
      raidCooldown = 0;
      startRaid(job.to, job.count, job.fac, false);
    } else if (job.stage == 1 && !raid.on) {
      if (raidWon) jobPay("DEFENCE CONTRACT");
      else {
        toast("DEFENCE JOB FAILED");
        job.type = JB_NONE;
      }
    }
    break;
  default:
    break;
  }
}

bool jobOnDock(int st) {
  if ((job.type == JB_BOUNTY || job.type == JB_SALVAGE) && job.stage == 1) job.stage = 0;
  if (job.type == JB_DEFEND && job.stage == 1) job.stage = 0;
  if (job.type == JB_DELIVER && job.to == st) {
    cargo[CM_PARCEL] = 0;
    credits += job.reward;
    rep[STATIONS[st].fac] += 3;
    dlgBegin();
    dlgLinef(SPK_CTRL, "PARCELS RECEIVED, CAPTAIN. %d CREDITS TRANSFERRED. ALWAYS NICE TO SEE A COURIER WHO DELIVERS.", job.reward);
    dlgRun(DT_JOBDONE);
    job.type = JB_NONE;
    return true;
  }
  return false;
}

void jobOnKill(int si) { (void)si; }

void jobOnScan(int si) {
  ships[si].flags |= SF_SCANNED;
  ships[si].flags &= (uint16_t)~SF_JOB;
  if (job.type != JB_SALVAGE) return;
  dlgBegin();
  dlgLine(SPK_NARR, "THE BLACK BOX IS INTACT. ITS LAST ENTRY:");
  dlgLine(SPK_NARR, LORE_LOGS[irand(8)]);
  bool ambush = irand(100) < (cm.seer ? 15 : 30);
  if (ambush) dlgLine(SPK_NARR, "SOMEBODY ELSE WANTED THIS BOX. CONTACTS INBOUND!");
  dlgRun(DT_INFO);
  if (ambush) spawnGroup(ENC_AMBUSH, ships[PL].x, ships[PL].y);
  jobPay("SALVAGE");
}

/* =============================================================== economy */
int priceAt(int st, int c, bool buy) {
  float base = COMS[c].base * (100 + STATIONS[st].price[c]) / 100.0f;
  base *= 1 + 0.08f * sinf(playTime * 0.004f + c * 1.7f + st * 2.3f);
  float p = base * (buy ? cm.tradeBuy : cm.tradeSell);
  int v = (int)(p + 0.5f);
  return v < 1 ? 1 : v;
}
int cargoUsed() {
  int n = 0;
  for (int c = 0; c < NCOM; c++) n += cargo[c];
  return n;
}
int cargoCap() { return designs[D_PLAYER].st.cargo; }

/* ============================================================ dialog end */
void onDialog(int tag, int choice) {
  Ship &p = ships[PL];
  switch (tag) {
  case DT_M0: mstep = 1; break;
  case DT_M1: mis = MS_LANE; mstep = 0; m2Dropped = false; break;
  case DT_M2_DONE:
    credits += 600;
    inv[M_SHIELD]++;
    known |= KN_WRECK;
    mis = MS_ECHOES;
    mstep = 0;
    saveGame();
    break;
  case DT_M3_SCAN: {
    mstep = 1;
    uint16_t fl = SF_MISSION | SF_HOSTILE | SF_ATTACKER;
    spawnRing(D_MITE, p.x, p.y, 250, 3, fl);
    spawnRing(D_SHARD, p.x, p.y, 280, 1, fl);
    crewBark(BK_HUSH);
    break;
  }
  case DT_M3_DONE:
    credits += 700;
    known |= (uint8_t)(1 << ST_ROOK);
    mis = MS_SABLE;
    mstep = 0;
    saveGame();
    break;
  case DT_M4_MEET:
    mstep = 1;
    known |= KN_NEST;
    saveGame();
    break;
  case DT_M4_DONE:
    credits += 900;
    inv[M_MISSILE]++;
    rep[F_SYND] = rep[F_SYND] < 0 ? 45 : rep[F_SYND] + 45;
    mis = MS_MERIDIAN;
    mstep = 0;
    saveGame();
    break;
  case DT_M5_MEET:
    mstep = 1;
    saveGame();
    launchFrom(ST_MERIDIAN);
    break;
  case DT_M5_DONE:
    credits += 800;
    inv[M_TURRET]++;
    rep[F_THREN] += 30;
    mis = MS_ABBEY;
    mstep = 0;
    saveGame();
    break;
  case DT_M6:
    credits += 500;
    inv[M_REPAIR]++;
    knownGate = true;
    known |= (uint8_t)(1 << ST_FARPOINT);
    mstep = 1;
    saveGame();
    break;
  case DT_M6B:
    laneOpen[3] = true;
    mis = MS_FARPOINT;
    mstep = 0;
    saveGame();
    break;
  case DT_M7:
    credits += 1000;
    inv[M_ARMOR] += 2;
    p.hp = p.hpMax;
    mis = MS_MAW;
    mstep = 0;
    saveGame();
    break;
  case DT_M8:
    ending = choice == 1 ? 1 : 2;
    dlgBegin();
    if (ending == 1) dlgLines(D_END_SEAL, N(D_END_SEAL));
    else dlgLines(D_END_OPEN, N(D_END_OPEN));
    dlgRun(ending == 1 ? DT_END_SEAL : DT_END_OPEN);
    break;
  case DT_END_SEAL:
  case DT_END_OPEN:
    mis = MS_FREE;
    mstep = 0;
    for (int i = FIRST_DYN; i < MAXSHIP; i++)
      if (ships[i].on && ships[i].fac == F_HUSH) removeShip(i);
    mode = MD_ENDING;
    break;
  case DT_HAIL_PIRATE: {
    if (hailShip < 0) break;
    switch (choice) {
    case 1:
      groupDo(hailShip, AI_ATTACK, true);
      crewBark(BK_COMBAT);
      break;
    case 2:
      credits -= tollAmt;
      groupDo(hailShip, AI_FLEE, false);
      toast("TOLL PAID");
      break;
    case 3:
      for (int c = 0; c < CM_PARCEL; c++) cargo[c] = 0;
      groupDo(hailShip, AI_FLEE, false);
      toast("CARGO JETTISONED");
      break;
    case 4:
      if (irand(100) < 70) {
        groupDo(hailShip, AI_FLEE, false);
        bark(SPK_PIRATE, -1, "...YOU KNOW SABLE PERSONALLY? WHY DIDN'T YOU SAY SO? WE WERE NEVER HERE.");
      } else {
        groupDo(hailShip, AI_ATTACK, true);
        bark(SPK_PIRATE, -1, "NICE TRY. NOBODY KNOWS SABLE'S MIDDLE NAME.");
      }
      break;
    case 5:
      if (irand(100) < 60) {
        groupDo(hailShip, AI_FLEE, false);
        bark(SPK_PIRATE, -1, "IS THAT A GRASK ON YOUR GUNS? ...FORGET IT. NOT WORTH IT.");
      } else {
        groupDo(hailShip, AI_ATTACK, true);
        bark(SPK_PIRATE, -1, "BIG TALK. LET'S SEE THE BIG GUNS.");
      }
      break;
    default:
      groupDo(hailShip, AI_PATROL, false);
      break;
    }
    break;
  }
  case DT_HAIL_TRADER:
    if (hailShip < 0) break;
    if (choice == 1) {
      if (offerType == 0) {
        credits -= offerPrice;
        inv[offerArg]++;
        toast("BOUGHT %s", MODS[offerArg].name);
      } else if (offerType == 1) {
        credits -= offerPrice;
        cargo[offerArg] += 5;
        toast("5 %s LOADED", COMS[offerArg].name);
      } else {
        credits += offerPrice * cargo[CM_RELIC];
        cargo[CM_RELIC] = 0;
        toast("RELICS SOLD");
      }
      rep[F_THREN] += 2;
    }
    groupDo(hailShip, AI_TRAVEL, false);
    break;
  case DT_HAIL_PATROL:
    if (hailShip < 0) break;
    if (choice == 1) {
      credits -= tollAmt;
      cargo[CM_RELIC] = 0;
      groupDo(hailShip, AI_PATROL, false);
    } else if (choice == 2) {
      cargo[CM_RELIC] = 0;
      rep[F_AUTH] += 2;
      groupDo(hailShip, AI_PATROL, false);
    } else if (choice == 3) {
      rep[F_AUTH] -= 15;
      groupDo(hailShip, AI_ATTACK, true);
      toast("THE PATROL OPENS FIRE");
    } else {
      groupDo(hailShip, AI_PATROL, false);
    }
    break;
  case DT_DISTRESS_DONE:
    credits += 250;
    rep[F_THREN] += 10;
    if (hailShip >= 0) groupDo(hailShip, AI_TRAVEL, false);
    break;
  case DT_DISMISS:
    if (choice >= 10 && choice - 10 < nCrew && nCrew > 1) {
      for (int i = choice - 10; i < nCrew - 1; i++) crew[i] = crew[i + 1];
      nCrew--;
      computeCrewMods();
    }
    break;
  case DT_CONFIRM_NEW:
    if (choice == 1) mode = MD_CREW;
    break;
  default:
    break;
  }
  if (tag == DT_HAIL_PIRATE || tag == DT_HAIL_TRADER || tag == DT_HAIL_PATROL || tag == DT_DISTRESS_DONE)
    hailShip = -1;
  rep[F_AUTH] = clampi(rep[F_AUTH], -100, 100);
  rep[F_SYND] = clampi(rep[F_SYND], -100, 100);
  rep[F_THREN] = clampi(rep[F_THREN], -100, 100);
  if (credits < 0) credits = 0;
}

/* ================================================================== save */
struct SaveData {
  uint32_t magic;
  uint16_t ver, size;
  int32_t credits;
  int16_t rep[F_COUNT];
  uint8_t mis, mstep, ending, paint;
  uint8_t crew[6], nCrew, known, knownGate, lastStation;
  uint8_t laneOpen[NLANE];
  uint8_t inv[M_COUNT];
  int16_t cargo[NCOM];
  Grid grid;
  uint16_t kills;
  uint32_t playTime;
  Job job;
  float hp;
};
static const uint32_t SAVE_MAGIC = 0x534C4E31; /* "SLN1" */
static const uint16_t SAVE_VER = 1;

void saveGame() {
  if (mode == MD_INTRO || mode == MD_TITLE || mode == MD_CREW) return;
  SaveData s;
  memset(&s, 0, sizeof(s));
  s.magic = SAVE_MAGIC;
  s.ver = SAVE_VER;
  s.size = sizeof(SaveData);
  s.credits = credits;
  for (int f = 0; f < F_COUNT; f++) s.rep[f] = (int16_t)rep[f];
  s.mis = (uint8_t)mis;
  s.mstep = (uint8_t)mstep;
  s.ending = ending;
  s.paint = paint;
  memcpy(s.crew, crew, 6);
  s.nCrew = (uint8_t)nCrew;
  s.known = known;
  s.knownGate = knownGate;
  s.lastStation = (uint8_t)lastStation;
  for (int k = 0; k < NLANE; k++) s.laneOpen[k] = laneOpen[k];
  memcpy(s.inv, inv, M_COUNT);
  for (int c = 0; c < NCOM; c++) s.cargo[c] = (int16_t)cargo[c];
  s.grid = designs[D_PLAYER].g;
  s.kills = (uint16_t)kills;
  s.playTime = (uint32_t)playTime;
  s.job = job;
  s.hp = ships[PL].hp;
  platformSave(&s, sizeof(s));
  hasSave = true;
}

static bool readSave(SaveData &s) {
  memset(&s, 0, sizeof(s));
  if (!platformLoad(&s, sizeof(s))) return false;
  return s.magic == SAVE_MAGIC && s.ver == SAVE_VER && s.size == sizeof(SaveData);
}

bool loadGame() {
  SaveData s;
  if (!readSave(s)) return false;
  credits = s.credits;
  for (int f = 0; f < F_COUNT; f++) rep[f] = s.rep[f];
  mis = s.mis;
  mstep = s.mstep;
  ending = s.ending;
  paint = s.paint & 3;
  memcpy(crew, s.crew, 6);
  nCrew = clampi(s.nCrew, 0, 6);
  for (int i = 0; i < nCrew; i++)
    if (crew[i] >= NCREW) crew[i] = 0;
  known = s.known;
  knownGate = s.knownGate;
  lastStation = clampi(s.lastStation, 0, ST_COUNT - 1);
  for (int k = 0; k < NLANE; k++) laneOpen[k] = s.laneOpen[k] != 0;
  memcpy(inv, s.inv, M_COUNT);
  for (int c = 0; c < NCOM; c++) cargo[c] = s.cargo[c];
  designs[D_PLAYER].g = s.grid;
  kills = s.kills;
  playTime = (float)s.playTime;
  job = s.job;
  worldReset();
  storyRevert();
  m2Dropped = false;
  playerRefresh(true);
  ships[PL].hp = clampf(s.hp, 1, ships[PL].hpMax);
  dockedAt = lastStation;
  mode = MD_STATION;
  genJobs(dockedAt);
  jobOnDock(dockedAt);
  return true;
}

/* ============================================================= lifecycle */
void newGame() {
  credits = 300;
  memset(cargo, 0, sizeof(cargo));
  memset(inv, 0, sizeof(inv));
  paint = 0;
  dockedAt = ST_HAVEN;
  lastStation = ST_HAVEN;
  known = (uint8_t)((1 << ST_HAVEN) | (1 << ST_TOLLE) | (1 << ST_MERIDIAN) | (1 << ST_LANTERN) | (1 << ST_FARPOINT));
  knownGate = false;
  kills = 0;
  playTime = 0;
  rep[F_PLAYER] = 0;
  rep[F_AUTH] = 10;
  rep[F_SYND] = -30;
  rep[F_THREN] = 0;
  rep[F_HUSH] = -100;
  laneOpen[0] = laneOpen[1] = laneOpen[2] = true;
  laneOpen[3] = false;
  memset(&job, 0, sizeof(job));
  Grid &g = designs[D_PLAYER].g;
  memset(&g, 0, sizeof(g));
  g.m[2][3] = M_COCKPIT;
  g.m[3][2] = M_HULL;
  g.m[3][3] = M_REACTOR;
  g.m[3][4] = M_HULL;
  g.m[4][2] = M_ENGINE;
  g.m[4][3] = M_SHIELD;
  g.m[4][4] = M_ENGINE;
  g.m[5][3] = M_CARGO;
  inv[M_LASER] = 2;
  inv[M_HULL] = 2;
  worldReset();
  playerRefresh(true);
  mode = MD_STATION;
  storyNewGame();
  genJobs(ST_HAVEN);
  saveGame();
}

void respawn() {
  storyRevert();
  for (int c = 0; c < CM_PARCEL; c++) cargo[c] = 0;
  credits -= credits / 10;
  if (job.stage) job.stage = 0;
  worldReset();
  playerRefresh(true);
  dockedAt = lastStation;
  mode = MD_STATION;
  genJobs(dockedAt);
  saveGame();
}

void init(uint32_t seed) {
  rngState ^= seed * 2654435761u;
  if (!rngState) rngState = 1;
  worldInit();
  SaveData s;
  hasSave = readSave(s);
  computeCrewMods();
  startIntro();
}

void tick(bool down, int x, int y) {
  inTap = down && !prevDown;
  prevDown = down;
  inDown = down;
  inX = x;
  inY = y;
  tapEaten = false;
  frameNo++;
  uiLock = dlg.on;
  switch (mode) {
  case MD_INTRO: uiIntro(); break;
  case MD_TITLE: uiTitle(); break;
  case MD_CREW: uiCrew(); break;
  case MD_FLIGHT: flightFrame(); break;
  case MD_STATION: uiStation(); break;
  case MD_SHIPYARD: uiShipyard(); break;
  case MD_TRADE: uiTrade(); break;
  case MD_JOBS: uiJobs(); break;
  case MD_BAR: uiBar(); break;
  case MD_MAP: uiMap(); break;
  case MD_MENU: uiMenu(); break;
  case MD_DEAD: uiDead(); break;
  case MD_ENDING: uiEnding(); break;
  default: mode = MD_TITLE; break;
  }
  if (dlg.on) {
    uiLock = false;
    dlgDraw();
  }
  uiLock = false;
}

} // namespace sl
