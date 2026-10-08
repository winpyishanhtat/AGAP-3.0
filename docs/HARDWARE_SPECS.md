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
| **Servo pod has 6 servo units**, in a 3 x 2 grid: columns 24.0 mm apart, rows 47 mm apart. Each unit is a solid 17.6 x 40.6 mm foot with two 3.7 mm screw holes (34 mm apart), topped by a pair of thin walls about 2.5 mm thick and 24.8 mm long, with a clear gap of about 13 mm between them, standing about 13 mm high. The gap is where a servo would sit | Matches the firmware's 6 servos (D2-D7). No change needed |
| **6 pick arms** printed (about 7.6 x 34.6 mm each), plus 4 hole-clamp pieces | Matches 6 picks, one per string |
| The arm is called **adjustable** | The firmware only had compile-time `pickA`/`pickB` angles, so tuning meant re-flashing. Added runtime calibration: `PICK`, `PLUCK`, `PICKS` (below), with clamped angles |
| **Central deck**, exact outline in the supplied drawing (below): 114 x 112 mm plate, one 72 x 24 mm cutout, 12 slots of 6.3 x 3.3 mm, 4 slots of 3 x 7 mm, 8 round holes | Nothing in the software depends on these |

### Central deck: the exact drawing

`docs/hardware/02_central_deck_top_view.svg` is a 1:1 top-view outline of
`02_central_deck` (a mid-thickness section of its STL), so these are the design
sizes, not toolpath measurements. In the file's own millimetre coordinates:

- plate **114 x 112** mm, with one **72 x 24** mm cutout centred in it
- **12 slots, 6.3 x 3.3 mm**, in four rows of three on a **20 mm pitch**:
  x = 32, 52, 72 at y = 7 and 41; x = 42, 62, 82 at y = 71 and 105
- 4 slots **3 x 7** mm at (16, 26), (98, 26), (16, 86), (98, 86)
- 4 round holes **dia 4.4** at (11, 8), (103, 8), (11, 104), (103, 104)
- 4 round holes **dia 3.3** at (5, 40), (109, 40), (5, 72), (109, 72)

What this drawing does not say is what any of those features are for.

**One thing to check against the real parts.** The pod's 12 screw holes come in
pairs 34 mm apart, and the deck's 12 slots also come in pairs 34 mm apart, so
the pod looks designed to bolt onto this deck. But as drawn they do not line up
in one placement: the pod's units are 24 mm apart across and 47 mm apart down,
while the deck's slots are 20 mm apart across (and each slot gives only about
3 mm of sideways play) and its two slot groups are 64 mm apart down. Either one
of the two is still being revised (the deck is described as a draft), or the
units are meant to be fitted one at a time. The files do not say which.

**This also checks the G-code measuring method.** All 26 features measured from
`center.hvs` match this drawing, centres to within 0.01 mm. Sizes differ by
exactly one print line width, 0.4 mm: the outer edge measures 0.4 mm smaller
than the design, and every hole measures 0.4 mm larger. So the rule used for
the LH pockets (measured loop minus 0.4 mm gives the real hole) is confirmed on
a part where the true answer is known. It was checked on one part and one
printer setup.

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
- **The servo model.** The gap between each pair of walls (about 13 mm wide,
  24.8 mm long) is roughly what a small 9 g servo body needs (typically about
  12 mm wide and 22-23 mm long; check yours), and a larger standard servo
  would not fit. That is consistent with a small servo, not proof. Measure
  your servo against the gap before printing more.
  (An earlier version of this doc called the 17.6 x 40.6 mm shape a "bay".
  That is the solid foot at the bottom, not the space the servo sits in.)
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
