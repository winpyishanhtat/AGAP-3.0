#ifndef AGAP_FRET_H
#define AGAP_FRET_H

#include <stdint.h>

// The fretboard: one solenoid per string per fret on frets 1-5, 30 in all,
// held in five six-socket plates (04-fret-1 ... 04-fret-5). Pure mapping logic,
// no Arduino calls, so it is unit-tested on a PC.
//
// Strings are 0 = low E ... 5 = high e. Open strings (fret 0) have no solenoid.
//
// Channel numbering keeps each plate's six solenoids together, so one cable
// harness serves one plate:
//     channel = (fret - 1) * 6 + string          fret 1 = channels 0-5, ...
//
// Output pins (placeholders until the real wiring is chosen; the sketch's
// Timer2 interrupt drives all four ports at once):
//     channels  0- 7  PORTA bit 0-7   D22 D23 D24 D25 D26 D27 D28 D29
//     channels  8-15  PORTC bit 0-7   D37 D36 D35 D34 D33 D32 D31 D30
//     channels 16-23  PORTL bit 0-7   D49 D48 D47 D46 D45 D44 D43 D42
//     channels 24-29  PORTK bit 0-5   A8  A9  A10 A11 A12 A13

namespace agap {

constexpr uint8_t FRET_STRINGS = 6;
constexpr uint8_t FRET_COUNT = 5;
constexpr uint8_t FRET_CHANNELS = FRET_STRINGS * FRET_COUNT;  // 30

// A fingering presses at most one fret per string, so never more than six
// coils at once. The power supply is sized for this, and raw single-channel
// commands are held to the same limit.
constexpr uint8_t MAX_COILS = 6;

// Channel for (string, fret), or -1 if there is no solenoid there.
inline int8_t fretChannel(uint8_t stringIdx, uint8_t fret) {
  if (stringIdx >= FRET_STRINGS || fret < 1 || fret > FRET_COUNT) return -1;
  return (int8_t)((fret - 1) * FRET_STRINGS + stringIdx);
}

inline uint8_t channelString(uint8_t channel) { return (uint8_t)(channel % FRET_STRINGS); }
inline uint8_t channelFret(uint8_t channel) { return (uint8_t)(channel / FRET_STRINGS + 1); }

// Every entry must be -1 (muted) or 0..5.
inline bool voicingFits(const int8_t fret[6]) {
  for (uint8_t s = 0; s < FRET_STRINGS; s++)
    if (fret[s] < -1 || fret[s] > (int8_t)FRET_COUNT) return false;
  return true;
}

// Bit s set = string s sounds (open or fretted). Used as the strum mask.
inline uint8_t soundingMask(const int8_t fret[6]) {
  uint8_t m = 0;
  for (uint8_t s = 0; s < FRET_STRINGS; s++)
    if (fret[s] != -1) m |= (uint8_t)(1u << s);
  return m;
}

// The solenoids a fingering needs, lowest string first. Open and muted
// strings need none. Returns how many were written to out (at most 6).
inline uint8_t voicingChannels(const int8_t fret[6], uint8_t out[6]) {
  uint8_t n = 0;
  for (uint8_t s = 0; s < FRET_STRINGS; s++)
    if (fret[s] >= 1) out[n++] = (uint8_t)fretChannel(s, (uint8_t)fret[s]);
  return n;
}

// The 32-bit channel word split into the four port bytes the interrupt writes.
struct PortBytes {
  uint8_t a, c, l, k;
};

inline PortBytes splitMask(uint32_t m) {
  PortBytes p;
  p.a = (uint8_t)(m & 0xFF);
  p.c = (uint8_t)((m >> 8) & 0xFF);
  p.l = (uint8_t)((m >> 16) & 0xFF);
  p.k = (uint8_t)((m >> 24) & 0x3F);  // only 6 of PORTK's 8 bits are used
  return p;
}

}  // namespace agap

#endif  // AGAP_FRET_H
