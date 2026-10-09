# Tuning guide (do this on the real build)

None of the values in the firmware have been measured: they are placeholders
(`README_AGAP.md` steps 2, 3, 5). This is the order to set them in. The
rules of thumb below are mine, not from a datasheet. The advertised solenoid
figure (0.2 N initial force) says nothing about force through the stroke or
about holding, so **watch the coil temperature at every step and stop if it
gets hot.**

Rules for the whole process:

- Change **one** value at a time, then re-test.
- Write each attempt in `docs/measurement_log_template.csv`, failures too.
- Nothing survives a power cycle until you send `SAVE`.
- `STOP` (serial or the panel button) works at any time, even mid-test.

| Value | Command | Placeholder | Saved by `SAVE` |
|---|---|---|---|
| Kick pulse (ms of full power) | `KICK <ms>` (10-300) | 60 | yes |
| Hold power (percent) | `HOLD <percent>` (10-100) | 60/255, about 23% | yes |
| Tempo | `TEMPO <bpm>` (20-200) | 50 | yes |
| Pick arm end angles | `PICK <1-6> <A\|B> <deg>` (10-170) | 70 / 110 | yes |
| Settle delay before strum | `SETTLE_MS` in the sketch | 15 ms | no, edit and re-flash |
| Gap between strings | `STRUM_GAP_MS` in the sketch | 18 ms | no, edit and re-flash |
| Auto-release timeout | `CMD_TIMEOUT_MS` in the sketch | 8000 ms | no, edit and re-flash |

## 1. Pick arms first (no solenoid power needed)

1. `PICKS` to see the current angles. String numbers: 1 = high e ... 6 = low E.
   The six pods are independent and sit in two rows of three, so which pod serves
   which string depends on how you wired D2-D7. `PLUCK 6` shows which pod is
   the low-E one; write the pod-to-string order down and swap signal wires if
   it is wrong. (Wire order is fixed in the firmware, so it is a wiring fix.)
2. `PLUCK 6` swings the low-E pick to its other side; repeat to swing back.
3. `PICK 6 A 70` and `PICK 6 B 110` move each end. Adjust until the pick
   crosses its own string cleanly on both swings and clears the next string.
4. Repeat for strings 5 to 1. Check the sound with the strings tuned.
5. `SAVE`.

The 10-170 degree limit only stops typos. It is not the arm's measured
travel, so watch for an arm hitting its stop or the neighbouring string.

## 2. Kick: the smallest pulse that seats the button reliably

1. Mount one solenoid in a socket (fretboard: `AGAP_Fretboard/USER_MANUAL.md`, Steps 1 and 5).
2. `KICK 40`, then `CALIB 6 1 1000 5 800` on the fretboard (string 6, fret 1: press, hold 1 s, release,
   5 times). On the earlier helper build the same test is `CALIB EM 1000 5 800`.
3. Look and listen: does the plunger press the string firmly every time (fretboard), or the helper
   button reach the bottom (helper build), with a clean note or chord when strummed? Raise `KICK` in steps of 10 ms until all 5
   presses seat. Then add about 20% margin and test again.
4. Longer kicks only add heat, so do not go higher than needed.

## 3. Hold: the lowest power that keeps it pressed

1. With the kick set, run `CALIB 6 1 3000 3 1500` at `HOLD 40` (helper build: `CALIB EM 3000 3 1500`).
2. If the button creeps back up during the hold, raise `HOLD` by 5 percent.
   Once it holds steadily, add about 5-10 percent margin.
3. Run a few holds of the longest length you will use, then **feel the coil and
   driver**. Warm is expected; if it is too hot to keep a finger on, lower
   `HOLD` or shorten the hold.
4. `SAVE`.

## 4. Strum timing

1. `CHORD Em` (helper build: `CHORD EM`). If the chord sounds muted or buzzy only on the first strum,
   the strum is starting before the button has seated: raise `SETTLE_MS`.
2. If strings sound uneven, adjust `STRUM_GAP_MS`. Re-flash after each change.

## 5. Tempo and sequences

`TEMPO 50`, then `SEQUENCE C G AM F`. Raise the tempo until chord changes
start to sound late or muted, then back off. With the placeholder timings one
chord takes at least about 0.2 s (kick 60 + settle 15 + five 18 ms strum gaps),
and the previous button also has to release, so there is a floor on tempo that
only the real build can show.

## Saving and recovering

- `SAVE` writes everything in the first table marked "yes" to EEPROM and reads it
  back to confirm. Next power-up prints "Loaded saved tuning".
- `LOAD` re-reads it; `DEFAULTS` returns to the compiled values without
  touching what is saved.
- Blank or corrupted EEPROM is detected (magic number, version, checksum) and
  ignored, and the firmware falls back to the compiled defaults. Out-of-range
  stored values are clamped on load.
- A normal upload does not erase EEPROM, so tuning should survive re-flashing.
  Check with `LOAD` afterwards, and keep a copy of your final numbers in the
  log in case it does not.
