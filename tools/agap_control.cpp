// AGAP control tool - C++ version of tools/agap_control.py (Windows).
//
// Same commands and same serial protocol as the Python tool, so the whole
// stack (firmware + PC-side control) can be C++ with no Python installed.
// Opens a COM port with the Win32 API, sends one text command per line to
// AGAP_HelperButton.ino, and prints what the board replies.
//
// Build (MinGW g++ or any C++14 compiler on Windows):
//   g++ -std=c++14 -O2 agap_control.cpp -o agap_control.exe
//
// This file is the thin hardware-touching layer (COM port I/O + argument
// parsing). Everything testable - button_map.json parsing, label/chord
// resolution, command-string building - is in agap_control_logic.h and is
// covered by tools/tests/. Like the Python tool, this only sends commands
// and shows replies: it cannot tell whether a button physically pressed or
// a chord sounded right (README_AGAP.md steps 3 and 5 - that's you,
// listening and watching).

#ifndef _WIN32
#error "agap_control.cpp uses the Win32 serial API and builds on Windows only."
#endif

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agap_control_logic.h"

using namespace agapctl;

static const char* kDefaultMapPath = "../AGAP_HelperButton/button_map.json";
static const DWORD kBaud = 115200;

// ----------------------------- serial port -----------------------------

class SerialPort {
 public:
  ~SerialPort() { close(); }

  bool open(const std::string& name, DWORD baud) {
    // The \\.\ prefix is required for COM10 and up and harmless below that.
    std::string path = "\\\\.\\" + name;
    h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h_ == INVALID_HANDLE_VALUE) return false;

    DCB dcb;
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h_, &dcb)) return fail();
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;  // lets the Mega auto-reset on open, like pyserial
    if (!SetCommState(h_, &dcb)) return fail();

    COMMTIMEOUTS to;
    ZeroMemory(&to, sizeof(to));
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 50;  // each read waits up to 50 ms for data
    to.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(h_, &to)) return fail();

    PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
  }

  void close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

  bool writeLine(const std::string& line) {
    std::string out = line + "\n";
    DWORD written = 0;
    return WriteFile(h_, out.data(), (DWORD)out.size(), &written, NULL) && written == out.size();
  }

  // Reads whatever arrives within timeoutMs and returns complete lines
  // (any trailing partial line is kept for the next call).
  std::vector<std::string> readLines(DWORD timeoutMs) {
    std::vector<std::string> lines;
    DWORD start = GetTickCount();
    char buf[256];
    do {
      DWORD n = 0;
      if (!ReadFile(h_, buf, sizeof(buf), &n, NULL)) break;
      pending_.append(buf, n);
    } while (GetTickCount() - start < timeoutMs);

    size_t pos;
    while ((pos = pending_.find('\n')) != std::string::npos) {
      std::string line = pending_.substr(0, pos);
      pending_.erase(0, pos + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty()) lines.push_back(line);
    }
    return lines;
  }

 private:
  bool fail() {
    close();
    return false;
  }

  HANDLE h_ = INVALID_HANDLE_VALUE;
  std::string pending_;
};

// ------------------------------- helpers -------------------------------

static void printLines(const std::vector<std::string>& lines) {
  for (const auto& l : lines) std::printf("%s\n", l.c_str());
}

// Send a command, then collect replies for waitMs.
static void sendAndPrint(SerialPort& port, const std::string& cmd, DWORD waitMs = 300) {
  if (!port.writeLine(cmd)) {
    std::fprintf(stderr, "ERR could not write to the serial port\n");
    std::exit(1);
  }
  printLines(port.readLines(waitMs));
}

// For commands that run for a known time (SEQUENCE, CALIB): keep printing
// the board's output until totalMs has passed or it reports ABORTED/ERR.
static void streamFor(SerialPort& port, DWORD totalMs) {
  DWORD start = GetTickCount();
  while (GetTickCount() - start < totalMs) {
    for (const auto& l : port.readLines(100)) {
      std::printf("%s\n", l.c_str());
      if (l.rfind("ABORTED", 0) == 0 || l.rfind("ERR", 0) == 0) return;
    }
  }
}

static std::string requireLabel(const std::string& token, const std::vector<ButtonEntry>& map) {
  std::string label = resolveLabel(token, map);
  if (label.empty()) {
    std::string known;
    for (const auto& e : map) known += (known.empty() ? "" : ", ") + e.label;
    std::fprintf(stderr, "Unknown chord/label '%s'. Known labels: %s\n", token.c_str(), known.c_str());
    std::exit(1);
  }
  return label;
}

static void warnIfUnconfirmed(const std::string& label, const std::vector<ButtonEntry>& map) {
  if (!isConfirmed(label, map)) {
    std::fprintf(stderr,
                 "  NOTE: '%s' is not marked confirmed in button_map.json - its chord/function "
                 "hasn't been verified against the real device (README_AGAP.md step 1/3).\n",
                 label.c_str());
  }
}

static void listPorts() {
  HKEY key;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) {
    std::printf("(no serial ports found)\n");
    return;
  }
  char name[256], data[256];
  bool any = false;
  for (DWORD i = 0;; i++) {
    DWORD nameLen = sizeof(name), dataLen = sizeof(data), type = 0;
    if (RegEnumValueA(key, i, name, &nameLen, NULL, &type, (LPBYTE)data, &dataLen) != ERROR_SUCCESS) break;
    if (type == REG_SZ) {
      std::printf("%s\n", data);
      any = true;
    }
  }
  RegCloseKey(key);
  if (!any) std::printf("(no serial ports found)\n");
}

