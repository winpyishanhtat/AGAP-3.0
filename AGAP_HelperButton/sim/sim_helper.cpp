// Runs the REAL AGAP_HelperButton.ino (the earlier 10-button chord-helper build) on a PC
// against the mock Arduino in ../../sim, and checks the behaviours that matter most.
// Smaller than the fretboard's simulation: that one is the final design.
//
//   ./sim --list        list the scenarios
//   ./sim <scenario>    power on and run one scenario
// (run_tests.sh builds it and runs them all.)

#include "../AGAP_HelperButton.ino"

#include "../../sim/sim_common.h"

// Button channels: EM=0 AM=1 D=2 C=3 F=4 DM=5 G=6 BM=7 X1=8 X2=9
static void s_boot() {
  g_coilCap = 10;
  bootAndKeep();
  CHECK(has(bootLines, "AGAP (helper-button) ready - type HELP"));
  CHECK(has(bootLines, "AGAP-helper-button fw " FW_VERSION));
  CHECK(has(bootLines, "EM AM D C F DM G BM X1 X2 "));
  run(300);
  CHECK(pins().on == 0);
  for (int pin = 2; pin <= 7; pin++) CHECK(sim::servoAttached(pin));
  end_of_scenario();
}

static void s_chord_press_hold() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  uint32_t t0 = sim::now();
  send("CHORD EM");
  run(30);
  CHECK(has(take(), "CHORD EM"));
  CHECK(pins().on == 1u && pins().kick == 1u);  // kicking at full power
  run(120);
  CHECK(pins().on == 1u && pins().kick == 0);   // then holding
  run(300);
  int plucks = 0;
  for (auto& w : sim::servoWrites()) if (w.at >= t0) plucks++;
  CHECK_MSG(plucks == 6, "the helper frets every string at once, so all 6 are plucked (got %d)", plucks);
  end_of_scenario();
}

static void s_repress_keeps_the_button_down() {
  // Found by running the real firmware: re-pressing a held button in the same loop() pass made the
  // safety timeout fire at once (the refresh time was ahead of the clock update() was given).
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  send("PRESS EM");
  run(200);
  take();
  CHECK(pins().on == 1u);
  send("PRESS EM");
  run(50);
  CHECK_MSG(pins().on == 1u, "a second PRESS of a held button must keep it pressed");
  send("PRESS EM");
  send("PRESS AM");
  run(50);
  CHECK(pins().on == 3u);
  end_of_scenario();
}

static void s_sequence_stop_serial() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  send("TEMPO 60");
  run(10);
  take();
  uint32_t t0 = sim::now();
  feedAt(t0 + 1500, "STOP\n");
  send("SEQUENCE EM AM D C");
  run(5);
  auto out = take();
  CHECK_MSG(has(out, "ABORTED (STOP)"), "%s", joined(out).c_str());
  CHECK_MSG(sim::now() - t0 < 2200, "STOP at 1.5 s took until %u ms", sim::now() - t0);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_sequence_stop_button() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  send("TEMPO 60");
  run(10);
  take();
  uint32_t t0 = sim::now();
  sim::pinEvents().push_back({t0 + 1500, A3, LOW});
  send("SEQUENCE EM AM D C");
  run(5);
  CHECK(has(take(), "ABORTED (STOP)"));
  CHECK(sim::now() - t0 < 2200);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_sequence_validation() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  size_t before = sim::servoWrites().size();
  send("SEQUENCE EM Zz AM");
  run(400);
  auto out = take();
  CHECK_MSG(has(out, "ERR unknown label Zz"), "%s", joined(out).c_str());
  CHECK_MSG(pins().on == 0, "nothing may move when a label late in the list is wrong");
  CHECK(sim::servoWrites().size() == before);
  send("SEQUENCE");
  run(50);
  CHECK(has(take(), "ERR empty sequence"));
  end_of_scenario();
}

static void s_calib() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  uint32_t t0 = sim::now();
  send("CALIB EM 500 3 300");
  run(5);
  auto out = take();
  CHECK_MSG(has(out, "CALIB EM x3") && has(out, "  rep 3 done"), "%s", joined(out).c_str());
  CHECK_MSG(sim::now() - t0 >= 2300 && sim::now() - t0 <= 2800, "took %u ms", sim::now() - t0);
  CHECK(pins().on == 0);
  const char* bad[] = {"CALIB EM 1000 300", "CALIB EM 70000 1", "CALIB EM 1000 -5", "CALIB EM 0 1", "CALIB EM 20000 1",
                       "CALIB EM x 1", "CALIB ZZ 100 1", "CALIB EM 100", "CALIB EM 100 1 99999"};
  for (const char* b : bad) {
    uint32_t s0 = sim::now();
    send(b);
    run(5);
    auto o = take();
    CHECK_MSG(hasPrefix(o, "ERR"), "%s -> %s", b, joined(o).substr(0, 80).c_str());
    CHECK_MSG(sim::now() - s0 < 500, "%s ran for %u ms", b, sim::now() - s0);
    CHECK(pins().on == 0);
  }
  end_of_scenario();
}

static void s_calib_stop() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  uint32_t t0 = sim::now();
  feedAt(t0 + 700, "STOP\n");
  send("CALIB EM 1000 10 500");
  run(5);
  CHECK(has(take(), "ABORTED (STOP)"));
  CHECK(sim::now() - t0 < 1300);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_auto_release() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  send("PRESS EM");
  run(100);
  CHECK(pins().on == 1u);
  run(7700);
  CHECK(pins().on == 1u);
  run(400);
  CHECK(pins().on == 0);
  end_of_scenario();
}

