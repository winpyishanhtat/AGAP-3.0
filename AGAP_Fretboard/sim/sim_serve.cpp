// The REAL AGAP_Fretboard.ino as a program that speaks the board's serial protocol on stdin / stdout,
// so the remote bridge (and through it the phone page) can be tested against the firmware's own
// code instead of a stand-in. Lines you type (or the bridge sends) go to the sketch's Serial; what
// the sketch prints comes back on stdout.
//
//   g++ -std=c++14 -include Arduino.h -I../../sim/mock -I.. sim_serve.cpp -o sim_serve
//   ./sim_serve [--speed N]          N simulated ms per real ms (default 1 = real time)
//   python ../../bridge/agap_bridge.py --sim-exe ./sim_serve --fretboard --allow-unconfirmed
//
// Lines starting with '#' are for this harness, not the sketch:
//   #pins     ->  #PINS on=<hex> kick=<hex>        which coil channels the pins show as on / kicking
//   #status   ->  #STATUS violations=N maxcoils=N now=T
//   #quit     ->  exit
//
// Same limits as the other simulations: it runs the sketch's logic (parser, chord search, state
// machines, blocking SEQUENCE / CALIB loops, pin masks) with a simulated clock. It does not
// reproduce the AVR's speed, interrupt timing or any electrical behaviour. The simulated clock
// advances 1 ms every time the sketch reads it, and is held to real time x --speed by sleeping.

#include "../AGAP_Fretboard.ino"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../sim/sim_common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

static double g_speed = 1.0;

static double wallMs() {
#ifdef _WIN32
  static LARGE_INTEGER freq = {};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static void sleepMs(double ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  usleep((useconds_t)(ms * 1000.0));
#endif
}

// Non-blocking read of whatever is waiting on stdin. Returns false at end of input.
static bool readStdin(std::string& into) {
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD avail = 0;
  if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;   // pipe closed
  while (avail) {
    char buf[512];
    DWORD got = 0;
    if (!ReadFile(h, buf, avail < sizeof buf ? avail : sizeof buf, &got, nullptr) || !got) return false;
    into.append(buf, got);
    avail -= got;
  }
  return true;
#else
  static bool setup = false;
  if (!setup) { fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK); setup = true; }
  char buf[512];
  for (;;) {
    ssize_t n = read(0, buf, sizeof buf);
    if (n > 0) { into.append(buf, (size_t)n); continue; }
    if (n == 0) return false;
    return errno == EAGAIN || errno == EWOULDBLOCK;
  }
#endif
}

static void say(const char* fmt, unsigned a = 0, unsigned b = 0, unsigned c = 0) {
  std::printf(fmt, a, b, c);
  std::fflush(stdout);
}

static void harnessCommand(const std::string& cmd) {
  if (cmd == "#pins") {
    Pins p = pins();
    say("#PINS on=%08x kick=%08x\n", p.on, p.kick);
  } else if (cmd == "#status") {
    say("#STATUS violations=%u maxcoils=%u now=%u\n", (unsigned)g_violations, (unsigned)g_maxCoils, (unsigned)sim::now());
  } else if (cmd == "#quit") {
    std::fflush(stdout);
    std::exit(0);
  }
}

static std::string g_in;
static std::deque<std::string> g_lines;
static double g_t0 = 0;

// Runs on every read of the simulated clock: safety checks, input, output, pacing.
static void tick() {
  invariants();

  std::string& out = sim::serialOut();
  if (!out.empty()) {
    for (char c : out) if (c != '\r') std::fputc(c, stdout);
    std::fflush(stdout);
    out.clear();
  }

  bool open = readStdin(g_in);
  const char NL = 10, CR = 13;   // spelled as numbers so no tool can flatten an escape sequence
  size_t nl;
  while ((nl = g_in.find(NL)) != std::string::npos) {
    std::string line = g_in.substr(0, nl);
    g_in.erase(0, nl + 1);
    if (!line.empty() && line.back() == CR) line.pop_back();
    g_lines.push_back(line);
  }
  // Keep the order the lines arrived in: a '#' command is answered only once the sketch has taken
  // every serial byte sent before it, so "CHORD Bm" then "#pins" shows the pins after the chord.
  while (!g_lines.empty()) {
    const std::string& line = g_lines.front();
    if (!line.empty() && line[0] == '#') {
      if (!sim::serialIn().empty()) break;
      harnessCommand(line);
    } else {
      feedAt(sim::now(), line + std::string(1, NL));
    }
    g_lines.pop_front();
  }
  if (!open && g_lines.empty()) {
    std::fflush(stdout);
    std::exit(0);
  }

  // hold simulated time to (real time x speed)
  for (;;) {
    double ahead = (double)sim::now() / g_speed - (wallMs() - g_t0);
    if (ahead <= 0.5) break;
    if (ahead > 16) sleepMs(ahead - 15); else sleepMs(0);
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; i++)
    if (std::strcmp(argv[i], "--speed") == 0) g_speed = std::atof(argv[i + 1]);
  if (g_speed <= 0) g_speed = 1.0;
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  for (int i = 0; i < 4096; i++) sim::eeprom()[i] = 0xFF;   // erased EEPROM: first power-up
  for (int i = 0; i < 80; i++) sim::pinLevel()[i] = HIGH;   // panel buttons idle (pulled up)
  g_t0 = wallMs();
  sim::tickHook() = tick;
  setup();
  for (;;) loop();
}
