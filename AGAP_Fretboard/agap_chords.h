#ifndef AGAP_CHORDS_H
#define AGAP_CHORDS_H

#include <ctype.h>
#include <stdint.h>
#include <string.h>

// Chord name -> easiest fingering, the same search as ChordAI/chord_ai.py
// (and its JavaScript port). Pure computation with no Arduino calls, so it
// is unit-tested on a PC and checked against the Python on every chord
// (AGAP_Fretboard/tests). Keep the tables and costs in step with the Python.
//
// Strings are indexed 0 = low E ... 5 = high e. A fret is -1 (muted) or
// 0..maxFret. The fretboard has solenoids on frets 1-5, so callers pass
// maxFret = 5.

namespace agap {

constexpr int8_t MUTED = -1;

struct Quality {
  const char* alias[2];  // first is the canonical name; second may be null
  uint8_t n;             // number of chord tones
  uint8_t iv[5];         // semitones above the root
  uint8_t w[5];          // cost of leaving that tone out
};

inline const Quality* qualityTable(uint8_t& count) {
  static const Quality T[] = {
      {{"", "maj"}, 3, {0, 4, 7}, {40, 40, 8}},
      {{"m", "min"}, 3, {0, 3, 7}, {40, 40, 8}},
      {{"7", 0}, 4, {0, 4, 7, 10}, {40, 40, 6, 25}},
      {{"maj7", "M7"}, 4, {0, 4, 7, 11}, {40, 40, 6, 25}},
      {{"m7", 0}, 4, {0, 3, 7, 10}, {40, 40, 6, 25}},
      {{"6", 0}, 4, {0, 4, 7, 9}, {40, 40, 6, 25}},
      {{"m6", 0}, 4, {0, 3, 7, 9}, {40, 40, 6, 25}},
      {{"add9", 0}, 4, {0, 4, 7, 2}, {40, 40, 6, 25}},
      {{"sus2", 0}, 3, {0, 2, 7}, {40, 40, 8}},
      {{"sus4", "sus"}, 3, {0, 5, 7}, {40, 40, 8}},
      {{"dim", 0}, 3, {0, 3, 6}, {40, 40, 30}},
      {{"aug", 0}, 3, {0, 4, 8}, {40, 40, 30}},
      {{"5", 0}, 2, {0, 7}, {40, 40}},
      {{"9", 0}, 5, {0, 4, 7, 10, 2}, {40, 40, 6, 20, 15}},
      {{"m9", 0}, 5, {0, 3, 7, 10, 2}, {40, 40, 6, 20, 15}},
      {{"7sus4", 0}, 4, {0, 5, 7, 10}, {40, 40, 8, 20}},
      {{"m7b5", "m7-5"}, 4, {0, 3, 6, 10}, {40, 40, 20, 20}},
  };
  count = (uint8_t)(sizeof(T) / sizeof(T[0]));
  return T;
}

inline bool chordCiEq(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

// The part of a chord name after the root ("m7", "maj7", "sus4"), or null.
// Exact spelling wins, because capital M and lowercase m differ (CM7 is a
// major seventh, Cm7 a minor seventh). Only then a case-insensitive match for
// spelled-out words (Maj7, DIM), and never for a bare capital M or M plus a
// number (CM, CM9): those mean major in common notation, so guessing minor
// would be wrong. Same rule as chord_ai.py's lookup_quality().
inline const Quality* lookupQuality(const char* s) {
  uint8_t count;
  const Quality* T = qualityTable(count);
  for (uint8_t i = 0; i < count; i++)
    for (uint8_t k = 0; k < 2; k++)
      if (T[i].alias[k] && strcmp(T[i].alias[k], s) == 0) return &T[i];
  if (strlen(s) < 3 || (s[0] == 'M' && isdigit((unsigned char)s[1]))) return 0;
  const Quality* found = 0;
  for (uint8_t i = 0; i < count; i++)
    for (uint8_t k = 0; k < 2; k++)
      if (T[i].alias[k] && chordCiEq(T[i].alias[k], s)) {
        if (found && found != &T[i]) return 0;  // two qualities share this lowercase form
        found = &T[i];
      }
  return found;
}

inline bool parseChord(const char* text, uint8_t& root, const Quality*& q) {
  char buf[16];
  while (*text == ' ' || *text == '\t') text++;
  size_t n = strlen(text);
  while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) n--;
  if (n == 0 || n >= sizeof(buf)) return false;
  memcpy(buf, text, n);
  buf[n] = '\0';

  static const int8_t LETTER_PC[7] = {9, 11, 0, 2, 4, 5, 7};  // A..G
  char L = (char)toupper((unsigned char)buf[0]);
  if (L < 'A' || L > 'G') return false;
  int8_t pc = LETTER_PC[L - 'A'];
  const char* rest = buf + 1;
  if (*rest == '#') { pc++; rest++; }
  else if (*rest == 'b') { pc--; rest++; }
  const Quality* found = lookupQuality(rest);
  if (!found) return false;
  root = (uint8_t)((pc + 12) % 12);
  q = found;
  return true;
}

struct Voicing {
  int8_t fret[6];
  int16_t cost;
  uint8_t root;
  const Quality* q;
};

namespace detail {

constexpr uint8_t OPEN_PC[6] = {4, 9, 2, 7, 11, 4};  // standard tuning E A D G B E
constexpr int16_t COST_MUTE = 6;
constexpr int16_t COST_INNER_MUTE = 4;
constexpr int16_t COST_FRETTED = 1;
constexpr int16_t COST_BASS_NOT_ROOT = 15;
constexpr int16_t COST_THIN = 60;  // per string below three sounding

struct Search {
  uint8_t root;
  const Quality* q;
  uint8_t maxFret;
  int8_t cur[6];
  int8_t best[6];
  int16_t bestCost;

