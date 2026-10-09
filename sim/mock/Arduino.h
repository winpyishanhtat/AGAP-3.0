#pragma once
// A just-enough fake of the Arduino core, so the REAL AGAP_Fretboard.ino can be
// compiled and run on a PC. Everything the sketch touches is simulated:
//   - Serial: bytes arrive at scheduled simulated times; output is captured
//   - millis(): a simulated clock that moves by 1 ms every time it is read, so the
//     sketch's blocking loops (SEQUENCE, CALIB) always finish
//   - PORTA/C/L/K, Timer2: plain variables; the test calls the two Timer2
//     interrupt routines itself to see what the pins would show
//   - EEPROM, Servo, buttons (digitalRead)
// It cannot reproduce real interrupt timing, electrical behaviour or the AVR's
// speed. It runs the sketch's logic, not the hardware.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<strings.h>)
#include <strings.h>
#define SIM_HAVE_STRINGS_H 1
#endif
#endif
#ifndef SIM_HAVE_STRINGS_H
#include <ctype.h>
// avr-libc has strcasecmp in <string.h>; some PC toolchains (old MinGW) do not.
inline int strcasecmp(const char* a, const char* b) {
  while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
#endif

#include <deque>
#include <string>
#include <utility>
#include <vector>

#define HIGH 1
#define LOW 0
#define INPUT_PULLUP 2

constexpr uint8_t A0 = 54, A1 = 55, A2 = 56, A3 = 57, A4 = 58, A5 = 59, A6 = 60, A7 = 61;
constexpr uint8_t A8 = 62, A9 = 63, A10 = 64, A11 = 65, A12 = 66, A13 = 67, A14 = 68, A15 = 69;

#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define _BV(bit) (1 << (bit))
#define ISR(vec) void vec##_isr(void)
#define ATOMIC_RESTORESTATE 0
#define ATOMIC_BLOCK(x) for (int sim_once_ = 1; sim_once_; sim_once_ = 0)

class __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper*>(s))

// ---- AVR registers the sketch uses (one translation unit, so static is fine) ----
static volatile uint8_t DDRA, DDRC, DDRL, DDRK;
static volatile uint8_t PORTA, PORTC, PORTL, PORTK;
static volatile uint8_t TCCR2A, TCCR2B, OCR2A, TIMSK2;
constexpr int CS21 = 1, TOIE2 = 0, OCIE2A = 1;

namespace sim {

inline uint32_t& now() { static uint32_t t = 0; return t; }
inline std::string& serialOut() { static std::string s; return s; }
// (arrival time, byte), in arrival order
inline std::deque<std::pair<uint32_t, char>>& serialIn() { static std::deque<std::pair<uint32_t, char>> q; return q; }
inline uint8_t* eeprom() { static uint8_t m[4096]; return m; }
inline uint8_t* pinLevel() { static uint8_t p[80]; return p; }
struct PinEvent { uint32_t at; uint8_t pin, level; };
inline std::vector<PinEvent>& pinEvents() { static std::vector<PinEvent> v; return v; }
struct ServoWrite { uint32_t at; int pin; int angle; };
inline std::vector<ServoWrite>& servoWrites() { static std::vector<ServoWrite> v; return v; }
inline int* servoAngle() { static int a[80]; return a; }
inline bool& servoAttached(int pin) { static bool a[80]; return a[pin]; }

}  // namespace sim

namespace sim {
// Called on every read of the clock, so a test can check safety invariants even
// while the sketch is inside one of its own blocking loops.
inline void (*&tickHook())() { static void (*h)() = nullptr; return h; }
}  // namespace sim

inline uint32_t millis() {
  if (sim::tickHook()) sim::tickHook()();
  return sim::now()++;
}

inline void pinMode(uint8_t, uint8_t) {}
inline int digitalRead(uint8_t pin) {
  for (auto& e : sim::pinEvents())
    if (e.pin == pin && e.at <= sim::now()) sim::pinLevel()[pin] = e.level;
  return sim::pinLevel()[pin];
}

class Stream {
 public:
  void print(const char* s) { sim::serialOut() += s; }
  void print(const __FlashStringHelper* s) { sim::serialOut() += reinterpret_cast<const char*>(s); }
  void print(char c) { sim::serialOut() += c; }
  void print(unsigned char v) { sim::serialOut() += std::to_string((int)v); }
  void print(int v) { sim::serialOut() += std::to_string(v); }
  void print(unsigned int v) { sim::serialOut() += std::to_string(v); }
  void print(long v) { sim::serialOut() += std::to_string(v); }
  void print(unsigned long v) { sim::serialOut() += std::to_string(v); }
  template <class T> void println(T v) { print(v); sim::serialOut() += "\r\n"; }
  void println() { sim::serialOut() += "\r\n"; }
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long) {}
  int available() {
    int n = 0;
    for (auto& b : sim::serialIn())
      if (b.first <= sim::now()) n++;
    return n;
  }
  int read() {
    if (!sim::serialIn().empty() && sim::serialIn().front().first <= sim::now()) {
      char c = sim::serialIn().front().second;
      sim::serialIn().pop_front();
      return (unsigned char)c;
    }
    return -1;
  }
};
static HardwareSerial Serial;
