#ifndef AGAP_CONTROL_FRET_H
#define AGAP_CONTROL_FRET_H

#include <cstddef>
#include <string>
#include <vector>

// The fretboard (final 30-solenoid design) half of the C++ control tool. Serial-independent, like
// agap_control_logic.h: it turns what you typed into the exact lines AGAP_Fretboard.ino expects, with
// strict argument checking, and reads the board's fingering reply back. Compiles with plain g++ and is
// unit-tested natively (tools/tests/test_agap_control_fret.cpp).
//
// Limits are the firmware's own (AGAP_Fretboard.ino): a sequence is at most 12 chords (MAX_PROG),
// CALIB takes hold 10-7000 ms, reps 1-100 and gap 0-10000 ms, strings are 1-6 (6 = low E), frets 1-5.
// Tempo 20-200 matches the remote bridge. Every refusal here is something the firmware would have
// refused or misread anyway; catching it first gives a clearer message and never sends a half-valid line.

namespace agapctl {
namespace fret {

// ------------------------------------------------------------------ numbers
// A whole decimal number in [lo, hi] and nothing else (no sign other than a leading '-', no spaces,
// no trailing text). atoi() turned "abc" into 0, and the old tool then divided by it.
inline bool parseInt(const std::string& s, int lo, int hi, int& out) {
  if (s.empty() || s.size() > 11) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-') { neg = true; i = 1; }
  if (i >= s.size()) return false;
  long long v = 0;
  for (; i < s.size(); i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  if (neg) v = -v;
  if (v < lo || v > hi) return false;
  out = (int)v;
  return true;
}

// Same rule as the remote bridge: a letter A-G (either case), an optional # or b, then up to 10 ASCII
// letters, digits, '+' or '-'. Nothing else can reach the serial port.
inline bool isChordName(const std::string& s) {
  if (s.size() < 1 || s.size() > 12) return false;
  auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
  if (!((s[0] >= 'A' && s[0] <= 'G') || (s[0] >= 'a' && s[0] <= 'g'))) return false;
  size_t i = 1;
  if (i < s.size() && (s[i] == '#' || s[i] == 'b')) i++;
  size_t rest = s.size() - i;
  if (rest > 10) return false;
  for (; i < s.size(); i++) {
    char c = s[i];
    if (!(letter(c) || (c >= '0' && c <= '9') || c == '+' || c == '-')) return false;
  }
  return true;
}

// -------------------------------------------------------------------- plans
struct Step {
  std::string line;    // sent as is, followed by one newline
  int waitMs = 300;    // how long to collect replies afterwards
  bool stream = false; // a long command (SEQUENCE, CALIB): keep printing until waitMs, or ABORTED / ERR
};

struct Plan {
  std::string error;                // non-empty: nothing is sent; print this and the usage
  std::vector<Step> steps;
  std::vector<std::string> notes;   // warnings to show before sending
  bool ok() const { return error.empty(); }
};

constexpr int kMaxSequence = 12;
constexpr int kBpmMin = 20, kBpmMax = 200, kBpmDefault = 50;

namespace detail {

inline std::string upper(std::string s) {
  for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  return s;
}

inline Plan fail(const std::string& msg) {
  Plan p;
  p.error = msg;
  return p;
}

inline Plan one(const std::string& line, int waitMs) {
  Plan p;
  Step s;
  s.line = line;
  s.waitMs = waitMs;
  p.steps.push_back(s);
  return p;
}

// Removes "--name value" from args. Returns false (with err set) if the value is missing, not a whole
// number in range, or the option appears twice. `found` says whether it was there.
inline bool takeInt(std::vector<std::string>& args, const std::string& name, int lo, int hi, int& value,
                    bool& found, std::string& err) {
  found = false;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] != name) continue;
    if (found) { err = name + " given twice"; return false; }
    if (i + 1 >= args.size()) { err = name + " needs a value"; return false; }
    if (!parseInt(args[i + 1], lo, hi, value)) {
      err = name + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi) +
            ", not '" + args[i + 1] + "'";
      return false;
    }
    found = true;
    args.erase(args.begin() + (long)i, args.begin() + (long)i + 2);
    i--;
  }
  return true;
}

inline bool stringArg(const std::string& s, int& v) { return parseInt(s, 1, 6, v); }
inline bool fretArg(const std::string& s, int& v) { return parseInt(s, 1, 5, v); }

}  // namespace detail

