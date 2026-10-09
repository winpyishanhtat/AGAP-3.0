// Checks agap_chords.h against ChordAI/chord_ai.py on every chord, plus a few
// shapes with known answers.
//   g++ -std=c++14 -Wall -Wextra -I.. test_agap_chords.cpp -o t && ./t expected_chords.txt
// or: tests/run_tests.sh (generates the expected file with Python first)

#include "../agap_chords.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace agap;

static int g_total = 0, g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    g_total++;                                                         \
    if (!(cond)) {                                                     \
      g_failures++;                                                    \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                  \
  } while (0)

static bool shape(const char* name, uint8_t maxFret, const int8_t want[6], int16_t cost) {
  Voicing v;
  if (!solveChord(name, maxFret, v)) return false;
  return memcmp(v.fret, want, 6) == 0 && v.cost == cost;
}

static void known_shapes() {
  std::printf("-- known shapes\n");
  const int8_t bm[6] = {-1, 2, 0, -1, 0, 2};   // the firmware's documented example
  const int8_t c[6] = {-1, 3, 2, 0, 1, 0};
  const int8_t g[6] = {3, 2, 0, 0, 0, 3};
  const int8_t am[6] = {-1, 0, 2, 2, 1, 0};
  const int8_t e[6] = {0, 2, 2, 1, 0, 0};
  CHECK(shape("Bm", 3, bm, 18));
  // With the fretboard's five frets the search finds an easier Bm (G string, fret 4).
  const int8_t bm5[6] = {-1, 2, 0, 4, 0, 2};
  CHECK(shape("Bm", 5, bm5, 9));
  CHECK(shape("C", 3, c, 9));
  CHECK(shape("G", 3, g, 3));
  CHECK(shape("Am", 3, am, 9));
  CHECK(shape("E", 3, e, 3));
}

static void limits() {
  std::printf("-- limits\n");
  const char* names[] = {"C", "F", "F#m7", "Bb", "Ddim", "Gsus4", "Aadd9", "Em"};
  for (const char* n : names)
    for (uint8_t mf = 1; mf <= 5; mf++) {
      Voicing v;
      CHECK(solveChord(n, mf, v));
      bool within = true, any = false;
      for (int i = 0; i < 6; i++) {
        if (v.fret[i] < -1 || v.fret[i] > (int8_t)mf) within = false;
        if (v.fret[i] != MUTED) any = true;
      }
      CHECK(within);
      CHECK(any);
    }
}

static void bad_names() {
  std::printf("-- bad names\n");
  Voicing v;
  CHECK(!solveChord("", 5, v));
  CHECK(!solveChord("   ", 5, v));
  CHECK(!solveChord("H", 5, v));
  CHECK(!solveChord("Cfoo", 5, v));
  CHECK(!solveChord("CM", 5, v));          // bare capital M is not guessed
  CHECK(!solveChord("C-this-name-is-far-too-long", 5, v));
  CHECK(solveChord("  Em \n", 5, v));       // whitespace is trimmed, like the Python
}

static void display_names() {
  std::printf("-- display names\n");
  Voicing v;
  char buf[16];
  solveChord("Bbmaj7", 5, v);
  chordDisplayName(v, buf);
  CHECK(strcmp(buf, "A#maj7") == 0);
  solveChord("CM7", 5, v);
  chordDisplayName(v, buf);
  CHECK(strcmp(buf, "Cmaj7") == 0);
  solveChord("C", 5, v);
  chordDisplayName(v, buf);
  CHECK(strcmp(buf, "C") == 0);
}

static void parity(const char* path) {
  std::printf("-- parity with ChordAI/chord_ai.py (%s)\n", path);
  FILE* f = std::fopen(path, "r");
  if (!f) {
    std::printf("  FAIL cannot open %s\n", path);
    g_failures++;
    return;
  }
  char kind[4], a[32], b[32];
  int solved = 0, parsed = 0;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == 'S') {
      int mf, fr[6], cost;
      if (std::sscanf(line, "%3s %31s %d %d %d %d %d %d %d %d", kind, a, &mf, &fr[0], &fr[1], &fr[2], &fr[3], &fr[4], &fr[5], &cost) != 10) continue;
      Voicing v;
      bool ok = solveChord(a, (uint8_t)mf, v);
      bool same = ok && v.cost == cost;
      for (int i = 0; i < 6 && same; i++) same = v.fret[i] == fr[i];
      g_total++;
      if (!same) {
        g_failures++;
        std::printf("  FAIL solve %s maxFret=%d\n", a, mf);
      }
      solved++;
    } else if (line[0] == 'P') {
      if (std::sscanf(line, "%3s %31s %31s", kind, a, b) != 3) continue;
      uint8_t root;
      const Quality* q;
      bool ok = parseChord(a, root, q);
      char got[16] = "-";
      if (ok) {
        Voicing v;
        v.root = root;
        v.q = q;
        chordDisplayName(v, got);
      }
      g_total++;
      if (strcmp(got, b) != 0) {
        g_failures++;
        std::printf("  FAIL parse \"%s\": c++ %s vs python %s\n", a, got, b);
      }
      parsed++;
    }
  }
  std::fclose(f);
  CHECK(solved > 400);
  CHECK(parsed > 30);
  std::printf("  compared %d solves and %d name parses\n", solved, parsed);
}

int main(int argc, char** argv) {
  known_shapes();
  limits();
  bad_names();
  display_names();
  if (argc > 1) parity(argv[1]);
  else std::printf("(no expected file given: parity check skipped)\n");
  std::printf("\n%d/%d checks passed\n", g_total - g_failures, g_total);
  return g_failures ? 1 : 0;
}
