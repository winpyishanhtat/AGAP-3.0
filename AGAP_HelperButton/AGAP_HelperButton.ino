/*
  AGAP - Helper-Button Architecture  (see README_AGAP.md in this repo)
  Target board: Arduino Mega 2560

  This is the CURRENT-direction firmware. It replaces the earlier
  18-solenoid direct-fretting design (see ../AGAP_Mega) with:

    10 solenoids, one per button on a purchased mechanical chord helper
    (labelled EM AM D C F DM G BM + 2 unidentified positions) press the
    helper's buttons; the helper itself frets the strings. 6 servos
    (the AutoStrummer: six independent servo pods on the original clamp)
    strum all six strings together.

  ---------------------------------------------------------------------
  EVERY numeric value below marked "TBD" is a placeholder. README_AGAP.md
  section 9 lists the physical measurements this firmware is waiting on:
    step 1 - button layout / positions -> confirms which buttons exist
    step 2 - solenoid body/shaft/current -> confirms driver sizing
    step 3 - manual chord test -> confirms press travel & hold force
    step 5 - one powered channel -> confirms press/hold/release timing
  Nothing here has been run against real hardware. Treat every pin
  assignment and timing constant as a starting point to edit in
  button_map.json (loaded by the Python side) and in the TIMING block
  below, not as a measured or verified value.
  ---------------------------------------------------------------------

  Safety (README_AGAP.md section 7, restated):
    - Never power a solenoid coil directly from a GPIO pin. Each channel
      needs its own suitably rated driver (MOSFET or relay) plus flyback
      suppression across the coil.
    - Confirm coil current, polarity and duty rating before raising
      HOLD_DUTY or KICK_MS.
    - STOP always drops every output, regardless of mode or timers.
    - Any channel held longer than CMD_TIMEOUT_MS with no refresh
      command is released automatically (busy controller, dropped
      serial link, forgotten chord).

  ------------------------------ Pin map (TBD) ------------------------------
  Button driver channels (index -> Mega pin), indices 0-7 on PORTA,
  8-9 on PORTC bits 0-1, same bit-banged Timer2 kick-and-hold scheme as
  the earlier sketch:
    0 EM  D22   1 AM  D23   2 D   D24   3 C   D25   4 F   D26
    5 DM  D27   6 G   D28   7 BM  D29   8 X1  D37   9 X2  D36
  Labels and this pin order are a GUESS from the photographed layout in
  README_AGAP.md section 2. Re-confirm against the real chord chart
  before wiring (step 1). Channels 0-9 are fixed to D22-D29/D37/D36 by
  the direct PORTA/PORTC register writes in agap_logic.h - there's no
  per-channel pin variable to edit. To remap which label means which
  physical button, either wire that button to the pin matching its
  intended channel index (the list above), or reorder BUTTON_LABEL[]
  below to match however you actually wired it - whichever is easier
  given your helper and enclosure.

  Servos (AutoStrummer pods, 6th..1st string): D2 D3 D4 D5 D6 D7
    The pods sit in two rows of three, so which pod serves which string depends
    on how the signal wires go to D2-D7. Find out with PLUCK <1-6> and swap
    wires if the order is wrong (see docs/TUNING_GUIDE.md).
  Buttons (to GND, internal pull-up): A0 STRUM  A1 NEXT  A2 PREV  A3 STOP

  Serial: USB 115200. One port only for now; the Python sequencer
  (../tools/agap_control.py) talks to this port.

  The channel state machine, mask computation and strum sequencing below
  are implemented in agap_logic.h, which has no Arduino/AVR dependency and
  is covered by tests/test_agap_logic.cpp (run with tests/run_tests.sh on
  any machine with g++ - no board needed). This file wires that tested
  logic to real hardware (Serial, millis(), the Timer2 ISR, Servo) and
  should stay thin - if you're changing *what* the state machine does
  rather than *how it reaches real pins*, change agap_logic.h and update
  its tests, not here.
*/

#include <EEPROM.h>
#include <Servo.h>
#include <util/atomic.h>
#include <string.h>
#include <stdlib.h>

#include "agap_logic.h"

// Reported by VERSION so a PC tool can tell this sketch from any other one
// on the board and see which build is flashed. Bump when behaviour changes.
#define FW_NAME "AGAP-helper-button"
#define FW_VERSION "0.4"

// ============================ Button channels ============================
const uint8_t NUM_BUTTONS = 10;

