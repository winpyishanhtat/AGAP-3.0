"""Tests for lh_fit.py. Pocket size: 50.0 x 8.5 x 11.1 mm, 2 pockets."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import lh_fit as lf


class TestFit(unittest.TestCase):
    def test_small_solenoid_fits_five_per_pocket(self):
        best = lf.options((7, 9, 10))[0]
        self.assertEqual((best["across"], best["along"], best["vertical"]), (7, 9, 10))
        self.assertEqual((best["per_pocket"], best["total"], best["protrudes_mm"]), (5, 10, 0.0))

    def test_too_wide_in_every_direction_does_not_fit(self):
        self.assertEqual(lf.options((12, 12, 12)), [])
        text, ok = lf.report((12, 12, 12))
        self.assertFalse(ok)
        self.assertIn("DOES NOT FIT", text)

    def test_clearance_matters_at_the_edge(self):
        # 8.2 across leaves 0.3 mm total: fits with 0.1 clearance, not with 0.3.
        self.assertTrue(lf.options((8.2, 10, 10), clearance=0.1))
        self.assertEqual(lf.options((8.2, 10, 10), clearance=0.3), [])

    def test_tall_body_prefers_sitting_inside_over_more_but_sticking_out(self):
        opts = lf.options((7, 9, 20))
        self.assertEqual(opts[0]["protrudes_mm"], 0.0)       # lies down, fully inside
        self.assertEqual(opts[0]["total"], 4)                 # along 20: floor(50.5 / 20.5) = 2 per pocket
        standing = [o for o in opts if o["vertical"] == 20][0]
        self.assertEqual(standing["total"], 10)               # standing: 5 per pocket...
        self.assertEqual(standing["protrudes_mm"], 8.9)       # ...but 8.9 mm above the top

    def test_need_check(self):
        self.assertTrue(lf.report((7, 9, 10), need=10)[1])
        text, ok = lf.report((7, 12, 10), need=10)            # along 12: 4 per pocket
        self.assertFalse(ok)
        self.assertIn("at most 8", text)

    def test_count_is_floor_not_round(self):
        # along 9.9: floor(50.5 / 10.4) = 4, even though 4.86 is close to 5
        self.assertEqual(lf.options((7, 9.9, 9.9))[0]["per_pocket"], 4)

    def test_pocket_constants_match_the_measurement_doc(self):
        doc = (Path(__file__).resolve().parents[2] / "docs" / "HARDWARE_SPECS.md").read_text(encoding="utf-8")
        self.assertIn("50.4 x 8.9", doc)  # the measured loop these constants are derived from
        self.assertAlmostEqual(lf.POCKET_LENGTH_MM, 50.4 - 0.4, places=1)
        self.assertAlmostEqual(lf.POCKET_WIDTH_MM, 8.9 - 0.4, places=1)


if __name__ == "__main__":
    unittest.main()
