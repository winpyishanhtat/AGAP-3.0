"""Checks the committed central-deck drawing against what docs/HARDWARE_SPECS.md says."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SVG = ROOT / "docs" / "hardware" / "02_central_deck_top_view.svg"


def features():
    out = []
    for pts in re.findall(r'<polygon id="[^"]+" points="([^"]+)"', SVG.read_text(encoding="utf-8")):
        p = [tuple(map(float, q.split(","))) for q in pts.split()]
        xs, ys = [a for a, _ in p], [b for _, b in p]
        out.append((round(max(xs) - min(xs), 1), round(max(ys) - min(ys), 1),
                    round((min(xs) + max(xs)) / 2, 1), round((min(ys) + max(ys)) / 2, 1)))
    return out


class TestDeckDrawing(unittest.TestCase):
    def test_feature_counts_and_sizes(self):
        f = features()
        self.assertEqual(len(f), 26)
        sizes = [(w, h) for w, h, _, _ in f]
        self.assertIn((114.0, 112.0), sizes)              # plate
        self.assertIn((72.0, 24.0), sizes)                # central cutout
        self.assertEqual(sizes.count((6.3, 3.3)), 12)     # slots
        self.assertEqual(sizes.count((3.0, 7.0)), 4)
        self.assertEqual(sizes.count((4.4, 4.4)), 4)
        self.assertEqual(sizes.count((3.3, 3.3)), 4)

    def test_slots_are_four_rows_of_three_on_a_20mm_pitch(self):
        slots = sorted((cy, cx) for w, h, cx, cy in features() if (w, h) == (6.3, 3.3))
        rows = {}
        for cy, cx in slots:
            rows.setdefault(cy, []).append(cx)
        self.assertEqual(len(rows), 4)
        for xs in rows.values():
            self.assertEqual(len(xs), 3)
            self.assertEqual([round(b - a, 1) for a, b in zip(xs, xs[1:])], [20.0, 20.0])

    def test_hardware_doc_quotes_the_drawing(self):
        doc = (ROOT / "docs" / "HARDWARE_SPECS.md").read_text(encoding="utf-8")
        for needle in ("114 x 112", "72 x 24", "6.3 x 3.3", "20 mm pitch"):
            self.assertIn(needle, doc)


if __name__ == "__main__":
    unittest.main()