// TBD - confirm against the real device (README_AGAP.md step 1) before wiring.
const char* const BUTTON_LABEL[NUM_BUTTONS] =
  {"EM", "AM", "D", "C", "F", "DM", "G", "BM", "X1", "X2"};

// ================================ Timing (TBD) ================================
// Nothing here is measured. README_AGAP.md section 6: force/travel/hold are
// all still unknown. Start low and raise only after a real press/release
// test (step 5) confirms the button moves cleanly and the coil stays cool.
uint16_t kickMs      = 60;    // TBD: full-power pulse that seats the button
uint8_t  holdDuty     = 60;   // TBD: 0-255 hold power once seated
const uint32_t CMD_TIMEOUT_MS = 8000;  // auto-release a channel/chord held this long
const uint8_t  STAGGER_MS     = 4;     // spread kicks to limit inrush current
const uint8_t  SETTLE_MS      = 15;    // TBD: wait after last kick before strumming
const uint8_t  STRUM_GAP_MS   = 18;    // delay between strings in a strum

const uint8_t SERVO_PIN[6] = {2, 3, 4, 5, 6, 7};  // AutoStrummer pods, string 6 first
uint8_t pickA[6] = {70, 70, 70, 70, 70, 70};       // TBD: calibrate per string
uint8_t pickB[6] = {110, 110, 110, 110, 110, 110};

enum { BTN_STRUM, BTN_NEXT, BTN_PREV, BTN_STOP, NUM_CTRL_BTN };

// Result of feeding one received byte to the line reader (feedLine, below). Declared up
// here because the Arduino builder inserts function prototypes before the first function,
// so any type those prototypes mention must already exist.
enum LineResult : uint8_t { LINE_NONE, LINE_READY, LINE_TOO_LONG };
const uint8_t CTRL_BTN_PIN[NUM_CTRL_BTN] = {A0, A1, A2, A3};

// ========================= Driver (kick-and-hold PWM) =========================
// Same approach as the earlier sketch: a full-power "kick" seats the button,
// then a PWM "hold" (holdDuty/256) keeps it down without overheating the coil.
// 10 channels fit on PORTA (0-7) + PORTC bits 0-1. The state machine and
// mask math live in agap_logic.h (ChannelDriver) - this is just the ISR and
// the glue that pushes its computed masks into the volatiles the ISR reads.
volatile uint8_t onA, onC, kickA, kickC;

ISR(TIMER2_OVF_vect)   { PORTA = onA;   PORTC = onC;   }
ISR(TIMER2_COMPA_vect) { PORTA = kickA; PORTC = kickC; }

agap::ChannelDriver<NUM_BUTTONS> channels(kickMs, CMD_TIMEOUT_MS);

void pushMasks() {
  agap::Masks m = channels.computeMasks();
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { onA = m.a; onC = m.c; kickA = m.ka; kickC = m.kc; }
}

void driverBegin() {
  DDRA = 0xFF; DDRC = 0xFF;
  PORTA = 0;   PORTC = 0;
  TCCR2A = 0;              // normal mode, OC2A/OC2B pins disconnected
  TCCR2B = _BV(CS21);      // clk/8 -> 16 MHz / 8 / 256 = 7.8 kHz
  OCR2A  = holdDuty;
  TIMSK2 = _BV(TOIE2) | _BV(OCIE2A);
}

int8_t findButton(const char* label) {
  return agap::findLabel(BUTTON_LABEL, NUM_BUTTONS, label);
}

void releaseAll() {
  channels.releaseAll();
  pushMasks();
}

void updateChannels(uint32_t now) {
  if (channels.update(now)) pushMasks();
}

// One button, pressed now, with the stagger pattern used for a full chord.
uint16_t pressOne(uint8_t i) {
  uint32_t now = millis();
  channels.press(i, now, now);
  pushMasks();
  return kickMs + SETTLE_MS;
}

// =============================== Strummer ===============================
// The helper frets all six strings at once, so (unlike the old per-string
// solver) a chord here always strums every string in order.
Servo picks[6];
bool  pickAtB[6];

void pluck(uint8_t s) {
  pickAtB[s] = !pickAtB[s];
  picks[s].write(pickAtB[s] ? pickB[s] : pickA[s]);
}

agap::Strummer strum;

void startStrum(bool down, uint32_t startAt, uint8_t gap = STRUM_GAP_MS) {
  strum.start(down, startAt, gap);
}

void updateStrum(uint32_t now) {
  int8_t s = strum.update(now);
  if (s >= 0) pluck((uint8_t)s);
}

