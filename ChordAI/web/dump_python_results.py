"""Dumps the Python solver's answers to JSON so parity_test.js can compare."""
import json, sys
sys.path.insert(0, "..")
from chord_ai import NOTE_NAMES, QUALITIES, solve, recognize

out = {"solve": [], "recognize": []}
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
json.dump(out, sys.stdout)
