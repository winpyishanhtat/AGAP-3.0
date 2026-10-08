# Printed hardware — what the .hvs files tell us

Five `.hvs` files have been supplied (four first, then `LH.hvs` as an updated design version). They are **G-code for the 3DP-210F printer**
(sliced with CubiEngine2, ABS, 0.2 mm layers), one per printed part. They
are print instructions, not drawings, so everything below was **measured
from the toolpaths** with [`tools/hvs_inspect.py`](../tools/hvs_inspect.py).
Treat sizes as +/- 0.5 mm: the toolpath is the wall's centre line, so true
edges are about one extrusion width (0.4 mm) further out.

| File | Part (from the file's own name) | Measured size X x Y x height |
|---|---|---|
| `center.hvs` | `02_central_deck` | 113.6 x 111.6 x 4.6 mm |
| `servomount.hvs` | `03_servo_pod` | 65.6 x 87.6 x 31.6 mm |
| `picks+holeclamps.hvs` | `04_adjustable_pick_arm` (+ hole clamps) | 123.3 x 61.7 x 19.2 mm |
| `twoholeslides.hvs` | `08_Hole Slide 1` | 100.3 x 89.1 x 10.6 mm |
| `LH.hvs` | `08_LH Base` (updated design) | 65.8 x 39.6 x 12.6 mm |

The part numbers are not unique: `08_Hole Slide 1` and `08_LH Base` both carry
08, so the numbering can't be used to count missing parts. Some parts are
still not supplied, since nothing here covers the helper mount.

## What the files establish, and how the software lines up

| Finding | Software |
|---|---|
| **Servo pod has 6 servo bays**, in a 3 x 2 grid: columns 24.0 mm apart (X = 48.8, 72.8, 96.8), rows 47 mm apart (Y = 53.8, 100.8), each bay outline about 17.6 x 40.6 mm at the base | Matches the firmware's 6 servos (D2-D7). No change needed |
| **6 pick arms** printed (about 7.6 x 34.6 mm each), plus 4 hole-clamp pieces | Matches 6 picks, one per string |
| The arm is called **adjustable** | The firmware only had compile-time `pickA`/`pickB` angles, so tuning meant re-flashing. Added runtime calibration: `PICK`, `PLUCK`, `PICKS` (below), with clamped angles |
| Central deck: 12 slots of 6.7 x 3.7 mm (four groups of three), a 72.4 x 24.4 mm central cutout, 4 slots of 3.4 x 7.4 mm, and 8 round holes (4 x 4.8 mm, 4 x 3.7 mm) | Nothing in the software depends on these |

### `LH.hvs` (updated design): the solenoid base

`08_LH Base` holds the solenoids (stated by the project owner; the file itself
does not say what "LH" means). It is a block 65.8 x 39.6 x 12.6 mm that steps
in to 53.6 mm wide after the first few layers. Measured from the toolpaths:

- **Two parallel pockets**, each one single undivided cavity (no ribs or
  cells), measured loop **50.4 x 8.9 mm**, centred 27.5 mm apart (Y = 67.1 and
  94.6). A hole's real void is about one extrusion width (0.4 mm) smaller than
  its centre line, so plan on about **50.0 x 8.5 mm**.
- **Open-topped and about 11.1 mm deep**, on a floor about 1.5 mm thick.
- **A small ramp-shaped cavity** between them, 12.4 mm long at the bottom
  narrowing to under 2 mm near the top.
- **Three 3.4 mm holes** at the top (a size that suits M3 screws).

What the geometry does and does not say:

- It says how much room the solenoids have: two lanes, each at most about
  8.5 mm across and 50 mm long. Any solenoid body wider than about 8 mm in
  the across direction will not fit.
- It does **not** say how many solenoids there are, how they are oriented,
  which one drives which helper button, or how the plunger reaches the button.
  Undivided pockets give no per-solenoid positions. So the firmware's
  **10 channels are unchanged** and still come from `README_AGAP.md`. For
  scale only: 10 solenoids is 5 per pocket, which needs each body to be
  about 9 mm or less along the pocket. Measure yours.
- Use [`tools/lh_fit.py`](../tools/lh_fit.py) with your measured solenoid
  body to see how many fit and in which orientation:

```bash
python tools/lh_fit.py 7 9 10 --need 10     # body dimensions in mm
```

It assumes 0.3 mm clearance per side and 0.5 mm between bodies. Those are
assumptions, not measurements, and it cannot know which side is the plunger
axis.

The base is 65.8 mm wide and the servo pod is 65.6 mm wide, which may mean
they are meant to mate, but the files do not say so.

### New firmware commands (pick calibration)

String numbers are as a player says them: **1 = high e ... 6 = low E**.

```
PICKS                    list every pick's A and B angle
PICK <1-6> <A|B> <deg>   set one end-of-swing angle (moves the arm if it is on that side)
PLUCK <1-6>              swing that pick to its other side
```

Angles are clamped to 10-170 degrees. **Those limits are placeholders** that
only guard against a typo; they are not the measured travel of the printed
arm. Set the real A and B for each string by eye on the assembled build, then
copy them into `pickA[]`/`pickB[]` in `AGAP_HelperButton.ino` so they survive
a power cycle (the commands change RAM only).

## What the files do NOT tell us

Do not assume any of these; each needs a look at the real parts:

- **How many solenoids there are or where each one goes.** The LH base holds
  them, but its pockets are undivided, so the file gives lanes, not positions.
  The deck's 12 slots are not evidence of 12 (or 10) channels. The firmware's
  10 channels still come from `README_AGAP.md`, not from these files.
- **Which servo drives which string.** The bays are a 3 x 2 grid; the files
  don't say the order. Check it when wiring D2-D7 (low E first).
- **The servo model.** A bay outline of about 17.6 x 40.6 mm is not a
  servo name. Check your servo's body size against it before printing more.
- **The pick arm's real travel.** The G-code can't say how far the arm is
  meant to swing.
- **What the "Hole Slide" and "hole clamp" parts are for.** The slide file
  contains two plates of about 70 x 70 mm, each with a slot of about
  20 x 10 mm; the file doesn't say what slides in them.
- **The other parts**: the helper mount, and anything else not supplied.
- **What else the updated design changes** compared with the first four
  files, for example whether the central deck is still used.

## Reproducing the measurements

```bash
python tools/hvs_inspect.py center.hvs --layers 5 --sha
```

SHA-256 of the files as supplied (the files are 2-7 MB each and not
committed):

```
7fd61915b0baea073d63738fb586e12c7fea8baa1426e218b204ccf59f3ed702  center.hvs
9410488d267cd70edcdb0e8a6d4cf5c82edd4ef0cee32a04d47f45d060837008  servomount.hvs
db1d026d1fde3de16d657e52aece25d39b779cc3482646d6cac6e869ed21b56b  picks+holeclamps.hvs
6e513e2920af3f8d8bb3b4503eab17fc517a1f4f96858b5d3c782a24de68c83c  twoholeslides.hvs
117818025f7b35433ee9ca670398d69ae21eea7279965b978050e11f5432c971  LH.hvs
```
