# Printed hardware — what the .hvs files tell us

Five `.hvs` files (G-code) were supplied before the final STL design above. They are **G-code for the 3DP-210F printer**
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

## Final design: the 14 STL parts (30 solenoids, frets 1-5)

The supplied design is a set of 14 binary STL meshes (measured with
[`tools/stl_inspect.py`](../tools/stl_inspect.py)). Unlike the `.hvs` print
paths below, an STL is the part itself, so these are design sizes, not
estimates. They describe **direct fretting**: five six-socket plates, one per
fret (1 to 5), each socket holding one solenoid that presses one string at one
fret. That is 5 x 6 = **30 solenoids** and replaces the 10-button chord-helper
arrangement. (The chord helper and the LH base do not appear in this part list.
I am treating them as superseded and kept the earlier firmware for reference;
tell me if that is wrong.)

| Part | Qty | Size X x Y x Z (mm) | Role (from its file name) |
|---|---|---|---|
| `00-PRINT-FIRST-solenoid-fit-coupon` | 1 | 15.0 x 16.0 x 14.5 | One socket, to test the solenoid fits before printing the plates |
| `01-rail-left` / `01-rail-right` | 1 + 1 | 10 x 180 x 6 | Rails along the neck; two 4.6 mm holes and five 4.6 x 12.6 mm slots |
| `02-upper-clamp-body-end` / `-nut-end` | 1 + 1 | 82 x 8 x 10 | Upper clamp at each end; two 4.6 mm holes |
| `03-lower-jaw-body-end` / `-nut-end` | 1 + 1 | 82 x 12 x 6 | Lower jaw at each end; two 4.6 mm holes |
| `04-fret-1` ... `04-fret-5-six-socket-plate` | 5 | 82 x 26 x 14.5 | One plate per fret, six solenoid sockets each |
| `05-height-shim-1mm` / `-2mm` | 1 + 1 | 10 x 10 x 1 / 2 | Slotted shims (8.6 x 4.6 mm slot) for height adjustment |

The left/right rails, the body-end/nut-end clamps and the body-end/nut-end jaws
are mirror pairs (identical size and volume). All screw holes are 4.6 mm, a
clearance size for M4. "Body end" and "nut end" are the guitar's body end and
nut end of the neck.

### The six-socket plate

Sliced at several heights:

- **Base slab**, 58 x 26 mm and about 2.5 mm thick, with six 4.8 mm holes (one
  under each socket, presumably for the plunger) and twelve 1.6 x 3.4 mm slots.
- **Six sockets** standing on it. Each is an open-topped pocket **7.5 x 10.5 mm**
  inside a 9.9 x 12.9 mm wall, about **12 mm deep** (from 2.5 mm to the 14.5 mm
  top). The fit coupon is exactly one of these sockets.
- **Staggered in two rows**, 13 mm apart (y = 6.5 and 19.5 mm), alternating
  along the plate. That is what lets six 9.9 mm-wide sockets sit on string
  spacing of 7-8 mm.
- **End posts and tabs** that bring the overall width to 82 mm.

The socket spacing **grows from plate to plate**, the way string spacing grows
down a real neck. First socket to last, centre to centre:

| Plate (fret) | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| Span of the six sockets (mm) | 35.6 | 36.7 | 37.8 | 38.7 | 39.6 |
| Average spacing (mm) | 7.13 | 7.34 | 7.55 | 7.74 | 7.93 |

That widening is the strongest evidence that each socket sits over one string.

### What this means for the software

- **30 channels**, one per (string, fret), each needing its own driver and
  flyback diode. `AGAP_Fretboard` numbers them `(fret - 1) * 6 + string`, so
  **one plate is six consecutive channels** and one cable harness serves one
  plate (pin table in `AGAP_Fretboard/agap_fret.h`).
- A fingering presses **at most one fret per string, so at most 6 coils at
  once**; the 12 V supply is sized for 6 coils, not 30, and the firmware refuses
  a seventh.
- Frets 0-5 are reachable (open plus five), so the on-board chord search
  now uses `maxFret = 5`.

### What these files do not tell us

- **The solenoid.** Its body must fit 7.5 x 10.5 mm and about 12 mm deep, and
  the plunger must pass a 4.8 mm hole, but no file names the model, its plunger
  length, its stroke or the gap to the string. Print the **coupon first** and
  try the real solenoid in it.
- **Which socket is which string.** The sockets are ordered by position along
  the plate; whether the lowest-x socket is the low E or the high e depends on
  how the plate is mounted. `PRESS 6 1` (low E, fret 1) shows which socket moves;
  fix a wrong order by swapping wires.
- **How the parts join:** how the plates attach to the rails and clamps, and how
  the assembly clamps the neck. The part names suggest an order (rail, clamp,
  jaw, plate, shim) but no file gives it.
- **Rail slot use.** The rail's five 4.6 x 12.6 mm slots match the five plates;
  they likely let each plate slide about 4 mm along the neck, but that is a guess.

## The earlier parts (`.hvs` print files)

These are the strummer and the earlier chord-helper parts. The strummer (servo pod, deck, pick arms) is still part of the final design; the LH base and the chord helper are not in the final part list.

## What the files establish, and how the software lines up

| Finding | Software |
|---|---|
| **Servo pod has 6 servo units**, in a 3 x 2 grid: columns 24.0 mm apart, rows 47 mm apart. Each unit is a solid 17.6 x 40.6 mm foot with two 3.7 mm screw holes (34 mm apart), topped by a pair of thin walls about 2.5 mm thick and 24.8 mm long, with a clear gap of about 13 mm between them, standing about 13 mm high. The gap is where a servo would sit | Matches the firmware's 6 servos (D2-D7). No change needed |
| **6 pick arms** printed (about 7.6 x 34.6 mm each), plus 4 hole-clamp pieces | Matches 6 picks, one per string |
| The arm is called **adjustable** | The firmware only had compile-time `pickA`/`pickB` angles, so tuning meant re-flashing. Added runtime calibration: `PICK`, `PLUCK`, `PICKS` (below), with clamped angles |
| **Central deck**, exact outline in the supplied drawing (below): 114 x 112 mm plate, one 72 x 24 mm cutout, 12 slots of 6.3 x 3.3 mm, 4 slots of 3 x 7 mm, 8 round holes | Nothing in the software depends on these |

### Prototype v1 render (strummer assembly)

`docs/hardware/prototype_v1_render.jpg` is a 3D render titled "AutoStrummer |
six independent servo pods, Prototype v1" (grey = original clamp, teal = new
prints, dark = servo envelopes). It is an image only, so nothing below is
measured; it is read by eye. It shows:

- **Six independent servo pods in two rows of three**, with a servo envelope
  (dark block) in each. This matches the 3 x 2 grid in the servo-pod print file.
- **Six pick arms** (orange) hanging down through the **central cutout** of the
  deck, toward six strings drawn below. This matches the 6 picks, one per
  string, that the firmware drives.
- **The teal deck sits on an "original clamp"** (grey), with four posts and
  round feet at the corners. So the strummer's pods are new prints and only the clamp is reused, which
  fits `README_AGAP.md`'s note that some strumming and support parts may be
  reusable.
- **The two pod rows sit either side of the cutout**, so in this render the pod
  and the deck do combine. That makes the hole-alignment mismatch described
  under the central-deck section more likely a difference between the print
  files and this render (different versions) than a design intent. Worth
  checking that they are the same version.

What it does not show: the solenoids, the LH base or the chord helper (it is the
strummer only); which servo drives which string; the servo's real size (the dark
blocks are labelled "envelopes", not a model); or the pick-arm travel.

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
