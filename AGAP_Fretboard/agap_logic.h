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

// ===================== Saved tuning (EEPROM image) =====================
// What gets tuned on the real build: the kick pulse, the hold power, the
// tempo, and each pick arm's two end angles. SAVE writes this block to EEPROM
// and the next power-up loads it, so tuning survives a power cycle.
//
// The block is encoded byte by byte (not by copying a struct) so the layout
// is the same on AVR and on the host, and it carries a magic number, a
// version and a checksum so an empty or corrupted EEPROM is rejected and the
// firmware falls back to its compiled defaults instead of using garbage.
struct Tuning {
  uint16_t kickMs;
  uint8_t holdDuty;  // 0-255 PWM duty while holding
  uint16_t bpm;
  uint8_t pickA[6];
  uint8_t pickB[6];
};

// Strict integer parse for command arguments. atoi() reads "abc" as 0 and "12abc"
// as 12, which turned typos into silent settings. Accepts an optional leading '-'
// then 1-9 digits and nothing else. Leaves `out` untouched when it returns false.
inline bool parseInt(const char* s, long& out) {
  if (!s) return false;
  const char* p = s;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  }
  if (*p == '\0') return false;
  long v = 0;
  uint8_t digits = 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9' || ++digits > 9) return false;
    v = v * 10 + (*p - '0');
  }
  out = neg ? -v : v;
  return true;
}

// A hold below about 10% would let the plunger go after the kick, so the string
// would be released while the chord is meant to ring.
constexpr uint8_t HOLD_DUTY_MIN = 25;

constexpr uint16_t TUNING_MAGIC = 0xA6A9;
constexpr uint8_t TUNING_VERSION = 1;
constexpr uint8_t TUNING_BYTES = 21;
constexpr uint16_t KICK_MS_MIN = 10, KICK_MS_MAX = 300;
constexpr uint16_t BPM_MIN = 20, BPM_MAX = 200;

// Forces every field into its allowed range (the same limits the serial
// commands enforce), so a stored or typed value can't exceed them.
inline void sanitizeTuning(Tuning& t) {
  if (t.holdDuty < HOLD_DUTY_MIN) t.holdDuty = HOLD_DUTY_MIN;
  if (t.kickMs < KICK_MS_MIN) t.kickMs = KICK_MS_MIN;
  if (t.kickMs > KICK_MS_MAX) t.kickMs = KICK_MS_MAX;
  if (t.bpm < BPM_MIN) t.bpm = BPM_MIN;
  if (t.bpm > BPM_MAX) t.bpm = BPM_MAX;
  for (uint8_t i = 0; i < 6; i++) {
    t.pickA[i] = clampPickAngle(t.pickA[i]);
    t.pickB[i] = clampPickAngle(t.pickB[i]);
  }
}

inline uint8_t tuningChecksum(const uint8_t* b, uint8_t n) {
  uint8_t c = 0xA5;
  for (uint8_t i = 0; i < n; i++) c = (uint8_t)(((c << 1) | (c >> 7)) ^ b[i]);
  return c;
}

inline void encodeTuning(const Tuning& t, uint8_t out[TUNING_BYTES]) {
  uint8_t i = 0;
  out[i++] = (uint8_t)(TUNING_MAGIC & 0xFF);
  out[i++] = (uint8_t)(TUNING_MAGIC >> 8);
  out[i++] = TUNING_VERSION;
  out[i++] = (uint8_t)(t.kickMs & 0xFF);
  out[i++] = (uint8_t)(t.kickMs >> 8);
  out[i++] = t.holdDuty;
  out[i++] = (uint8_t)(t.bpm & 0xFF);
  out[i++] = (uint8_t)(t.bpm >> 8);
  for (uint8_t k = 0; k < 6; k++) out[i++] = t.pickA[k];
  for (uint8_t k = 0; k < 6; k++) out[i++] = t.pickB[k];
  out[i] = tuningChecksum(out, TUNING_BYTES - 1);
}

