# AGAP Fretboard — step-by-step manual

From printed parts to a chord. Steps that need something I could not measure are
marked **TBD**. Nothing here has run on real hardware yet, so expect to adjust.

> **Easiest path:** `python agap.py` in the repo root gives a menu that flashes the
> firmware, checks the connection (read-only) and walks you through the first
> power-up one solenoid at a time, saving what you confirm. The steps below are
> the same procedure by hand, plus the wiring.

## What you need

- Arduino **Mega 2560** and a USB data cable (a PC is needed to flash it, not to
  run it afterwards).
- **30 solenoids** that fit the sockets (**TBD**: model not chosen; see Step 1),
  with **30 drivers** (logic-level MOSFET or relay + flyback diode each).
- A 12 V supply sized for **6 coils at kick current** (not 30) and a separate
  5-6 V supply for the 6 servos; all grounds joined.
- 6 hobby servos for the AutoStrummer pods; 4 panel buttons (STRUM, NEXT, PREV, STOP).
- The printed set: coupon, 2 rails, 2 upper clamps, 2 lower jaws, 5 fret plates,
  1 mm and 2 mm shims (`../docs/HARDWARE_SPECS.md`).

## Step 1 — print the coupon first, and check the solenoid fits

Print `00-PRINT-FIRST-solenoid-fit-coupon` alone. It is one socket. The solenoid
body must go in a **7.5 x 10.5 mm** pocket about **12 mm deep**, with its plunger
free to pass the 4.8 mm hole in the floor. If it does not fit, stop here: change
the solenoid or the design before printing five plates. (Printed pockets come out
a little tight; allow for it.)

## Step 2 — print and assemble the mechanics (**TBD**)

Print the rails, clamps, jaws, five plates and shims. Two print files are ready: the clamp
bed holds 2 clamps, 2 jaws and 22 shims; the plate bed (named `04-fret-1...`) holds **all five
plates**, takes about 5.5 hours in ABS at 0.2 mm, and is sliced **with supports and a raft**, so
cut the support out from under each plate's two mounting ears and out of the ear slots. **Mark the two upper clamps: the
body-end and nut-end ones are not interchangeable** (their walls are 44.5 mm and
39.0 mm apart). The thin shims print 0.9 mm and the thick 1.95 mm, a little under
their 1 and 2 mm names, so measure them. The files do not say how
they join, so assemble from your own drawing. Use the shims to set the plunger's
distance from the string. Check for yourself that each socket sits over its
string; the sockets are 7.1 mm apart on plate 1 and 7.9 mm on plate 5.

## Step 3 — flash the firmware, nothing else connected

```bash
python agap.py flash          # or open AGAP_Fretboard.ino in the Arduino IDE
```

Board: *Arduino Mega or Mega 2560*. Then `python agap.py doctor`. It should report
the AGAP fretboard firmware, **30 channels**, and the on-board chord search giving
`x 2 0 4 0 2` for Bm (frets 0-5; the older three-fret search gives `x 2 0 x 0 2`). The doctor never moves anything.

## Step 4 — wire the drivers (power off)

Connect each plate's six drivers to its six consecutive channels
(pin table in [`README.md`](README.md)). Check polarity and continuity with a
multimeter before applying power; a reversed flyback diode fails at the first
switch-off. Start with **one** solenoid, not thirty.

## Step 5 — power up one solenoid at a time

```bash
python agap.py bringup
```

It asks for a safety confirmation, then tests the six pick arms, then **one**
solenoid (string 6 = low E, fret 1) and asks whether it clicked. A failure sends
STOP and tells you which pin to check. Only after one works, test the rest, plate
by plate. It never energises two coils at once. Results go to `logs/`.

If `PRESS 6 1` moves the high-E socket instead, the plate is mounted the other
way round: swap the wiring, not the firmware.

## Step 6 — tune

Follow [`../docs/TUNING_GUIDE.md`](../docs/TUNING_GUIDE.md): pick arms first, then
`KICK`, then `HOLD`, then settle and tempo; `SAVE` keeps the values. In the
fretboard firmware a single solenoid is `CALIB <string> <fret> <holdMs> <reps>`
(for example `CALIB 6 1 1000 5`).

## Step 7 — play

```text
python agap.py console
agap> show Bm            # fingering only
agap> chord Em           # press and strum
agap> seq C G Am F bpm 60
agap> stop
```

Or press the panel buttons: STRUM, NEXT and PREV step through the progression
(default `C G Am F`; change it with `PROG`), STOP releases everything.

## If something goes wrong

| Symptom | Likely cause |
|---|---|
| `doctor` says channels are not 30 | An old build is flashed; run `python agap.py flash` |
| A solenoid does not move on `PRESS` | 12 V off, driver input not on that pin, flyback diode reversed, no common ground |
| Wrong string moves | Plate mounted the other way round, or wires in the wrong order |
| A chord sounds muted | Raise `SETTLE_MS` in the sketch (strum starts before the coils seat), or `KICK` |
| A coil gets too hot | Lower `HOLD`, shorten the hold, check the supply and driver |
| `ERR already 6 coils on` | `RELEASE ALL` first; six at once is the limit by design |
| Anything else | Send the latest file in `logs/` |
