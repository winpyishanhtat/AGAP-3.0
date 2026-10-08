// Unit tests for tools/agap_control_logic.h - native g++, no serial port.
//   g++ -std=c++14 -Wall -Wextra -I.. test_agap_control_logic.cpp -o t && ./t
// or: tools/tests/run_tests.sh

#include "../agap_control_logic.h"

#include <cstdio>

using namespace agapctl;

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

#define CHECK_THROWS(expr)                                               \
  do {                                                                   \
    g_total++;                                                           \
    bool threw = false;                                                  \
    try { expr; } catch (const JsonParseError&) { threw = true; }        \
    if (!threw) {                                                        \
      g_failures++;                                                      \
      std::printf("  FAIL %s:%d: expected JsonParseError from %s\n",     \
                  __FILE__, __LINE__, #expr);                            \
    }                                                                    \
  } while (0)

#define RUN(fn)                      \
  do {                               \
    std::printf("-- %s\n", #fn);     \
    fn();                            \
  } while (0)

static const char* kSample = R"({
  "_comment": "ignored",
  "buttons": [
    {"label": "EM", "chord": "Em", "confirmed": false},
    {"label": "D",  "chord": "D",  "confirmed": true},
    {"label": "X1", "chord": null, "confirmed": false, "note": "unidentified"}
  ]
})";

void test_parse_reads_labels_chords_and_confirmed() {
  auto m = parseButtonMap(kSample);
  CHECK(m.size() == 3);
  CHECK_STR(m[0].label, "EM");
  CHECK_STR(m[0].chord, "Em");
  CHECK(!m[0].confirmed);
  CHECK_STR(m[1].label, "D");
  CHECK(m[1].confirmed);
}

void test_parse_null_chord_becomes_empty() {
  auto m = parseButtonMap(kSample);
  CHECK_STR(m[2].label, "X1");
  CHECK_STR(m[2].chord, "");
}

void test_parse_empty_array_is_ok() {
  auto m = parseButtonMap("{\"buttons\": []}");
  CHECK(m.empty());
}

void test_parse_errors_throw() {
  CHECK_THROWS(parseButtonMap("{}"));                               // no buttons key
  CHECK_THROWS(parseButtonMap("{\"buttons\": 5}"));                 // not an array
  CHECK_THROWS(parseButtonMap("{\"buttons\": [{\"label\": \"A\""));  // unterminated
  CHECK_THROWS(loadButtonMap("this/file/does/not/exist.json"));
}

void test_resolve_by_label_case_insensitive() {
  auto m = parseButtonMap(kSample);
  CHECK_STR(resolveLabel("EM", m), "EM");
  CHECK_STR(resolveLabel("em", m), "EM");
  CHECK_STR(resolveLabel("d", m), "D");
}

void test_resolve_by_chord_name_is_exact_case() {
  auto m = parseButtonMap(kSample);
  CHECK_STR(resolveLabel("Em", m), "EM");  // chord name -> device label
  // "em" matches label EM case-insensitively, so it still resolves - but via
  // the label rule, not the chord rule. A chord name that is NOT also a
  // label in any case must be exact: "Dm" isn't in this sample at all.
  CHECK_STR(resolveLabel("Dm", m), "");
}

void test_resolve_unknown_returns_empty() {
  auto m = parseButtonMap(kSample);
  CHECK_STR(resolveLabel("ZZZ", m), "");
  CHECK_STR(resolveLabel("", m), "");
}

void test_null_chord_entry_never_matches_empty_token_as_chord() {
  auto m = parseButtonMap(kSample);
  // X1 has an empty chord; an empty token must not resolve to it.
  CHECK_STR(resolveLabel("", m), "");
}

void test_is_confirmed() {
  auto m = parseButtonMap(kSample);
  CHECK(!isConfirmed("EM", m));
  CHECK(isConfirmed("D", m));
  CHECK(!isConfirmed("NOPE", m));
}

void test_command_builders_match_firmware_protocol() {
  CHECK_STR(buildPressCmd("EM"), "PRESS EM");
  CHECK_STR(buildReleaseCmd("EM"), "RELEASE EM");
  CHECK_STR(buildReleaseCmd("ALL"), "RELEASE ALL");
  CHECK_STR(buildChordCmd("G"), "CHORD G");
  CHECK_STR(buildStopCmd(), "STOP");
  CHECK_STR(buildStatusCmd(), "STATUS");
  CHECK_STR(buildLabelsCmd(), "LABELS");
  CHECK_STR(buildTempoCmd(50), "TEMPO 50");
  CHECK_STR(buildSequenceCmd({"C", "G", "AM", "F"}), "SEQUENCE C G AM F");
  CHECK_STR(buildSequenceCmd({}), "SEQUENCE");
  CHECK_STR(buildCalibCmd("EM", 1000, 5, 800), "CALIB EM 1000 5 800");
}

// Integration check against the REAL file the firmware docs point at.
void test_real_button_map_json_parses() {
  auto m = loadButtonMap("../../AGAP_HelperButton/button_map.json");
  CHECK(m.size() == 10);
  const char* expected[] = {"EM", "AM", "D", "C", "F", "DM", "G", "BM", "X1", "X2"};
  for (size_t i = 0; i < m.size() && i < 10; i++) CHECK_STR(m[i].label, expected[i]);
  CHECK_STR(resolveLabel("Em", m), "EM");
  CHECK_STR(resolveLabel("Bm", m), "BM");
  CHECK_STR(resolveLabel("X1", m), "X1");
}

int main() {
  RUN(test_parse_reads_labels_chords_and_confirmed);
  RUN(test_parse_null_chord_becomes_empty);
  RUN(test_parse_empty_array_is_ok);
  RUN(test_parse_errors_throw);
  RUN(test_resolve_by_label_case_insensitive);
  RUN(test_resolve_by_chord_name_is_exact_case);
  RUN(test_resolve_unknown_returns_empty);
  RUN(test_null_chord_entry_never_matches_empty_token_as_chord);
  RUN(test_is_confirmed);
  RUN(test_command_builders_match_firmware_protocol);
  RUN(test_real_button_map_json_parses);

  std::printf("\n%d/%d checks passed\n", g_total - g_failures, g_total);
  return g_failures ? 1 : 0;
}
