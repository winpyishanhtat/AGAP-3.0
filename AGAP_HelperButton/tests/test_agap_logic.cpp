// Unit tests for agap_logic.h - compiled and run natively (no Arduino/AVR
// toolchain needed), so these run on any machine with g++.
//
//   g++ -std=c++14 -Wall -Wextra -I.. test_agap_logic.cpp -o test_agap_logic
//   ./test_agap_logic
//
// or just: tests/run_tests.sh

#include "../agap_logic.h"

#include <cstdio>
#include <initializer_list>

using namespace agap;

static int g_total = 0;
static int g_failures = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    g_total++;                                                             \
    if (!(cond)) {                                                          \
      g_failures++;                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                       \
  } while (0)

#define CHECK_EQ(a, b)                                                      \
  do {                                                                      \
    g_total++;                                                             \
    auto va = (a);                                                          \
    auto vb = (b);                                                          \
    if (!(va == vb)) {                                                      \
      g_failures++;                                                        \
      std::printf("  FAIL %s:%d: %s (%ld) != %s (%ld)\n", __FILE__, __LINE__, \
                   #a, (long)va, #b, (long)vb);                             \
    }                                                                       \
  } while (0)

#define RUN(fn)                                                             \
  do {                                                                      \
    std::printf("-- %s\n", #fn);                                           \
    fn();                                                                   \
  } while (0)

// --------------------------- findLabel ---------------------------

void test_findLabel_matches_case_insensitively() {
  const char* labels[] = {"EM", "AM", "D", "C", "F", "DM", "G", "BM", "X1", "X2"};
  CHECK_EQ(findLabel(labels, 10, "EM"), 0);
  CHECK_EQ(findLabel(labels, 10, "em"), 0);
  CHECK_EQ(findLabel(labels, 10, "Bm"), 7);
  CHECK_EQ(findLabel(labels, 10, "x2"), 9);
}

void test_findLabel_returns_minus_one_when_not_found() {
  const char* labels[] = {"EM", "AM", "D"};
  CHECK_EQ(findLabel(labels, 3, "ZZZ"), -1);
  CHECK_EQ(findLabel(labels, 3, "E"), -1);  // not a substring match, must be exact
}

// --------------------------- pick calibration ---------------------------

void test_clampPickAngle_limits() {
  CHECK_EQ(clampPickAngle(90), 90);
  CHECK_EQ(clampPickAngle(PICK_ANGLE_MIN), PICK_ANGLE_MIN);
  CHECK_EQ(clampPickAngle(PICK_ANGLE_MAX), PICK_ANGLE_MAX);
  CHECK_EQ(clampPickAngle(-50), PICK_ANGLE_MIN);
  CHECK_EQ(clampPickAngle(0), PICK_ANGLE_MIN);
  CHECK_EQ(clampPickAngle(180), PICK_ANGLE_MAX);
  CHECK_EQ(clampPickAngle(100000), PICK_ANGLE_MAX);
}

void test_stringToServoIndex_maps_player_numbering() {
  CHECK_EQ(stringToServoIndex(6), 0);  // low E is servo 0
  CHECK_EQ(stringToServoIndex(1), 5);  // high e is servo 5
  CHECK_EQ(stringToServoIndex(3), 3);
  CHECK_EQ(stringToServoIndex(0), -1);
  CHECK_EQ(stringToServoIndex(7), -1);
  CHECK_EQ(stringToServoIndex(-1), -1);
}

// --------------------------- saved tuning ---------------------------

static Tuning sampleTuning() {
  Tuning t{};
  t.kickMs = 85;
  t.holdDuty = 90;
  t.bpm = 72;
  for (uint8_t i = 0; i < 6; i++) { t.pickA[i] = 60 + i; t.pickB[i] = 120 + i; }
  return t;
}

void test_tuning_roundtrip() {
  uint8_t buf[TUNING_BYTES];
  encodeTuning(sampleTuning(), buf);
  Tuning got{};
  CHECK(decodeTuning(buf, got));
  CHECK_EQ(got.kickMs, 85);
  CHECK_EQ(got.holdDuty, 90);
  CHECK_EQ(got.bpm, 72);
  for (uint8_t i = 0; i < 6; i++) {
    CHECK_EQ(got.pickA[i], 60 + i);
    CHECK_EQ(got.pickB[i], 120 + i);
  }
}

void test_tuning_rejects_blank_eeprom() {
  uint8_t blank[TUNING_BYTES], zeros[TUNING_BYTES] = {0};
  for (auto& b : blank) b = 0xFF;  // erased EEPROM reads 0xFF
  Tuning t{};
  CHECK(!decodeTuning(blank, t));
  CHECK(!decodeTuning(zeros, t));
}

void test_tuning_detects_any_single_corrupted_byte() {
  uint8_t buf[TUNING_BYTES];
  encodeTuning(sampleTuning(), buf);
  for (uint8_t i = 0; i < TUNING_BYTES; i++) {
    uint8_t bad[TUNING_BYTES];
    for (uint8_t k = 0; k < TUNING_BYTES; k++) bad[k] = buf[k];
    bad[i] ^= 0x10;
    Tuning t{};
    CHECK(!decodeTuning(bad, t));
  }
}

void test_tuning_rejects_other_version() {
  uint8_t buf[TUNING_BYTES];
  encodeTuning(sampleTuning(), buf);
  buf[2] = TUNING_VERSION + 1;
  buf[TUNING_BYTES - 1] = tuningChecksum(buf, TUNING_BYTES - 1);  // valid checksum, wrong version
  Tuning t{};
  CHECK(!decodeTuning(buf, t));
}

void test_tuning_decode_leaves_output_untouched_on_failure() {
  Tuning t = sampleTuning();
  uint8_t blank[TUNING_BYTES];
  for (auto& b : blank) b = 0xFF;
  CHECK(!decodeTuning(blank, t));
  CHECK_EQ(t.kickMs, 85);
}

void test_sanitize_clamps_everything() {
  Tuning t{};
  t.kickMs = 5000; t.bpm = 1; t.holdDuty = 255;
  for (uint8_t i = 0; i < 6; i++) { t.pickA[i] = 0; t.pickB[i] = 255; }
  sanitizeTuning(t);
  CHECK_EQ(t.kickMs, KICK_MS_MAX);
  CHECK_EQ(t.bpm, BPM_MIN);
  CHECK_EQ(t.pickA[0], PICK_ANGLE_MIN);
  CHECK_EQ(t.pickB[5], PICK_ANGLE_MAX);
  t.kickMs = 1; t.bpm = 999;
  sanitizeTuning(t);
  CHECK_EQ(t.kickMs, KICK_MS_MIN);
  CHECK_EQ(t.bpm, BPM_MAX);
}

void test_out_of_range_saved_values_are_clamped_on_load() {
  // A valid checksum doesn't make the values safe: load must still clamp.
  Tuning t = sampleTuning();
  t.kickMs = 9000;
  t.pickA[2] = 3;
  uint8_t buf[TUNING_BYTES];
  encodeTuning(t, buf);
  Tuning got{};
  CHECK(decodeTuning(buf, got));
  CHECK_EQ(got.kickMs, KICK_MS_MAX);
  CHECK_EQ(got.pickA[2], PICK_ANGLE_MIN);
}

// --------------------------- 30-channel masks, active cap, strum mask ---------------------------

void test_mask32_covers_30_channels_across_four_ports() {
  ChannelDriver<30> d(60, 8000);
  for (uint8_t i = 0; i < 30; i++) d.press(i, 0, 0);
  d.update(0);  // all KICK
  Mask32 m = d.computeMask32();
  CHECK_EQ(m.on, 0x3FFFFFFFu);
  CHECK_EQ(m.kick, 0x3FFFFFFFu);
  d.update(60);  // all HOLD
  m = d.computeMask32();
  CHECK_EQ(m.on, 0x3FFFFFFFu);
  CHECK_EQ(m.kick, 0u);
}

void test_mask32_single_channels_land_on_the_right_bit() {
  for (uint8_t ch : std::initializer_list<uint8_t>{0, 7, 8, 15, 16, 23, 24, 29}) {
    ChannelDriver<30> d(60, 8000);
    d.press(ch, 0, 0);
    d.update(0);
    Mask32 m = d.computeMask32();
    CHECK_EQ(m.on, 1u << ch);
    CHECK_EQ(m.kick, 1u << ch);
  }
}

void test_masks_and_mask32_agree_for_the_10_channel_layout() {
  ChannelDriver<10> d(60, 8000);
  for (uint8_t i = 0; i < 10; i += 2) d.press(i, 0, 0);
  d.update(0);
  Masks m = d.computeMasks();
  Mask32 w = d.computeMask32();
  CHECK_EQ((uint32_t)m.a | ((uint32_t)m.c << 8), w.on);
  CHECK_EQ((uint32_t)m.ka | ((uint32_t)m.kc << 8), w.kick);
}

void test_activeCount_counts_pending_kick_and_hold() {
  ChannelDriver<30> d(60, 8000);
  CHECK_EQ(d.activeCount(), 0);
  d.press(3, 0, 0);
  d.press(9, 100, 0);  // pending, not yet kicking
  CHECK_EQ(d.activeCount(), 2);
  d.update(0);
  d.update(60);
  d.release(3);
  CHECK_EQ(d.activeCount(), 1);
}

void test_tryPress_refuses_beyond_the_cap_but_allows_refresh() {
  ChannelDriver<30> d(60, 8000);
  for (uint8_t i = 0; i < 6; i++) CHECK(d.tryPress(i, 0, 0, 6));
  CHECK(!d.tryPress(6, 0, 0, 6));            // a 7th coil is refused
  CHECK(d.stateOf(6) == ChanState::OFF);      // and nothing was started
  CHECK(d.tryPress(2, 0, 100, 6));            // re-pressing one already on is fine
  d.release(0);
  CHECK(d.tryPress(6, 0, 100, 6));            // room again after a release
}

void test_strum_mask_skips_unplayed_strings_without_waiting() {
  Strummer s;
  s.start(true, 0, 18, 0b101011);  // strings 0,1,3,5 sound; 2 and 4 are muted
  CHECK_EQ(s.update(0), 0);
  CHECK_EQ(s.update(18), 1);
  CHECK_EQ(s.update(36), 3);   // string 2 skipped: no extra gap spent on it
  CHECK_EQ(s.update(54), 5);
  CHECK(s.active());
  CHECK_EQ(s.update(72), -1);
  CHECK(!s.active());
}

void test_strum_mask_up_direction_and_empty_mask() {
  Strummer s;
  s.start(false, 0, 10, 0b010010);  // strings 1 and 4
  CHECK_EQ(s.update(0), 4);
  CHECK_EQ(s.update(10), 1);
  CHECK_EQ(s.update(20), -1);
  Strummer e;
  e.start(true, 0, 10, 0);
  CHECK_EQ(e.update(0), -1);
  CHECK(!e.active());
}

void test_strum_default_mask_is_all_six() {
  Strummer s;
  s.start(true, 0, 5);
  int plucks = 0;
  for (uint32_t t = 0; t < 100; t += 5) if (s.update(t) >= 0) plucks++;
  CHECK_EQ(plucks, 6);
}

// --------------------------- found by running the real firmware (sim/) ---------------------------

void test_refreshing_a_held_coil_in_the_same_loop_does_not_time_it_out() {
  // loop() reads the clock once at the top, then a command re-presses a held coil with a
  // LATER clock reading, then update() runs with the earlier value. The refresh time is
  // then ahead of "now"; that must not wrap around and trip the 8 s safety timeout.
  ChannelDriver<10> d(60, 8000);
  d.press(0, 0, 0);
  d.update(0);
  d.update(60);  // HOLD
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.press(0, 110, /*now=*/110);  // re-press, refresh stamped at 110
  d.update(100);                 // but the update clock is still 100
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.update(105);
  CHECK(d.stateOf(0) == ChanState::HOLD);
}

void test_hold_timeout_still_fires_after_a_refresh_ahead_of_the_clock() {
  ChannelDriver<10> d(60, 8000);
  d.press(0, 0, 0);
  d.update(0);
  d.update(60);
  d.press(0, 110, 110);
  d.update(100);
  d.update(110 + 7999);
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.update(110 + 8000);
  CHECK(d.stateOf(0) == ChanState::OFF);
}

void test_hold_timeout_works_across_the_32_bit_clock_wrap() {
  // millis() wraps after about 49 days; elapsed time must still be measured correctly.
  ChannelDriver<10> d(60, 8000);
  const uint32_t t0 = 0xFFFFFF00u;
  d.press(0, t0, t0);
  d.update(t0);
  d.update(t0 + 60);
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.update(t0 + 7999);  // wrapped past zero, 7999 ms later
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.update(t0 + 8000);
  CHECK(d.stateOf(0) == ChanState::OFF);
}

void test_parseInt_accepts_only_plain_integers() {
  long v = -1;
  CHECK(parseInt("0", v) && v == 0);
  CHECK(parseInt("60", v) && v == 60);
  CHECK(parseInt("-5", v) && v == -5);
  CHECK(parseInt("999999999", v) && v == 999999999);
  v = 42;
  const char* bad[] = {"", "-", "abc", "12abc", "1.5", " 7", "7 ", "+3", "--1", "1000000000", "99999999999", "0x10", "1e3"};
  for (const char* b : bad) {
    CHECK(!parseInt(b, v));
    CHECK_EQ(v, 42);  // a rejected value is never written
  }
  CHECK(!parseInt(nullptr, v));
}

void test_tuning_hold_has_a_floor() {
  Tuning t = sampleTuning();
  t.holdDuty = 0;
  sanitizeTuning(t);
  CHECK(t.holdDuty >= HOLD_DUTY_MIN);
  uint8_t buf[TUNING_BYTES];
  Tuning zero = sampleTuning();
  zero.holdDuty = 3;
  encodeTuning(zero, buf);
  Tuning got{};
  CHECK(decodeTuning(buf, got));
  CHECK(got.holdDuty >= HOLD_DUTY_MIN);
  Tuning high = sampleTuning();
  high.holdDuty = 255;
  sanitizeTuning(high);
  CHECK_EQ(high.holdDuty, 255);
}

// --------------------------- ChannelDriver ---------------------------

void test_channel_starts_off() {
  ChannelDriver<10> d(60, 8000);
  CHECK(d.stateOf(0) == ChanState::OFF);
  Masks m = d.computeMasks();
  CHECK_EQ(m.a, 0);
  CHECK_EQ(m.c, 0);
}

void test_channel_press_then_update_goes_pending_then_kick() {
  ChannelDriver<10> d(60, 8000);
  d.press(0, /*startAt=*/1000, /*now=*/1000);
  CHECK(d.stateOf(0) == ChanState::PENDING);
  d.update(999);  // before startAt - still pending
  CHECK(d.stateOf(0) == ChanState::PENDING);
  d.update(1000);  // startAt reached - kicks now
  CHECK(d.stateOf(0) == ChanState::KICK);
}

void test_channel_kick_transitions_to_hold_after_kickMs() {
  ChannelDriver<10> d(/*kickMs=*/60, 8000);
  d.press(0, 0, 0);
  d.update(0);  // -> KICK
  CHECK(d.stateOf(0) == ChanState::KICK);
  d.update(59);
  CHECK(d.stateOf(0) == ChanState::KICK);
  d.update(60);  // exactly kickMs - must have seated by now
  CHECK(d.stateOf(0) == ChanState::HOLD);
}

void test_setKickMs_changes_the_kick_duration() {
  // The KICK command must change how long the coil really gets full power,
  // not just a number the firmware prints back.
  ChannelDriver<10> d(/*kickMs=*/60, 8000);
  d.setKickMs(120);
  d.press(0, 0, 0);
  d.update(0);  // -> KICK
  d.update(60);  // old duration: would already be HOLD
  CHECK(d.stateOf(0) == ChanState::KICK);
  d.update(119);
  CHECK(d.stateOf(0) == ChanState::KICK);
  d.update(120);
  CHECK(d.stateOf(0) == ChanState::HOLD);
}

void test_masks_kick_vs_hold() {
  ChannelDriver<10> d(60, 8000);
  d.press(0, 0, 0);
  d.update(0);  // -> KICK
  Masks m = d.computeMasks();
  CHECK_EQ(m.a, 0x01);   // channel 0 on in the steady mask
  CHECK_EQ(m.ka, 0x01);  // ...and in the kick-only mask (full power)

  d.update(60);  // -> HOLD
  m = d.computeMasks();
  CHECK_EQ(m.a, 0x01);   // still on in the steady mask
  CHECK_EQ(m.ka, 0x00);  // but dropped from the kick mask (PWM hold now)
}

void test_masks_split_across_porta_and_portc() {
  // Channels 0-7 -> PORTA, 8-9 -> PORTC bits 0-1 (this firmware's 10
  // channels). Confirms the bit split at the register boundary is right.
  ChannelDriver<10> d(60, 8000);
  for (uint8_t i = 0; i < 10; i++) d.press(i, 0, 0);
  d.update(0);  // all -> KICK
  Masks m = d.computeMasks();
  CHECK_EQ(m.a, 0xFF);   // channels 0-7
  CHECK_EQ(m.c, 0x03);   // channels 8-9
  CHECK_EQ(m.ka, 0xFF);
  CHECK_EQ(m.kc, 0x03);
}

void test_channel_auto_releases_after_timeout() {
  ChannelDriver<10> d(60, /*cmdTimeoutMs=*/8000);
  d.press(0, 0, /*now=*/0);  // refresh_[0] = 0
  d.update(0);
  d.update(60);  // -> HOLD
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.update(7999);  // 7999ms since refresh - just under the timeout
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.update(8000);  // 8000ms since refresh - timeout fires
  CHECK(d.stateOf(0) == ChanState::OFF);
}

void test_channel_press_while_held_refreshes_timeout_without_rekicking() {
  ChannelDriver<10> d(60, 8000);
  d.press(0, 0, 0);
  d.update(0);
  d.update(60);  // -> HOLD at t=60
  CHECK(d.stateOf(0) == ChanState::HOLD);

  d.press(0, /*startAt=*/4000, /*now=*/4000);  // re-press while already on
  CHECK(d.stateOf(0) == ChanState::HOLD);  // must NOT re-kick (startAt ignored)

  d.update(4000 + 8000 - 1);
  CHECK(d.stateOf(0) == ChanState::HOLD);  // timeout measured from the refresh (4000)...
  d.update(4000 + 8000);
  CHECK(d.stateOf(0) == ChanState::OFF);   // ...not from the original press (0)
}

void test_channel_release_is_immediate() {
  ChannelDriver<10> d(60, 8000);
  d.press(0, 0, 0);
  d.update(0);
  d.update(60);
  CHECK(d.stateOf(0) == ChanState::HOLD);
  d.release(0);
  CHECK(d.stateOf(0) == ChanState::OFF);
  Masks m = d.computeMasks();
  CHECK_EQ(m.a, 0);
}

void test_releaseAll_clears_every_channel() {
  ChannelDriver<10> d(60, 8000);
  for (uint8_t i = 0; i < 10; i++) d.press(i, 0, 0);
  d.update(0);
  d.update(60);  // all HOLD
  d.releaseAll();
  Masks m = d.computeMasks();
  CHECK_EQ(m.a, 0);
  CHECK_EQ(m.c, 0);
}

void test_update_returns_dirty_only_on_transition() {
  ChannelDriver<10> d(60, 8000);
  CHECK_EQ(d.update(0), false);  // nothing scheduled - no change
  d.press(0, 100, 0);
  CHECK_EQ(d.update(50), false);  // still waiting for startAt
  CHECK_EQ(d.update(100), true);  // PENDING -> KICK
  CHECK_EQ(d.update(100), false); // no change this tick
  CHECK_EQ(d.update(160), true);  // KICK -> HOLD
}

void test_staggered_press_schedules_independent_kick_times() {
  // Mirrors how a multi-channel chord is pressed: each channel gets its own
  // startAt a few ms apart to limit simultaneous inrush current.
  ChannelDriver<10> d(60, 8000);
  d.press(0, 0, 0);
  d.press(1, 4, 0);
  d.press(2, 8, 0);
  d.update(0);
  CHECK(d.stateOf(0) == ChanState::KICK);
  CHECK(d.stateOf(1) == ChanState::PENDING);
  CHECK(d.stateOf(2) == ChanState::PENDING);
  d.update(4);
  CHECK(d.stateOf(1) == ChanState::KICK);
  CHECK(d.stateOf(2) == ChanState::PENDING);
  d.update(8);
  CHECK(d.stateOf(2) == ChanState::KICK);
}

// --------------------------- Strummer ---------------------------

void test_strummer_idle_by_default() {
  Strummer s;
  CHECK(!s.active());
  CHECK_EQ(s.update(0), -1);
}

void test_strummer_down_strum_order_is_6th_to_1st() {
  Strummer s;
  s.start(/*down=*/true, /*startAt=*/0, /*gapMs=*/18);
  CHECK_EQ(s.update(0), 0);
  CHECK_EQ(s.update(5), -1);  // gap not elapsed yet
  CHECK_EQ(s.update(18), 1);
  CHECK_EQ(s.update(36), 2);
  CHECK_EQ(s.update(54), 3);
  CHECK_EQ(s.update(72), 4);
  CHECK_EQ(s.update(90), 5);
  // active() only clears on the NEXT call after the last pluck, not on the
  // same call that returns it - that's how updateStrum() in the .ino works
  // too (the idx>=6 check and active=false are in that next call's `else`
  // branch). Harmless: the only effect is one extra tight-loop spin in
  // SEQUENCE/CALIB before they see strum.active go false.
  CHECK(s.active());
  CHECK_EQ(s.update(108), -1);
  CHECK(!s.active());
  CHECK_EQ(s.update(200), -1);
}

void test_strummer_up_strum_order_is_1st_to_6th() {
  Strummer s;
  s.start(/*down=*/false, 0, 10);
  CHECK_EQ(s.update(0), 5);
  CHECK_EQ(s.update(10), 4);
  CHECK_EQ(s.update(20), 3);
  CHECK_EQ(s.update(30), 2);
  CHECK_EQ(s.update(40), 1);
  CHECK_EQ(s.update(50), 0);
  CHECK(s.active());  // see the down-strum test above: clears one call later
  CHECK_EQ(s.update(60), -1);
  CHECK(!s.active());
}

void test_strummer_start_at_future_time_waits() {
  Strummer s;
  s.start(true, /*startAt=*/500, 10);
  CHECK_EQ(s.update(0), -1);
  CHECK_EQ(s.update(499), -1);
  CHECK_EQ(s.update(500), 0);
}

void test_strummer_stop_cancels_mid_strum() {
  Strummer s;
  s.start(true, 0, 10);
  CHECK_EQ(s.update(0), 0);
  s.stop();
  CHECK(!s.active());
  CHECK_EQ(s.update(10), -1);
}

int main() {
  RUN(test_findLabel_matches_case_insensitively);
  RUN(test_findLabel_returns_minus_one_when_not_found);

  RUN(test_clampPickAngle_limits);
  RUN(test_stringToServoIndex_maps_player_numbering);

  RUN(test_tuning_roundtrip);
  RUN(test_tuning_rejects_blank_eeprom);
  RUN(test_tuning_detects_any_single_corrupted_byte);
  RUN(test_tuning_rejects_other_version);
  RUN(test_tuning_decode_leaves_output_untouched_on_failure);
  RUN(test_sanitize_clamps_everything);
  RUN(test_out_of_range_saved_values_are_clamped_on_load);

  RUN(test_mask32_covers_30_channels_across_four_ports);
  RUN(test_mask32_single_channels_land_on_the_right_bit);
  RUN(test_masks_and_mask32_agree_for_the_10_channel_layout);
  RUN(test_activeCount_counts_pending_kick_and_hold);
  RUN(test_tryPress_refuses_beyond_the_cap_but_allows_refresh);
  RUN(test_strum_mask_skips_unplayed_strings_without_waiting);
  RUN(test_strum_mask_up_direction_and_empty_mask);
  RUN(test_strum_default_mask_is_all_six);

  RUN(test_refreshing_a_held_coil_in_the_same_loop_does_not_time_it_out);
  RUN(test_hold_timeout_still_fires_after_a_refresh_ahead_of_the_clock);
  RUN(test_hold_timeout_works_across_the_32_bit_clock_wrap);
  RUN(test_parseInt_accepts_only_plain_integers);
  RUN(test_tuning_hold_has_a_floor);

  RUN(test_channel_starts_off);
  RUN(test_channel_press_then_update_goes_pending_then_kick);
  RUN(test_channel_kick_transitions_to_hold_after_kickMs);
  RUN(test_setKickMs_changes_the_kick_duration);
  RUN(test_masks_kick_vs_hold);
  RUN(test_masks_split_across_porta_and_portc);
  RUN(test_channel_auto_releases_after_timeout);
  RUN(test_channel_press_while_held_refreshes_timeout_without_rekicking);
  RUN(test_channel_release_is_immediate);
  RUN(test_releaseAll_clears_every_channel);
  RUN(test_update_returns_dirty_only_on_transition);
  RUN(test_staggered_press_schedules_independent_kick_times);

  RUN(test_strummer_idle_by_default);
  RUN(test_strummer_down_strum_order_is_6th_to_1st);
  RUN(test_strummer_up_strum_order_is_1st_to_6th);
  RUN(test_strummer_start_at_future_time_waits);
  RUN(test_strummer_stop_cancels_mid_strum);

  std::printf("\n%d/%d checks passed\n", g_total - g_failures, g_total);
  return g_failures ? 1 : 0;
}
