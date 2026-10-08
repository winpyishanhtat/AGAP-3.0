#!/usr/bin/env python3
"""
Will my solenoid fit the LH base, and how many?

The LH base (LH.hvs, "08_LH Base") holds the solenoids in two open-topped
parallel pockets. Measured from the print paths: each pocket is a single
undivided cavity about 50.0 x 8.5 mm and about 11 mm deep, 1.5 mm floor,
no roof. (The toolpath loop is 50.4 x 8.9 mm; a hole's real void is one
extrusion width, about 0.4 mm, smaller than the centre line.) Treat these as
+/- 0.5 mm.

Give it the solenoid BODY size (measure it, README_AGAP.md step 2). It tries
every way of laying the three dimensions into the pocket and reports how many
fit per pocket and in total. It cannot know which dimension is the plunger
axis or how the plunger must reach the helper button - that is a design
question for the real parts.

    python lh_fit.py 7 9 10
    python lh_fit.py 7 9 10 --need 10 --clearance 0.4
"""

import argparse
import itertools
import math

POCKET_LENGTH_MM = 50.0
POCKET_WIDTH_MM = 8.5
POCKET_DEPTH_MM = 11.1
POCKETS = 2

# Defaults are assumptions, not measurements: printed pockets come out a little
# tight, so leave some room on each side; and keep a small gap between bodies.
DEFAULT_CLEARANCE_MM = 0.3   # per side, across the pocket
DEFAULT_GAP_MM = 0.5         # between neighbouring bodies along the pocket


def options(dims, clearance=DEFAULT_CLEARANCE_MM, gap=DEFAULT_GAP_MM):
    """Every orientation that fits across the pocket. Each is a dict with the
    per-pocket and total count and how far the body stands above the top."""
    found = []
    for across, along, vertical in set(itertools.permutations(dims)):
        if across + 2 * clearance > POCKET_WIDTH_MM:
            continue
        per_pocket = int(math.floor((POCKET_LENGTH_MM + gap) / (along + gap)))
        if per_pocket < 1:
            continue
        found.append({
            "across": across, "along": along, "vertical": vertical,
            "per_pocket": per_pocket, "total": per_pocket * POCKETS,
            "protrudes_mm": round(max(0.0, vertical - POCKET_DEPTH_MM), 1),
        })
    # Fully sunk in the pocket first, then most solenoids.
    found.sort(key=lambda o: (o["protrudes_mm"] > 0, -o["total"], o["protrudes_mm"]))
    return found


def report(dims, need=None, clearance=DEFAULT_CLEARANCE_MM, gap=DEFAULT_GAP_MM, top=3):
    lines = ["Solenoid body %.1f x %.1f x %.1f mm in the LH base (%d pockets, each %.1f x %.1f x %.1f mm deep)"
             % (dims[0], dims[1], dims[2], POCKETS, POCKET_LENGTH_MM, POCKET_WIDTH_MM, POCKET_DEPTH_MM),
             "(clearance %.1f mm per side, %.1f mm between bodies)" % (clearance, gap)]
    opts = options(dims, clearance, gap)
    if not opts:
        widest_ok = POCKET_WIDTH_MM - 2 * clearance
        lines.append("DOES NOT FIT: no side of the body is narrow enough. The pocket allows a body at most "
                     "%.1f mm across; the smallest side here is %.1f mm." % (widest_ok, min(dims)))
        return "\n".join(lines), False
    for o in opts[:top]:
        note = "" if o["protrudes_mm"] == 0 else "  (stands %.1f mm above the top)" % o["protrudes_mm"]
        lines.append("  across %.1f, along %.1f, vertical %.1f -> %d per pocket, %d total%s"
                     % (o["across"], o["along"], o["vertical"], o["per_pocket"], o["total"], note))
    ok = True
    if need is not None:
        best = max(o["total"] for o in opts)
        ok = best >= need
        lines.append("Need %d: %s" % (need, "enough room." if ok else
                                        "NOT enough room - at most %d fit." % best))
    return "\n".join(lines), ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dims", type=float, nargs=3, metavar="MM", help="the solenoid body's three dimensions")
    ap.add_argument("--need", type=int, help="how many solenoids you need to fit (e.g. 10)")
    ap.add_argument("--clearance", type=float, default=DEFAULT_CLEARANCE_MM)
    ap.add_argument("--gap", type=float, default=DEFAULT_GAP_MM)
    a = ap.parse_args()
    if min(a.dims) <= 0:
        ap.error("dimensions must be positive")
    text, ok = report(a.dims, a.need, a.clearance, a.gap)
    print(text)
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
