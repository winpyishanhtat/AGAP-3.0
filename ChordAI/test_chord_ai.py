#!/usr/bin/env python3
"""
Regression tests for chord_ai.py. Stdlib unittest only - no pytest needed.

Run with:  python -m unittest ChordAI.test_chord_ai -v
      or:  python ChordAI/test_chord_ai.py
"""

import unittest

from chord_ai import (
    MUTED,
    is_barre,
    parse_chord,
    recognize,
    solve,
)


class TestParseChord(unittest.TestCase):
    def test_plain_major(self):
        root, q = parse_chord("C")
        self.assertEqual(root, 0)
        self.assertEqual(q.names[0], "")

    def test_sharp_minor(self):
        root, q = parse_chord("F#m")
        self.assertEqual(root, 6)
        self.assertEqual(q.names[0], "m")

    def test_flat(self):
        root, _ = parse_chord("Bb")
        self.assertEqual(root, 10)

    def test_unknown_quality_raises(self):
        with self.assertRaises(ValueError):
            parse_chord("Cfoo")

    def test_bad_letter_raises(self):
        with self.assertRaises(ValueError):
            parse_chord("Hmaj")


class TestSolveKnownVoicings(unittest.TestCase):
    """These expected shapes are the standard open-chord voicings a
    guitarist would actually use - if the search ever stops producing
    them, something in the cost function broke."""

    def assertVoicing(self, chord, expected):
        v = solve(chord, max_fret=3)
        self.assertEqual(v.frets, expected, f"{chord} -> {v.frets}, expected {expected}")

    def test_bm_matches_firmware_doc_example(self):
        # AGAP_Mega.ino's header comment documents this exact result.
        self.assertVoicing("Bm", (MUTED, 2, 0, MUTED, 0, 2))

    def test_c_major(self):
        self.assertVoicing("C", (MUTED, 3, 2, 0, 1, 0))

    def test_g_major(self):
        self.assertVoicing("G", (3, 2, 0, 0, 0, 3))

    def test_a_minor(self):
        self.assertVoicing("Am", (MUTED, 0, 2, 2, 1, 0))

    def test_e_major(self):
        self.assertVoicing("E", (0, 2, 2, 1, 0, 0))

    def test_d_major(self):
        self.assertVoicing("D", (MUTED, MUTED, 0, 2, 3, 2))


class TestSolveGeneralProperties(unittest.TestCase):
    """For every chord the solver knows, the result must obey the hard
    constraints regardless of what the cost weights happen to be."""

    def test_every_voicing_within_max_fret_and_sounds_a_chord_tone(self):
        root_letters = "C D E F G A B".split()
        qualities = ["", "m", "7", "maj7", "m7", "sus4", "dim", "aug", "5"]
        for letter in root_letters:
            for q in qualities:
                name = letter + q
                for max_fret in (3, 4):
                    v = solve(name, max_fret=max_fret)
                    for fret in v.frets:
                        self.assertTrue(MUTED <= fret <= max_fret, f"{name}: fret {fret} out of 0..{max_fret}")
                    # at least one string must sound (a chord with every
                    # string muted plays nothing, and should never win)
                    self.assertTrue(any(f != MUTED for f in v.frets), f"{name}: every string muted")

    def test_cost_never_negative(self):
        for name in ("C", "Bm", "F#m7", "Caug", "Gdim"):
            self.assertGreaterEqual(solve(name).cost, 0)

    def test_higher_max_fret_never_increases_cost(self):
        # Letting the search range further can only find an equal-or-better
        # voicing, never force a worse one.
        for name in ("Bm", "F#m7", "C#", "Ddim"):
            cost3 = solve(name, max_fret=3).cost
            cost5 = solve(name, max_fret=5).cost
            self.assertLessEqual(cost5, cost3, f"{name}: cost grew when max_fret increased")


class TestIsBarre(unittest.TestCase):
    def test_true_barre_f_detected(self):
        self.assertTrue(is_barre((1, 3, 3, 2, 1, 1)))

    def test_open_c_not_barre(self):
        self.assertFalse(is_barre((MUTED, 3, 2, 0, 1, 0)))

    def test_open_g_not_barre(self):
        self.assertFalse(is_barre((3, 2, 0, 0, 0, 3)))

    def test_power_chord_two_strings_not_barre(self):
        self.assertFalse(is_barre((MUTED, MUTED, MUTED, MUTED, 3, 3)))


class TestRecognize(unittest.TestCase):
    def test_full_barre_f_recognized(self):
        candidates = recognize((1, 3, 3, 2, 1, 1))
        top_name = candidates[0][1].names[0]
        from chord_ai import NOTE_NAMES
        full_name = NOTE_NAMES[candidates[0][0]] + top_name
        self.assertEqual(full_name, "F")

    def test_a_shape_barre_bm_recognized(self):
        candidates = recognize((MUTED, 2, 4, 4, 3, 2))
        from chord_ai import NOTE_NAMES
        full_name = NOTE_NAMES[candidates[0][0]] + candidates[0][1].names[0]
        self.assertEqual(full_name, "Bm")

    def test_all_muted_raises(self):
        with self.assertRaises(ValueError):
            recognize((MUTED,) * 6)


class TestQualityNames(unittest.TestCase):
    """Capital M is major and lowercase m is minor. A case-insensitive lookup
    used to merge them: CM7 became Cm7 and CM became C minor."""

    def name(self, text):
        from chord_ai import NOTE_NAMES
        root, q = parse_chord(text)
        return NOTE_NAMES[root] + q.names[0]

    def test_capital_M7_is_major_seventh(self):
        self.assertEqual(self.name("CM7"), "Cmaj7")
        self.assertEqual(self.name("Cmaj7"), "Cmaj7")

    def test_lowercase_m7_is_minor_seventh(self):
        self.assertEqual(self.name("Cm7"), "Cm7")

    def test_bare_capital_M_is_rejected_not_guessed(self):
        for text in ("CM", "CM9", "CM6", "CM7b5"):
            with self.assertRaises(ValueError, msg=text):
                parse_chord(text)

    def test_capitalised_words_still_work(self):
        self.assertEqual(self.name("CMaj7"), "Cmaj7")
        self.assertEqual(self.name("CMAJ7"), "Cmaj7")
        self.assertEqual(self.name("CMin"), "Cm")
        self.assertEqual(self.name("CDim"), "Cdim")
        self.assertEqual(self.name("CSus4"), "Csus4")

    def test_plain_major_and_minor(self):
        self.assertEqual(self.name("C"), "C")
        self.assertEqual(self.name("Cm"), "Cm")
        self.assertEqual(self.name("Cmaj"), "C")


if __name__ == "__main__":
    unittest.main()
