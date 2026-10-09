"""Checks the claims in docs/HARDWARE_SPECS.md against the real design files.

The .hvs and .stl files are 2-7 MB each and are not committed, so this suite only runs where
they exist: point AGAP_PARTS_DIR at the folder, or keep them in Downloads/Telegram Desktop. On a
machine without them (CI) every test is skipped, which is reported as a skip, not a pass.
"""
import math
import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import hvs_inspect as hv
import stl_inspect as si

DOCS = (HERE.parent.parent / "docs" / "HARDWARE_SPECS.md").read_text(encoding="utf-8")


def parts_dir():
    cands = [os.environ.get("AGAP_PARTS_DIR"), str(Path.home() / "Downloads" / "Telegram Desktop")]
    for c in cands:
        if c and (Path(c) / "01-rail-left.stl").exists():
            return Path(c)
    return None


PARTS = parts_dir()
RAIL_BED = "01-rail-left(3DP-210F_ABS).hvs"
PLATE_BED = "04-fret-1-six-socket-plate(3DP-210F_ABS).hvs"
CLAMP_BED = "02-upper-clamp-body-end(3DP-210F_ABS).hvs"


def need(*names):
    ok = PARTS is not None and all((PARTS / n).exists() for n in names)
    return unittest.skipUnless(ok, "design files not found (set AGAP_PARTS_DIR)")


def box(b):
    return (b[1] - b[0], b[3] - b[2], (b[0] + b[1]) / 2, (b[2] + b[3]) / 2)


class TestStlClaims(unittest.TestCase):
    @need("01-rail-left.stl", "01-rail-right.stl")
    def test_left_and_right_rails_are_one_part(self):
        self.assertEqual(si.compare(PARTS / "01-rail-left.stl", PARTS / "01-rail-right.stl"), "identical")

    @need("03-lower-jaw-body-end.stl", "03-lower-jaw-nut-end.stl")
    def test_the_two_lower_jaws_are_the_same_shape(self):
        self.assertIn(si.compare(PARTS / "03-lower-jaw-body-end.stl", PARTS / "03-lower-jaw-nut-end.stl"),
                      ("identical", "same shape"))

    @need("02-upper-clamp-body-end.stl", "02-upper-clamp-nut-end.stl")
    def test_the_two_upper_clamps_are_not_interchangeable(self):
        self.assertEqual(si.compare(PARTS / "02-upper-clamp-body-end.stl", PARTS / "02-upper-clamp-nut-end.stl"),
                         "different")

    @need(*["04-fret-%d-six-socket-plate.stl" % n for n in range(1, 6)])
    def test_the_five_plates_are_five_different_parts(self):
        names = ["04-fret-%d-six-socket-plate.stl" % n for n in range(1, 6)]
        for i in range(5):
            for j in range(i + 1, 5):
                self.assertEqual(si.compare(PARTS / names[i], PARTS / names[j]), "different", (i + 1, j + 1))

    @need("01-rail-left.stl")
    def test_rail_slot_pitch_follows_the_fret_rule(self):
        t = si.load(PARTS / "01-rail-left.stl")
        slots = sorted(i["cy"] for i in map(si.loop_info, si.slice_loops(t, 3.0)) if i["h"] > 8 and i["area"] < 500)
        self.assertEqual(len(slots), 5)
        gaps = [b - a for a, b in zip(slots, slots[1:])]
        for g0, g1 in zip(gaps, gaps[1:]):
            self.assertAlmostEqual(g1 / g0, 2 ** (-1 / 12), delta=0.005)


class TestBedClaims(unittest.TestCase):
    @need(RAIL_BED)
    def test_rail_bed_reaches_past_the_150mm_profile_and_is_flagged(self):
        warnings = hv.bed_warnings(PARTS / RAIL_BED)
        self.assertTrue(any("X" in w and "155" in w for w in warnings), warnings)

    @need(PLATE_BED)
    def test_plate_bed_fits_the_profile(self):
        self.assertEqual(hv.bed_warnings(PARTS / PLATE_BED), [])

    @need(RAIL_BED)
    def test_rail_bed_holds_two_rails_with_five_slots_each(self):
        _, h, layers = hv.parse(PARTS / RAIL_BED)
        rows = {r["size"]: r["count"] for r in hv.census(layers, h)}
        self.assertEqual(rows[(135.9, 131.7)], 2)
        self.assertEqual(rows[(10.8, 10.6)], 10)
        self.assertEqual(rows[(5.0, 5.0)], 4)

    @need(PLATE_BED)
    def test_plate_bed_holds_thirty_sockets(self):
        _, h, layers = hv.parse(PARTS / PLATE_BED)
        rows = hv.census(layers, h)
        inner = sum(r["count"] for r in rows if r["size"] in ((7.9, 10.9), (11.0, 8.1)))
        outer = sum(r["count"] for r in rows if r["size"] in ((9.5, 12.5), (12.7, 9.7)))
        self.assertEqual((inner, outer), (30, 30))

    @need(PLATE_BED, *["04-fret-%d-six-socket-plate.stl" % n for n in range(1, 5)])
    def test_upright_plates_match_their_stl_socket_positions(self):
        _, h, layers = hv.parse(PARTS / PLATE_BED)
        loops = [box(b) for b in layers[30]["loops"]]
        inner = [b for b in loops if abs(b[0] - 7.9) < 0.05 and abs(b[1] - 10.9) < 0.05]
        slabs = sorted((box(b) for b in layers[5]["loops"] if abs(box(b)[0] - 57.6) < 0.1), key=lambda b: b[3])
        self.assertEqual(len(slabs), 4)
        for n, slab in enumerate(slabs, start=1):
            bed = sorted(round(b[2] - slab[2], 2) for b in inner if abs(b[3] - slab[3]) < 13)
            t = si.load(PARTS / ("04-fret-%d-six-socket-plate.stl" % n))
            s = max((si.loop_info(l) for l in si.slice_loops(t, 1.0)), key=lambda i: i["area"])
            stl = sorted(round(i["cx"] - s["cx"], 2) for i in map(si.loop_info, si.slice_loops(t, 8.0))
                         if abs(i["w"] - 7.5) < 0.3 and abs(i["h"] - 10.5) < 0.3)
            self.assertEqual(len(bed), 6)
            for a, b in zip(bed, stl):
                self.assertLess(abs(a - b), 0.05, "plate %d" % n)

    @need(PLATE_BED)
    def test_plate_bed_prints_supports(self):
        cfg = hv.settings(PARTS / PLATE_BED)
        self.assertEqual(cfg["support_enable"], "True")
        self.assertTrue(math.isclose(hv.print_seconds(PARTS / PLATE_BED) / 3600, 5.5, abs_tol=0.1))


class TestDocsMatchFiles(unittest.TestCase):
    """Every print file named in the docs has its SHA-256 recorded there, and it is the right one."""

    def check(self, name):
        sha = hv.sha256(PARTS / name)
        self.assertIn(sha, DOCS, "%s: its SHA-256 is not recorded in HARDWARE_SPECS.md (%s)" % (name, sha))

    @need(RAIL_BED)
    def test_rail_bed_sha_recorded(self):
        self.check(RAIL_BED)

    @need(PLATE_BED)
    def test_plate_bed_sha_recorded(self):
        self.check(PLATE_BED)

    @need(CLAMP_BED)
    def test_clamp_bed_sha_recorded(self):
        self.check(CLAMP_BED)


if __name__ == "__main__":
    unittest.main()
