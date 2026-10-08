# AGAP — Overview and System Flow

## In simple words

AGAP (Automated Guitar Player) is a robot that plays a real guitar, so
someone who can't press the strings (an injured hand, a weak grip) can still
make music by pressing a button.

Playing a chord takes two things:

- **The left hand presses the strings** into a chord shape. This is the hard
  part, and it's what the robot replaces.
- **The right hand strums.**

The robot does it like this:

1. You pick a chord, with a panel button or from a phone or computer.
2. A small computer board (an **Arduino Mega**) receives the choice.
3. Small electromagnets (**solenoids**), held in the printed **LH base**, push a button on a **chord helper**,
   a purchased device clamped to the guitar neck that presses the strings
   into a chord shape.
4. Six small motors (**servos**) strum the strings, so the guitar makes a
   real chord, not a synthesized one.

The board can't power the electromagnets itself. It switches driver circuits
(a MOSFET or relay plus a flyback diode per channel), and those drive the
coils from a separate 12 V supply.

## System flow

```
ChordAI (optional, standalone)     "Bm" -> easiest shape x 2 0 x 0 2
        |
You choose a chord ── panel button ──────────────────────┐
        |                                                |
 phone / browser                                         |
        | HTTP (token)                                   |
   Remote bridge  (bridge/)                              |
        | text commands                                  |
   PC control tool (tools/)  ── USB serial, 115200 ──────┤
                                                         v
                                              Arduino Mega firmware
                                          (AGAP_HelperButton/*.ino)
                                                         |
                          kick (full power) then hold (low power) PWM
                                                         v
                                   driver circuits -> 12 V solenoids
                                                         v
                                  solenoid pushes a button on the chord helper
                                                         v
                                  helper presses the strings into the chord
                                                         v
                                  6 servos strum the strings (down / up)
```

### Step by step

1. **Choosing a chord.** Optionally, [ChordAI](../ChordAI/) simplifies a hard
   chord (it also runs as a [web page](https://winpyishanhtat.github.io/AGAP-3.0/)).
   It needs no robot.
2. **Sending commands.** The control tool (C++ or Python) reads
   `AGAP_HelperButton/button_map.json`, turns a name like `Em` into the
   helper's label `EM`, and sends a line like `CHORD EM` over USB.
   The remote bridge does the same for requests from a phone, behind a token.
3. **The firmware** (`loop()`) reads serial lines and the four panel buttons
   (STRUM, NEXT, PREV, STOP on A0-A3). The state machine in `agap_logic.h`
   decides what happens next. With only the panel buttons, steps 1-2 are
   skipped and no PC is needed.
4. **Pressing.** Each channel goes off -> pending -> kick (full power for
   `kickMs`) -> hold (about 60/255 duty). The kick seats the button; the
   lower hold power keeps it down without overheating the coil. A Timer2
   interrupt (about 7.8 kHz) applies the masks to pins D22-D29, D37, D36.
5. **Strumming.** After a short settle delay the six servos (D2-D7) pluck the
   strings one at a time, 18 ms apart, alternating down and up.
6. **Safety.** `STOP` works from the serial line or the panel button, even
   in the middle of `SEQUENCE` or `CALIB`. Any channel held longer than
   8 seconds without a refresh releases itself. The bridge sends `STOP`
   when it exits.

## The "AI" part

Some chords (F, B minor) are hard because one finger must press several
strings at once, a barre. ChordAI searches the possible finger positions,
scores each one (fingers needed, notes missing, muted strings, whether the
bass note is the root), and keeps the easiest shape that still sounds like
the chord. B minor becomes `x 2 0 x 0 2`.

## What is in the repo

| Folder | What it is |
|---|---|
| `AGAP_HelperButton/` | Current firmware, wiring/user manual, 74 logic tests |
| `AGAP_Mega/` | Earlier design: 18 solenoids pressing strings directly. Kept for reference |
| `tools/` | PC control tool in C++ and Python, 49 logic tests for the C++ one; `hvs_inspect.py` measures printed parts; `lh_fit.py` checks solenoid fit in the LH base |
| `bridge/` | Remote-control bridge and phone page, 28 tests |
| `ChordAI/` | Chord simplifier (Python, plus the browser version in `web/`) |
| `.github/workflows/` | CI that runs every test and compiles both sketches; Pages deploy |
| `README_AGAP.md` | Design status, physical constraints, and the bench steps still to do |

## Status

- **Done:** all the software. It compiles and passes its automated checks
  in CI.
- **Not done:** everything that touches real hardware. Nothing has run on a
  real board or real solenoids. The pin order, button labels and timings are
  placeholders until the bench measurements in `README_AGAP.md` are taken.
