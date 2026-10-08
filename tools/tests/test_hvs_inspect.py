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


if __name__ == "__main__":
    unittest.main()
