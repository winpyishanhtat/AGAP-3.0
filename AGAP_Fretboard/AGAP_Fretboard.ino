/*
  AGAP - Fretboard (final design: direct fretting, frets 1-5)
  Target board: Arduino Mega 2560

  The final printed design (docs/HARDWARE_SPECS.md) fingers the chord directly:
  five six-socket plates, one per fret 1-5, hold 30 solenoids - one per string
  per fret. A chord is turned into a fingering on the board (the same search as
  ChordAI), the matching solenoids press the strings, then the six servo picks
  of the AutoStrummer sound only the strings that ring.

    CHORD Bm   ->  x 2 0 4 0 2  ->  press 3 solenoids  ->  strum 5 strings

  Everything marked "placeholder" below is an unmeasured starting value
  (README_AGAP.md step 5). Nothing here has run on real hardware. Tune with
  docs/TUNING_GUIDE.md.

  Safety:
    - Never power a solenoid directly from a pin. Each of the 30 channels needs
      its own driver (MOSFET or relay) plus flyback suppression.
    - A fingering presses at most one fret per string, so at most 6 coils are
      on at once. Raw PRESS commands are held to the same limit (MAX_COILS).
    - STOP releases everything, even in the middle of SEQUENCE or CALIB.
    - A channel held longer than CMD_TIMEOUT_MS with no refresh auto-releases.

  Output channels (see agap_fret.h): channel = (fret - 1) * 6 + string, string
  0 = low E. One plate = six consecutive channels.
    channels  0- 7  PORTA  D22 D23 D24 D25 D26 D27 D28 D29
    channels  8-15  PORTC  D37 D36 D35 D34 D33 D32 D31 D30
    channels 16-23  PORTL  D49 D48 D47 D46 D45 D44 D43 D42
    channels 24-29  PORTK  A8  A9  A10 A11 A12 A13
  Servos (AutoStrummer pods, string 6 first): D2 D3 D4 D5 D6 D7
  Panel buttons (to GND): A0 STRUM  A1 NEXT  A2 PREV  A3 STOP
  Serial: USB 115200.

  The logic lives in agap_logic.h (channels, strum, saved tuning), agap_chords.h
  (the chord search) and agap_fret.h (string/fret -> channel). All three have
  no Arduino calls and are unit-tested on a PC (tests/run_tests.sh).
*/

#include <EEPROM.h>
#include <Servo.h>
#include <stdlib.h>
#include <string.h>
#include <util/atomic.h>

#include "agap_chords.h"
#include "agap_fret.h"
#include "agap_logic.h"

#define FW_NAME "AGAP-fretboard"
#define FW_VERSION "0.1"

// ================================ Timing (placeholders) ================================
uint16_t kickMs = 60;           // full-power pulse that seats a solenoid
uint8_t holdDuty = 60;          // 0-255 hold power afterwards
const uint32_t CMD_TIMEOUT_MS = 8000;
const uint8_t STAGGER_MS = 4;   // spread kicks to limit inrush current
const uint8_t SETTLE_MS = 15;   // wait after the last kick before strumming
const uint8_t STRUM_GAP_MS = 18;

const uint8_t SERVO_PIN[6] = {2, 3, 4, 5, 6, 7};
uint8_t pickA[6] = {70, 70, 70, 70, 70, 70};
uint8_t pickB[6] = {110, 110, 110, 110, 110, 110};

enum { BTN_STRUM, BTN_NEXT, BTN_PREV, BTN_STOP, NUM_CTRL_BTN };
const uint8_t CTRL_BTN_PIN[NUM_CTRL_BTN] = {A0, A1, A2, A3};

// A saved chord shape in the progression.
struct Step {
  char name[8];
  int8_t fret[6];
};
const uint8_t MAX_PROG = 12;
Step prog[MAX_PROG];
uint8_t progLen = 0, progPos = 0;

// ========================= Driver (kick-and-hold PWM, 30 channels) =========================
// Timer2 overflow raises every active coil; compare-match A drops the ones that are
// only holding, so kicking coils get full power and holding coils get holdDuty/256.
volatile uint8_t onA, onC, onL, onK, kickA, kickC, kickL, kickK;

ISR(TIMER2_OVF_vect) { PORTA = onA; PORTC = onC; PORTL = onL; PORTK = onK; }
ISR(TIMER2_COMPA_vect) { PORTA = kickA; PORTC = kickC; PORTL = kickL; PORTK = kickK; }

