// AGAP control tool - the C++ command-line tool for the robot.
//
// Talks to the FINAL 30-solenoid firmware (AGAP_Fretboard.ino) by default: chord names, drawn
// fingerings, strum, press/release, sequence, calib. `--helper` talks to the earlier chord-helper
// firmware instead (AGAP_HelperButton.ino, button_map.json), as before.
//
// Build (any C++14 compiler):
//   g++ -std=c++14 -O2 agap_control.cpp -o agap_control          (Linux, macOS, Raspberry Pi)
//   g++ -std=c++14 -O2 agap_control.cpp -o agap_control.exe      (Windows, MinGW)
//
// Transports: a serial port (Win32 API on Windows, termios elsewhere), or --stdio, which writes
// commands to stdout and reads the board's lines from stdin so the tool can be tested against the real
// firmware logic running on a PC (bridge/test_agap_control_e2e.py). In --stdio mode everything meant for
// a person goes to stderr.
//
// The hardware-touching layer is this file (port I/O). Everything else is unit-tested natively:
// agap_control_fret.h (fretboard commands, limits, fingering reading and drawing) and
// agap_control_logic.h (button_map.json and the helper protocol). Like every other tool here, this only
// sends commands and shows replies: it cannot tell whether a coil really pressed or a chord sounded
// right, and the serial transports have not been tried on a real board.

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <glob.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agap_control_fret.h"
#include "agap_control_logic.h"

using namespace agapctl;

static const char* kDefaultMapPath = "../AGAP_HelperButton/button_map.json";
static const unsigned kBoot = 2000;   // the Mega resets when a serial port is opened; wait out its boot

static FILE* g_human = stdout;        // where text meant for a person goes (stderr in --stdio mode)

static void say(const std::string& s) {
  std::fputs(s.c_str(), g_human);
  std::fputc(10, g_human);
  std::fflush(g_human);
}

static void warn(const std::string& s) {
  std::fputs(s.c_str(), stderr);
  std::fputc(10, stderr);
  std::fflush(stderr);
}

static unsigned long nowMs() {
#ifdef _WIN32
  return (unsigned long)GetTickCount();
#else
  timeval tv;
  gettimeofday(&tv, NULL);
  return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)tv.tv_usec / 1000UL;
#endif
}

static void sleepMs(unsigned ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  usleep(ms * 1000u);
#endif
}

// ------------------------------- transports -------------------------------

class Transport {
 public:
  virtual ~Transport() {}
  virtual bool writeLine(const std::string& line) = 0;
  // Everything that arrives within timeoutMs, as whole lines (a trailing partial line is kept).
  std::vector<std::string> readLines(unsigned timeoutMs) {
    pump(timeoutMs);
    std::vector<std::string> lines;
    size_t pos;
    while ((pos = pending_.find((char)10)) != std::string::npos) {
      std::string line = pending_.substr(0, pos);
      pending_.erase(0, pos + 1);
      while (!line.empty() && (line.back() == (char)13 || line.back() == ' ')) line.pop_back();
      if (!line.empty()) lines.push_back(line);
    }
    return lines;
  }

 protected:
  virtual void pump(unsigned timeoutMs) = 0;   // append whatever arrives to pending_
  std::string pending_;
};

#ifdef _WIN32
class SerialPort : public Transport {
 public:
  ~SerialPort() { close(); }

  bool open(const std::string& name, DWORD baud) {
    // The prefix backslash backslash dot backslash is required for COM10 and up and harmless below that.
    const char bs = 92;
    std::string path = std::string(2, bs) + "." + std::string(1, bs) + name;
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

  bool writeLine(const std::string& line) override {
    std::string out = line + (char)10;
    DWORD written = 0;
    return WriteFile(h_, out.data(), (DWORD)out.size(), &written, NULL) && written == out.size();
  }

 protected:
  void pump(unsigned timeoutMs) override {
    DWORD start = GetTickCount();
    char buf[256];
    do {
      DWORD n = 0;
      if (!ReadFile(h_, buf, sizeof(buf), &n, NULL)) break;
      pending_.append(buf, n);
    } while (GetTickCount() - start < timeoutMs);
  }

 private:
  bool fail() {
    close();
    return false;
  }
  HANDLE h_ = INVALID_HANDLE_VALUE;
};
#else
class SerialPort : public Transport {
 public:
  ~SerialPort() { close(); }