inline Plan plan(const std::string& commandWord, std::vector<std::string> args) {
  using namespace detail;
  const std::string cmd = upper(commandWord);

  if (cmd == "STATUS" || cmd == "STOP" || cmd == "VERSION" || cmd == "PING" || cmd == "FRETS" || cmd == "HELP") {
    if (!args.empty()) return fail(commandWord + " takes no arguments");
    return one(cmd, cmd == "HELP" ? 600 : 300);
  }

  if (cmd == "CHORD" || cmd == "SHOW") {
    if (args.size() != 1) return fail(commandWord + " takes exactly one chord name, e.g. " + commandWord + " F#m7");
    if (!isChordName(args[0]))
      return fail("'" + args[0] + "' is not a chord name (a letter A-G, optional # or b, then letters, digits, + or -)");
    return one(cmd + " " + args[0], cmd == "CHORD" ? 600 : 400);
  }

  if (cmd == "STRUM") {
    if (args.size() > 1) return fail("strum takes D (down) or U (up)");
    std::string d = args.empty() ? "D" : upper(args[0]);
    if (d != "D" && d != "U") return fail("strum direction must be D or U, not '" + args[0] + "'");
    return one("STRUM " + d, 300);
  }

  if (cmd == "PRESS") {
    int s = 0, f = 0;
    if (args.size() != 2 || !stringArg(args[0], s) || !fretArg(args[1], f))
      return fail("press needs a string (1-6, 6 = low E) and a fret (1-5), e.g. press 6 1");
    return one("PRESS " + std::to_string(s) + " " + std::to_string(f), 300);
  }

  if (cmd == "RELEASE") {
    if (args.size() == 1 && upper(args[0]) == "ALL") return one("RELEASE ALL", 300);
    int s = 0, f = 0;
    if (args.size() != 2 || !stringArg(args[0], s) || !fretArg(args[1], f))
      return fail("release needs ALL, or a string (1-6) and a fret (1-5), e.g. release 6 1");
    return one("RELEASE " + std::to_string(s) + " " + std::to_string(f), 300);
  }

  if (cmd == "RAW") {
    if (args.size() != 6) return fail("raw needs six values, low E first, each x or a fret 0-5, e.g. raw x 3 2 0 1 0");
    std::string line = "RAW";
    for (const auto& a : args) {
      int v = 0;
      if (a == "x" || a == "X") line += " x";
      else if (parseInt(a, 0, 5, v)) line += " " + std::to_string(v);
      else return fail("raw values are x or 0-5, not '" + a + "'");
    }
    return one(line, 400);
  }

  if (cmd == "SEQUENCE") {
    int bpm = kBpmDefault;
    bool found = false;
    std::string err;
    if (!takeInt(args, "--bpm", kBpmMin, kBpmMax, bpm, found, err)) return fail(err);
    if (args.empty()) return fail("sequence needs at least one chord, e.g. sequence C G Am F --bpm 60");
    if ((int)args.size() > kMaxSequence)
      return fail("the board plays at most " + std::to_string(kMaxSequence) + " chords in one sequence (you gave " +
                  std::to_string(args.size()) + ")");
    std::string line = "SEQUENCE";
    for (const auto& a : args) {
      if (!isChordName(a)) return fail("'" + a + "' is not a chord name");
      line += " " + a;
    }
    Plan p = one("TEMPO " + std::to_string(bpm), 150);
    Step s;
    s.line = line;
    s.stream = true;
    s.waitMs = (int)(60000L * (long)args.size() / bpm) + 5000;   // one beat per chord, plus slack
    p.steps.push_back(s);
    return p;
  }

  if (cmd == "CALIB") {
    int hold = 1000, reps = 5, gap = 800;
    bool f1 = false, f2 = false, f3 = false;
    std::string err;
    if (!takeInt(args, "--hold-ms", 10, 7000, hold, f1, err)) return fail(err);
    if (!takeInt(args, "--reps", 1, 100, reps, f2, err)) return fail(err);
    if (!takeInt(args, "--gap-ms", 0, 10000, gap, f3, err)) return fail(err);
    int s = 0, f = 0;
    if (args.size() != 2 || !stringArg(args[0], s) || !fretArg(args[1], f))
      return fail("calib needs a string (1-6) and a fret (1-5), e.g. calib 3 2 --hold-ms 800 --reps 3");
    Plan p;
    Step st;
    st.line = "CALIB " + std::to_string(s) + " " + std::to_string(f) + " " + std::to_string(hold) + " " +
              std::to_string(reps) + " " + std::to_string(gap);
    st.stream = true;
    st.waitMs = (hold + gap) * reps + 5000;
    p.steps.push_back(st);
    p.notes.push_back("CALIB presses the same coil " + std::to_string(reps) + " times; watch it for heat and press Ctrl+C or run `stop` if anything smells.");
    return p;
  }

  if (cmd == "LABELS" || cmd == "CHORDS")
    return fail("'" + commandWord + "' belongs to the earlier chord-helper firmware; add --helper to use it");
  return fail("unknown command '" + commandWord + "' (run with --help)");
}

