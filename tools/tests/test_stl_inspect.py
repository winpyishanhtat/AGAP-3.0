"""Tests for stl_inspect.py using small meshes with known sizes."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import stl_inspect as si


def box_tris(x0, y0, z0, x1, y1, z1):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    out = []
    for a, b, c, d in quads:
        out.append((v[a], v[b], v[c]))
        out.append((v[a], v[c], v[d]))
    return out


def write_stl(path, tris):
    with open(path, "wb") as f:
        f.write(b"\0" * 80)
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<12fH", 0, 0, 0, *t[0], *t[1], *t[2], 0))


def drilled_tris(x0, y0, x1, y1, hx0, hy0, hx1, hy1, z0, z1):
    """A block with a rectangular through-hole: the outer box plus the hole's
    four side walls (enough for slicing, which only needs the outlines)."""
    walls = []
    for (ax, ay), (bx, by) in [((hx0, hy0), (hx1, hy0)), ((hx1, hy0), (hx1, hy1)),
                               ((hx1, hy1), (hx0, hy1)), ((hx0, hy1), (hx0, hy0))]:
        walls.append(((ax, ay, z0), (bx, by, z0), (bx, by, z1)))
        walls.append(((ax, ay, z0), (bx, by, z1), (ax, ay, z1)))
    return box_tris(x0, y0, z0, x1, y1, z1) + walls


class TestStl(unittest.TestCase):
    def tmp(self, tris):
        p = Path(tempfile.mkdtemp()) / "t.stl"
        write_stl(p, tris)
        return p

    def test_size_and_volume_of_a_box(self):
        t = si.load(self.tmp(box_tris(0, 0, 0, 10, 20, 5)))
        self.assertEqual(len(t), 12)
        (x0, x1), (y0, y1), (z0, z1) = si.bounds(t)
        self.assertEqual((x1 - x0, y1 - y0, z1 - z0), (10, 20, 5))
        self.assertAlmostEqual(si.volume(t), 1000.0, places=3)

    def test_slice_of_a_plain_box_is_one_rectangle(self):
        t = si.load(self.tmp(box_tris(0, 0, 0, 10, 20, 5)))
        loops = si.slice_loops(t, 2.5)
        self.assertEqual(len(loops), 1)
        info = si.loop_info(loops[0])
        self.assertAlmostEqual(info["w"], 10, places=3)
        self.assertAlmostEqual(info["h"], 20, places=3)
        self.assertAlmostEqual(info["fill"], 1.0, places=3)

    def test_slice_finds_the_hole_and_its_position(self):
        t = si.load(self.tmp(drilled_tris(0, 0, 40, 30, 10, 8, 17.5, 20, 0, 6)))
        infos = sorted((si.loop_info(l) for l in si.slice_loops(t, 3)), key=lambda i: -i["area"])
        self.assertEqual(len(infos), 2)
        self.assertEqual((round(infos[1]["w"], 2), round(infos[1]["h"], 2)), (7.5, 12.0))
        self.assertEqual((round(infos[1]["cx"], 2), round(infos[1]["cy"], 2)), (13.75, 14.0))

    def test_slice_above_the_part_is_empty(self):
        t = si.load(self.tmp(box_tris(0, 0, 0, 10, 10, 5)))
        self.assertEqual(si.slice_loops(t, 9), [])

    def test_non_stl_input_is_rejected(self):
        p = Path(tempfile.mkdtemp()) / "bad.stl"
        p.write_bytes(b"solid ascii stl\nendsolid\n")
        with self.assertRaises(ValueError):
            si.load(p)
        p.write_bytes(b"\0" * 80 + struct.pack("<I", 5) + b"\0" * 10)
        with self.assertRaises(ValueError):
            si.load(p)


if __name__ == "__main__":
    unittest.main()