  bool open(const std::string& name, unsigned /*baud: 115200 is fixed below*/) {
    fd_ = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return false;
    termios tio;
    if (tcgetattr(fd_, &tio) != 0) return fail();
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
    if (tcsetattr(fd_, TCSANOW, &tio) != 0) return fail();
    tcflush(fd_, TCIOFLUSH);
    return true;
  }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  bool writeLine(const std::string& line) override {
    std::string out = line + (char)10;
    size_t off = 0;
    while (off < out.size()) {
      ssize_t n = ::write(fd_, out.data() + off, out.size() - off);
      if (n < 0) { sleepMs(5); continue; }
      off += (size_t)n;
    }
    return true;
  }

 protected:
  void pump(unsigned timeoutMs) override {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd_, &set);
    timeval tv;
    tv.tv_sec = (long)(timeoutMs / 1000);
    tv.tv_usec = (long)(timeoutMs % 1000) * 1000;
    // wait for the first bytes, then take whatever else has already arrived
    if (select(fd_ + 1, &set, NULL, NULL, &tv) <= 0) return;
    char buf[256];
    for (;;) {
      ssize_t n = ::read(fd_, buf, sizeof buf);
      if (n <= 0) break;
      pending_.append(buf, (size_t)n);
      sleepMs(2);
    }
  }

 private:
  bool fail() {
    close();
    return false;
  }
  int fd_ = -1;
};
#endif

// No port: commands go to stdout, the board's lines come from stdin.
class StdioTransport : public Transport {
 public:
  bool writeLine(const std::string& line) override {
    std::fputs(line.c_str(), stdout);
    std::fputc(10, stdout);
    std::fflush(stdout);
    return true;
  }

 protected:
  void pump(unsigned timeoutMs) override {
    unsigned long start = nowMs();
    do {
      if (!readAvailable()) { sleepMs(timeoutMs > 40 ? 40 : timeoutMs); return; }   // end of input
      sleepMs(4);
    } while (nowMs() - start < timeoutMs);
  }

 private:
  // Reads what is waiting without blocking. Returns false at end of input.
  bool readAvailable() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return false;
    while (avail) {
      char buf[512];
      DWORD got = 0;
      if (!ReadFile(h, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &got, NULL) || !got) return false;
      pending_.append(buf, got);
      avail -= got;
    }
    return true;
#else
    if (!set_) { fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK); set_ = true; }
    char buf[512];
    for (;;) {
      ssize_t n = ::read(0, buf, sizeof buf);
      if (n > 0) { pending_.append(buf, (size_t)n); continue; }
      if (n == 0) return false;
      return true;   // nothing waiting right now
    }
#endif
  }
  bool set_ = false;
};

// ------------------------------- helpers -------------------------------

static bool g_sawError = false;   // the board answered ERR to something: the exit code says so

static void showLine(const std::string& l, bool diagram) {
  say(l);
  if (l.compare(0, 3, "ERR") == 0) g_sawError = true;
  if (diagram) {
    fret::Fingering f;
    if (fret::parseFingering(l, f)) {
      std::string d = fret::renderDiagram(f);
      while (!d.empty() && d.back() == (char)10) d.pop_back();
      say(d);
    }
  }
}

static void printLines(const std::vector<std::string>& lines, bool diagram = false) {
  for (const auto& l : lines) showLine(l, diagram);
}

static bool sendLine(Transport& port, const std::string& cmd) {
  if (!port.writeLine(cmd)) {
    warn("ERR could not write to the serial port");
    return false;
  }
  return true;
}

// Send a command, then collect replies for waitMs.
static bool sendAndPrint(Transport& port, const std::string& cmd, unsigned waitMs, bool diagram = false) {
  if (!sendLine(port, cmd)) return false;
  printLines(port.readLines(waitMs), diagram);
  return true;
}

// For commands that run for a known time (SEQUENCE, CALIB): keep printing the board's output until
// totalMs has passed or it reports ABORTED / ERR.
static void streamFor(Transport& port, unsigned totalMs) {
  unsigned long start = nowMs();
  while (nowMs() - start < totalMs) {
    for (const auto& l : port.readLines(100)) {
      showLine(l, false);
      if (l.compare(0, 7, "ABORTED") == 0 || l.compare(0, 3, "ERR") == 0) return;
    }
  }
}

static void listPorts() {
#ifdef _WIN32
  HKEY key;
  const char bs = 92;
  std::string sub = std::string("HARDWARE") + bs + "DEVICEMAP" + bs + "SERIALCOMM";
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    say("(no serial ports found)");
    return;
  }
  char name[256], data[256];
  bool any = false;
  for (DWORD i = 0;; i++) {
    DWORD nameLen = sizeof(name), dataLen = sizeof(data), type = 0;
    if (RegEnumValueA(key, i, name, &nameLen, NULL, &type, (LPBYTE)data, &dataLen) != ERROR_SUCCESS) break;
    if (type == REG_SZ) {
      say(data);
      any = true;
    }
  }
  RegCloseKey(key);
  if (!any) say("(no serial ports found)");
