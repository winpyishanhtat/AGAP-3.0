# AGAP — Overview and System Flow

## In simple words

AGAP (Automated Guitar Player) is a robot that plays a real guitar, so
someone who can't press the strings (an injured hand, a weak grip) can still
make music by pressing a button.

Playing a chord takes two things:

- **The left hand presses the strings** into a chord shape. This is the hard
  part, and it's what the robot replaces.
- **The right hand strums.**

The final design does it like this:

1. You pick a chord (say `Bm`), with a panel button or from a phone or computer.
2. A small computer board (an **Arduino Mega**) works out the easiest way to
   finger that chord (`x 2 0 4 0 2`), using the same search as the standalone
   Chord AI.
3. **30 small electromagnets (solenoids)** sit in five printed plates, one plate
   per fret (frets 1 to 5), six sockets each, one per string. The board switches
   on just the ones that finger the chord (at most six at once), and they press
   the strings down.
4. **Six small motors (servos)**, one per pod of the **AutoStrummer**, pluck only
   the strings that ring, so the guitar makes a real chord, not a synthesized one.

The board can't power the electromagnets itself. It switches driver circuits
(a MOSFET or relay plus a flyback diode per channel), and those drive the
coils from a separate 12 V supply.

## System flow

```
You choose a chord ── panel button ───────────────────────┐
        |                                                 |
 phone / browser                                          |
        | HTTP (token)                                    |
   Remote bridge  (bridge/)                               |
        | text commands                                   |
   Launcher / control tools  ── USB serial, 115200 ───────┤
                                                          v
                                              Arduino Mega firmware
                                          (AGAP_Fretboard/*.ino)
                                                          |
              "Bm" -> on-board chord search -> x 2 0 4 0 2  (frets 0-5)
                                                          |
                 fingering -> solenoid channels, one per string per fret
                          kick (full power), then hold (low power) PWM
                                                          v
                                    driver circuits -> 12 V solenoids
                                                          v
                      solenoids in the five fret plates press the strings
                                                          v
             6 AutoStrummer servos pluck only the strings that ring
```

### Step by step

1. **Choosing a chord.** From the panel buttons (progression), the console, a
   script, or the phone page. [ChordAI](../ChordAI/) does the same simplification
   on a PC, and also runs as a [web page](https://winpyishanhtat.github.io/AGAP-3.0/).
2. **On the board.** `CHORD Bm` is solved by the search in `agap_chords.h`, a C++
   port of ChordAI checked against the Python on every chord. It only uses frets
   0-5, because the fretboard has solenoids on frets 1-5.
3. **To solenoids.** Each pressed fret is one channel, `(fret - 1) * 6 + string`.
   One plate is six consecutive channels. The kick-and-hold PWM runs on four output
   ports from one timer interrupt.
4. **Strumming.** After a short settle delay the servos pluck the strings that
   ring, 18 ms apart, alternating down and up. Muted strings are skipped.
5. **Safety.** At most 6 coils on at once (enforced); `STOP` from the serial line
   or panel button works at any time, even mid-sequence; a coil held 8 s without a
   refresh releases itself; the remote bridge sends `STOP` when it exits.

## The "AI" part

Some chords (F, B minor) are hard because one finger must press several strings
at once, a barre. ChordAI searches the possible finger positions, scores each one
(fingers needed, notes missing, muted strings, whether the bass note is the root),
and keeps the easiest shape that still sounds like the chord. With 30 solenoids
the robot can finger almost any shape inside frets 1-5, so the search is used to
pick a *good* shape rather than to avoid a hard one.

## What is in the repo

| Folder | What it is |
|---|---|
| `AGAP_Fretboard/` | **Final design firmware**: 30 solenoids, on-board chord search, manual |
| `AGAP_HelperButton/` | Earlier design: 10 solenoids pressing a purchased chord helper (kept for reference) |
| `AGAP_Mega/` | First design: 18 solenoids on frets 1-3 (kept for reference) |
| `tools/` | PC tools: C++ and Python control tools (earlier helper protocol), part measuring (`hvs_inspect.py`, `stl_inspect.py`, `lh_fit.py`) |
| `bridge/` | Remote-control bridge and phone page (see its README for what it supports) |
| `ChordAI/` | Chord simplifier (Python, plus the browser version in `web/`) |
| `agap.py` | Launcher: doctor, bring-up, console, flash, selftest |
| `docs/` | Hardware measurements, tuning guide, test plan and report, this overview |
| `.github/workflows/` | CI that runs every test and compiles the sketches; Pages deploy |
| `README_AGAP.md` | Design status and the bench steps still to do |

## Status

- **Done:** the software for the final design. It compiles for the Mega and passes
  its automated checks in CI, including the C++ chord search against the Python.
- **Not done:** everything that touches real hardware. Nothing has run on a real
  board or real solenoids. The pin order, button/panel wiring and timings are
  placeholders, the solenoid model is not chosen, and how the printed parts join
  is not in the files. The first job is to print the fit coupon.