agap::ChannelDriver<agap::FRET_CHANNELS> channels(kickMs, CMD_TIMEOUT_MS);

void pushMasks() {
  agap::Mask32 m = channels.computeMask32();
  agap::PortBytes on = agap::splitMask(m.on), kick = agap::splitMask(m.kick);
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    onA = on.a; onC = on.c; onL = on.l; onK = on.k;
    kickA = kick.a; kickC = kick.c; kickL = kick.l; kickK = kick.k;
  }
}

void driverBegin() {
  DDRA = 0xFF; DDRC = 0xFF; DDRL = 0xFF; DDRK = 0x3F;
  PORTA = 0; PORTC = 0; PORTL = 0; PORTK = 0;
  TCCR2A = 0;
  TCCR2B = _BV(CS21);  // clk/8 -> 16 MHz / 8 / 256 = 7.8 kHz
  OCR2A = holdDuty;
  TIMSK2 = _BV(TOIE2) | _BV(OCIE2A);
}

// =============================== Strummer ===============================
Servo picks[6];
bool pickAtB[6];
agap::Strummer strum;

void pluck(uint8_t s) {
  pickAtB[s] = !pickAtB[s];
  picks[s].write(pickAtB[s] ? pickB[s] : pickA[s]);
}

void updateStrum(uint32_t now) {
  int8_t s = strum.update(now);
  if (s >= 0) pluck((uint8_t)s);
}

// ================================ Fingering ================================
int8_t curFret[6] = {-1, -1, -1, -1, -1, -1};
bool curValid = false;
bool strumDown = true;
uint16_t bpm = 50;

void releaseAll() {
  channels.releaseAll();
  pushMasks();
}

// Presses exactly the solenoids this fingering needs and lets go of every other
// one. Returns how many ms until the new ones are seated (0 if none moved).
uint16_t applyVoicing(const int8_t fret[6]) {
  uint8_t ch[6];
  uint8_t n = agap::voicingChannels(fret, ch);
  bool want[agap::FRET_CHANNELS] = {false};
  for (uint8_t k = 0; k < n; k++) want[ch[k]] = true;
  for (uint8_t c = 0; c < agap::FRET_CHANNELS; c++)
    if (!want[c]) channels.release(c);
  uint32_t t = millis();
  uint8_t kicked = 0;
  for (uint8_t k = 0; k < n; k++) {
    bool wasOff = channels.stateOf(ch[k]) == agap::ChanState::OFF;
    // Cannot be refused: at most six channels are wanted and the rest were released.
    channels.tryPress(ch[k], t + STAGGER_MS * kicked, t, agap::MAX_COILS);
    if (wasOff) kicked++;
  }
  pushMasks();
  memcpy(curFret, fret, 6);
  curValid = true;
  return kicked ? (uint16_t)(STAGGER_MS * (kicked - 1) + kickMs + SETTLE_MS) : 0;
}

void playVoicing(const int8_t fret[6], bool down) {
  uint16_t settle = applyVoicing(fret);
  strum.start(down, millis() + settle, STRUM_GAP_MS, agap::soundingMask(fret));
}

void stopAll() {
  strum.stop();
  releaseAll();
  curValid = false;
}

void printVoicing(Stream& o, const char* name, const int8_t fret[6], int16_t cost) {
  o.print(name);
  o.print(F(" -> "));
  for (uint8_t s = 0; s < 6; s++) {
    if (fret[s] < 0) o.print('x'); else o.print(fret[s]);
    o.print(' ');
  }
  if (cost >= 0) { o.print(F(" (cost ")); o.print(cost); o.print(')'); }
  o.println();
}

// Fingering for a chord name, limited to the frets that have solenoids.
bool solve(const char* name, agap::Voicing& v) {
  return agap::solveChord(name, agap::FRET_COUNT, v);
}

// ========================= Saved tuning (EEPROM) =========================
agap::Tuning compiledDefaults;

agap::Tuning currentTuning() {
  agap::Tuning t;
  t.kickMs = kickMs; t.holdDuty = holdDuty; t.bpm = bpm;
  for (uint8_t i = 0; i < 6; i++) { t.pickA[i] = pickA[i]; t.pickB[i] = pickB[i]; }
  return t;
}