#else
  bool any = false;
  const char* patterns[] = {"/dev/ttyACM*", "/dev/ttyUSB*", "/dev/tty.usb*", "/dev/cu.usb*"};
  for (const char* pat : patterns) {
    glob_t g;
    if (glob(pat, 0, NULL, &g) == 0) {
      for (size_t i = 0; i < g.gl_pathc; i++) { say(g.gl_pathv[i]); any = true; }
    }
    globfree(&g);
  }
  if (!any) say("(no serial ports found)");
#endif
}

// ------------------------------ the earlier helper firmware ------------------------------

static void helperUsage() {
  say("AGAP control tool (C++), earlier chord-helper firmware (--helper)\n\n"
      "usage: agap_control --helper [--port PORT | --stdio] [--map button_map.json] <command> [args]\n\n"
      "commands:\n"
      "  ports                      list serial ports (no --port needed)\n"
      "  labels | status | stop     query the board / release every channel\n"
      "  press <label|chord>        seat one button, no strum\n"
      "  release <label|chord|ALL>  release one channel or everything\n"
      "  chord <label|chord>        press + strum\n"
      "  sequence <t1> <t2> ... [--bpm N]\n"
      "  calib <label|chord> [--hold-ms N] [--reps N] [--gap-ms N]\n"
      "  raw <firmware command words...>");
}

static bool requireLabel(const std::string& token, const std::vector<ButtonEntry>& map, std::string& label) {
  label = resolveLabel(token, map);
  if (!label.empty()) return true;
  std::string known;
  for (const auto& e : map) known += (known.empty() ? "" : ", ") + e.label;
  warn("Unknown chord/label '" + token + "'. Known labels: " + known);
  return false;
}

static void warnIfUnconfirmed(const std::string& label, const std::vector<ButtonEntry>& map) {
  if (!isConfirmed(label, map)) {
    warn("  NOTE: '" + label + "' is not marked confirmed in button_map.json - its chord/function hasn't been "
         "verified against the real device (README_AGAP.md step 1/3).");
  }
}

static int runHelper(Transport& port, const std::string& cmd, std::vector<std::string> args, const std::string& mapPath) {
  std::vector<ButtonEntry> map;
  try {
    map = loadButtonMap(mapPath);
  } catch (const JsonParseError& e) {
    warn(std::string("Could not read button map: ") + e.what() + "\n(use --map to point at button_map.json)");
    return 1;
  }
  std::string label, err;
  if (cmd == "labels") {
    sendAndPrint(port, buildLabelsCmd(), 300);
  } else if (cmd == "status") {
    sendAndPrint(port, buildStatusCmd(), 300);
  } else if (cmd == "stop") {
    sendAndPrint(port, buildStopCmd(), 300);
  } else if (cmd == "press" && args.size() == 1) {
    if (!requireLabel(args[0], map, label)) return 1;
    sendAndPrint(port, buildPressCmd(label), 300);
  } else if (cmd == "release" && args.size() == 1) {
    std::string target;
    if (toUpper(args[0]) == "ALL") target = "ALL";
    else if (!requireLabel(args[0], map, target)) return 1;
    sendAndPrint(port, buildReleaseCmd(target), 300);
  } else if (cmd == "chord" && args.size() == 1) {
    if (!requireLabel(args[0], map, label)) return 1;
    warnIfUnconfirmed(label, map);
    sendAndPrint(port, buildChordCmd(label), 600);
  } else if (cmd == "sequence") {
    int bpm = fret::kBpmDefault;
    bool found = false;
    if (!fret::detail::takeInt(args, "--bpm", fret::kBpmMin, fret::kBpmMax, bpm, found, err)) { warn(err); return 2; }
    if (args.empty()) { helperUsage(); return 2; }
    std::vector<std::string> labels;
    for (const auto& t : args) {
      if (!requireLabel(t, map, label)) return 1;
      warnIfUnconfirmed(label, map);
      labels.push_back(label);
    }
    sendAndPrint(port, buildTempoCmd(bpm), 150);
    say("Sending: " + buildSequenceCmd(labels));
    if (!sendLine(port, buildSequenceCmd(labels))) return 1;
    streamFor(port, (unsigned)(60000.0 / bpm * labels.size()) + 5000);
  } else if (cmd == "calib") {
    int holdMs = 1000, reps = 5, gapMs = 800;
    bool f = false;
    if (!fret::detail::takeInt(args, "--hold-ms", 10, 7000, holdMs, f, err) ||
        !fret::detail::takeInt(args, "--reps", 1, 100, reps, f, err) ||
        !fret::detail::takeInt(args, "--gap-ms", 0, 10000, gapMs, f, err)) { warn(err); return 2; }
    if (args.size() != 1) { helperUsage(); return 2; }
    if (!requireLabel(args[0], map, label)) return 1;
    say("Sending: " + buildCalibCmd(label, holdMs, reps, gapMs));
    if (!sendLine(port, buildCalibCmd(label, holdMs, reps, gapMs))) return 1;
    streamFor(port, (unsigned)(holdMs + gapMs) * (unsigned)reps + 5000);
  } else if (cmd == "raw" && !args.empty()) {
    std::string line;
    for (const auto& w : args) line += (line.empty() ? "" : " ") + w;
    sendAndPrint(port, line, 500);
  } else {
    helperUsage();
    return 2;
  }
  return g_sawError ? 1 : 0;
}

