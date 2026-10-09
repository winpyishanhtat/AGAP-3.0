// Unit tests for agap_fret.h, and for how it works with agap_logic.h's
// ChannelDriver (30 channels, the six-coil cap, the strum mask).
//   g++ -std=c++14 -Wall -Wextra -I.. test_agap_fret.cpp -o t && ./t

#include "../agap_chords.h"
#include "../agap_fret.h"
#include "../agap_logic.h"

#include <cstdio>
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
#define CHECK_EQ(a, b)                                                                       \
  do {                                                                                       \
    g_total++;                                                                               \
    long va = (long)(a), vb = (long)(b);                                                     \
    if (va != vb) {                                                                          \
      g_failures++;                                                                          \
      std::printf("  FAIL %s:%d: %s = %ld, expected %ld\n", __FILE__, __LINE__, #a, va, vb); \
    }                                                                                        \
  } while (0)
#define RUN(fn)                  \
  do {                           \
    std::printf("-- %s\n", #fn); \
    fn();                        \
  } while (0)

static void test_channels_are_a_bijection_over_30() {
  bool seen[30] = {false};
  for (uint8_t f = 1; f <= 5; f++)
    for (uint8_t s = 0; s < 6; s++) {
      int8_t ch = fretChannel(s, f);
      CHECK(ch >= 0 && ch < 30);
      if (ch >= 0 && ch < 30) {
        CHECK(!seen[ch]);
        seen[ch] = true;
        CHECK_EQ(channelString((uint8_t)ch), s);
        CHECK_EQ(channelFret((uint8_t)ch), f);
      }
    }
  for (int i = 0; i < 30; i++) CHECK(seen[i]);
}

static void test_a_plate_owns_six_consecutive_channels() {
  CHECK_EQ(fretChannel(0, 1), 0);
  CHECK_EQ(fretChannel(5, 1), 5);
  CHECK_EQ(fretChannel(0, 2), 6);
  CHECK_EQ(fretChannel(5, 5), 29);
}

static void test_no_solenoid_for_open_or_out_of_range() {
  CHECK_EQ(fretChannel(0, 0), -1);  // open string
  CHECK_EQ(fretChannel(0, 6), -1);  // beyond fret 5
  CHECK_EQ(fretChannel(6, 1), -1);  // no seventh string
}

static void test_voicing_checks() {
  const int8_t ok[6] = {-1, 3, 2, 0, 1, 0};
  const int8_t high[6] = {0, 0, 0, 0, 0, 6};
  const int8_t low[6] = {-2, 0, 0, 0, 0, 0};
  const int8_t five[6] = {5, 5, 5, 5, 5, 5};
  CHECK(voicingFits(ok));
  CHECK(!voicingFits(high));
  CHECK(!voicingFits(low));
  CHECK(voicingFits(five));
}

static void test_c_chord_channels_and_strum_mask() {
  const int8_t c[6] = {-1, 3, 2, 0, 1, 0};  // x 3 2 0 1 0
  uint8_t ch[6];
  uint8_t n = voicingChannels(c, ch);
  CHECK_EQ(n, 3);
  CHECK_EQ(ch[0], 13);  // A string, fret 3: (3-1)*6 + 1
  CHECK_EQ(ch[1], 8);   // D string, fret 2: (2-1)*6 + 2
  CHECK_EQ(ch[2], 4);   // B string, fret 1: 0*6 + 4
  CHECK_EQ(soundingMask(c), 0x3E);  // low E muted, the other five ring
}

static void test_open_chord_needs_no_solenoids() {
  const int8_t open[6] = {0, 0, 0, 0, 0, 0};
  uint8_t ch[6];
  CHECK_EQ(voicingChannels(open, ch), 0);
  CHECK_EQ(soundingMask(open), 0x3F);
}

static void test_split_mask_matches_the_port_layout() {
  PortBytes p = splitMask(0x3FFFFFFFu);
  CHECK_EQ(p.a, 0xFF);
  CHECK_EQ(p.c, 0xFF);
  CHECK_EQ(p.l, 0xFF);
  CHECK_EQ(p.k, 0x3F);
  p = splitMask(1u << 29);
  CHECK_EQ(p.k, 0x20);
  CHECK_EQ(p.a | p.c | p.l, 0);
  p = splitMask(1u << 8);
  CHECK_EQ(p.c, 0x01);
  p = splitMask(1u << 16);
  CHECK_EQ(p.l, 0x01);
  p = splitMask(0xC0000000u);  // bits beyond channel 29 never reach the pins
  CHECK_EQ(p.k, 0);
}

static void test_every_solved_chord_fits_and_stays_within_the_coil_limit() {
  const char* names[] = {"C", "D", "E", "F", "G", "A", "B", "Bm", "F#m7", "Bb", "Cmaj7", "Dsus4", "Em", "Am",
                         "G7", "Cdim", "Eaug", "A5", "Dm9", "Bm7b5"};
  for (const char* n : names) {
    Voicing v;
    CHECK(solveChord(n, FRET_COUNT, v));
    CHECK(voicingFits(v.fret));
    uint8_t ch[6];
    CHECK(voicingChannels(v.fret, ch) <= MAX_COILS);
  }
}

static void test_a_chord_can_be_pressed_within_the_cap() {
  ChannelDriver<FRET_CHANNELS> d(60, 8000);
  Voicing v;
  CHECK(solveChord("F", FRET_COUNT, v));
  uint8_t ch[6];
  uint8_t n = voicingChannels(v.fret, ch);
  for (uint8_t i = 0; i < n; i++) CHECK(d.tryPress(ch[i], 0, 0, MAX_COILS));
  CHECK_EQ(d.activeCount(), n);
}

static void test_raw_presses_are_held_to_six_coils() {
  ChannelDriver<FRET_CHANNELS> d(60, 8000);
  uint8_t accepted = 0;
  for (uint8_t ch = 0; ch < FRET_CHANNELS; ch++)
    if (d.tryPress(ch, 0, 0, MAX_COILS)) accepted++;
  CHECK_EQ(accepted, MAX_COILS);
  d.update(0);
  Mask32 m = d.computeMask32();
  int bits = 0;
  for (int i = 0; i < 32; i++)
    if (m.on & (1u << i)) bits++;
  CHECK_EQ(bits, MAX_COILS);
}

static void test_changing_chord_releases_the_old_coils_first() {
  ChannelDriver<FRET_CHANNELS> d(60, 8000);
  Voicing a, b;
  solveChord("F", FRET_COUNT, a);
  solveChord("Bm", FRET_COUNT, b);
  uint8_t ca[6], cb[6];
  uint8_t na = voicingChannels(a.fret, ca), nb = voicingChannels(b.fret, cb);
  for (uint8_t i = 0; i < na; i++) d.tryPress(ca[i], 0, 0, MAX_COILS);
  d.releaseAll();
  CHECK_EQ(d.activeCount(), 0);
  for (uint8_t i = 0; i < nb; i++) CHECK(d.tryPress(cb[i], 0, 0, MAX_COILS));
}

static void test_muted_strings_are_not_plucked() {
  const int8_t c[6] = {-1, 3, 2, 0, 1, 0};
  Strummer s;
  s.start(true, 0, 10, soundingMask(c));
  CHECK_EQ(s.update(0), 1);  // string 0 (low E) is muted and skipped
  CHECK_EQ(s.update(10), 2);
  CHECK_EQ(s.update(20), 3);
  CHECK_EQ(s.update(30), 4);
  CHECK_EQ(s.update(40), 5);
  CHECK_EQ(s.update(50), -1);
}

int main() {
  RUN(test_channels_are_a_bijection_over_30);
  RUN(test_a_plate_owns_six_consecutive_channels);
  RUN(test_no_solenoid_for_open_or_out_of_range);
  RUN(test_voicing_checks);
  RUN(test_c_chord_channels_and_strum_mask);
  RUN(test_open_chord_needs_no_solenoids);
  RUN(test_split_mask_matches_the_port_layout);
  RUN(test_every_solved_chord_fits_and_stays_within_the_coil_limit);
  RUN(test_a_chord_can_be_pressed_within_the_cap);
  RUN(test_raw_presses_are_held_to_six_coils);
  RUN(test_changing_chord_releases_the_old_coils_first);
  RUN(test_muted_strings_are_not_plucked);
  std::printf("\n%d/%d checks passed\n", g_total - g_failures, g_total);
  return g_failures ? 1 : 0;
}
