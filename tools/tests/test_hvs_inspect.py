"""Tests for hvs_inspect.py using small synthetic G-code with known sizes."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import hvs_inspect as hv


def rect(x0, y0, x1, y1, e0):
    """A closed rectangle as extruding moves, starting at (x0, y0)."""
    pts = [(x1, y0), (x1, y1), (x0, y1), (x0, y0)]
    lines, e = [f"G0 X{x0} Y{y0}"], e0
    for x, y in pts:
        e += 1.0
        lines.append(f"G1 X{x} Y{y} E{e}")
    return lines, e


def make_gcode():
    out = [";M902 99_test_part", "M82", "; tool H0.2000 W0.4000"]
    e = 0.0
    for layer in range(5):
        z = 0.4 + layer * 0.2
        out += [f";LAYER:{layer}", f"G0 Z{z}", ";TYPE:WALL-OUTER"]
        body, e = rect(10, 20, 50, 60, e)       # 40 x 40 outline
        out += body
        hole, e = rect(25, 35, 35, 45, e)       # 10 x 10 hole
        out += hole
    return "\n".join(out) + "\n"


class TestInspect(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile("w", suffix=".hvs", delete=False)
        self.tmp.write(make_gcode())
        self.tmp.close()

    def tearDown(self):
        Path(self.tmp.name).unlink()

    def test_overall_size_and_name(self):
        r = hv.summarize(self.tmp.name)
        self.assertEqual(r["name"], "99_test_part")
        self.assertEqual(r["layers"], 5)
        self.assertEqual(r["size_mm"], (40.0, 40.0, 1.0))  # 4 steps of 0.2 + one layer

    def test_finds_outline_and_hole_with_centres(self):
        r = hv.summarize(self.tmp.name, [2])
        loops = r["samples"][2]
        self.assertEqual(loops[0], (40.0, 40.0, 30.0, 40.0))   # w, d, cx, cy
        self.assertEqual(loops[1], (10.0, 10.0, 30.0, 40.0))

    def test_not_a_part_file_raises(self):
        Path(self.tmp.name).write_text("G28\nM104 S200\n")
        with self.assertRaises(ValueError):
            hv.summarize(self.tmp.name)

    def test_sha256_is_stable(self):
        self.assertEqual(hv.sha256(self.tmp.name), hv.sha256(self.tmp.name))
        self.assertEqual(len(hv.sha256(self.tmp.name)), 64)


def make_bed_gcode():
    """A print bed with three kinds of part: two tall 40 x 8 blocks (10 layers), one 20 x 20
    block (4 layers), and a stray tiny outline that appears in a single layer (slicer noise)."""
    out = [";M902 99_bed", "M82", "; tool H0.1500 W0.4000"]
    e = 0.0
    for layer in range(10):
        out += [";LAYER:%d" % layer, "G0 Z%s" % (0.4 + layer * 0.15), ";TYPE:WALL-OUTER"]
        for y0 in (0, 20):
            body, e = rect(0, y0, 40, y0 + 8, e)
            out += body
        if layer < 4:
            body, e = rect(60, 0, 80, 20, e)
            out += body
        if layer == 2:
            body, e = rect(90, 0, 90.2, 0.4, e)
            out += body
        # two parts with the SAME outline but different heights (like 1 mm and 2 mm shims)
        if layer < 3:
            body, e = rect(100, 0, 110, 10, e)
            out += body
        if layer < 6:
            body, e = rect(100, 20, 110, 30, e)
            out += body
    return "\n".join(out) + "\n"


class TestCensus(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile("w", suffix=".hvs", delete=False)
        self.tmp.write(make_bed_gcode())
        self.tmp.close()

    def tearDown(self):
        Path(self.tmp.name).unlink()

    def rows(self):
        _, h, layers = hv.parse(self.tmp.name)
        return {r["size"]: r for r in hv.census(layers, h)}

    def test_counts_each_kind_of_part_on_the_bed(self):
        rows = self.rows()
        self.assertEqual(rows[(40.0, 8.0)]["count"], 2)
        self.assertEqual(rows[(20.0, 20.0)]["count"], 1)

    def test_height_comes_from_how_many_layers_the_part_spans(self):
        rows = self.rows()
        self.assertAlmostEqual(rows[(40.0, 8.0)]["height_mm"], 1.5, places=2)   # 10 layers of 0.15
        self.assertAlmostEqual(rows[(20.0, 20.0)]["height_mm"], 0.6, places=2)  # 4 layers

    def test_same_outline_with_different_heights_is_reported_separately(self):
        _, h, layers = hv.parse(self.tmp.name)
        shims = [r for r in hv.census(layers, h) if r["size"] == (10.0, 10.0)]
        shims.sort(key=lambda r: r["height_mm"])
        self.assertEqual([(r["count"], round(r["height_mm"], 2)) for r in shims], [(1, 0.45), (1, 0.9)])

    def test_two_equal_parts_are_one_row_not_two(self):
        _, h, layers = hv.parse(self.tmp.name)
        rows = [r for r in hv.census(layers, h) if r["size"] == (40.0, 8.0)]
        self.assertEqual(len(rows), 1)

    def test_one_layer_noise_is_left_out(self):
        self.assertNotIn((0.2, 0.4), self.rows())

    def test_report_text_mentions_counts_and_heights(self):
        _, h, layers = hv.parse(self.tmp.name)
        text = hv.format_census(hv.census(layers, h))
        self.assertIn("2 x", text)
        self.assertIn("40.0 x 8.0 mm", text)
        self.assertIn("1.50 mm", text)


if __name__ == "__main__":
    unittest.main()