void applyTuning(const agap::Tuning& t) {
  kickMs = t.kickMs;
  channels.setKickMs(kickMs);
  holdDuty = t.holdDuty;
  OCR2A = holdDuty;
  bpm = t.bpm;
  for (uint8_t i = 0; i < 6; i++) {
    pickA[i] = t.pickA[i];
    pickB[i] = t.pickB[i];
    picks[i].write(pickAtB[i] ? pickB[i] : pickA[i]);
  }
}

bool readSavedTuning(agap::Tuning& t) {
  uint8_t buf[agap::TUNING_BYTES];
  for (uint8_t i = 0; i < agap::TUNING_BYTES; i++) buf[i] = EEPROM.read(i);
  return agap::decodeTuning(buf, t);
}

bool writeSavedTuning(const agap::Tuning& t) {
  uint8_t buf[agap::TUNING_BYTES];
  agap::encodeTuning(t, buf);
  for (uint8_t i = 0; i < agap::TUNING_BYTES; i++) EEPROM.update(i, buf[i]);
  agap::Tuning check;
  return readSavedTuning(check);
}

// =========================== Command protocol ===========================
bool eq(const char* a, const char* b) { return agap::ciStrEq(a, b); }

char lineBuf[96];
uint8_t lineLen = 0;
bool abortRequested = false;

void printHelp(Stream& o) {
  o.println(F("CHORD <name>             solve + press + strum (e.g. CHORD F#m7)"));
  o.println(F("SHOW <name>              print the fingering, move nothing"));
  o.println(F("RAW <6 frets>            x or 0-5 per string, low E first (e.g. RAW x 3 2 0 1 0)"));
  o.println(F("PRESS <string 1-6> <fret 1-5>   energise one solenoid (bring-up); RELEASE <string> <fret> | ALL"));
  o.println(F("STRUM [D|U]              strum the sounding strings of the current fingering"));
  o.println(F("PROG <c1> <c2> ...  NEXT  PREV     progression (max 12), stepped by panel buttons"));
  o.println(F("SEQUENCE <c1> <c2> ...   play chords one after another at TEMPO"));
  o.println(F("CALIB <string> <fret> <holdMs> <reps> [gapMs]   repeated press test"));
  o.println(F("PICK <1-6> <A|B> <deg>  PLUCK <1-6>  PICKS      pick-arm tuning (1 = high e)"));
  o.println(F("SAVE | LOAD | DEFAULTS   KICK <ms>  HOLD <percent>  TEMPO <bpm>"));
  o.println(F("FRETS  STATUS  VERSION  PING  STOP"));
}

bool checkAbort() {
  if (digitalRead(CTRL_BTN_PIN[BTN_STOP]) == LOW) abortRequested = true;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen && eq(lineBuf, "STOP")) abortRequested = true;
      else if (lineLen) { Serial.print(F("BUSY, ignored: ")); Serial.println(lineBuf); }
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
  return abortRequested;
}

// Reads 6 fret tokens ("x" or 0-5) from the rest of the line.
bool parseFrets(int8_t out[6]) {
  for (uint8_t s = 0; s < 6; s++) {
    char* tok = strtok(NULL, " \t");
    if (!tok) return false;
    if (tok[0] == 'x' || tok[0] == 'X') out[s] = -1;
    else if (tok[0] >= '0' && tok[0] <= '9' && tok[1] == '\0') out[s] = tok[0] - '0';
    else return false;
  }
  return agap::voicingFits(out);
}

// 1-based string number as a player says it (1 = high e) -> index (0 = low E).
int8_t stringArg(const char* tok) {
  return tok ? agap::stringToServoIndex(atoi(tok)) : (int8_t)-1;
}

void waitBeat(uint32_t startedAt, uint32_t beat) {
  while (strum.active() || millis() - startedAt < beat) {
    updateStrum(millis());
    if (channels.update(millis())) pushMasks();
    if (checkAbort()) return;
  }
}

void handleSequence(char* rest, Stream& out) {
  Step steps[MAX_PROG];
  uint8_t n = 0;
  for (char* tok = strtok(rest, " \t"); tok; tok = strtok(NULL, " \t")) {
    if (n >= MAX_PROG) { out.println(F("ERR too many chords")); return; }
    agap::Voicing v;
    if (strlen(tok) >= sizeof(steps[n].name) || !solve(tok, v)) {
      out.print(F("ERR unknown chord ")); out.println(tok);
      return;  // nothing has moved yet
    }
    strcpy(steps[n].name, tok);
    memcpy(steps[n].fret, v.fret, 6);
    n++;
  }
  if (!n) { out.println(F("ERR empty sequence")); return; }
  const uint32_t beat = 60000UL / bpm;
  abortRequested = false;
  for (uint8_t i = 0; i < n; i++) {
    out.print(F("STEP ")); out.println(steps[i].name);
    uint32_t t0 = millis();
    playVoicing(steps[i].fret, strumDown);
    strumDown = !strumDown;
    waitBeat(t0, beat);
    if (abortRequested) { stopAll(); out.println(F("ABORTED (STOP)")); return; }
  }
}