  int8_t toneIndex(uint8_t pc) const {
    uint8_t iv = (uint8_t)((pc + 12 - root) % 12);
    for (uint8_t k = 0; k < q->n; k++)
      if (q->iv[k] == iv) return (int8_t)k;
    return -1;
  }

  int16_t finalCost(uint8_t covered) const {
    int16_t c = 0;
    for (uint8_t k = 0; k < q->n; k++)
      if (!(covered & (1u << k))) c += q->w[k];
    int8_t first = -1, last = -1;
    uint8_t sounded = 0;
    for (uint8_t s = 0; s < 6; s++)
      if (cur[s] != MUTED) {
        if (first < 0) first = (int8_t)s;
        last = (int8_t)s;
        sounded++;
      }
    if (sounded < 3) c += (int16_t)(COST_THIN * (3 - sounded));
    if (first >= 0) {
      if ((OPEN_PC[first] + cur[first]) % 12 != root) c += COST_BASS_NOT_ROOT;
      for (int8_t s = (int8_t)(first + 1); s < last; s++)
        if (cur[s] == MUTED) c += COST_INNER_MUTE;
    }
    return c;
  }

  void dfs(uint8_t s, uint8_t covered, int16_t partial) {
    if (partial >= bestCost) return;  // branch and bound
    if (s == 6) {
      int16_t c = (int16_t)(partial + finalCost(covered));
      if (c < bestCost) {
        bestCost = c;
        memcpy(best, cur, 6);
      }
      return;
    }
    for (int8_t f = 0; f <= (int8_t)maxFret; f++) {
      int8_t k = toneIndex((uint8_t)((OPEN_PC[s] + f) % 12));
      if (k < 0) continue;  // only chord tones may sound
      cur[s] = f;
      dfs((uint8_t)(s + 1), (uint8_t)(covered | (1u << k)), (int16_t)(partial + (f ? COST_FRETTED : 0)));
    }
    cur[s] = MUTED;
    dfs((uint8_t)(s + 1), covered, (int16_t)(partial + COST_MUTE));
  }
};

}  // namespace detail

// Fills `out` with the lowest-cost fingering using frets 0..maxFret. Returns
// false if the name is not a chord this table knows.
inline bool solveChord(const char* name, uint8_t maxFret, Voicing& out) {
  uint8_t root;
  const Quality* q;
  if (!parseChord(name, root, q)) return false;
  detail::Search s;
  s.root = root;
  s.q = q;
  s.maxFret = maxFret;
  s.bestCost = 32767;
  for (uint8_t i = 0; i < 6; i++) s.cur[i] = s.best[i] = MUTED;
  s.dfs(0, 0, 0);
  memcpy(out.fret, s.best, 6);
  out.cost = s.bestCost;
  out.root = root;
  out.q = q;
  return true;
}

// "C#m7" style name into buf (at least 12 bytes).
inline void chordDisplayName(const Voicing& v, char* buf) {
  static const char* const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  strcpy(buf, N[v.root % 12]);
  strcat(buf, v.q->alias[0]);
}

}  // namespace agap

#endif  // AGAP_CHORDS_H