static void usage() {
  std::printf(
      "AGAP control tool (C++)\n\n"
      "usage: agap_control [--port COMx] [--map button_map.json] <command> [args]\n\n"
      "commands:\n"
      "  ports                      list serial ports (no --port needed)\n"
      "  labels | status | stop     query the board / release every channel\n"
      "  press <label|chord>        seat one button, no strum\n"
      "  release <label|chord|ALL>  release one channel or everything\n"
      "  chord <label|chord>        press + strum\n"
      "  sequence <t1> <t2> ... [--bpm N]\n"
      "  calib <label|chord> [--hold-ms N] [--reps N] [--gap-ms N]\n"
      "  raw <firmware command words...>\n");
}

// Pulls "--name value" out of args (removing both) and returns value, or def.
static int takeIntOption(std::vector<std::string>& args, const std::string& name, int def) {
  for (size_t i = 0; i + 1 < args.size(); i++) {
    if (args[i] == name) {
      int v = std::atoi(args[i + 1].c_str());
      args.erase(args.begin() + i, args.begin() + i + 2);
      return v;
    }
  }
  return def;
}

// ---------------------------------- main ----------------------------------

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string portName, mapPath = kDefaultMapPath;

  // Global options may come before the command.
  while (!args.empty() && args[0].rfind("--", 0) == 0) {
    if (args[0] == "--port" && args.size() >= 2) {
      portName = args[1];
      args.erase(args.begin(), args.begin() + 2);
    } else if (args[0] == "--map" && args.size() >= 2) {
      mapPath = args[1];
      args.erase(args.begin(), args.begin() + 2);
    } else if (args[0] == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "Unknown option %s\n", args[0].c_str());
      return 2;
    }
  }
  if (args.empty()) {
    usage();
    return 2;
  }

  const std::string cmd = args[0];
  args.erase(args.begin());

  if (cmd == "ports") {
    listPorts();
    return 0;
  }
  if (portName.empty()) {
    std::fprintf(stderr, "--port is required (run `agap_control ports` to list them)\n");
    return 2;
  }

  std::vector<ButtonEntry> map;
  try {
    map = loadButtonMap(mapPath);
  } catch (const JsonParseError& e) {
    std::fprintf(stderr, "Could not read button map: %s\n(use --map to point at button_map.json)\n", e.what());
    return 1;
  }

  SerialPort port;
  if (!port.open(portName, kBaud)) {
    std::fprintf(stderr, "ERR could not open %s (wrong port, or another program has it open?)\n", portName.c_str());
    return 1;
  }
  Sleep(2000);  // the Mega resets when the port opens; wait out its boot
  printLines(port.readLines(200));  // boot banner

  if (cmd == "labels") {
    sendAndPrint(port, buildLabelsCmd());
  } else if (cmd == "status") {
    sendAndPrint(port, buildStatusCmd());
  } else if (cmd == "stop") {
    sendAndPrint(port, buildStopCmd());
  } else if (cmd == "press" && args.size() == 1) {
    sendAndPrint(port, buildPressCmd(requireLabel(args[0], map)));
  } else if (cmd == "release" && args.size() == 1) {
    std::string target = toUpper(args[0]) == "ALL" ? "ALL" : requireLabel(args[0], map);
    sendAndPrint(port, buildReleaseCmd(target));
  } else if (cmd == "chord" && args.size() == 1) {
    std::string label = requireLabel(args[0], map);
    warnIfUnconfirmed(label, map);
    sendAndPrint(port, buildChordCmd(label), 600);
  } else if (cmd == "sequence") {
    int bpm = takeIntOption(args, "--bpm", 50);
    if (args.empty()) {
      usage();
      return 2;
    }
    std::vector<std::string> labels;
    for (const auto& t : args) {
      std::string label = requireLabel(t, map);
      warnIfUnconfirmed(label, map);
      labels.push_back(label);
    }
    sendAndPrint(port, buildTempoCmd(bpm), 150);
    std::printf("Sending: %s\n", buildSequenceCmd(labels).c_str());
    port.writeLine(buildSequenceCmd(labels));
    streamFor(port, (DWORD)(60000.0 / bpm * labels.size()) + 5000);
  } else if (cmd == "calib") {
    int holdMs = takeIntOption(args, "--hold-ms", 1000);
    int reps = takeIntOption(args, "--reps", 5);
    int gapMs = takeIntOption(args, "--gap-ms", 800);
    if (args.size() != 1) {
      usage();
      return 2;
    }
    std::string label = requireLabel(args[0], map);
    std::printf("Sending: %s\n", buildCalibCmd(label, holdMs, reps, gapMs).c_str());
    port.writeLine(buildCalibCmd(label, holdMs, reps, gapMs));
    streamFor(port, (DWORD)(holdMs + gapMs) * reps + 5000);
  } else if (cmd == "raw" && !args.empty()) {
    std::string line;
    for (const auto& w : args) line += (line.empty() ? "" : " ") + w;
    sendAndPrint(port, line, 500);
  } else {
    usage();
    return 2;
  }
  return 0;
}
