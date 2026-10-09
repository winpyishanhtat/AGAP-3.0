"""Writes the Python solver's answers so the C++ solver can be checked against them.
S <chord> <maxFret> <6 frets, -1 = muted> <cost>      solve results
P <text> <canonical name or ->                         chord-name parsing
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "ChordAI"))
from chord_ai import NOTE_NAMES, QUALITIES, parse_chord, solve  # noqa: E402

for root in NOTE_NAMES:
    for q in QUALITIES:
        for alias in q.names:
            for max_fret in (3, 5):
                v = solve(root + alias, max_fret)
                print("S %s %d %s %d" % (root + alias, max_fret, " ".join(str(f) for f in v.frets), v.cost))

PARSE = ["CM7", "Cm7", "CM", "CM9", "CM6", "CM7b5", "CMaj7", "CMAJ7", "Cmaj7", "CMin", "Cmin", "Cm", "C",
         "CDim", "Caug", "CSus4", "Csus", "CSUS2", "Cm7b5", "Cm7-5", "C7sus4", "C9", "Cm9", "Cadd9", "Cfoo",
         "F#m", "Bbmaj7", "BbM7", "Ebm7", "Gb", "A5", "CAdd9", "CMaj", "CM5", "H", "Cb", "E#m", "bm"]
for text in PARSE:
    try:
        r, q = parse_chord(text)
        print("P %s %s" % (text, NOTE_NAMES[r] + q.names[0]))
    except ValueError:
        print("P %s -" % text)
