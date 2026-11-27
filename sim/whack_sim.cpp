/* Desktop simulator for NEON WHACK's core (src/whack_core.cpp).
 *
 * Plays one scripted round with a bot that taps moles, writes selected frames
 * as PPM images into the output folder, and times render() the way the board
 * calls it (480x480 in 60-line strips). Build + run: sim/run.sh
 * HUD text is drawn by LVGL on the board, so it is not in these frames. */
#include <chrono>
#include <stdio.h>
#include <string>
#include "../src/whack_core.h"

using namespace whack;

static uint16_t frame[W * H];

static void renderFrame() {
  for (int y = 0; y < H; y += 60)
    render(frame + y * W, W, 0, y, 0, y, W, y + 60 < H ? y + 60 : H);
}

static void savePPM(const std::string &path) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) {
    const uint16_t c = frame[i];
    const unsigned char px[3] = {(unsigned char)((c >> 11) * 255 / 31),
                                 (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                 (unsigned char)((c & 31) * 255 / 31)};
    fwrite(px, 1, 3, f);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  init(1234, 0xC0FFEEu);
  { // showcase: one of each type in every row
    const int types[9] = {1, 0, 2, 2, 1, 0, 0, 2, 1};
    for (int i = 0; i < 9; i++) simSpawn(i, types[i]);
    update(1.0f / 30);
    renderFrame();
    savePPM(out + "/showcase.ppm");
    init(1234, 0xC0FFEEu);
  }
  const float dt = 1.0f / 30;
  double renderUs = 0, worstUs = 0;
  int timed = 0, saved = 0, hits = 0, afterHit = -1;
  bool bombTapped = false, bombFrames = false;
  float reaction[9] = {0};
  for (int f = 0; f < 30 * 82; f++) {
    const float t = f * dt;
    if (f == 75) tap(240, 300); // start: countdown, then 60 s round

    if (status().phase == Phase::Playing) {
      for (int i = 0; i < 9; i++) {
        int x, y, type;
        if (!simMoleTarget(i, x, y, type)) {
          reaction[i] = 0;
          continue;
        }
        reaction[i] += dt;
        const bool wantBomb = !bombTapped && t > 20;
        if (type == 2 && !wantBomb) continue;
        if (reaction[i] > 0.25f + (i % 3) * 0.08f) {
          tap(x, y);
          if (type == 2) bombTapped = bombFrames = true;
          hits++;
          afterHit = 0;
          reaction[i] = 0;
          break;
        }
      }
    }
    update(dt);

    const auto t0 = std::chrono::steady_clock::now();
    renderFrame();
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (status().phase == Phase::Playing) {
      renderUs += us;
      worstUs = us > worstUs ? us : worstUs;
      timed++;
    }

    std::string tag;
    if (f == 30 || f == 66) tag = "title";
    if (f == 100) tag = "countdown";
    if (status().phase == Phase::Playing && f % 45 == 0) tag = "play";
    if (afterHit >= 0) {
      if ((afterHit == 2 || afterHit == 7) && hits % 6 == 1) tag = "hit";
      if (bombFrames && (afterHit == 1 || afterHit == 4 || afterHit == 9)) tag = "bomb";
      afterHit++;
      if (afterHit > 10) {
        afterHit = -1;
        bombFrames = false;
      }
    }
    if (status().phase == Phase::Over && (f % 30 == 0)) tag = "over";
    if (!tag.empty()) {
      char name[64];
      snprintf(name, sizeof name, "/f%04d_%s.ppm", f, tag.c_str());
      savePPM(out + name);
      saved++;
    }
  }
  printf("mountain runs: %d\n", whack::simMountainRuns());
  printf("NEON WHACK render: %.1f us/frame avg, %.1f worst (host), %d frames saved, "
         "score %d, hits %d\n",
         renderUs / timed, worstUs, saved, status().score, hits);
}
