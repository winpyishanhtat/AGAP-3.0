// Unit tests for agap_logic.h - compiled and run natively (no Arduino/AVR
// toolchain needed), so these run on any machine with g++.
//
//   g++ -std=c++14 -Wall -Wextra -I.. test_agap_logic.cpp -o test_agap_logic
//   ./test_agap_logic
//
// or just: tests/run_tests.sh

#include "../agap_logic.h"

#include <cstdio>

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

  RUN(test_channel_starts_off);
  RUN(test_channel_press_then_update_goes_pending_then_kick);
  RUN(test_channel_kick_transitions_to_hold_after_kickMs);
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
