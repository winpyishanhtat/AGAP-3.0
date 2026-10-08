#ifndef AGAP_CONTROL_LOGIC_H
#define AGAP_CONTROL_LOGIC_H

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Serial-independent logic for the C++ control tool (agap_control.cpp):
// reading button_map.json, resolving a typed label/chord name to the
// device's real label, and building the exact command strings the
// firmware's serial protocol expects. Nothing here touches Win32 or a COM
// port, so it compiles and is unit-tested natively with plain g++ - the
// same split as AGAP_HelperButton/agap_logic.h on the firmware side.
//
// The JSON reader is deliberately small and NOT a general JSON parser: it
// handles exactly button_map.json's shape (a "buttons" array of flat
// objects with string / true / false / null values, no nesting, no
// escaped quotes inside labels). That keeps the tool dependency-free.

namespace agapctl {

struct ButtonEntry {
  std::string label;
  std::string chord;  // empty = null/unknown (the unidentified X1, X2 positions)
  bool confirmed = false;
};

class JsonParseError : public std::runtime_error {
 public:
  explicit JsonParseError(const std::string& msg) : std::runtime_error(msg) {}
};

inline std::string toUpper(const std::string& s) {
  std::string r = s;
  for (auto& c : r) c = (char)std::toupper((unsigned char)c);
  return r;
}

namespace detail {

inline void skipWs(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
}

inline std::string parseString(const std::string& s, size_t& i) {
  if (i >= s.size() || s[i] != '"') throw JsonParseError("expected '\"' at offset " + std::to_string(i));
  i++;
  std::string out;
  while (i < s.size() && s[i] != '"') {
    if (s[i] == '\\' && i + 1 < s.size()) {
      out += s[i + 1];
      i += 2;
    } else {
      out += s[i++];
    }
  }
  if (i >= s.size()) throw JsonParseError("unterminated string");
  i++;  // closing quote
  return out;
}

// A string literal, or a bare token (true/false/null/number).
inline std::string parseScalar(const std::string& s, size_t& i, bool& isNull) {
  isNull = false;
  skipWs(s, i);
  if (i < s.size() && s[i] == '"') return parseString(s, i);
  size_t start = i;
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
  std::string tok = s.substr(start, i - start);
  while (!tok.empty() && std::isspace((unsigned char)tok.back())) tok.pop_back();
  if (tok == "null") isNull = true;
  return tok;
}

}  // namespace detail

inline std::vector<ButtonEntry> parseButtonMap(const std::string& json) {
  using namespace detail;
  std::vector<ButtonEntry> out;
  size_t i = json.find("\"buttons\"");
  if (i == std::string::npos) throw JsonParseError("no \"buttons\" key found");
  i = json.find('[', i);
  if (i == std::string::npos) throw JsonParseError("\"buttons\" is not an array");
  i++;
  while (true) {
    skipWs(json, i);
    if (i >= json.size()) throw JsonParseError("unterminated buttons array");
    if (json[i] == ']') break;
    if (json[i] == ',') { i++; continue; }
    if (json[i] != '{') throw JsonParseError("expected '{' at offset " + std::to_string(i));
    i++;
    ButtonEntry entry;
    while (true) {
      skipWs(json, i);
      if (i >= json.size()) throw JsonParseError("unterminated button object");
      if (json[i] == '}') { i++; break; }
      if (json[i] == ',') { i++; continue; }
      std::string key = parseString(json, i);
      skipWs(json, i);
      if (i >= json.size() || json[i] != ':') throw JsonParseError("expected ':' after key \"" + key + "\"");
      i++;
      bool isNull = false;
      std::string val = parseScalar(json, i, isNull);
      if (key == "label") entry.label = val;
      else if (key == "chord") entry.chord = isNull ? "" : val;
      else if (key == "confirmed") entry.confirmed = (val == "true");
      // "note" and any other key: ignored
    }
    if (!entry.label.empty()) out.push_back(entry);
  }
  return out;
}

inline std::vector<ButtonEntry> loadButtonMap(const std::string& path) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if (!f) throw JsonParseError("could not open " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return parseButtonMap(ss.str());
}

// Accepts the device's own label (case-insensitive: "em", "EM") or a chord
// name (exact case: "Em"). Same rules as tools/agap_control.py. Returns the
// device label, or "" if nothing matches.
inline std::string resolveLabel(const std::string& token, const std::vector<ButtonEntry>& map) {
  std::string upper = toUpper(token);
  for (const auto& e : map) {
    if (toUpper(e.label) == upper) return e.label;
  }
  for (const auto& e : map) {
    if (!e.chord.empty() && e.chord == token) return e.label;
  }
  return "";
}

inline bool isConfirmed(const std::string& label, const std::vector<ButtonEntry>& map) {
  for (const auto& e : map) {
    if (e.label == label) return e.confirmed;
  }
  return false;
}

// Exact strings the firmware's serial protocol expects.
inline std::string buildPressCmd(const std::string& label) { return "PRESS " + label; }
inline std::string buildReleaseCmd(const std::string& label) { return "RELEASE " + label; }
inline std::string buildChordCmd(const std::string& label) { return "CHORD " + label; }
inline std::string buildStopCmd() { return "STOP"; }
inline std::string buildStatusCmd() { return "STATUS"; }
inline std::string buildLabelsCmd() { return "LABELS"; }
inline std::string buildTempoCmd(int bpm) { return "TEMPO " + std::to_string(bpm); }

inline std::string buildSequenceCmd(const std::vector<std::string>& labels) {
  std::string out = "SEQUENCE";
  for (const auto& l : labels) out += " " + l;
  return out;
}

inline std::string buildCalibCmd(const std::string& label, int holdMs, int reps, int gapMs) {
  return "CALIB " + label + " " + std::to_string(holdMs) + " " + std::to_string(reps) + " " + std::to_string(gapMs);
}

}  // namespace agapctl

#endif  // AGAP_CONTROL_LOGIC_H