// ================================ State ================================
bool strumDown = true;
int8_t curButton = -1;  // index into BUTTON_LABEL of the last pressed chord, -1 = none

void playButton(int8_t i, bool down) {
  if (i < 0) return;
  curButton = i;
  uint16_t settle = pressOne(i);
  startStrum(down, millis() + settle);
}

void stopAll() {
  strum.stop();
  releaseAll();
  curButton = -1;
}

// =========================== Command protocol ===========================
bool eq(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

void printHelp(Stream& o) {
  o.println(F("VERSION | PING            which firmware is flashed / is the board listening"));
  o.println(F("PRESS <label>            seat one button, no strum (wiring/fit checks)"));
  o.println(F("RELEASE <label>|ALL      drop one channel or everything"));
  o.println(F("CHORD <label>            press + strum (down)"));
  o.println(F("STRUM [D|U]              strum the currently held chord"));
  o.println(F("SEQUENCE <l1> <l2> ...   play a progression, one strum each, TEMPO-spaced"));
  o.println(F("CALIB <label> <holdMs> <repeats> [gapMs]   repeated loaded-press test (step 5)"));
  o.println(F("PICK <1-6> <A|B> <angle>  set one pick arm's end angle (1=high e .. 6=low E)"));
  o.println(F("PLUCK <1-6>              swing one pick to its other side (calibration)"));
  o.println(F("PICKS                    list every pick's A/B angles"));
  o.println(F("SAVE | LOAD | DEFAULTS   keep tuning across power-off / reload it / go back to compiled values"));
  o.println(F("TEMPO <bpm 20-200>   KICK <ms 10-300>   HOLD <percent 10-100>   LABELS   STATUS   STOP   (no argument = just report)"));
}

void printLabels(Stream& o) {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) { o.print(BUTTON_LABEL[i]); o.print(' '); }
  o.println();
}

uint16_t bpm = 50;

// ---------------------------- saved tuning ----------------------------
// SAVE stores the tuned values (kick, hold, tempo, pick angles) in EEPROM and
// the next power-up loads them; DEFAULTS goes back to the compiled values.
// The encoding, checksum and range-clamping live in agap_logic.h (tested).
agap::Tuning compiledDefaults;  // snapshot of the values above, taken at boot

agap::Tuning currentTuning() {
  agap::Tuning t;
  t.kickMs = kickMs;
  t.holdDuty = holdDuty;
  t.bpm = bpm;
  for (uint8_t i = 0; i < 6; i++) { t.pickA[i] = pickA[i]; t.pickB[i] = pickB[i]; }
  return t;
}

void applyTuning(const agap::Tuning& t) {
  kickMs = t.kickMs;
  channels.setKickMs(kickMs);  // the driver keeps its own copy of the kick time
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
  for (uint8_t i = 0; i < agap::TUNING_BYTES; i++) EEPROM.update(i, buf[i]);  // update() skips unchanged bytes (EEPROM wear)
  agap::Tuning check;
  return readSavedTuning(check);  // read it back: confirms the write took
}

char    lineBuf[96];
uint8_t lineLen = 0;
bool    lineOverflow = false;

// Feeds one received byte into lineBuf. LINE_READY: a complete line is in lineBuf
// (NUL-terminated, length lineLen) and the CALLER must take it and set lineLen = 0.
// LINE_TOO_LONG: the line did not fit and has been discarded whole, so a long line is
// refused instead of its first 95 characters being run as a different command.
LineResult feedLine(char c) {
  if (c == '\r') return LINE_NONE;
  if (c == '\n') {
    bool over = lineOverflow;
    lineOverflow = false;
    if (over) { lineLen = 0; return LINE_TOO_LONG; }
    lineBuf[lineLen] = '\0';
    return lineLen ? LINE_READY : LINE_NONE;
  }
  if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
  else lineOverflow = true;
  return LINE_NONE;
}

// Strict number parsing for command arguments (atoi() turned typos into silent settings).
bool num(const char* tok, long& out) { return agap::parseInt(tok, out); }

// An optional numeric argument clamped into [lo, hi]. No argument = report only (value untouched).
// Returns false (after printing the error) when the argument is not a number.
bool optionalNumber(const char* cmd, const char* tok, long lo, long hi, long& value, Stream& out) {
  if (!tok) return true;
  long v;
  if (!num(tok, v)) {
    out.print(F("ERR ")); out.print(cmd); out.println(F(" needs a number"));
    return false;
  }
  value = constrain(v, lo, hi);
  return true;
}