static void s_numeric_arguments() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  send("KICK 80");
  run(10);
  take();
  const char* garbage[] = {"KICK abc", "KICK 12abc", "TEMPO fast", "HOLD lots", "PICK 6 A x", "PICK x A 70"};
  for (const char* g : garbage) {
    send(g);
    run(10);
    CHECK_MSG(hasPrefix(take(), "ERR"), "%s", g);
  }
  send("KICK");
  send("TEMPO");
  send("HOLD");
  run(30);
  auto out = take();
  CHECK_MSG(has(out, "KICK ms 80"), "%s", joined(out).c_str());
  CHECK(has(out, "TEMPO 50"));
  CHECK_MSG(hasPrefix(out, "HOLD duty 60"), "HOLD alone reports and must not reset: %s", joined(out).c_str());
  send("HOLD 0");
  run(10);
  take();
  CHECK_MSG(OCR2A >= 25, "a 0%% hold would drop the button after the kick (OCR2A=%d)", (int)OCR2A);
  end_of_scenario();
}

static void s_long_line() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  std::string longline = "PRESS EM" + std::string(200, ' ') + "RELEASE ALL";
  send(longline);
  run(30);
  auto out = take();
  CHECK_MSG(has(out, "ERR line too long"), "%s", joined(out).c_str());
  CHECK_MSG(pins().on == 0, "the truncated PRESS must not run");
  send("PING");
  send("PRESS EM");
  run(30);
  CHECK(has(take(), "PRESSED EM"));
  end_of_scenario();
}

static void s_fuzz() {
  g_coilCap = 10;
  bootAndKeep();
  quiet();
  static const std::vector<std::string> labels = {"EM", "AM", "D", "C", "F", "DM", "G", "BM", "X1", "X2", "em", "ZZ", "", "EM;STOP"};
  static const std::vector<std::string> nums = {"0", "1", "6", "10", "60", "100", "255", "256", "1000", "9999", "70000", "-1", "abc", "", "1x"};
  uint32_t rng = 777;
  auto rnd = [&rng](uint32_t n) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng % n; };
  auto pickOne = [&](const std::vector<std::string>& v) { return v[rnd((uint32_t)v.size())]; };
  uint32_t longest = 0;
  for (int i = 0; i < 3000; i++) {
    std::string line;
    switch (rnd(14)) {
      case 0: line = "CHORD " + pickOne(labels); break;
      case 1: line = "PRESS " + pickOne(labels); break;
      case 2: line = "RELEASE " + pickOne(labels); break;
      case 3: line = "SEQUENCE " + pickOne(labels) + " " + pickOne(labels) + " " + pickOne(labels); break;
      case 4: line = "CALIB " + pickOne(labels) + " " + pickOne(nums) + " " + pickOne(nums) + " " + pickOne(nums); break;
      case 5: line = "KICK " + pickOne(nums); break;
      case 6: line = "HOLD " + pickOne(nums); break;
      case 7: line = "TEMPO " + pickOne(nums); break;
      case 8: line = "PICK " + pickOne(nums) + " " + pickOne({"A", "B", "C"}) + " " + pickOne(nums); break;
      case 9: line = pickOne({"STRUM", "STRUM U", "SAVE", "LOAD", "DEFAULTS", "STATUS", "PICKS", "LABELS", "HELP", "STOP", "PING"}); break;
      case 10: { uint32_t n = rnd(60); for (uint32_t k = 0; k < n; k++) line += (char)(32 + rnd(95)); break; }
      case 11: line = std::string(90 + rnd(120), 'A'); break;
      case 12: line = "RELEASE ALL"; break;
      default: line = "PLUCK " + pickOne(nums); break;
    }
    uint32_t t0 = sim::now();
    feedAt(t0 + 800 + rnd(800), "STOP\n");
    send(line);
    run(10 + rnd(1500));
    longest = std::max(longest, sim::now() - t0);
    take();
    CHECK_MSG(sim::now() - t0 < 20000, "command %d took %u ms", i, sim::now() - t0);
    if (g_violations) { std::printf("  stopped at command %d: %.60s\n", i, line.c_str()); break; }
  }
  sim::serialIn().clear();
  sim::pinEvents().clear();
  for (int i = 0; i < 80; i++) sim::pinLevel()[i] = HIGH;
  run(200);
  take();
  send("STOP");
  run(50);
  take();
  send("CHORD EM");
  run(400);
  CHECK(has(take(), "CHORD EM"));
  CHECK(pins().on == 1u);
  std::printf("  fuzz: 3000 commands, most coils on at once: %d, longest command %u ms\n", g_maxCoils, longest);
  end_of_scenario();
}

struct Scenario {
  const char* name;
  void (*fn)();
};
static const Scenario kScenarios[] = {
    {"boot", s_boot},
    {"chord_press_hold", s_chord_press_hold},
    {"repress_keeps_the_button_down", s_repress_keeps_the_button_down},
    {"sequence_stop_serial", s_sequence_stop_serial},
    {"sequence_stop_button", s_sequence_stop_button},
    {"sequence_validation", s_sequence_validation},
    {"calib", s_calib},
    {"calib_stop", s_calib_stop},
    {"auto_release", s_auto_release},
    {"numeric_arguments", s_numeric_arguments},
    {"long_line", s_long_line},
    {"fuzz", s_fuzz},
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
    for (int i = 0; i < 4096; i++) sim::eeprom()[i] = 0xFF;
    s.fn();
    std::printf("%s: %s (%d checks)\n", s.name, g_fail ? "FAIL" : "ok", g_total);
    return g_fail ? 1 : 0;
  }
  std::printf("unknown scenario %s\n", argv[1]);
  return 2;
}
