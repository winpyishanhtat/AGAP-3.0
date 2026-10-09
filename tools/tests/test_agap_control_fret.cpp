// Unit tests for tools/agap_control_fret.h - the fretboard (final design) half of the C++ control tool.
//   g++ -std=c++14 -Wall -Wextra -Werror -I.. test_agap_control_fret.cpp -o t && ./t
// or: tools/tests/run_tests.sh
//
// The planner turns what you typed into the exact serial lines the firmware expects, so every
// refusal here is a command the firmware would have refused (or misread) anyway, caught earlier
// and with a clearer message. Limits are the firmware's own: AGAP_Fretboard.ino.

#include "../agap_control_fret.h"

#include <cstdio>

using namespace agapctl::fret;

static int g_total = 0;
static int g_failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    g_total++;                                                           \
    if (!(cond)) {                                                       \
      g_failures++;                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                    \
  } while (0)

#define CHECK_STR(a, b)                                                  \
  do {                                                                   \
    g_total++;                                                           \
    std::string va = (a), vb = (b);                                      \
    if (va != vb) {                                                      \
      g_failures++;                                                      \
      std::printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n",         \
                  __FILE__, __LINE__, #a, va.c_str(), vb.c_str());       \
    }                                                                    \
  } while (0)

#define RUN(fn)                                                          \
  do {                                                                   \
    int before = g_failures;                                             \
    fn();                                                                \
    std::printf("%-58s %s\n", #fn, g_failures == before ? "ok" : "FAILED"); \
  } while (0)

static std::vector<std::string> V(std::initializer_list<const char*> w) {
  std::vector<std::string> v;
  for (const char* s : w) v.push_back(s);
  return v;
}

// ---------------------------------------------------------------- numbers
static void test_parse_int_is_strict() {
  int v = -1;
  CHECK(parseInt("5", 1, 9, v) && v == 5);
  CHECK(parseInt("200", 20, 200, v) && v == 200);
  const char* bad[] = {"", "abc", "5x", "x5", " 5", "5 ", "+5", "-", "--5", "0x10", "1e3", "5.0", "٣", "99999999999999999999"};
  for (const char* s : bad) CHECK(!parseInt(s, -1000, 1000000, v));
  CHECK(!parseInt("0", 1, 9, v));
  CHECK(!parseInt("10", 1, 9, v));
  CHECK(parseInt("-3", -5, 5, v) && v == -3);
}

// ------------------------------------------------------------- chord names
static void test_chord_names_follow_the_bridges_rule() {
  const char* ok[] = {"C", "Am", "F#m7", "Bb", "CM7", "Cmaj7", "G7", "Dsus4", "E-7", "a", "Cadd9", "C+", "C#b", "g#"};
  for (const char* s : ok) CHECK(isChordName(s));
  const char* bad[] = {"", "H", "C 7", "C\n", " C", "C;STOP", "C/G", "Cmaj7sus4extra", "C'", "\xc3\x84", "1C", "#C"};
  for (const char* s : bad) CHECK(!isChordName(s));
  CHECK(isChordName(std::string("C") + std::string(10, 'a')));
  CHECK(!isChordName(std::string("C") + std::string(11, 'a')));
}

// --------------------------------------------------------------- the plans
static void test_simple_commands_are_uppercased_single_lines() {
  const char* words[] = {"status", "stop", "version", "ping", "frets", "help"};
  const char* lines[] = {"STATUS", "STOP", "VERSION", "PING", "FRETS", "HELP"};
  for (int i = 0; i < 6; i++) {
    Plan p = plan(words[i], {});
    CHECK(p.ok());
    CHECK(p.steps.size() == 1);
    CHECK_STR(p.steps[0].line, lines[i]);
    CHECK(!p.steps[0].stream);
  }
  CHECK(!plan("status", V({"extra"})).ok());
  CHECK(plan("STOP", {}).ok());                  // the command word is case-insensitive
}

static void test_chord_and_show() {
  Plan c = plan("chord", V({"F#m7"}));
  CHECK(c.ok());
  CHECK_STR(c.steps[0].line, "CHORD F#m7");
  CHECK(c.steps[0].waitMs >= 400);
  Plan s = plan("show", V({"Bm"}));
  CHECK_STR(s.steps[0].line, "SHOW Bm");
  CHECK(!plan("chord", {}).ok());
  CHECK(!plan("chord", V({"C", "G"})).ok());
  CHECK(!plan("chord", V({"C;STOP"})).ok());     // would otherwise reach the serial port as typed
  CHECK(!plan("show", V({"not a chord"})).ok());
  CHECK(!plan("chord", V({""})).ok());
}

static void test_strum() {
  CHECK_STR(plan("strum", {}).steps[0].line, "STRUM D");
  CHECK_STR(plan("strum", V({"u"})).steps[0].line, "STRUM U");
  CHECK_STR(plan("strum", V({"D"})).steps[0].line, "STRUM D");
  CHECK(!plan("strum", V({"sideways"})).ok());
  CHECK(!plan("strum", V({"D", "U"})).ok());
}

static void test_press_and_release_take_a_string_and_a_fret() {
  CHECK_STR(plan("press", V({"6", "1"})).steps[0].line, "PRESS 6 1");
  CHECK_STR(plan("press", V({"1", "5"})).steps[0].line, "PRESS 1 5");
  CHECK_STR(plan("release", V({"3", "2"})).steps[0].line, "RELEASE 3 2");
  CHECK_STR(plan("release", V({"all"})).steps[0].line, "RELEASE ALL");
  CHECK_STR(plan("release", V({"ALL"})).steps[0].line, "RELEASE ALL");
  const char* bad[][2] = {{"0", "1"}, {"7", "1"}, {"1", "0"}, {"1", "6"}, {"a", "1"}, {"1", "b"}, {"-1", "1"}, {"1.5", "1"}, {"", "1"}};
  for (auto& b : bad) {
    CHECK(!plan("press", V({b[0], b[1]})).ok());
    CHECK(!plan("release", V({b[0], b[1]})).ok());
  }
  CHECK(!plan("press", V({"3"})).ok());
  CHECK(!plan("press", V({"3", "1", "1"})).ok());
  CHECK(!plan("release", {}).ok());
}

static void test_raw_takes_exactly_six_values() {
  CHECK_STR(plan("raw", V({"x", "3", "2", "0", "1", "0"})).steps[0].line, "RAW x 3 2 0 1 0");
  CHECK_STR(plan("raw", V({"X", "3", "2", "0", "1", "0"})).steps[0].line, "RAW x 3 2 0 1 0");
  CHECK(!plan("raw", V({"x", "3", "2", "0", "1"})).ok());
  CHECK(!plan("raw", V({"x", "3", "2", "0", "1", "0", "0"})).ok());
  CHECK(!plan("raw", V({"x", "3", "2", "0", "1", "6"})).ok());
  CHECK(!plan("raw", V({"x", "3", "2", "0", "1", "-1"})).ok());
  CHECK(!plan("raw", V({"x", "3", "2", "0", "1", "y"})).ok());
}

static void test_sequence_builds_tempo_then_sequence_and_streams() {
  Plan p = plan("sequence", V({"C", "G", "Am", "F"}));
  CHECK(p.ok());
  CHECK(p.steps.size() == 2);
  CHECK_STR(p.steps[0].line, "TEMPO 50");
  CHECK_STR(p.steps[1].line, "SEQUENCE C G Am F");
  CHECK(p.steps[1].stream);
  CHECK(p.steps[1].waitMs >= 4 * 60000 / 50);       // at least the playing time
  Plan q = plan("sequence", V({"C", "G", "--bpm", "120"}));
  CHECK_STR(q.steps[0].line, "TEMPO 120");
  CHECK_STR(q.steps[1].line, "SEQUENCE C G");
  Plan r = plan("sequence", V({"--bpm", "90", "Em"}));
  CHECK_STR(r.steps[0].line, "TEMPO 90");
}

static void test_sequence_limits_are_the_firmwares() {
  std::vector<std::string> twelve(12, "C"), thirteen(13, "C");
  CHECK(plan("sequence", twelve).ok());
  Plan big = plan("sequence", thirteen);
  CHECK(!big.ok());
  CHECK(big.error.find("12") != std::string::npos);
  CHECK(!plan("sequence", {}).ok());
  CHECK(!plan("sequence", V({"--bpm", "90"})).ok());
  CHECK(plan("sequence", V({"C", "--bpm", "20"})).ok());
  CHECK(plan("sequence", V({"C", "--bpm", "200"})).ok());
  CHECK(!plan("sequence", V({"C", "--bpm", "19"})).ok());
  CHECK(!plan("sequence", V({"C", "--bpm", "201"})).ok());
  CHECK(!plan("sequence", V({"C", "--bpm", "abc"})).ok());     // atoi used to turn this into 0 and divide by it
  CHECK(!plan("sequence", V({"C", "--bpm", "0"})).ok());
  CHECK(!plan("sequence", V({"C", "--bpm"})).ok());
  CHECK(!plan("sequence", V({"C", "G;STOP"})).ok());
  CHECK(!plan("sequence", V({"C", "--bpm", "60", "--bpm", "70"})).ok());
}

static void test_calib_defaults_ranges_and_warning() {
  Plan p = plan("calib", V({"3", "2"}));
  CHECK(p.ok());
  CHECK_STR(p.steps[0].line, "CALIB 3 2 1000 5 800");
  CHECK(p.steps[0].stream);
  CHECK(p.steps[0].waitMs >= (1000 + 800) * 5);
  CHECK(!p.notes.empty());                            // repeated presses: remind about heat
  Plan q = plan("calib", V({"6", "1", "--hold-ms", "500", "--reps", "3", "--gap-ms", "0"}));
  CHECK_STR(q.steps[0].line, "CALIB 6 1 500 3 0");
  CHECK(!plan("calib", V({"3", "2", "--hold-ms", "9"})).ok());
  CHECK(!plan("calib", V({"3", "2", "--hold-ms", "7001"})).ok());
  CHECK(!plan("calib", V({"3", "2", "--reps", "0"})).ok());
  CHECK(!plan("calib", V({"3", "2", "--reps", "101"})).ok());
  CHECK(!plan("calib", V({"3", "2", "--gap-ms", "10001"})).ok());
  CHECK(!plan("calib", V({"3", "2", "--hold-ms", "x"})).ok());
  CHECK(!plan("calib", V({"3"})).ok());
  CHECK(!plan("calib", V({"7", "2"})).ok());
}

static void test_unknown_commands_and_the_helper_only_ones() {
  Plan p = plan("frobnicate", {});
  CHECK(!p.ok());
  CHECK(p.error.find("frobnicate") != std::string::npos);
  Plan l = plan("labels", {});                         // a helper-build command
  CHECK(!l.ok());
  CHECK(l.error.find("--helper") != std::string::npos);
}

static void test_a_plan_never_contains_a_newline() {
  // every line is written followed by one newline; an embedded one would be a second command
  const char* evil[] = {"C\nSTOP", "C\rSTOP", "C\tSTOP", "C\x1b[2J"};
  for (const char* e : evil) {
    CHECK(!plan("chord", V({e})).ok());
    CHECK(!plan("sequence", V({"C", e})).ok());
  }
}

// ----------------------------------------------------- reading the board back
static void test_detect_which_firmware_is_on_the_board() {
  CHECK(detectFirmware(V({"AGAP (fretboard) ready - type HELP"})) == Firmware::Fretboard);
  CHECK(detectFirmware(V({"C -> x 3 2 0 1 0  (cost 9)", "AGAP (fretboard) ready - type HELP"})) == Firmware::Fretboard);
  CHECK(detectFirmware(V({"AGAP-fretboard fw 0.1 built Oct  9 2026 10:00:00"})) == Firmware::Fretboard);
  CHECK(detectFirmware(V({"AGAP (helper-button) ready - type HELP"})) == Firmware::Helper);
  CHECK(detectFirmware(V({"AGAP-helper-button fw 0.4"})) == Firmware::Helper);
  CHECK(detectFirmware(V({"hello"})) == Firmware::Unknown);
  CHECK(detectFirmware({}) == Firmware::Unknown);
}

static void test_fingering_lines_are_read_like_the_pages_parser_reads_them() {
  Fingering f;
  CHECK(parseFingering("Bm -> x 2 0 4 0 2  (cost 9)", f));
  CHECK_STR(f.name, "Bm");
  CHECK(f.frets[0] == -1 && f.frets[1] == 2 && f.frets[2] == 0 && f.frets[3] == 4 && f.frets[4] == 0 && f.frets[5] == 2);
  CHECK(f.hasCost && f.cost == 9);
  CHECK(parseFingering("RAW -> x 3 2 0 1 0 ", f));
  CHECK(!f.hasCost);
  CHECK(parseFingering("F#m7 -> 2 4 2 2 2 2  (cost 20)", f));
  CHECK_STR(f.name, "F#m7");
  const char* bad[] = {"", "ERR unknown chord", "CHORD Bm", "STOPPED", "Bm -> x 2 0 4 0", "Bm -> x 2 0 4 0 2 9",
                       "X -> 0 0 0 0 0 6", "Bm -> x 2 0 4 0 -3", "-> 1 2 3 4 5 5", "Bm -> x 2 0 4 0 y"};
  for (const char* s : bad) CHECK(!parseFingering(s, f));
}

static void test_the_diagram() {
  Fingering f;
  parseFingering("Bm -> x 2 0 4 0 2  (cost 9)", f);
  std::string d = renderDiagram(f);
  CHECK(d.find("E  A  D  G  B  e") != std::string::npos);        // string names, low E first
  CHECK(d.find("x") != std::string::npos);                       // muted low E
  CHECK(d.find("o") != std::string::npos);                       // open strings
  int dots = 0;
  for (char c : d) if (c == '@') dots++;
  CHECK(dots == 3);                                              // one per pressed coil
  Fingering g;
  parseFingering("C -> x 3 2 0 1 0  (cost 9)", g);
  std::string e = renderDiagram(g);
  int dots2 = 0;
  for (char c : e) if (c == '@') dots2++;
  CHECK(dots2 == 3);
  Fingering open;
  parseFingering("O -> 0 0 0 0 0 0", open);
  CHECK(renderDiagram(open).find('@') == std::string::npos);
  CHECK(!renderDiagram(open).empty());
}

static void test_usage_names_every_command_the_planner_accepts() {
  std::string u = usage();
  const char* cmds[] = {"chord", "show", "strum", "press", "release", "raw", "sequence", "calib", "status", "stop"};
  for (const char* c : cmds) CHECK(u.find(c) != std::string::npos);
  CHECK(u.find("--bpm") != std::string::npos);
}

int main() {
  RUN(test_parse_int_is_strict);
  RUN(test_chord_names_follow_the_bridges_rule);
  RUN(test_simple_commands_are_uppercased_single_lines);
  RUN(test_chord_and_show);
  RUN(test_strum);
  RUN(test_press_and_release_take_a_string_and_a_fret);
  RUN(test_raw_takes_exactly_six_values);
  RUN(test_sequence_builds_tempo_then_sequence_and_streams);
  RUN(test_sequence_limits_are_the_firmwares);
  RUN(test_calib_defaults_ranges_and_warning);
  RUN(test_unknown_commands_and_the_helper_only_ones);
  RUN(test_a_plan_never_contains_a_newline);
  RUN(test_detect_which_firmware_is_on_the_board);
  RUN(test_fingering_lines_are_read_like_the_pages_parser_reads_them);
  RUN(test_the_diagram);
  RUN(test_usage_names_every_command_the_planner_accepts);
  std::printf("\n%d/%d checks passed\n", g_total - g_failures, g_total);
  return g_failures ? 1 : 0;
}