// SEQUENCE and CALIB below block inside their own loop for seconds at a
// time (that's the point - they drive real hardware on a timed schedule).
// While blocked they must still answer STOP, from either the panel button
// or the serial line, or a stuck/misbehaving run has no way to be cut off.
// Anything other than STOP received mid-run is reported and dropped: there
// is no safe way to also honour a second command while one is in flight.
bool abortRequested = false;

bool checkAbort() {
  if (digitalRead(CTRL_BTN_PIN[BTN_STOP]) == LOW) abortRequested = true;
  while (Serial.available()) {
    LineResult r = feedLine((char)Serial.read());
    if (r == LINE_READY) {
      if (eq(lineBuf, "STOP")) abortRequested = true;
      else { Serial.print(F("BUSY, ignored: ")); Serial.println(lineBuf); }
      lineLen = 0;
    } else if (r == LINE_TOO_LONG) {
      Serial.println(F("BUSY, ignored: line too long"));
    }
  }
  return abortRequested;
}

void handleSequence(char* rest, Stream& out) {
  // Check every label before moving anything, so a typo late in the list cannot leave the
  // first buttons pressed with nothing following.
  char check[96];
  strncpy(check, rest, sizeof(check) - 1);
  check[sizeof(check) - 1] = '\0';
  for (char* tok = strtok(check, " 	"); tok; tok = strtok(NULL, " 	")) {
    if (findButton(tok) < 0) { out.print(F("ERR unknown label ")); out.println(tok); return; }
  }
  const uint32_t beat = 60000UL / bpm;
  uint8_t n = 0;
  abortRequested = false;
  for (char* tok = strtok(rest, " \t"); tok; tok = strtok(NULL, " \t")) {
    int8_t i = findButton(tok);
    if (i < 0) { out.print(F("ERR unknown label ")); out.println(tok); return; }
    out.print(F("STEP ")); out.println(BUTTON_LABEL[i]);
    playButton(i, strumDown); strumDown = !strumDown;
    uint32_t stepStart = millis();
    while (strum.active() || millis() - stepStart < beat) {
      updateStrum(millis()); updateChannels(millis());
      if (checkAbort()) { stopAll(); out.println(F("ABORTED (STOP)")); return; }
    }
    n++;
  }
  if (!n) out.println(F("ERR empty sequence"));
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
  } else if (eq(cmd, "LABELS")) {
    printLabels(out);
  } else if (eq(cmd, "PRESS")) {
    char* label = strtok(NULL, " \t");
    int8_t i = label ? findButton(label) : -1;
    if (i < 0) { out.println(F("ERR unknown label, see LABELS")); return; }
    pressOne(i);
    out.print(F("PRESSED ")); out.println(BUTTON_LABEL[i]);
  } else if (eq(cmd, "RELEASE")) {
    char* label = strtok(NULL, " \t");
    if (label && eq(label, "ALL")) { releaseAll(); out.println(F("RELEASED ALL")); return; }
    int8_t i = label ? findButton(label) : -1;
    if (i < 0) { out.println(F("ERR unknown label, see LABELS")); return; }
    channels.release(i);
    pushMasks();
    out.print(F("RELEASED ")); out.println(BUTTON_LABEL[i]);
  } else if (eq(cmd, "CHORD")) {
    char* label = strtok(NULL, " \t");
    int8_t i = label ? findButton(label) : -1;
    if (i < 0) { out.println(F("ERR unknown label, see LABELS")); return; }
    playButton(i, true);
    strumDown = false;
    out.print(F("CHORD ")); out.println(BUTTON_LABEL[i]);
  } else if (eq(cmd, "STRUM")) {
    char* d = strtok(NULL, " \t");
    bool down = !(d && (d[0] == 'U' || d[0] == 'u'));
    startStrum(down, millis());
  } else if (eq(cmd, "CALIB")) {
    char* label = strtok(NULL, " \t");
    char* a = strtok(NULL, " \t");
    char* b = strtok(NULL, " \t");
    char* c = strtok(NULL, " \t");
    int8_t i = label ? findButton(label) : -1;
    // Limits, not clamps: a typo must not quietly become a different, possibly long, test.
    // The hold stays below CMD_TIMEOUT_MS so the safety timeout cannot cut a hold short.
    long holdMs = 0, reps = 0, gapMs = 400;
    bool ok = i >= 0 && num(a, holdMs) && num(b, reps) && (!c || num(c, gapMs));
    ok = ok && holdMs >= 10 && holdMs <= 7000 && reps >= 1 && reps <= 100 && gapMs >= 0 && gapMs <= 10000;
    if (!ok) { out.println(F("ERR CALIB <label> <holdMs 10-7000> <repeats 1-100> [gapMs 0-10000]")); return; }
    out.print(F("CALIB ")); out.print(BUTTON_LABEL[i]);
    out.print(F(" x")); out.println(reps);
    abortRequested = false;
    for (uint8_t r = 0; r < (uint8_t)reps; r++) {
      pressOne(i);
      uint32_t t0 = millis();
      while (millis() - t0 < (uint32_t)holdMs) {
        updateChannels(millis());
        if (checkAbort()) { releaseAll(); out.println(F("ABORTED (STOP)")); return; }
      }
      channels.release(i);
      pushMasks();
      out.print(F("  rep ")); out.print(r + 1); out.println(F(" done"));
      uint32_t t1 = millis();
      while (millis() - t1 < (uint32_t)gapMs) {
        updateChannels(millis());
        if (checkAbort()) { releaseAll(); out.println(F("ABORTED (STOP)")); return; }
      }
    }
  } else if (eq(cmd, "PICK")) {
    // The pick arms are adjustable prints, so A/B angles are tuned on the
    // real build. Angles are clamped to PICK_ANGLE_MIN..MAX (placeholders).
    char* n = strtok(NULL, " 	");
    char* side = strtok(NULL, " 	");
    char* ang = strtok(NULL, " 	");
    long sn = 0, deg = 0;
    int8_t s = num(n, sn) ? agap::stringToServoIndex((int)sn) : (int8_t)-1;
    bool isA = side && (side[0] == 'A' || side[0] == 'a') && side[1] == '\0';
    bool isB = side && (side[0] == 'B' || side[0] == 'b') && side[1] == '\0';
    if (s < 0 || !(isA || isB) || !num(ang, deg)) { out.println(F("ERR PICK <1-6> <A|B> <angle>")); return; }
    uint8_t angle = agap::clampPickAngle((int)deg);
    if (isA) pickA[s] = angle; else pickB[s] = angle;
    if (isA != pickAtB[s]) picks[s].write(angle);  // arm is on that side now: move it so you can see the change
    out.print(F("PICK ")); out.print(6 - s); out.print(isA ? F(" A=") : F(" B=")); out.println(angle);
  } else if (eq(cmd, "PLUCK")) {
    char* n = strtok(NULL, " 	");
    long sn = 0;
    int8_t s = num(n, sn) ? agap::stringToServoIndex((int)sn) : (int8_t)-1;
    if (s < 0) { out.println(F("ERR PLUCK <1-6>")); return; }
    pluck((uint8_t)s);
    out.print(F("PLUCKED ")); out.println(6 - s);
  } else if (eq(cmd, "PICKS")) {
    for (uint8_t i = 0; i < 6; i++) {
      out.print(F("string ")); out.print(6 - i);
      out.print(F("  A=")); out.print(pickA[i]);
      out.print(F("  B=")); out.println(pickB[i]);
    }
  } else if (eq(cmd, "TEMPO")) {
    long v = bpm;
    if (!optionalNumber("TEMPO", strtok(NULL, " \t"), agap::BPM_MIN, agap::BPM_MAX, v, out)) return;
    bpm = (uint16_t)v;
    out.print(F("TEMPO ")); out.println(bpm);
  } else if (eq(cmd, "KICK")) {
    long v = kickMs;
    if (!optionalNumber("KICK", strtok(NULL, " \t"), agap::KICK_MS_MIN, agap::KICK_MS_MAX, v, out)) return;
    kickMs = (uint16_t)v;
    channels.setKickMs(kickMs);
    out.print(F("KICK ms ")); out.println(kickMs);
  } else if (eq(cmd, "HOLD")) {
    // Percent, 10-100: below 10 the plunger would let go right after the kick. No argument just reports.
    long pct = (long)holdDuty * 100 / 255;
    if (!optionalNumber("HOLD", strtok(NULL, " \t"), 10, 100, pct, out)) return;
    if (pct != (long)holdDuty * 100 / 255) holdDuty = (uint8_t)(pct * 255 / 100);  // unchanged if only reporting
    OCR2A = holdDuty;
    out.print(F("HOLD duty ")); out.println(holdDuty);
  } else if (eq(cmd, "SAVE")) {
    if (writeSavedTuning(currentTuning())) {
      out.println(F("SAVED - loads automatically at power-up"));
    } else {
      out.println(F("ERR could not verify the EEPROM write"));
    }
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
    out.print(F("button ")); out.println(curButton >= 0 ? BUTTON_LABEL[curButton] : "none");
    out.print(F("bpm=")); out.print(bpm);
    out.print(F(" kick=")); out.print(kickMs);
    out.print(F(" holdDuty=")); out.print(holdDuty);
    out.print(F(" (")); out.print((uint16_t)holdDuty * 100 / 255); out.println(F("%)"));
  } else {
    out.println(F("ERR unknown command, type HELP"));
  }
}