// ------------------------------------------------------ reading the board back
enum class Firmware { Unknown, Fretboard, Helper };

inline Firmware detectFirmware(const std::vector<std::string>& lines) {
  for (const auto& l : lines) {
    if (l.find("(fretboard)") != std::string::npos || l.find("AGAP-fretboard") != std::string::npos) return Firmware::Fretboard;
    if (l.find("(helper-button)") != std::string::npos || l.find("AGAP-helper-button") != std::string::npos) return Firmware::Helper;
  }
  return Firmware::Unknown;
}

struct Fingering {
  std::string name;
  int frets[6] = {0, 0, 0, 0, 0, 0};   // -1 = muted, 0 = open, 1-5 = pressed; index 0 = low E
  bool hasCost = false;
  int cost = 0;
};

// "Bm -> x 2 0 4 0 2  (cost 9)"  (the firmware's printVoicing; RAW lines have no cost)
inline bool parseFingering(const std::string& line, Fingering& out) {
  size_t arrow = line.find(" -> ");
  if (arrow == std::string::npos || arrow == 0 || line.find(' ') != arrow) return false;
  Fingering f;
  f.name = line.substr(0, arrow);
  size_t i = arrow + 4;
  for (int k = 0; k < 6; k++) {
    if (i >= line.size()) return false;
    char c = line[i];
    if (c == 'x') f.frets[k] = -1;
    else if (c >= '0' && c <= '5') f.frets[k] = c - '0';
    else return false;
    i++;
    if (k < 5) { if (i >= line.size() || line[i] != ' ') return false; i++; }
  }
  while (i < line.size() && line[i] == ' ') i++;
  if (i < line.size()) {
    const std::string tail = line.substr(i);
    if (tail.compare(0, 6, "(cost ") != 0 || tail.back() != ')') return false;
    int c = 0;
    if (!parseInt(tail.substr(6, tail.size() - 7), -1000000, 1000000, c)) return false;
    f.hasCost = true;
    f.cost = c;
  }
  out = f;
  return true;
}

// A chord box: nut at the top, low E on the left, '@' where a coil presses, x muted, o open.
inline std::string renderDiagram(const Fingering& f) {
  int shown = 0;
  for (int k = 0; k < 6; k++) if (f.frets[k] > shown) shown = f.frets[k];
  if (shown < 3) shown = 3;
  std::string top = "  ", rule = "  ";
  for (int k = 0; k < 6; k++) {
    top += (f.frets[k] < 0 ? 'x' : (f.frets[k] == 0 ? 'o' : ' '));
    top += (k < 5 ? "  " : "");
    rule += "+--";
  }
  rule += "+";
  std::string out = top + "\n" + rule + "\n";
  for (int fret = 1; fret <= shown; fret++) {
    out += std::to_string(fret) + " ";
    for (int k = 0; k < 6; k++) {
      out += (f.frets[k] == fret ? '@' : ' ');
      out += (k < 5 ? "  " : "");
    }
    out += "\n";
  }
  out += "  E  A  D  G  B  e\n";
  return out;
}

inline std::string usage() {
  return
      "AGAP control tool (C++) - final 30-solenoid fretboard firmware\n\n"
      "usage: agap_control [--port COMx | --stdio] [--helper] <command> [args]\n\n"
      "commands:\n"
      "  ports                       list serial ports (no --port needed)\n"
      "  status | stop | version | ping | frets | help\n"
      "  chord <name>                solve, press and strum, e.g. chord F#m7  (draws the fingering)\n"
      "  show <name>                 solve and draw the fingering, move nothing\n"
      "  strum [D|U]                 strum down (default) or up\n"
      "  press <string> <fret>       press one coil: string 1-6 (6 = low E), fret 1-5\n"
      "  release <string> <fret> | release all\n"
      "  raw <x|0-5> x6              an explicit fingering, low E first, e.g. raw x 3 2 0 1 0\n"
      "  sequence <chord> ... [--bpm N]      at most 12 chords, 20-200 bpm (default 50)\n"
      "  calib <string> <fret> [--hold-ms N] [--reps N] [--gap-ms N]\n\n"
      "options:\n"
      "  --helper     talk to the earlier chord-helper firmware instead (uses button_map.json; see --map)\n"
      "  --stdio      no serial port: write commands to stdout and read the board's lines from stdin\n"
      "               (used to test this tool against the real firmware logic running on a PC)\n";
}

}  // namespace fret
}  // namespace agapctl

#endif  // AGAP_CONTROL_FRET_H
