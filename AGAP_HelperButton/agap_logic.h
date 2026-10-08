#ifndef AGAP_LOGIC_H
#define AGAP_LOGIC_H

#include <stdint.h>
#include <ctype.h>

// Hardware-independent logic shared between AGAP_HelperButton.ino (compiled
// for AVR) and tests/test_agap_logic.cpp (compiled natively with g++ and
// run on the host, no board needed).
//
// Nothing in this file touches a register, a timer, Serial, or a Servo -
// every function takes the current time and returns what to do, instead of
// calling millis()/digitalWrite() itself. That split is what makes it
// testable: the tests feed in whatever timestamps they want and check the
// resulting state, instead of needing real hardware or real elapsed time.
// If a change needs millis(), digitalRead(), Serial or Servo, it belongs
// in the .ino, not here.

namespace agap {

// Portable case-insensitive compare. strcasecmp() is POSIX/avr-libc, not
// standard C++, so it isn't guaranteed to exist with every host compiler -
// this works identically on AVR and on the host.
inline bool ciStrEq(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

// Case-insensitive linear search through a label table, e.g. BUTTON_LABEL[].
inline int8_t findLabel(const char* const* labels, uint8_t n, const char* query) {
  for (uint8_t i = 0; i < n; i++) {
    if (ciStrEq(labels[i], query)) return (int8_t)i;
  }
  return -1;
}

// ===================== Pick (servo) calibration =====================
// The printed pick arms are adjustable (04_adjustable_pick_arm), so each
// string's two end-of-swing angles (A and B) must be tuned on the real
// build. These helpers keep that tuning inside sane limits and give it the
// string numbering people use (1 = high e ... 6 = low E).
//
// PICK_ANGLE_MIN/MAX are conservative placeholders to stop a typo driving a
// servo into its end stop, NOT measured limits of the printed arm. Widen
// them only after checking the real travel.
constexpr uint8_t PICK_ANGLE_MIN = 10;
constexpr uint8_t PICK_ANGLE_MAX = 170;

inline uint8_t clampPickAngle(int angle) {
  if (angle < PICK_ANGLE_MIN) return PICK_ANGLE_MIN;
  if (angle > PICK_ANGLE_MAX) return PICK_ANGLE_MAX;
  return (uint8_t)angle;
}

// String number as a player says it (1 = high e .. 6 = low E) -> servo index
// (0 = low E .. 5 = high e, the order the strummer and servo pins use).
// Returns -1 if out of range.
inline int8_t stringToServoIndex(int stringNumber) {
  return (stringNumber >= 1 && stringNumber <= 6) ? (int8_t)(6 - stringNumber) : (int8_t)-1;
}

// ===================== Kick-and-hold channel driver =====================
// One instance of this replaces AGAP_HelperButton.ino's chanState[]/
// chanTime[]/chanRefresh[] arrays plus rebuildMasks()/updateChannels()/
// pressButton()/releaseButton(). Same state machine, same transitions:
//
//   OFF --press()--> PENDING --[now reaches startAt]--> KICK
//   KICK --[kickMs elapsed]--> HOLD
//   HOLD --[cmdTimeoutMs since last press() refresh]--> OFF
//   any state --release()--> OFF
//
// N is the channel count (10 for this firmware's button layout, up to 15
// total since masks only span PORTA (0-7) + PORTC bits 0-1 - a 3rd
// register would be needed past channel 15, same ceiling the .ino has).
enum class ChanState : uint8_t { OFF, PENDING, KICK, HOLD };

struct Masks {
  uint8_t a, c, ka, kc;  // onA/onC/kickA/kickC, as written into the ISR's volatiles
};

template <uint8_t N>
class ChannelDriver {
 public:
  ChannelDriver(uint16_t kickMs, uint32_t cmdTimeoutMs)
      : kickMs_(kickMs), cmdTimeoutMs_(cmdTimeoutMs) {
    for (uint8_t i = 0; i < N; i++) {
      state_[i] = ChanState::OFF;
      time_[i] = 0;
      refresh_[i] = 0;
    }
  }

  // Schedules channel i to kick at startAt (usually `now`, or staggered
  // later when several channels are pressed together). If the channel is
  // already on, startAt is ignored but the auto-release timeout still
  // resets - matches pressButton()'s "refresh keeps a held chord alive"
  // behaviour in the .ino.
  void press(uint8_t i, uint32_t startAt, uint32_t now) {
    if (state_[i] == ChanState::OFF) {
      state_[i] = ChanState::PENDING;
      time_[i] = startAt;
    }
    refresh_[i] = now;
  }

  void release(uint8_t i) { state_[i] = ChanState::OFF; }

  void releaseAll() {
    for (uint8_t i = 0; i < N; i++) release(i);
  }

  // Advances every channel's state machine to time `now`. Returns true if
  // any channel changed state, i.e. the caller should push fresh masks to
  // the hardware. Call this once per firmware loop() iteration.
  bool update(uint32_t now) {
    bool dirty = false;
    for (uint8_t i = 0; i < N; i++) {
      switch (state_[i]) {
        case ChanState::PENDING:
          if ((int32_t)(now - time_[i]) >= 0) {
            state_[i] = ChanState::KICK;
            time_[i] = now;
            dirty = true;
          }
          break;
        case ChanState::KICK:
          if (now - time_[i] >= kickMs_) {
            state_[i] = ChanState::HOLD;
            dirty = true;
          }
          break;
        case ChanState::HOLD:
          if (now - refresh_[i] >= cmdTimeoutMs_) {
            state_[i] = ChanState::OFF;  // safety timeout
            dirty = true;
          }
          break;
        default:
          break;
      }
    }
    return dirty;
  }

  ChanState stateOf(uint8_t i) const { return state_[i]; }

  // KICK channels are high in both the steady mask and the kick-only mask
  // (so they read 100% duty from the ISR); HOLD channels are high only in
  // the steady mask (so they read holdDuty/256). Same split as the .ino.
  Masks computeMasks() const {
    Masks m{0, 0, 0, 0};
    for (uint8_t i = 0; i < N; i++) {
      if (state_[i] == ChanState::KICK) {
        setBit(i, m.a, m.c);
        setBit(i, m.ka, m.kc);
      } else if (state_[i] == ChanState::HOLD) {
        setBit(i, m.a, m.c);
      }
    }
    return m;
  }

 private:
  static void setBit(uint8_t i, uint8_t& a, uint8_t& c) {
    if (i < 8) a |= (uint8_t)(1u << i);
    else c |= (uint8_t)(1u << (i - 8));
  }

  ChanState state_[N];
  uint32_t time_[N];
  uint32_t refresh_[N];
  uint16_t kickMs_;
  uint32_t cmdTimeoutMs_;
};

// ================================ Strummer ================================
// Replaces the .ino's anonymous `strum` struct + startStrum()/updateStrum().
// update() returns which string (0-5, 0=6th/low E) to pluck now, or -1 if
// there's nothing to do this tick - the caller does the actual pluck (a
// Servo write), this class only decides the order and timing.
class Strummer {
 public:
  void start(bool down, uint32_t startAt, uint8_t gapMs) {
    down_ = down;
    idx_ = 0;
    gap_ = gapMs;
    nextAt_ = startAt;
    active_ = true;
  }

  int8_t update(uint32_t now) {
    if (!active_ || (int32_t)(now - nextAt_) < 0) return -1;
    if (idx_ >= 6) {
      active_ = false;
      return -1;
    }
    uint8_t s = down_ ? idx_ : (uint8_t)(5 - idx_);
    idx_++;
    nextAt_ = now + gap_;
    return (int8_t)s;
  }

  void stop() { active_ = false; }
  bool active() const { return active_; }

 private:
  bool down_ = false;
  uint8_t idx_ = 0;
  uint8_t gap_ = 0;
  uint32_t nextAt_ = 0;
  bool active_ = false;
};

}  // namespace agap

#endif  // AGAP_LOGIC_H
