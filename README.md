# AGAP — Arduino Guitar Auto Player (Arduino Mega 2560)

A robot that plays a real guitar. **New here? Read [`docs/SYSTEM_FLOW.md`](docs/SYSTEM_FLOW.md)** for a plain-words overview and the system flow. This repo has two hardware architectures plus a standalone software piece:


## Quick start (first time with the real board)

```
pip install -r requirements.txt     # just pyserial
python agap.py                      # menu  (on Windows: double-click agap.bat)
```

| Step | Menu item / command | What it does |
|---|---|---|
| 1 | `python agap.py flash` | Compile and upload the firmware to the Mega |
| 2 | `python agap.py doctor` | Read-only check of the connection. Says what is wrong and how to fix it. Never moves anything |
| 3 | `python agap.py bringup` | Guided first power-up: one pick arm / one solenoid at a time, you confirm each, and results are saved |
| 4 | `python agap.py console` | Type `chord Em`, `seq C G Am F`, `stop`; replies shown live. Ctrl+C sends STOP |
| 5 | `docs/TUNING_GUIDE.md` | Tune kick, hold and the pick angles, then `save` |

Every session is recorded in `logs/` (what was sent, what the board said). If
something misbehaves, that transcript is the first thing to look at.
`python agap.py selftest` runs every test on your computer. Wiring and the
power-up order are in [`AGAP_HelperButton/USER_MANUAL.md`](AGAP_HelperButton/USER_MANUAL.md).

- **[`AGAP_HelperButton/`](AGAP_HelperButton/) — current direction.** A purchased mechanical chord helper frets the strings; 10 solenoids press the helper's own buttons, 6 servos (reused) strum. Driven by Arduino firmware plus a Python sequencing layer ([`tools/agap_control.py`](tools/agap_control.py)). **Step-by-step build/setup guide: [`AGAP_HelperButton/USER_MANUAL.md`](AGAP_HelperButton/USER_MANUAL.md).** See [`README_AGAP.md`](README_AGAP.md) for the full design status, physical constraints and next steps — the firmware/software side of that document's steps 5–7 is implemented here; steps 1–4 (measuring the real helper, the solenoid, and 3D-printing a fit test) are physical work that still needs doing on the bench. Runs standalone on just the Mega once flashed (no PC needed at runtime) — but still needs per-channel solenoid drivers and external power; the Mega's pins can't power the hardware directly. See the manual's first section for the full answer.
- **[`AGAP_Mega/`](AGAP_Mega/) — earlier direction, kept for reference.** 18 solenoids press the strings directly on frets 1–3, with an on-board AI search that swaps barre chords for open-chord voicings. Superseded by the helper-button approach above, but the chord-solving idea and strummer code are reused there.
- **[`ChordAI/`](ChordAI/) — the "AI search engine software", standalone.** A plain-Python port of the barre-to-open chord search, with no robot/hardware needed at all — the original spec's requirement that the chord simplifier work on its own as a practice tool. Also does the reverse: name the chord from a fretted shape, then simplify it. 21 passing tests.
- **[`bridge/`](bridge/) — remote control.** A locked-down HTTP bridge (token, allowlisted actions, STOP always wins) plus a phone page, so the robot can be driven from another device behind a tunnel/VPN. 28 tests; not yet run against a real board.

Both sketches compile clean against `arduino:avr:mega` (verified with `arduino-cli` in this repo).

## Can each feature run on an Arduino?

| Feature (from the project sheet) | Uno | Mega 2560 | How the sketch does it |
|---|---|---|---|
| 18 solenoids (6 strings × frets 1–3) | ✗ only 20 I/O pins in total | ✓ | D22–D29, D30–D37, D48–D49 via MOSFETs |
| 6 servos for plucking/strumming | ✗ not enough pins left | ✓ | D2–D7, `Servo` lib (Timer5) |
| Kick-and-hold PWM on all 18 coils | ✗ 6 PWM pins | ✓ | Timer2 ISR: 100 % kick, then ~30 % hold, at 7.8 kHz |
| AI constraint search + cost function (barre → open) | ⚠ only 2 KB RAM | ✓ | Branch-and-bound DFS, finishes in a few ms |
| Search engine usable without the robot | — | ✓ | `SHOW <chord>` on the board, or [`ChordAI/chord_ai.py`](ChordAI/chord_ai.py) on any PC with just Python |
| Assistive mode (button / screen) | ⚠ | ✓ | A0–A4 buttons, Nextion on Serial2, phone over HC-05 on Serial1 |
| Teaching mode (slow, repeated finger placement) | ⚠ | ✓ | Places one finger per beat, plays an arpeggio, then strums 4 times |
| Real guitar sound, no guitar modification | ✓ | ✓ | Mechanical (cradle), so the board choice doesn't matter |

An Uno would need 2× PCA9685 or shift registers, and it has only one UART, which the PC and Bluetooth would have to share. Use the **Mega 2560**.

## Hardware notes
- **Solenoid drivers:** for each coil, a logic-level MOSFET (IRLZ44N or IRLB8721), a 220 Ω gate resistor, a 10 k pull-down and a fast flyback diode (SS34 or 1N5819) across the coil. Don't use a ULN2803 if a kick draws more than 0.5 A.
- **Power:** use a 12 V supply for the solenoids, sized for 6 kicks at once. Kicks are staggered by 4 ms. Power the servos from a separate 5–6 V, ≥3 A buck converter. Connect all grounds together.
- **Don't use `tone()` or `analogWrite` on pins 9/10.** The solenoid PWM uses Timer2.

## Serial commands (USB 115200, Bluetooth/Nextion 9600)
```
SHOW Bm          -> Bm -> x 2 0 x 0 2   (AI voicing only)
CHORD F#m7       press + strum
RAW x 3 2 0 1 0  explicit frets 6th..1st
PROG C G Am F    load progression
MODE TEACH | MODE ASSIST, NEXT, PREV, STRUM [D|U], STOP, STATUS
TEMPO 40, HOLD 30, KICK 40, PLUCK 1-6 (calibration)
```
