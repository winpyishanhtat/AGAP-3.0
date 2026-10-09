"""Dumps the Python solver's answers to JSON so parity_test.js can compare."""
import json, sys
sys.path.insert(0, "..")
from chord_ai import NOTE_NAMES, QUALITIES, solve, recognize, parse_chord

out = {"solve": [], "recognize": [], "parse": []}
for root in NOTE_NAMES:
    for q in QUALITIES:
        for alias in q.names:
            for max_fret in (3, 4):
                name = root + alias
                v = solve(name, max_fret)
                out["solve"].append({"name": name, "maxFret": max_fret,
                                     "frets": list(v.frets), "cost": v.cost})
shapes = [(1,3,3,2,1,1), (-1,2,4,4,3,2), (0,2,2,1,0,0), (3,2,0,0,0,3),
          (-1,0,2,2,1,0), (-1,-1,0,2,3,2), (-1,3,5,5,5,3), (2,4,4,2,2,2)]
for shape in shapes:
    cands = recognize(shape)
    out["recognize"].append({"frets": list(shape),
        "top": [[NOTE_NAMES[r] + q.names[0], s] for r, q, s in cands]})
for text in ["CM7", "Cm7", "CM", "CM9", "CM6", "CM7b5", "CMaj7", "CMAJ7", "Cmaj7", "CMin", "Cmin", "Cm", "C",
             "CDim", "Caug", "CSus4", "Csus", "CSUS2", "Cm7b5", "Cm7-5", "C7sus4", "C9", "Cm9", "Cadd9", "Cfoo",
             "F#m", "Bbmaj7", "BbM7", "Ebm7", "Gb", "A5", "CAdd9", "CMaj", "CM5"]:
    try:
        r, q = parse_chord(text)
        out["parse"].append({"text": text, "name": NOTE_NAMES[r] + q.names[0]})
    except ValueError:
        out["parse"].append({"text": text, "name": None})
json.dump(out, sys.stdout)
