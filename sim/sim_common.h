// Shared by the simulations of the real sketches (sim_fretboard.cpp, sim_helper.cpp).
// Include it AFTER the sketch (it calls the sketch's own interrupt routines and setup()).
// See sim_fretboard.cpp for what a simulation does and does not prove.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- checking
static int g_total = 0, g_fail = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    g_total++;                                                               \
    if (!(cond)) {                                                           \
      g_fail++;                                                              \
      std::printf("  FAIL line %d: %s\n", __LINE__, #cond);                  \
    }                                                                        \
  } while (0)
#define CHECK_MSG(cond, ...)                                                 \
  do {                                                                       \
    g_total++;                                                               \
    if (!(cond)) {                                                           \
      g_fail++;                                                              \
      std::printf("  FAIL line %d: %s -- ", __LINE__, #cond);                \
      std::printf(__VA_ARGS__);                                              \
      std::printf("\n");                                                     \
    }                                                                        \
  } while (0)

// ------------------------------------------------------------ the "hardware"
struct Pins {
  uint32_t on, kick;
};

static int popcount(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) n++;
  return n;
}

// What the pins show: the Timer2 overflow interrupt raises every active coil, then the
// compare-match interrupt drops the ones that are only holding.
static Pins pins() {
  TIMER2_OVF_vect_isr();
  uint32_t on = (uint32_t)PORTA | ((uint32_t)PORTC << 8) | ((uint32_t)PORTL << 16) | ((uint32_t)PORTK << 24);
  uint8_t k_hi_on = PORTK;
  TIMER2_COMPA_vect_isr();
  uint32_t kick = (uint32_t)PORTA | ((uint32_t)PORTC << 8) | ((uint32_t)PORTL << 16) | ((uint32_t)PORTK << 24);
  uint8_t k_hi_kick = PORTK;
  static bool warned = false;
  if (((k_hi_on | k_hi_kick) & 0xC0) && !warned) {
    warned = true;
    std::printf("  FAIL: PORTK bits 6-7 (unused pins) were driven\n");
    g_fail++;
  }
  return {on, kick};
}

static int g_violations = 0;
static std::string g_firstViolation;
static int g_maxCoils = 0;
static int g_coilCap = 6;  // most coils allowed on at once; each sketch's simulation sets its own

// Safety invariants, checked on every simulated millisecond (also inside the sketch's blocking loops).
static void invariants() {
  static bool inside = false;
  if (inside) return;
  inside = true;
  Pins p = pins();
  int coils = popcount(p.on);
  if (coils > g_maxCoils) g_maxCoils = coils;
  const char* why = nullptr;
  if (coils > g_coilCap) why = "more coils on at once than the limit";
  else if (p.kick & ~p.on) why = "kick mask has a coil that is not on";
  else if (p.on >> 30) why = "a channel beyond 29 is driven";
  if (why) {
    g_violations++;
    if (g_firstViolation.empty()) g_firstViolation = std::string(why) + " at t=" + std::to_string(sim::now());
  }
  inside = false;
}

// ----------------------------------------------------------------- driving it
static void enqueue(uint32_t at, char c) {
  auto& q = sim::serialIn();
  auto it = q.begin();
  while (it != q.end() && it->first <= at) ++it;
  q.insert(it, {at, c});
}
static void feedAt(uint32_t at, const std::string& s) {
  for (char c : s) enqueue(at, c);
}
static void send(const std::string& line, uint32_t delay = 0) { feedAt(sim::now() + delay, line + "\n"); }

static void run(uint32_t ms) {
  uint32_t end = sim::now() + ms;
  while (sim::now() < end) loop();
}

static std::vector<std::string> take() {
  std::string s;
  s.swap(sim::serialOut());
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find("\r\n", i);
    if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
    out.push_back(s.substr(i, j - i));
    i = j + 2;
  }
  return out;
}
static bool has(const std::vector<std::string>& v, const std::string& line) {
  return std::find(v.begin(), v.end(), line) != v.end();
}
static bool hasPrefix(const std::vector<std::string>& v, const std::string& pre) {
  for (auto& l : v) if (l.compare(0, pre.size(), pre) == 0) return true;
  return false;
}
static std::string joined(const std::vector<std::string>& v) {
  std::string s;
  for (auto& l : v) s += l + " | ";
  return s;
}

static void boot() {
  sim::tickHook() = invariants;
  for (int i = 0; i < 80; i++) sim::pinLevel()[i] = HIGH;  // buttons idle (pulled up)
  setup();
}
static std::vector<std::string> bootLines;
static void bootAndKeep() {
  boot();
  bootLines = take();
}
// Settle: let the boot-time chatter and any timers run out.
static void quiet(uint32_t ms = 300) { run(ms); take(); }

static void end_of_scenario() {
  CHECK_MSG(g_violations == 0, "%d invariant violation(s); first: %s", g_violations, g_firstViolation.c_str());
}

