# AGAP Fretboard — the final design (30 solenoids, frets 1-5)

This is the firmware for the final printed design in
[`../docs/HARDWARE_SPECS.md`](../docs/HARDWARE_SPECS.md): five six-socket plates,
one per fret, hold **30 solenoids** — one per string per fret — so the robot
fingers a chord directly. Six servo picks (the AutoStrummer) then sound the
strings. It replaces the earlier 10-button chord-helper build
([`../AGAP_HelperButton`](../AGAP_HelperButton), kept for reference).

```
CHORD Bm  ->  on-board search  ->  x 2 0 4 0 2  ->  press 3 solenoids  ->  strum the 5 strings that ring
```

**New to this build? Start with [`USER_MANUAL.md`](USER_MANUAL.md)** (flash, check,
power-up one solenoid at a time, tune). Or run `python ../agap.py` from the repo
root for a menu that does the same.

## How it works

- **Chord to fingering, on the board.** `CHORD F#m7` runs the same search as
  [`ChordAI`](../ChordAI/) (a C++ port, `agap_chords.h`) restricted to frets 0-5,
  and finds the easiest shape that still sounds the chord. The C++ is checked
  against the Python on every chord (528 solves) in CI.
- **Fingering to solenoids.** Each pressed fret is one channel,
  `channel = (fret - 1) * 6 + string` (string 0 = low E). One plate is six
  consecutive channels, so one cable harness serves one plate. Open and muted
  strings need no solenoid.
- **At most 6 coils at once.** A fingering has one fret per string. The supply
  is sized for six coils, not thirty, and the firmware refuses a seventh (also
  for raw `PRESS`).
- **Kick and hold.** Each coil gets full power for `kickMs` to seat, then a
  lower PWM duty to hold without overheating. A Timer2 interrupt drives all four
  output ports at once.
- **Strum only what rings.** Muted strings are skipped, without spending a gap.
- **Safety.** `STOP` (serial or panel button) works at any time, even during
  `SEQUENCE` or `CALIB`; a coil held 8 s without a refresh releases itself;
  `SEQUENCE` solves every chord before moving anything.

## Commands (USB serial, 115200)

| Command | What it does |
|---|---|
| `CHORD <name>` | Solve, press, strum (`CHORD Em`, `CHORD F#m7`) |
| `SHOW <name>` | Print the fingering only; moves nothing |
| `RAW <6 frets>` | Press an explicit fingering, `x` = muted (`RAW x 3 2 0 1 0`) |
| `PRESS <string 1-6> <fret 1-5>` | Energise one solenoid (bring-up). String 6 = low E |
| `RELEASE <string> <fret>` / `RELEASE ALL` | Let go |
| `STRUM [D\|U]` | Strum the sounding strings of the current fingering |
| `PROG C G Am F`, `NEXT`, `PREV` | A progression (max 12), stepped by the panel buttons |
| `SEQUENCE C G Am F` | Play chords in turn at `TEMPO` |
| `CALIB <string> <fret> <holdMs> <reps> [gapMs]` | Repeated press test for one solenoid |
| `PICK`, `PLUCK`, `PICKS` | Pick-arm tuning ([`../docs/TUNING_GUIDE.md`](../docs/TUNING_GUIDE.md)) |
| `KICK`, `HOLD`, `TEMPO`, `SAVE`, `LOAD`, `DEFAULTS` | Timing and saved tuning (EEPROM) |
| `FRETS`, `STATUS`, `VERSION`, `PING`, `STOP`, `HELP` | Info and the stop |

Panel buttons (to GND): A0 strum, A1 next, A2 previous, A3 STOP. Servos on D2-D7
(string 6 first).

## Wiring: 30 driver channels

Every solenoid needs its own driver (logic-level MOSFET or relay, with a flyback
diode across the coil) and a 12 V supply sized for **six** coils at kick current.
**Never connect a coil to a pin directly.** Pins are placeholders until you
choose the real wiring; change them in the sketch's driver section and
`agap_fret.h` together.

| Plate (fret) | String 6 (low E) | 5 | 4 | 3 | 2 | 1 (high e) |
|---|---|---|---|---|---|---|
| 1 | ch 0: D22 | ch 1: D23 | ch 2: D24 | ch 3: D25 | ch 4: D26 | ch 5: D27 |
| 2 | ch 6: D28 | ch 7: D29 | ch 8: D37 | ch 9: D36 | ch 10: D35 | ch 11: D34 |
| 3 | ch 12: D33 | ch 13: D32 | ch 14: D31 | ch 15: D30 | ch 16: D49 | ch 17: D48 |
| 4 | ch 18: D47 | ch 19: D46 | ch 20: D45 | ch 21: D44 | ch 22: D43 | ch 23: D42 |
| 5 | ch 24: A8 | ch 25: A9 | ch 26: A10 | ch 27: A11 | ch 28: A12 | ch 29: A13 |

Ports: channels 0-7 on PORTA, 8-15 on PORTC, 16-23 on PORTL, 24-29 on PORTK (A8-A13). `python agap.py` checks this table against the firmware.

## Tests

```bash
bash tests/run_tests.sh      # needs g++ and Python
```

703 checks that the C++ chord search matches `ChordAI/chord_ai.py` (every root,
every quality, 3 and 5 frets, plus name parsing), and 256 on the channel mapping,
the six-coil limit, port splitting and muted-string strumming. The shared
`agap_logic.h` is a copy of `../AGAP_HelperButton/agap_logic.h`; a test fails if
the two ever differ.

## Status

Compiles for the Mega 2560 (16 KB flash, 1.5 KB RAM) and passes its tests in CI.
**Not run on a board.** The pin map, kick/hold/settle timings and pick angles are
placeholders, and nothing is known yet about the real solenoid (see the
hardware doc). Search time on the Mega is estimated at about 10 ms for a typical
chord and about 0.1 s for the worst one (`D9`); that is a count-based estimate,
not a measurement.