// ------------------------------ the final fretboard firmware ------------------------------

static int runFretboard(Transport& port, const std::string& cmd, const std::vector<std::string>& args) {
  fret::Plan p = fret::plan(cmd, args);
  if (!p.ok()) {
    warn("ERR " + p.error);
    warn("(run with --help for the commands)");
    return 2;
  }
  for (const auto& n : p.notes) warn("NOTE: " + n);
  const std::string up = fret::detail::upper(cmd);
  const bool diagram = (up == "CHORD" || up == "SHOW" || up == "RAW");
  for (const auto& s : p.steps) {
    if (s.stream) {
      say("Sending: " + s.line);
      if (!sendLine(port, s.line)) return 1;
      streamFor(port, (unsigned)s.waitMs);
    } else if (!sendAndPrint(port, s.line, (unsigned)s.waitMs, diagram)) {
      return 1;
    }
  }
  return g_sawError ? 1 : 0;
}

// ---------------------------------- main ----------------------------------

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string portName, mapPath = kDefaultMapPath;
  bool helper = false, useStdio = false;

  // Global options may come before the command.
  while (!args.empty() && args[0].rfind("--", 0) == 0) {
    if (args[0] == "--port" && args.size() >= 2) {
      portName = args[1];
      args.erase(args.begin(), args.begin() + 2);
    } else if (args[0] == "--map" && args.size() >= 2) {
      mapPath = args[1];
      args.erase(args.begin(), args.begin() + 2);
    } else if (args[0] == "--helper") {
      helper = true;
      args.erase(args.begin());
    } else if (args[0] == "--fretboard") {
      helper = false;
      args.erase(args.begin());
    } else if (args[0] == "--stdio") {
      useStdio = true;
      g_human = stderr;
      args.erase(args.begin());
    } else if (args[0] == "--help") {
      if (helper) helperUsage(); else say(fret::usage());
      return 0;
    } else {
      warn("Unknown option " + args[0]);
      return 2;
    }
  }
  if (args.empty()) {
    if (helper) helperUsage(); else say(fret::usage());
    return 2;
  }

  const std::string cmd = args[0];
  args.erase(args.begin());

  if (cmd == "ports") {
    listPorts();
    return 0;
  }

  // Check the command is well formed BEFORE opening the port: a typo should not cost a 2-second board reset.
  if (!helper) {
    fret::Plan check = fret::plan(cmd, args);
    if (!check.ok()) {
      warn("ERR " + check.error);
      warn("(run with --help for the commands)");
      return 2;
    }
  }

  StdioTransport stdio;
  SerialPort serial;
  Transport* port = &stdio;
  if (!useStdio) {
    if (portName.empty()) {
      warn("--port is required (run `agap_control ports` to list them), or use --stdio");
      return 2;
    }
    if (!serial.open(portName, 115200)) {
      warn("ERR could not open " + portName + " (wrong port, or another program has it open?)");
      return 1;
    }
    sleepMs(kBoot);
    port = &serial;
  }

  std::vector<std::string> banner = port->readLines(useStdio ? 400 : 200);
  printLines(banner);
  fret::Firmware fw = fret::detectFirmware(banner);
  if (fw == fret::Firmware::Helper && !helper)
    warn("NOTE: the board says it runs the earlier chord-helper firmware. Add --helper to use its commands.");
  if (fw == fret::Firmware::Fretboard && helper)
    warn("NOTE: the board says it runs the fretboard firmware. Drop --helper to use its commands.");

  return helper ? runHelper(*port, cmd, args, mapPath) : runFretboard(*port, cmd, args);
}