void stepProgression(int8_t dir) {
  if (!progLen) return;
  progPos = (uint8_t)((progPos + progLen + dir) % progLen);
  playVoicing(prog[progPos].fret, true);
  strumDown = false;
  printVoicing(Serial, prog[progPos].name, prog[progPos].fret, -1);
}

void handleCommand(char* line, Stream& out) {
  char* cmd = strtok(line, " \t");
  if (!cmd) return;

  if (eq(cmd, "HELP")) {
    printHelp(out);
  } else if (eq(cmd, "PING")) {
    out.println(F("PONG"));
  } else if (eq(cmd, "VERSION")) {
    out.print(F(FW_NAME " fw " FW_VERSION " built " __DATE__ " " __TIME__));
    out.println();
  } else if (eq(cmd, "FRETS")) {
    out.println(F("6 strings x 5 frets = 30 channels, at most 6 coils at once"));
  } else if (eq(cmd, "SHOW") || eq(cmd, "CHORD")) {
    char* name = strtok(NULL, " \t");
    agap::Voicing v;
    if (!name || !solve(name, v)) { out.println(F("ERR unknown chord")); return; }
    char shown[16];
    agap::chordDisplayName(v, shown);
    printVoicing(out, shown, v.fret, v.cost);
    if (eq(cmd, "CHORD")) { playVoicing(v.fret, true); strumDown = false; }
  } else if (eq(cmd, "RAW")) {
    int8_t f[6];
    if (!parseFrets(f)) { out.println(F("ERR RAW needs 6 values, each x or 0-5")); return; }
    printVoicing(out, "RAW", f, -1);
    playVoicing(f, true);
    strumDown = false;
  } else if (eq(cmd, "PRESS")) {
    int8_t s = stringArg(strtok(NULL, " \t"));
    char* fr = strtok(NULL, " \t");
    int8_t ch = (s >= 0 && fr) ? agap::fretChannel((uint8_t)s, (uint8_t)atoi(fr)) : (int8_t)-1;
    if (ch < 0) { out.println(F("ERR PRESS <string 1-6> <fret 1-5>")); return; }
    uint32_t now = millis();
    if (!channels.tryPress((uint8_t)ch, now, now, agap::MAX_COILS)) {
      out.println(F("ERR already 6 coils on - RELEASE one first"));
      return;
    }
    pushMasks();
    out.print(F("PRESSED channel ")); out.println(ch);
  } else if (eq(cmd, "RELEASE")) {
    char* a = strtok(NULL, " \t");
    if (a && eq(a, "ALL")) { releaseAll(); out.println(F("RELEASED ALL")); return; }
    int8_t s = stringArg(a);
    char* fr = strtok(NULL, " \t");
    int8_t ch = (s >= 0 && fr) ? agap::fretChannel((uint8_t)s, (uint8_t)atoi(fr)) : (int8_t)-1;
    if (ch < 0) { out.println(F("ERR RELEASE ALL | RELEASE <string> <fret>")); return; }
    channels.release((uint8_t)ch);
    pushMasks();
    out.print(F("RELEASED channel ")); out.println(ch);
  } else if (eq(cmd, "STRUM")) {
    char* d = strtok(NULL, " \t");
    bool down = !(d && (d[0] == 'U' || d[0] == 'u'));
    strum.start(down, millis(), STRUM_GAP_MS, curValid ? agap::soundingMask(curFret) : 0x3F);
  } else if (eq(cmd, "PROG")) {
    Step tmp[MAX_PROG];
    uint8_t n = 0;
    for (char* tok = strtok(NULL, " \t"); tok && n < MAX_PROG; tok = strtok(NULL, " \t")) {
      agap::Voicing v;
      if (strlen(tok) >= sizeof(tmp[n].name) || !solve(tok, v)) {
        out.print(F("ERR unknown chord ")); out.println(tok);
        return;
      }
      strcpy(tmp[n].name, tok);
      memcpy(tmp[n].fret, v.fret, 6);
      printVoicing(out, tmp[n].name, v.fret, v.cost);
      n++;
    }
    if (!n) { out.println(F("ERR empty progression")); return; }
    memcpy(prog, tmp, sizeof(Step) * n);
    progLen = n;
    progPos = 0;
  } else if (eq(cmd, "NEXT")) {
    stepProgression(+1);
  } else if (eq(cmd, "PREV")) {
    stepProgression(-1);
  } else if (eq(cmd, "CALIB")) {
    int8_t s = stringArg(strtok(NULL, " \t"));
    char* fr = strtok(NULL, " \t");
    char* a = strtok(NULL, " \t");
    char* b = strtok(NULL, " \t");
    char* c = strtok(NULL, " \t");
    int8_t ch = (s >= 0 && fr) ? agap::fretChannel((uint8_t)s, (uint8_t)atoi(fr)) : (int8_t)-1;
    if (ch < 0 || !a || !b) { out.println(F("ERR CALIB <string 1-6> <fret 1-5> <holdMs> <reps> [gapMs]")); return; }
    uint16_t holdMs = atoi(a);
    uint8_t reps = atoi(b);
    uint16_t gapMs = c ? atoi(c) : 400;
    out.print(F("CALIB channel ")); out.print(ch); out.print(F(" x")); out.println(reps);
    abortRequested = false;
    for (uint8_t r = 0; r < reps; r++) {
      uint32_t now = millis();
      channels.tryPress((uint8_t)ch, now, now, agap::MAX_COILS);
      pushMasks();
      for (uint32_t t0 = millis(); millis() - t0 < holdMs;) {
        if (channels.update(millis())) pushMasks();
        if (checkAbort()) { releaseAll(); out.println(F("ABORTED (STOP)")); return; }
      }
      channels.release((uint8_t)ch);
      pushMasks();
      out.print(F("  rep ")); out.print(r + 1); out.println(F(" done"));
      for (uint32_t t1 = millis(); millis() - t1 < gapMs;) {
        if (channels.update(millis())) pushMasks();
        if (checkAbort()) { releaseAll(); out.println(F("ABORTED (STOP)")); return; }
      }
    }
  } else if (eq(cmd, "PICK")) {
    char* n = strtok(NULL, " \t");
    char* side = strtok(NULL, " \t");
    char* ang = strtok(NULL, " \t");
    int8_t s = stringArg(n);
    bool isA = side && (side[0] == 'A' || side[0] == 'a');
    bool isB = side && (side[0] == 'B' || side[0] == 'b');
    if (s < 0 || !(isA || isB) || !ang) { out.println(F("ERR PICK <1-6> <A|B> <angle>")); return; }
    uint8_t angle = agap::clampPickAngle(atoi(ang));
    if (isA) pickA[s] = angle; else pickB[s] = angle;
    if (isA != pickAtB[s]) picks[s].write(angle);
    out.print(F("PICK ")); out.print(atoi(n)); out.print(isA ? F(" A=") : F(" B=")); out.println(angle);
  } else if (eq(cmd, "PLUCK")) {
    int8_t s = stringArg(strtok(NULL, " \t"));
    if (s < 0) { out.println(F("ERR PLUCK <1-6>")); return; }
    pluck((uint8_t)s);
    out.println(F("PLUCKED"));
  } else if (eq(cmd, "PICKS")) {
    for (uint8_t i = 0; i < 6; i++) {
      out.print(F("string ")); out.print(6 - i);
      out.print(F("  A=")); out.print(pickA[i]);
      out.print(F("  B=")); out.println(pickB[i]);
    }
  } else if (eq(cmd, "TEMPO")) {
    char* a = strtok(NULL, " \t");
    bpm = constrain(a ? atoi(a) : bpm, agap::BPM_MIN, agap::BPM_MAX);
    out.print(F("TEMPO ")); out.println(bpm);
  } else if (eq(cmd, "KICK")) {
    char* a = strtok(NULL, " \t");
    kickMs = constrain(a ? atoi(a) : kickMs, agap::KICK_MS_MIN, agap::KICK_MS_MAX);
    channels.setKickMs(kickMs);
    out.print(F("KICK ms ")); out.println(kickMs);
  } else if (eq(cmd, "HOLD")) {
    char* a = strtok(NULL, " \t");
    holdDuty = (uint16_t)constrain(a ? atoi(a) : 30, 0, 100) * 255 / 100;
    OCR2A = holdDuty;
    out.print(F("HOLD duty ")); out.println(holdDuty);
  } else if (eq(cmd, "SAVE")) {
    out.println(writeSavedTuning(currentTuning()) ? F("SAVED - loads automatically at power-up")
                                                   : F("ERR could not verify the EEPROM write"));
  } else if (eq(cmd, "LOAD")) {
    agap::Tuning t;
    if (readSavedTuning(t)) { applyTuning(t); out.println(F("LOADED saved tuning")); }
    else out.println(F("ERR no valid saved tuning (SAVE one first)"));
  } else if (eq(cmd, "DEFAULTS")) {
    applyTuning(compiledDefaults);
    out.println(F("Back to compiled defaults (not saved - send SAVE to keep them)"));
  } else if (eq(cmd, "STOP")) {
    stopAll();
    out.println(F("STOPPED"));
  } else if (eq(cmd, "STATUS")) {
    out.print(F("coils on=")); out.println(channels.activeCount());
    out.print(F("bpm=")); out.print(bpm);
    out.print(F(" kick=")); out.print(kickMs);
    out.print(F(" holdDuty=")); out.print(holdDuty);
    out.print(F(" (")); out.print((uint16_t)holdDuty * 100 / 255); out.println(F("%)"));
    if (curValid) printVoicing(out, "fingering", curFret, -1);
  } else {
    out.println(F("ERR unknown command, type HELP"));
  }
}

