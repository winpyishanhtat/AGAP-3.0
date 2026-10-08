/*
  AGAP - Helper-Button Architecture  (see README_AGAP.md in this repo)
  Target board: Arduino Mega 2560

  This is the CURRENT-direction firmware. It replaces the earlier
  18-solenoid direct-fretting design (see ../AGAP_Mega) with:

    10 solenoids, one per button on a purchased mechanical chord helper
    (labelled EM AM D C F DM G BM + 2 unidentified positions) press the
    helper's buttons; the helper itself frets the strings. 6 servos
    (reused from the earlier design) strum all six strings together.

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

  Servos (reused strummer, 6th..1st string): D2 D3 D4 D5 D6 D7
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

#include <Servo.h>
#include <util/atomic.h>
#include <string.h>
#include <stdlib.h>

#include "agap_logic.h"

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

const uint8_t SERVO_PIN[6] = {2, 3, 4, 5, 6, 7};  // reused strummer, unchanged
uint8_t pickA[6] = {70, 70, 70, 70, 70, 70};       // TBD: calibrate per string
uint8_t pickB[6] = {110, 110, 110, 110, 110, 110};

enum { BTN_STRUM, BTN_NEXT, BTN_PREV, BTN_STOP, NUM_CTRL_BTN };
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
  o.println(F("PRESS <label>            seat one button, no strum (wiring/fit checks)"));
  o.println(F("RELEASE <label>|ALL      drop one channel or everything"));
  o.println(F("CHORD <label>            press + strum (down)"));
  o.println(F("STRUM [D|U]              strum the currently held chord"));
  o.println(F("SEQUENCE <l1> <l2> ...   play a progression, one strum each, TEMPO-spaced"));
  o.println(F("CALIB <label> <holdMs> <repeats> [gapMs]   repeated loaded-press test (step 5)"));
  o.println(F("TEMPO <bpm>   KICK <ms>   HOLD <percent>   LABELS   STATUS   STOP"));
}

void printLabels(Stream& o) {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) { o.print(BUTTON_LABEL[i]); o.print(' '); }
  o.println();
}

uint16_t bpm = 50;

char    lineBuf[96];
uint8_t lineLen = 0;

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

void handleSequence(char* rest, Stream& out) {
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
    if (i < 0 || !a || !b) { out.println(F("ERR CALIB <label> <holdMs> <repeats> [gapMs]")); return; }
    uint16_t holdMs = atoi(a);
    uint8_t  reps   = atoi(b);
    uint16_t gapMs  = c ? atoi(c) : 400;
    out.print(F("CALIB ")); out.print(BUTTON_LABEL[i]);
    out.print(F(" x")); out.println(reps);
    abortRequested = false;
    for (uint8_t r = 0; r < reps; r++) {
      pressOne(i);
      uint32_t t0 = millis();
      while (millis() - t0 < holdMs) {
        updateChannels(millis());
        if (checkAbort()) { releaseAll(); out.println(F("ABORTED (STOP)")); return; }
      }
      channels.release(i);
      pushMasks();
      out.print(F("  rep ")); out.print(r + 1); out.println(F(" done"));
      uint32_t t1 = millis();
      while (millis() - t1 < gapMs) {
        updateChannels(millis());
        if (checkAbort()) { releaseAll(); out.println(F("ABORTED (STOP)")); return; }
      }
    }
  } else if (eq(cmd, "TEMPO")) {
    char* a = strtok(NULL, " \t");
    bpm = constrain(a ? atoi(a) : bpm, 20, 200);
    out.print(F("TEMPO ")); out.println(bpm);
  } else if (eq(cmd, "KICK")) {
    char* a = strtok(NULL, " \t");
    kickMs = constrain(a ? atoi(a) : kickMs, 10, 300);
    out.print(F("KICK ms ")); out.println(kickMs);
  } else if (eq(cmd, "HOLD")) {
    char* a = strtok(NULL, " \t");
    holdDuty = (uint16_t)constrain(a ? atoi(a) : 30, 0, 100) * 255 / 100;
    OCR2A = holdDuty;
    out.print(F("HOLD duty ")); out.println(holdDuty);
  } else if (eq(cmd, "STOP")) {
    stopAll();
    out.println(F("STOPPED"));
  } else if (eq(cmd, "STATUS")) {
    out.print(F("button ")); out.println(curButton >= 0 ? BUTTON_LABEL[curButton] : "none");
    out.print(F("bpm=")); out.print(bpm);
    out.print(F(" kick=")); out.print(kickMs);
    out.print(F(" holdDuty=")); out.println(holdDuty);
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

// ============================== Main ==============================
void setup() {
  Serial.begin(115200);
  driverBegin();
  for (uint8_t s = 0; s < 6; s++) { picks[s].attach(SERVO_PIN[s]); picks[s].write(pickA[s]); }
  for (uint8_t b = 0; b < NUM_CTRL_BTN; b++) pinMode(CTRL_BTN_PIN[b], INPUT_PULLUP);

  Serial.println(F("AGAP (helper-button) ready - type HELP"));
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
