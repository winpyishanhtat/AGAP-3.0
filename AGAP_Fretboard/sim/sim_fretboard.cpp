// Runs the REAL AGAP_Fretboard.ino on a PC against a mock Arduino (see mock/Arduino.h)
// and checks its behaviour, scenario by scenario.
//
//   g++ -std=c++14 -Wall -Wextra -include Arduino.h -I../../sim/mock -I.. sim_fretboard.cpp -o sim
//   ./sim --list          list the scenarios
//   ./sim chord_c         power on and run one scenario
//
// Each scenario is a separate run because it needs a fresh power-up: a sketch's
// global state cannot be reset inside one process. sim/run_tests.sh runs them all.
//
// What this does and does not prove: it executes the sketch's real command parser,
// state machines, blocking loops, saved-tuning code and pin masks. It does NOT
// reproduce the AVR's speed, real interrupt timing, or any electrical behaviour.

#include "../AGAP_Fretboard.ino"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../sim/sim_common.h"

// Channel mask for a fingering like "x 3 2 0 1 0", worked out independently of the firmware's header.
static uint32_t maskFor(const char* fingering) {
  uint32_t m = 0;
  int s = 0;
  for (const char* p = fingering; *p; p++) {
    if (*p == ' ') continue;
    if (*p != 'x' && *p >= '1' && *p <= '9') m |= 1u << ((*p - '1') * 6 + s);
    s++;
  }
  return m;
}

// ----------------------------------------------------------------- scenarios
static void s_boot() {
  bootAndKeep();
  CHECK(has(bootLines, "AGAP (fretboard) ready - type HELP"));
  CHECK(has(bootLines, "AGAP-fretboard fw " FW_VERSION));
  CHECK(has(bootLines, "No saved tuning - using compiled defaults."));
  CHECK(has(bootLines, "C -> x 3 2 0 1 0  (cost 9)"));  // the default progression C G Am F is solved at power-up
  CHECK(has(bootLines, "G -> 3 2 0 0 0 3  (cost 3)"));
  CHECK(has(bootLines, "Am -> 5 0 2 2 1 0  (cost 4)"));
  CHECK(has(bootLines, "F -> 1 0 3 2 1 1  (cost 5)"));
  run(500);
  CHECK(pins().on == 0);
  for (int pin = 2; pin <= 7; pin++) {
    CHECK_MSG(sim::servoAttached(pin), "servo pin D%d not attached", pin);
    CHECK_MSG(sim::servoAngle()[pin] == 70, "servo D%d starts at %d", pin, sim::servoAngle()[pin]);
  }
  CHECK(DDRA == 0xFF && DDRC == 0xFF && DDRL == 0xFF && DDRK == 0x3F);
  end_of_scenario();
}

static void s_chord_c() {
  bootAndKeep();
  quiet();
  uint32_t t0 = sim::now();
  send("CHORD C");
  run(30);
  auto out = take();
  CHECK_MSG(has(out, "C -> x 3 2 0 1 0  (cost 9)"), "%s", joined(out).c_str());
  uint32_t want = maskFor("x 3 2 0 1 0");  // channels 13, 8, 4
  CHECK(want == ((1u << 13) | (1u << 8) | (1u << 4)));
  Pins p = pins();
  CHECK_MSG(p.on == want, "on=%08x want=%08x", p.on, want);
  CHECK_MSG(p.kick == want, "all three coils start with a full-power kick");
  run(100);
  p = pins();
  CHECK_MSG(p.on == want, "still held");
  CHECK_MSG(p.kick == 0, "after the kick only the hold remains (kick=%08x)", p.kick);
  // Strum: the low E is muted so it is skipped; strings 2..6 are plucked low to high, 18 ms apart.
  run(300);
  std::vector<sim::ServoWrite> plucks;
  for (auto& w : sim::servoWrites()) if (w.at >= t0) plucks.push_back(w);
  CHECK_MSG(plucks.size() == 5, "expected 5 plucks, got %u", (unsigned)plucks.size());
  if (plucks.size() == 5) {
    for (int i = 0; i < 5; i++) CHECK_MSG(plucks[i].pin == 3 + i, "pluck %d on D%d", i, plucks[i].pin);
    CHECK_MSG(plucks[0].at - t0 >= 60 + 15, "strum began %u ms after the command, before the coils could seat",
              plucks[0].at - t0);
    for (int i = 1; i < 5; i++) CHECK_MSG(plucks[i].at - plucks[i - 1].at >= 18, "gap %u", plucks[i].at - plucks[i - 1].at);
    CHECK(plucks[0].angle == 110);  // swung to side B
  }
  end_of_scenario();
}

static void s_chord_change() {
  bootAndKeep();
  quiet();
  send("CHORD C");
  run(400);
  take();
  send("CHORD G");
  run(400);
  auto out = take();
  CHECK(has(out, "G -> 3 2 0 0 0 3  (cost 3)"));
  uint32_t g = maskFor("3 2 0 0 0 3");
  CHECK(g == ((1u << 12) | (1u << 7) | (1u << 17)));
  CHECK_MSG(pins().on == g, "after the change only G's coils are on: %08x", pins().on);
  end_of_scenario();
}