// SEQUENCE needs the rest of the line intact, so route it before strtok eats it.
void dispatch(char* line, Stream& out) {
  char tmp[96];
  strncpy(tmp, line, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  char* cmd = strtok(tmp, " \t");
  if (cmd && eq(cmd, "SEQUENCE")) {
    char* sp = strchr(line, ' ');
    handleSequence(sp ? sp + 1 : (char*)"", out);
    return;
  }
  handleCommand(line, out);
}

// ================================ Panel buttons ================================
bool ctrlStable[NUM_CTRL_BTN], ctrlRaw[NUM_CTRL_BTN];
uint32_t ctrlT[NUM_CTRL_BTN];

bool ctrlPressed(uint8_t b, uint32_t now) {
  bool r = digitalRead(CTRL_BTN_PIN[b]) == LOW;
  if (r != ctrlRaw[b]) { ctrlRaw[b] = r; ctrlT[b] = now; }
  if (now - ctrlT[b] > 25 && r != ctrlStable[b]) { ctrlStable[b] = r; return r; }
  return false;
}

void pollButtons(uint32_t now) {
  if (ctrlPressed(BTN_STRUM, now)) {
    strum.start(strumDown, now, STRUM_GAP_MS, curValid ? agap::soundingMask(curFret) : 0x3F);
    strumDown = !strumDown;
  }
  if (ctrlPressed(BTN_NEXT, now)) stepProgression(+1);
  if (ctrlPressed(BTN_PREV, now)) stepProgression(-1);
  if (ctrlPressed(BTN_STOP, now)) stopAll();
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen) dispatch(lineBuf, Serial);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ================================== Main ==================================
void setup() {
  Serial.begin(115200);
  compiledDefaults = currentTuning();
  agap::Tuning saved;
  bool haveSaved = readSavedTuning(saved);
  if (haveSaved) applyTuning(saved);  // before driverBegin()/servo attach, which use these values
  driverBegin();
  for (uint8_t s = 0; s < 6; s++) { picks[s].attach(SERVO_PIN[s]); picks[s].write(pickA[s]); }
  for (uint8_t b = 0; b < NUM_CTRL_BTN; b++) pinMode(CTRL_BTN_PIN[b], INPUT_PULLUP);

  char demo[] = "PROG C G Am F";
  handleCommand(demo, Serial);

  Serial.println(F("AGAP (fretboard) ready - type HELP"));
  Serial.println(F(FW_NAME " fw " FW_VERSION));
  Serial.println(haveSaved ? F("Loaded saved tuning (EEPROM).") : F("No saved tuning - using compiled defaults."));
  Serial.println(F("Pins and timings are PLACEHOLDERS until measured on the real build."));
}

void loop() {
  uint32_t now = millis();
  pollSerial();
  pollButtons(now);
  updateStrum(now);
  if (channels.update(now)) pushMasks();
}