// Returns false (leaving `out` untouched) for blank, foreign, wrong-version
// or corrupted data.
inline bool decodeTuning(const uint8_t in[TUNING_BYTES], Tuning& out) {
  if ((uint16_t)(in[0] | (in[1] << 8)) != TUNING_MAGIC) return false;
  if (in[2] != TUNING_VERSION) return false;
  if (in[TUNING_BYTES - 1] != tuningChecksum(in, TUNING_BYTES - 1)) return false;
  Tuning t;
  t.kickMs = (uint16_t)(in[3] | (in[4] << 8));
  t.holdDuty = in[5];
  t.bpm = (uint16_t)(in[6] | (in[7] << 8));
  for (uint8_t k = 0; k < 6; k++) t.pickA[k] = in[8 + k];
  for (uint8_t k = 0; k < 6; k++) t.pickB[k] = in[14 + k];
  sanitizeTuning(t);
  out = t;
  return true;
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

// All channels as one word: bit i is channel i. 32 bits cover the 30 channels
// of the fretboard (6 strings x 5 frets); the sketch splits it across ports.
struct Mask32 {
  uint32_t on;    // channels driven at all (kicking or holding)
  uint32_t kick;  // channels in the full-power kick phase
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

  // Like press(), but refuses to energise one more channel than maxActive
  // allows (power-supply and heat limit). A channel that is already on can
  // always be refreshed. Returns false, changing nothing, when refused.
  bool tryPress(uint8_t i, uint32_t startAt, uint32_t now, uint8_t maxActive) {
    if (state_[i] == ChanState::OFF && activeCount() >= maxActive) return false;
    press(i, startAt, now);
    return true;
  }

  // Channels that are pending, kicking or holding.
  uint8_t activeCount() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < N; i++) if (state_[i] != ChanState::OFF) n++;
    return n;
  }

  // Takes effect for any kick that starts or is still running afterwards.
  void setKickMs(uint16_t kickMs) { kickMs_ = kickMs; }

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
          // Signed on purpose. A command can re-press a held coil with a clock reading
          // taken after the one the sketch's loop() passed here, so the refresh time can
          // be a few ms AHEAD of `now`. Unsigned subtraction would wrap to a huge number
          // and drop the coil that was just refreshed. (millis() rolling over after ~49
          // days still works: the difference of two close readings is small either way.)
          if ((int32_t)(now - refresh_[i]) >= (int32_t)cmdTimeoutMs_) {
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

  // Same split as computeMasks(), as one 32-bit word each (N <= 32).
  Mask32 computeMask32() const {
    Mask32 m{0, 0};
    for (uint8_t i = 0; i < N; i++) {
      if (state_[i] == ChanState::KICK) {
        m.on |= (uint32_t)1 << i;
        m.kick |= (uint32_t)1 << i;
      } else if (state_[i] == ChanState::HOLD) {
        m.on |= (uint32_t)1 << i;
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
  // mask: bit s set = string s (0 = low E) is plucked. Strings that are not
  // sounding (muted in a chord) are skipped without spending a gap on them.
  void start(bool down, uint32_t startAt, uint8_t gapMs, uint8_t mask = 0x3F) {
    down_ = down;
    idx_ = 0;
    gap_ = gapMs;
    nextAt_ = startAt;
    mask_ = mask & 0x3F;
    active_ = true;
  }

  int8_t update(uint32_t now) {
    if (!active_ || (int32_t)(now - nextAt_) < 0) return -1;
    while (idx_ < 6) {
      uint8_t s = down_ ? idx_ : (uint8_t)(5 - idx_);
      idx_++;
      if (mask_ & (1u << s)) {
        nextAt_ = now + gap_;
        return (int8_t)s;
      }
    }
    active_ = false;
    return -1;
  }

  void stop() { active_ = false; }
  bool active() const { return active_; }

 private:
  bool down_ = false;
  uint8_t idx_ = 0;
  uint8_t gap_ = 0;
  uint8_t mask_ = 0x3F;
  uint32_t nextAt_ = 0;
  bool active_ = false;
};

}  // namespace agap

#endif  // AGAP_LOGIC_H