// SEQUENCE needs the rest of the line intact (strtok inside handleSequence),
// so split it out here before the generic strtok-based dispatch above eats it.
void dispatch(char* line, Stream& out) {
  char tmp[96];
  strncpy(tmp, line, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  char* cmd = strtok(tmp, " \t");
  if (cmd && eq(cmd, "SEQUENCE")) {
    char* sp = strchr(line, ' ');  // rest of the ORIGINAL line, after "SEQUENCE "
    handleSequence(sp ? sp + 1 : (char*)"", out);
    return;
  }
  handleCommand(line, out);
}

// ================================ Buttons ================================
bool     ctrlStable[NUM_CTRL_BTN], ctrlRaw[NUM_CTRL_BTN];
uint32_t ctrlT[NUM_CTRL_BTN];

bool ctrlPressed(uint8_t b, uint32_t now) {
  bool r = digitalRead(CTRL_BTN_PIN[b]) == LOW;
  if (r != ctrlRaw[b]) { ctrlRaw[b] = r; ctrlT[b] = now; }
  if (now - ctrlT[b] > 25 && r != ctrlStable[b]) { ctrlStable[b] = r; return r; }
  return false;
}

void pollButtons(uint32_t now) {
  if (ctrlPressed(BTN_STRUM, now)) { startStrum(strumDown, now); strumDown = !strumDown; }
  if (ctrlPressed(BTN_NEXT, now) && NUM_BUTTONS) playButton((curButton + 1) % NUM_BUTTONS, true);
  if (ctrlPressed(BTN_PREV, now) && NUM_BUTTONS) playButton((curButton + NUM_BUTTONS - 1) % NUM_BUTTONS, true);
  if (ctrlPressed(BTN_STOP, now)) stopAll();
}

// =========================== Serial line reader ===========================
// (declared earlier than this section - checkAbort() above reads them too)

void pollSerial() {
  while (Serial.available()) {
    LineResult r = feedLine((char)Serial.read());
    if (r == LINE_TOO_LONG) {
      Serial.println(F("ERR line too long"));
    } else if (r == LINE_READY) {
      // Copy the command out and free lineBuf BEFORE running it. SEQUENCE and CALIB read
      // the serial port while they run (to catch STOP) and use lineBuf to do it; if the
      // command were still sitting in it, the incoming STOP would be glued onto the end of
      // it and never recognised, and it would corrupt the command being executed.
      char cmd[sizeof(lineBuf)];
      memcpy(cmd, lineBuf, lineLen + 1);
      lineLen = 0;
      dispatch(cmd, Serial);
    }
  }
}

// ============================== Main ==============================
void setup() {
  Serial.begin(115200);
  compiledDefaults = currentTuning();
  agap::Tuning saved;
  bool haveSaved = readSavedTuning(saved);
  if (haveSaved) applyTuning(saved);  // before driverBegin()/servo attach, which use these values
  driverBegin();
  for (uint8_t s = 0; s < 6; s++) { picks[s].attach(SERVO_PIN[s]); picks[s].write(pickA[s]); }
  for (uint8_t b = 0; b < NUM_CTRL_BTN; b++) pinMode(CTRL_BTN_PIN[b], INPUT_PULLUP);

  Serial.println(F("AGAP (helper-button) ready - type HELP"));
  Serial.println(F(FW_NAME " fw " FW_VERSION));
  Serial.println(haveSaved ? F("Loaded saved tuning (EEPROM).") : F("No saved tuning - using compiled defaults."));
  Serial.println(F("All button pins, labels and timings below are PLACEHOLDERS."));
  Serial.println(F("Confirm them against README_AGAP.md steps 1, 2 and 5 before trusting any press."));
  printLabels(Serial);
}

void loop() {
  uint32_t now = millis();
  pollSerial();
  pollButtons(now);
  updateStrum(now);
  updateChannels(now);
}