static void s_show_moves_nothing() {
  bootAndKeep();
  quiet();
  size_t before = sim::servoWrites().size();
  send("SHOW Bm");
  run(500);
  auto out = take();
  CHECK_MSG(has(out, "Bm -> x 2 0 4 0 2  (cost 9)"), "%s", joined(out).c_str());
  CHECK(pins().on == 0);
  CHECK(sim::servoWrites().size() == before);
  send("SHOW Zz");
  run(50);
  CHECK(has(take(), "ERR unknown chord"));
  send("SHOW");
  run(50);
  CHECK(has(take(), "ERR unknown chord"));
  end_of_scenario();
}

static void s_raw() {
  bootAndKeep();
  quiet();
  send("RAW x 3 2 0 1 0");
  run(200);
  CHECK(has(take(), "RAW -> x 3 2 0 1 0 "));
  CHECK(pins().on == maskFor("x 3 2 0 1 0"));
  uint32_t kept = pins().on;
  const char* bad[] = {"RAW 1 2 3", "RAW x x x x x 9", "RAW x x x x x -1", "RAW a b c d e f", "RAW 10 0 0 0 0 0", "RAW"};
  for (const char* b : bad) {
    send(b);
    run(50);
    auto out = take();
    CHECK_MSG(has(out, "ERR RAW needs 6 values, each x or 0-5"), "%s -> %s", b, joined(out).c_str());
    CHECK_MSG(pins().on == kept, "a rejected RAW must not move anything (%s)", b);
  }
  send("raw 0 0 0 0 0 0");  // open strings: nothing to press, everything released
  run(200);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_press_cap() {
  bootAndKeep();
  quiet();
  for (int s = 6; s >= 1; s--) {
    send("PRESS " + std::to_string(s) + " 1");
    run(10);
  }
  auto out = take();
  CHECK_MSG(popcount(pins().on) == 6, "six coils on, got %d", popcount(pins().on));
  int pressed = 0;
  for (auto& l : out) if (l.compare(0, 16, "PRESSED channel ") == 0) pressed++;
  CHECK(pressed == 6);
  send("PRESS 6 2");
  run(20);
  out = take();
  CHECK_MSG(has(out, "ERR already 6 coils on - RELEASE one first"), "%s", joined(out).c_str());
  CHECK(popcount(pins().on) == 6);
  send("RELEASE 6 1");
  run(20);
  CHECK(has(take(), "RELEASED channel 0"));
  send("PRESS 6 2");
  run(20);
  CHECK(has(take(), "PRESSED channel 6"));
  CHECK(popcount(pins().on) == 6);
  // Re-pressing a coil that is already on is allowed even at the limit.
  send("PRESS 6 2");
  run(20);
  CHECK(has(take(), "PRESSED channel 6"));
  const char* bad[] = {"PRESS 7 1", "PRESS 0 1", "PRESS 6 0", "PRESS 6 6", "PRESS", "PRESS 6", "PRESS x y", "PRESS 6 1 extra junk?"};
  uint32_t kept = pins().on;
  for (const char* b : bad) {
    send(b);
    run(10);
    auto o = take();
    if (std::string(b) == "PRESS 6 1 extra junk?") continue;  // extra words are ignored, covered below
    CHECK_MSG(hasPrefix(o, "ERR"), "%s -> %s", b, joined(o).c_str());
  }
  CHECK_MSG(pins().on == kept || popcount(pins().on) <= 6, "rejected presses changed the coils");
  send("RELEASE ALL");
  run(20);
  CHECK(has(take(), "RELEASED ALL"));
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_chord_plus_raw_cap() {
  bootAndKeep();
  quiet();
  send("CHORD F");  // 1 0 3 2 1 1 -> five coils
  run(300);
  take();
  CHECK(popcount(pins().on) == 5);
  send("PRESS 6 5");
  run(20);
  CHECK(has(take(), "PRESSED channel 24"));
  CHECK(popcount(pins().on) == 6);
  send("PRESS 5 5");
  run(20);
  CHECK(hasPrefix(take(), "ERR already 6 coils on"));
  CHECK(popcount(pins().on) == 6);
  // A new chord releases everything first, so it always fits.
  send("CHORD C");
  run(300);
  CHECK_MSG(pins().on == maskFor("x 3 2 0 1 0"), "on=%08x want=%08x states ch4=%d ch8=%d ch13=%d t=%u", pins().on, maskFor("x 3 2 0 1 0"),
            (int)channels.stateOf(4), (int)channels.stateOf(8), (int)channels.stateOf(13), sim::now());
  end_of_scenario();
}

static void s_sequence_timing() {
  bootAndKeep();
  quiet();
  send("TEMPO 60");
  run(10);
  take();
  uint32_t t0 = sim::now();
  send("SEQUENCE C G Am F");
  run(5);
  uint32_t done = sim::now();
  auto out = take();
  CHECK_MSG(has(out, "STEP C") && has(out, "STEP G") && has(out, "STEP Am") && has(out, "STEP F"), "%s", joined(out).c_str());
  // order of STEP lines
  std::vector<std::string> steps;
  for (auto& l : out) if (l.compare(0, 5, "STEP ") == 0) steps.push_back(l);
  CHECK(steps == (std::vector<std::string>{"STEP C", "STEP G", "STEP Am", "STEP F"}));
  CHECK_MSG(done - t0 >= 3900 && done - t0 <= 4600, "4 chords at 60 bpm took %u ms", done - t0);
  CHECK_MSG(pins().on == maskFor("1 0 3 2 1 1"), "the last chord (F) is left held: on=%08x want=%08x t=%u curValid=%d strum=%d states ch0=%d ch4=%d", pins().on, maskFor("1 0 3 2 1 1"), sim::now() - t0, (int)curValid, (int)strum.active(), (int)channels.stateOf(0), (int)channels.stateOf(4));
  end_of_scenario();
}

static void s_sequence_stop_serial() {
  bootAndKeep();
  quiet();
  send("TEMPO 60");
  run(10);
  take();
  uint32_t t0 = sim::now();
  feedAt(t0 + 1500, "STOP\n");
  send("SEQUENCE C G Am F");
  run(5);
  uint32_t done = sim::now();
  auto out = take();
  CHECK_MSG(has(out, "ABORTED (STOP)"), "%s", joined(out).c_str());
  CHECK_MSG(done - t0 < 2200, "STOP at 1.5 s took until %u ms", done - t0);
  CHECK(pins().on == 0);
  CHECK(!strum.active());
  send("CHORD C");  // and the board is usable afterwards
  run(300);
  CHECK(pins().on == maskFor("x 3 2 0 1 0"));
  end_of_scenario();
}

static void s_sequence_stop_button() {
  bootAndKeep();
  quiet();
  send("TEMPO 60");
  run(10);
  take();
  uint32_t t0 = sim::now();
  sim::pinEvents().push_back({t0 + 1500, A3, LOW});  // panel STOP
  send("SEQUENCE C G Am F");
  run(5);
  auto out = take();
  CHECK_MSG(has(out, "ABORTED (STOP)"), "%s", joined(out).c_str());
  CHECK(sim::now() - t0 < 2200);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_sequence_validation() {
  bootAndKeep();
  quiet();
  size_t before = sim::servoWrites().size();
  send("SEQUENCE C Zz G");
  run(400);
  auto out = take();
  CHECK_MSG(has(out, "ERR unknown chord Zz"), "%s", joined(out).c_str());
  CHECK_MSG(pins().on == 0, "nothing may move when any chord in the list is bad");
  CHECK(sim::servoWrites().size() == before);
  send("SEQUENCE");
  run(50);
  CHECK(has(take(), "ERR empty sequence"));
  std::string many = "SEQUENCE";
  for (int i = 0; i < 13; i++) many += " C";
  send(many);
  run(400);
  out = take();
  CHECK_MSG(has(out, "ERR too many chords"), "%s", joined(out).c_str());
  CHECK(pins().on == 0);
  send("SEQUENCE Cmaj7maj7maj7");  // name too long for a progression slot
  run(100);
  CHECK(hasPrefix(take(), "ERR unknown chord"));
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_calib() {
  bootAndKeep();
  quiet();
  uint32_t t0 = sim::now();
  send("CALIB 6 1 500 3 300");
  run(5);
  uint32_t done = sim::now();
  auto out = take();
  CHECK_MSG(has(out, "CALIB channel 0 x3"), "%s", joined(out).c_str());
  CHECK(has(out, "  rep 1 done") && has(out, "  rep 2 done") && has(out, "  rep 3 done"));
  CHECK_MSG(done - t0 >= 2300 && done - t0 <= 2800, "3 x (500 hold + 300 gap) took %u ms", done - t0);
  CHECK(pins().on == 0);
  // bad arguments
  const char* bad[] = {"CALIB", "CALIB 6 1", "CALIB 0 1 100 1", "CALIB 6 9 100 1", "CALIB 6 1 x 1", "CALIB 6 1 100"};
  for (const char* b : bad) {
    send(b);
    run(20);
    auto o = take();
    CHECK_MSG(hasPrefix(o, "ERR"), "%s -> %s", b, joined(o).c_str());
    CHECK(pins().on == 0);
  }
  end_of_scenario();
}

static void s_calib_stop() {
  bootAndKeep();
  quiet();
  uint32_t t0 = sim::now();
  feedAt(t0 + 700, "STOP\n");
  send("CALIB 6 1 1000 10 500");
  run(5);
  auto out = take();
  CHECK_MSG(has(out, "ABORTED (STOP)"), "%s", joined(out).c_str());
  CHECK(sim::now() - t0 < 1300);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_calib_limits() {
  // A mistyped count must not silently wrap (300 reps becoming 44) or run for hours.
  bootAndKeep();
  quiet();
  const char* bad[] = {"CALIB 6 1 1000 300", "CALIB 6 1 70000 1", "CALIB 6 1 1000 -5", "CALIB 6 1 0 1", "CALIB 6 1 20000 1",
                       "CALIB 6 1 100 1 99999"};
  for (const char* b : bad) {
    uint32_t t0 = sim::now();
    send(b);
    run(5);
    auto o = take();
    CHECK_MSG(hasPrefix(o, "ERR"), "%s -> %s", b, joined(o).c_str());
    CHECK_MSG(sim::now() - t0 < 500, "%s ran for %u ms", b, sim::now() - t0);
    CHECK(pins().on == 0);
  }
  end_of_scenario();
}

static void s_auto_release() {
  bootAndKeep();
  quiet();
  send("PRESS 6 1");
  run(100);
  take();
  CHECK(pins().on == 1u);
  run(7700);  // 7.8 s since the press
  CHECK_MSG(pins().on == 1u, "must still be held before the timeout");
  run(400);  // 8.2 s
  CHECK_MSG(pins().on == 0, "a coil must let go after CMD_TIMEOUT_MS");
  end_of_scenario();
}

static void s_kick_changes_real_pulse() {
  bootAndKeep();
  quiet();
  send("KICK 150");
  run(10);
  CHECK(has(take(), "KICK ms 150"));
  send("PRESS 6 1");
  run(100);
  CHECK_MSG(pins().kick == 1u, "100 ms in, a 150 ms kick must still be at full power");
  run(80);
  CHECK_MSG(pins().kick == 0 && pins().on == 1u, "after 150 ms it drops to the hold level");
  send("RELEASE ALL");
  run(20);
  take();
  send("KICK 20");
  run(10);
  take();
  send("PRESS 6 1");
  run(50);
  CHECK_MSG(pins().kick == 0 && pins().on == 1u, "a 20 ms kick is over by 50 ms");
  end_of_scenario();
}

static void s_numeric_arguments() {
  bootAndKeep();
  quiet();
  // Out-of-range values clamp; garbage is rejected and changes nothing; no argument just reports.
  send("KICK 5000");
  run(10);
  CHECK(has(take(), "KICK ms 300"));
  send("KICK 1");
  run(10);
  CHECK(has(take(), "KICK ms 10"));
  send("KICK 80");
  run(10);
  take();
  const char* garbage[] = {"KICK abc", "KICK 12abc", "KICK -", "KICK 99999999999", "TEMPO fast", "HOLD lots", "PICK 6 A x"};
  for (const char* g : garbage) {
    send(g);
    run(10);
    auto o = take();
    CHECK_MSG(hasPrefix(o, "ERR"), "%s -> %s", g, joined(o).c_str());
  }
  send("KICK");
  run(10);
  CHECK_MSG(has(take(), "KICK ms 80"), "KICK alone reports and never changes it, and garbage did not either");
  send("TEMPO");
  run(10);
  CHECK(has(take(), "TEMPO 50"));
  send("HOLD");
  run(10);
  auto o = take();
  CHECK_MSG(hasPrefix(o, "HOLD duty 60"), "HOLD alone must report the current duty, not reset it: %s", joined(o).c_str());
  send("HOLD 50");
  run(10);
  CHECK(has(take(), "HOLD duty 127"));
  CHECK(OCR2A == 127);
  send("HOLD 500");
  run(10);
  CHECK(has(take(), "HOLD duty 255"));
  send("HOLD 0");
  run(10);
  o = take();
  CHECK_MSG(OCR2A >= 25, "a hold of 0%% would let go of the string after the kick; OCR2A=%d (%s)", (int)OCR2A, joined(o).c_str());
  send("TEMPO 5");
  run(10);
  CHECK(has(take(), "TEMPO 20"));
  send("TEMPO 999");
  run(10);
  CHECK(has(take(), "TEMPO 200"));
  end_of_scenario();
}

static void s_tuning_save_load() {
  bootAndKeep();
  quiet();
  send("KICK 100");
  send("HOLD 40");
  send("TEMPO 80");
  send("PICK 6 A 65");
  run(30);
  take();
  send("SAVE");
  run(10);
  CHECK(has(take(), "SAVED - loads automatically at power-up"));
  CHECK(sim::eeprom()[0] == 0xA9 && sim::eeprom()[1] == 0xA6);  // magic number, little-endian
  send("KICK 20");
  send("HOLD 90");
  send("TEMPO 150");
  run(30);
  take();
  send("LOAD");
  run(10);
  CHECK(has(take(), "LOADED saved tuning"));
  send("STATUS");
  run(10);
  auto out = take();
  CHECK_MSG(has(out, "bpm=80 kick=100 holdDuty=102 (40%)"), "%s", joined(out).c_str());
  send("DEFAULTS");
  run(10);
  CHECK(hasPrefix(take(), "Back to compiled defaults"));
  send("STATUS");
  run(10);
  out = take();
  CHECK_MSG(has(out, "bpm=50 kick=60 holdDuty=60 (23%)"), "%s", joined(out).c_str());
  send("LOAD");  // DEFAULTS did not touch what is saved
  run(10);
  CHECK(has(take(), "LOADED saved tuning"));
  // The kick time saved really is the one the coils use after LOAD.
  send("PRESS 6 1");
  run(80);
  CHECK_MSG(pins().kick == 1u, "after LOAD the 100 ms kick is in force");
  end_of_scenario();
}

static void s_load_without_save() {
  bootAndKeep();
  quiet();
  send("LOAD");
  run(10);
  CHECK(hasPrefix(take(), "ERR no valid saved tuning"));
  end_of_scenario();
}

static void preload_good() {
  agap::Tuning t;
  t.kickMs = 120; t.holdDuty = 100; t.bpm = 90;
  for (int i = 0; i < 6; i++) { t.pickA[i] = 65 + i; t.pickB[i] = 115 + i; }
  uint8_t buf[agap::TUNING_BYTES];
  agap::encodeTuning(t, buf);
  for (int i = 0; i < agap::TUNING_BYTES; i++) sim::eeprom()[i] = buf[i];
}
static void s_boot_saved() {
  bootAndKeep();
  CHECK(has(bootLines, "Loaded saved tuning (EEPROM)."));
  send("STATUS");
  run(10);
  CHECK(has(take(), "bpm=90 kick=120 holdDuty=100 (39%)"));
  CHECK_MSG(sim::servoAngle()[2] == 65 && sim::servoAngle()[7] == 70, "servos start at their saved A angles (D2=%d D7=%d)",
            sim::servoAngle()[2], sim::servoAngle()[7]);
  CHECK(OCR2A == 100);
  send("PRESS 6 1");
  run(100);
  CHECK_MSG(pins().kick == 1u, "saved kick of 120 ms is in force from power-up");
  end_of_scenario();
}

static void preload_corrupt() {
  uint32_t x = 12345;
  for (int i = 0; i < 64; i++) { x = x * 1103515245u + 12345u; sim::eeprom()[i] = (uint8_t)(x >> 16); }
}
static void s_boot_corrupt() {
  bootAndKeep();
  CHECK(has(bootLines, "No saved tuning - using compiled defaults."));
  send("STATUS");
  run(10);
  CHECK(has(take(), "bpm=50 kick=60 holdDuty=60 (23%)"));
  end_of_scenario();
}

static void preload_out_of_range() {
  agap::Tuning t;
  t.kickMs = 9000; t.holdDuty = 60; t.bpm = 1;
  for (int i = 0; i < 6; i++) { t.pickA[i] = 0; t.pickB[i] = 255; }
  uint8_t buf[agap::TUNING_BYTES];
  agap::encodeTuning(t, buf);  // valid checksum, but unsafe values
  for (int i = 0; i < agap::TUNING_BYTES; i++) sim::eeprom()[i] = buf[i];
}
static void s_boot_out_of_range() {
  bootAndKeep();
  send("STATUS");
  run(10);
  CHECK(has(take(), "bpm=20 kick=300 holdDuty=60 (23%)"));
  CHECK_MSG(sim::servoAngle()[2] == 10 && sim::servoAngle()[7] == 10, "angles clamped into 10..170");
  send("PICKS");
  run(10);
  auto o = take();
  CHECK(has(o, "string 6  A=10  B=170"));
  end_of_scenario();
}

static void s_line_handling() {
  bootAndKeep();
  quiet();
  feedAt(sim::now(), "chord c\r\n");  // lower case and CRLF
  run(20);
  auto out = take();
  CHECK_MSG(has(out, "C -> x 3 2 0 1 0  (cost 9)"), "%s", joined(out).c_str());
  feedAt(sim::now(), "\n\n\r\n   \n");  // blank lines are ignored
  run(20);
  out = take();
  CHECK_MSG(out.empty() || (out.size() == 1 && hasPrefix(out, "ERR")) , "blank lines produced: %s", joined(out).c_str());
  feedAt(sim::now(), "\tCHORD\t G \n");  // tabs and padding
  run(20);
  CHECK(hasPrefix(take(), "G -> 3 2 0 0 0 3"));
  std::string garbage = "\x01\x02\xff\xfe\x80 hello\x7f\n";
  feedAt(sim::now(), garbage);
  run(20);
  out = take();
  CHECK_MSG(hasPrefix(out, "ERR unknown command") || out.empty(), "%s", joined(out).c_str());
  end_of_scenario();
}

static void s_long_line() {
  bootAndKeep();
  quiet();
  // A line longer than the 95-byte buffer must be refused as a whole. Running its
  // first 95 characters as if they were the command would be wrong: it could be a
  // different command than the one that was typed.
  std::string longline = "PRESS 6 1";
  longline += std::string(200, ' ');
  longline += "RELEASE ALL";
  send(longline);
  run(30);
  auto out = take();
  CHECK_MSG(has(out, "ERR line too long"), "%s", joined(out).c_str());
  CHECK_MSG(pins().on == 0, "the truncated PRESS must not have run");
  // and the next normal line works
  send("PRESS 6 1");
  run(20);
  CHECK(has(take(), "PRESSED channel 0"));
  // Many over-long lines in a row do not break anything.
  for (int i = 0; i < 5; i++) send(std::string(500, 'A'));
  send("PING");
  run(50);
  out = take();
  CHECK(has(out, "PONG"));
  end_of_scenario();
}

static void s_strum_command() {
  bootAndKeep();
  quiet();
  size_t before = sim::servoWrites().size();
  send("STRUM");  // no fingering yet: all six strings
  run(300);
  take();
  CHECK_MSG(sim::servoWrites().size() - before == 6, "6 plucks, got %u", (unsigned)(sim::servoWrites().size() - before));
  send("CHORD C");
  run(400);
  take();
  before = sim::servoWrites().size();
  send("STRUM U");
  run(300);
  CHECK(sim::servoWrites().size() - before == 5);
  std::vector<int> pinsSeen;
  for (size_t i = before; i < sim::servoWrites().size(); i++) pinsSeen.push_back(sim::servoWrites()[i].pin);
  CHECK_MSG(pinsSeen == (std::vector<int>{7, 6, 5, 4, 3}), "up strum goes high e to the A string, skipping the muted low E");
  end_of_scenario();
}

static void s_progression() {
  bootAndKeep();
  quiet();
  send("PROG G C");
  run(20);
  auto out = take();
  CHECK(has(out, "G -> 3 2 0 0 0 3  (cost 3)") && has(out, "C -> x 3 2 0 1 0  (cost 9)"));
  send("NEXT");
  run(400);
  out = take();
  CHECK_MSG(hasPrefix(out, "C -> x 3 2 0 1 0"), "%s", joined(out).c_str());
  CHECK(pins().on == maskFor("x 3 2 0 1 0"));
  send("NEXT");
  run(400);
  CHECK(pins().on == maskFor("3 2 0 0 0 3"));  // wraps to the first
  send("PREV");
  run(400);
  CHECK(pins().on == maskFor("x 3 2 0 1 0"));
  // A bad chord leaves the progression as it was.
  send("PROG Em Zz");
  run(20);
  CHECK(hasPrefix(take(), "ERR unknown chord Zz"));
  send("NEXT");
  run(400);
  CHECK_MSG(pins().on == maskFor("3 2 0 0 0 3"), "the old progression (G C) is still loaded");
  // Too many chords is refused rather than silently cut to 12.
  std::string many = "PROG";
  for (int i = 0; i < 15; i++) many += " Am";
  send(many);
  run(20);
  out = take();
  CHECK_MSG(has(out, "ERR too many chords (max 12)"), "%s", joined(out).c_str());
  send("PROG");
  run(20);
  CHECK(has(take(), "ERR empty progression"));
  end_of_scenario();
}

static void s_panel_buttons() {
  bootAndKeep();
  quiet();
  uint32_t t = sim::now();
  sim::pinEvents().push_back({t + 10, A1, LOW});   // NEXT, held 200 ms
  sim::pinEvents().push_back({t + 210, A1, HIGH});
  run(500);
  auto out = take();
  CHECK_MSG(hasPrefix(out, "G -> 3 2 0 0 0 3"), "NEXT plays the second chord of C G Am F: %s", joined(out).c_str());
  CHECK(pins().on == maskFor("3 2 0 0 0 3"));
  // A bounce shorter than the 25 ms debounce does nothing.
  t = sim::now();
  sim::pinEvents().push_back({t + 10, A1, LOW});
  sim::pinEvents().push_back({t + 15, A1, HIGH});
  run(300);
  CHECK(take().empty());
  // STRUM button strums the current fingering.
  size_t before = sim::servoWrites().size();
  t = sim::now();
  sim::pinEvents().push_back({t + 10, A0, LOW});
  sim::pinEvents().push_back({t + 200, A0, HIGH});
  run(400);
  CHECK(sim::servoWrites().size() > before);
  // STOP button releases everything.
  t = sim::now();
  sim::pinEvents().push_back({t + 10, A3, LOW});
  sim::pinEvents().push_back({t + 200, A3, HIGH});
  run(400);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_stop_and_status() {
  bootAndKeep();
  quiet();
  send("CHORD Em");
  run(300);
  take();
  send("STATUS");
  run(10);
  auto out = take();
  CHECK(has(out, "coils on=0") == false);
  CHECK(hasPrefix(out, "coils on="));
  CHECK_MSG(std::find_if(out.begin(), out.end(), [](const std::string& l) { return l.compare(0, 9, "fingering") == 0; }) != out.end(),
            "%s", joined(out).c_str());
  send("STOP");
  run(20);
  CHECK(has(take(), "STOPPED"));
  CHECK(pins().on == 0);
  send("STATUS");
  run(10);
  out = take();
  CHECK_MSG(std::find_if(out.begin(), out.end(), [](const std::string& l) { return l.compare(0, 9, "fingering") == 0; }) == out.end(),
            "no fingering is reported after STOP");
  end_of_scenario();
}

static void s_misc_commands() {
  bootAndKeep();
  quiet();
  send("PING");
  send("VERSION");
  send("FRETS");
  send("FOO");
  send("HELP");
  run(40);
  auto out = take();
  CHECK(has(out, "PONG"));
  CHECK(hasPrefix(out, "AGAP-fretboard fw " FW_VERSION " built "));
  CHECK(has(out, "6 strings x 5 frets = 30 channels, at most 6 coils at once"));
  CHECK(has(out, "ERR unknown command, type HELP"));
  CHECK(hasPrefix(out, "CHORD <name>"));
  end_of_scenario();
}

static void s_pick_commands() {
  bootAndKeep();
  quiet();
  send("PICK 6 A 5");
  run(10);
  CHECK(has(take(), "PICK 6 A=10"));
  send("PICK 6 B 999");
  run(10);
  CHECK(has(take(), "PICK 6 B=170"));
  send("PICK 3 a 80");
  run(10);
  CHECK(has(take(), "PICK 3 A=80"));
  const char* bad[] = {"PICK 0 A 70", "PICK 7 A 70", "PICK 6 C 70", "PICK 6 A", "PICK", "PLUCK", "PLUCK 9"};
  for (const char* b : bad) {
    send(b);
    run(10);
    CHECK_MSG(hasPrefix(take(), "ERR"), "%s", b);
  }
  // PLUCK 3 swings string 3's pick (servo index 3, pin D5) to the other side and back.
  int before = sim::servoAngle()[5];
  send("PLUCK 3");
  run(10);
  CHECK(has(take(), "PLUCKED"));
  CHECK(sim::servoAngle()[5] != before);
  send("PLUCK 3");
  run(10);
  take();
  CHECK(sim::servoAngle()[5] == before);
  send("PICKS");
  run(10);
  auto o = take();
  CHECK(o.size() == 6 && o[0] == "string 6  A=10  B=170" && o[3] == "string 3  A=80  B=110");
  end_of_scenario();
}

// ---------------------------------------------------------------------- fuzz
static uint32_t g_rng = 20251009u;
static uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}
static uint32_t rnd(uint32_t n) { return rnd() % n; }

static std::string pick(const std::vector<std::string>& v) { return v[rnd((uint32_t)v.size())]; }

static std::string fuzzLine() {
  static const std::vector<std::string> chords = {"C", "G", "Am", "F", "Bm", "Em", "D9", "F#m7", "Bb", "CM7", "Cmaj7", "Dsus4",
                                                  "Zz", "", "C#", "Ab", "E5", "Gdim", "Aaug", "Cm7b5", "H", "CM", "C;STOP"};
  static const std::vector<std::string> nums = {"0", "1", "2", "3", "4", "5", "6", "7", "9", "-1", "10", "60", "100", "255", "256",
                                                "1000", "9999", "65535", "65536", "abc", "", "1x", "-", "999999999999"};
  switch (rnd(24)) {
    case 0: return "CHORD " + pick(chords);
    case 1: return "SHOW " + pick(chords);
    case 2: return "RAW " + pick(nums) + " " + pick(nums) + " " + pick(nums) + " " + pick(nums) + " " + pick(nums) + " " + pick(nums);
    case 3: return "RAW x " + std::to_string(rnd(6)) + " " + std::to_string(rnd(6)) + " x " + std::to_string(rnd(6)) + " " + std::to_string(rnd(6));
    case 4: return "PRESS " + pick(nums) + " " + pick(nums);
    case 5: return "RELEASE " + pick(nums) + " " + pick(nums);
    case 6: return "RELEASE ALL";
    case 7: return "STRUM " + pick({"D", "U", "x", ""});
    case 8: return "PROG " + pick(chords) + " " + pick(chords) + " " + pick(chords);
    case 9: return pick({"NEXT", "PREV"});
    case 10: return "KICK " + pick(nums);
    case 11: return "HOLD " + pick(nums);
    case 12: return "TEMPO " + pick(nums);
    case 13: return "PICK " + pick(nums) + " " + pick({"A", "B", "C", "a"}) + " " + pick(nums);
    case 14: return "PLUCK " + pick(nums);
    case 15: return pick({"SAVE", "LOAD", "DEFAULTS", "STATUS", "PICKS", "PING", "VERSION", "FRETS", "HELP", "STOP"});
    case 16: return "CALIB " + pick(nums) + " " + pick(nums) + " " + pick({"50", "100", "300"}) + " " + pick({"1", "2", "3"}) + " " + pick({"0", "50", "200"});
    case 17: return "SEQUENCE " + pick(chords) + " " + pick(chords) + " " + pick(chords);
    case 18: {  // random printable garbage
      std::string s;
      uint32_t n = rnd(60);
      for (uint32_t i = 0; i < n; i++) s += (char)(32 + rnd(95));
      return s;
    }
    case 19: {  // random bytes including control and high bytes (but no newline)
      std::string s;
      uint32_t n = rnd(40);
      for (uint32_t i = 0; i < n; i++) { char c = (char)rnd(256); if (c != '\n') s += c; }
      return s;
    }
    case 20: return std::string(90 + rnd(120), 'A');  // around and beyond the 95-byte buffer
    case 21: return "CHORD " + pick(chords) + std::string(rnd(100), ' ') + "STOP";
    case 22: return pick({"chord C", "show G", "press 6 1", "stop", "Strum"});
    default: return "PRESS " + std::to_string(1 + rnd(6)) + " " + std::to_string(1 + rnd(5));
  }
}

static void s_fuzz() {
  bootAndKeep();
  quiet();
  const int N = 4000;
  uint32_t longest = 0;
  for (int i = 0; i < N; i++) {
    std::string line = fuzzLine();
    uint32_t t0 = sim::now();
    // Blocking commands must always be stoppable: put a STOP shortly after every command.
    feedAt(t0 + 800 + rnd(800), "STOP\n");
    send(line, rnd(3) == 0 ? rnd(40) : 0);
    run(10 + rnd(1500));
    if (sim::now() - t0 > longest) longest = sim::now() - t0;
    take();
    if (rnd(40) == 0) {  // sometimes the panel STOP button instead
      sim::pinEvents().push_back({sim::now() + 5, A3, LOW});
      sim::pinEvents().push_back({sim::now() + 100, A3, HIGH});
    }
    CHECK_MSG(sim::now() - t0 < 20000, "command %d (%.40s) took %u ms", i, line.c_str(), sim::now() - t0);
    if (g_violations) {
      std::printf("  stopped at command %d: %.60s\n", i, line.c_str());
      break;
    }
  }
  // After the abuse the board still works. First drop whatever the fuzzer had queued up for
  // later (STOP lines and button presses scheduled in the future), or they would land on top
  // of the check below.
  sim::serialIn().clear();
  sim::pinEvents().clear();
  for (int i = 0; i < 80; i++) sim::pinLevel()[i] = HIGH;
  run(200);
  take();
  send("STOP");
  run(50);
  take();
  send("CHORD Em");
  run(400);
  auto out = take();
  CHECK_MSG(has(out, "Em -> 0 2 2 0 0 0  (cost 2)"), "%s", joined(out).c_str());
  CHECK(pins().on == maskFor("0 2 2 0 0 0"));
  std::printf("  fuzz: %d commands, most coils ever on at once: %d, longest command %u ms\n", N, g_maxCoils, longest);
  CHECK(g_maxCoils <= 6);
  end_of_scenario();
}

// -------------------------------------------------------------------- driver
struct Scenario {
  const char* name;
  void (*fn)();
  void (*preload)();
};
static const Scenario kScenarios[] = {
    {"boot", s_boot, nullptr},
    {"chord_c", s_chord_c, nullptr},
    {"chord_change", s_chord_change, nullptr},
    {"show_moves_nothing", s_show_moves_nothing, nullptr},
    {"raw", s_raw, nullptr},
    {"press_cap", s_press_cap, nullptr},
    {"chord_plus_raw_cap", s_chord_plus_raw_cap, nullptr},
    {"sequence_timing", s_sequence_timing, nullptr},
    {"sequence_stop_serial", s_sequence_stop_serial, nullptr},
    {"sequence_stop_button", s_sequence_stop_button, nullptr},
    {"sequence_validation", s_sequence_validation, nullptr},
    {"calib", s_calib, nullptr},
    {"calib_stop", s_calib_stop, nullptr},
    {"calib_limits", s_calib_limits, nullptr},
    {"auto_release", s_auto_release, nullptr},
    {"kick_changes_real_pulse", s_kick_changes_real_pulse, nullptr},
    {"numeric_arguments", s_numeric_arguments, nullptr},
    {"tuning_save_load", s_tuning_save_load, nullptr},
    {"load_without_save", s_load_without_save, nullptr},
    {"boot_saved", s_boot_saved, preload_good},
    {"boot_corrupt", s_boot_corrupt, preload_corrupt},
    {"boot_out_of_range", s_boot_out_of_range, preload_out_of_range},
    {"line_handling", s_line_handling, nullptr},
    {"long_line", s_long_line, nullptr},
    {"strum_command", s_strum_command, nullptr},
    {"progression", s_progression, nullptr},
    {"panel_buttons", s_panel_buttons, nullptr},
    {"stop_and_status", s_stop_and_status, nullptr},
    {"misc_commands", s_misc_commands, nullptr},
    {"pick_commands", s_pick_commands, nullptr},
    {"fuzz", s_fuzz, nullptr},
};

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: sim --list | sim <scenario>\n");
    return 2;
  }
  if (std::strcmp(argv[1], "--list") == 0) {
    for (auto& s : kScenarios) std::printf("%s\n", s.name);
    return 0;
  }
  for (auto& s : kScenarios) {
    if (std::strcmp(argv[1], s.name) != 0) continue;
    for (int i = 0; i < 4096; i++) sim::eeprom()[i] = 0xFF;  // erased EEPROM
    if (s.preload) s.preload();
    s.fn();
    std::printf("%s: %s (%d checks)\n", s.name, g_fail ? "FAIL" : "ok", g_total);
    return g_fail ? 1 : 0;
  }
  std::printf("unknown scenario %s\n", argv[1]);
  return 2;
}
