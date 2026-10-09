# AGAP Chord AI — standalone barre-to-open chord simplifier

This is the **"AI Search Engine Software"** feature from the original AGAP
spec: software that turns a hard chord into the easiest voicing that still
sounds that chord, **usable entirely on its own, no robot or hardware
required.**

It's a plain-Python port of the same constraint-satisfaction + cost-function
search that [`../AGAP_Mega/AGAP_Mega.ino`](../AGAP_Mega/AGAP_Mega.ino) runs
on the Arduino (its `SHOW`/`CHORD` commands) — same chord table, same cost
weights, same branch-and-bound algorithm — just without needing a board
plugged in. No third-party dependencies.

## Install

Nothing to install beyond Python 3.8+. It's one file.

## Use

```bash
# chord name -> easiest voicing within frets 0-5 (the final robot's reach; --max-fret 3 for the earlier one)
python chord_ai.py simplify Bm
# Bm -> Bm: x 2 0 x 0 2  (cost 18, moderate)

python chord_ai.py simplify "F#m7" --max-fret 4
python chord_ai.py simplify F --no-diagram      # skip the ASCII chord box

# whole progression at once
python chord_ai.py progression C G Am F Bm "F#m7"

# the reverse direction: "what chord is this fretted shape, and can it
# be made easier?" - 6 values, 6th string first, x for muted
python chord_ai.py recognize "1,3,3,2,1,1"
#   1. F  (match score 6)
# Simplified F -> 1 0 3 2 1 1  (cost 5, easy (open chord))
```

Or use it as a library:

```python
from chord_ai import solve, recognize, is_barre

v = solve("Bm", max_fret=3)
print(v.frets, v.cost, v.chord_name)   # (-1, 2, 0, -1, 0, 2) 18 'Bm'
print(v.diagram())
```

## How it decides what's "easier"

Same search as the firmware: for every string, try every fret 0..max_fret
(or mute it), keep only combinations where every sounded note belongs to
the chord, and score each complete 6-string combination by adding cost for:

- a chord tone left out entirely (weighted by how essential that tone is —
  root and third cost more than a 7th or 9th)
- a muted string, especially one *between* two sounded strings (a "hole")
- the lowest sounded string not being the chord's root
- fewer than 3 strings sounding at all
- each fretted (non-open) string, a small cost so open strings are
  preferred when everything else is equal

The lowest-cost combination wins. `is_barre()` is a separate heuristic (3+
strings pressed at the same fret) used only to flag when even the
"simplified" result still needs a partial barre — some chords (full F,
F#m7, etc.) genuinely don't have a true open-string voicing.

## Chord table

Covers: major, m, 7, maj7, m7, 6, m6, add9, sus2, sus4, dim, aug, 5 (power
chord) — identical to the firmware's table — plus 9, m9, 7sus4 and m7b5,
which are standalone-only additions (more than the Arduino's flash budget
would want to carry for a feature that may rarely get used on-device).

## Tests

```bash
python -m unittest test_chord_ai -v
```

21 tests: parsing, known standard-shape regression checks (e.g. the
firmware's own documented Bm example), hard-constraint properties (every
result stays within the requested fret range, cost is monotonic as
`max_fret` grows), and barre detection/recognition on real barre shapes.

## Relationship to the rest of this repo

This module is independent of both hardware designs in this repo
([`../AGAP_Mega`](../AGAP_Mega) and [`../AGAP_HelperButton`](../AGAP_HelperButton))
and doesn't require either. The original spec asked for it to work that
way — a search engine a guitarist could use on its own as a learning tool,
separate from the physical robot.

## Web version

`web/index.html` is the same solver as a single-page browser app (no server, no install): simplify a chord, a whole progression, or name a fretted shape, with chord diagrams. `web/chord_ai.js` is a JavaScript port of `chord_ai.py`; `web/parity_test.js` checks it against the Python on every root, chord quality and fret range (538 checks, also run in CI).

To try it locally: `cd web && python -m http.server` then open http://localhost:8000.

To publish it: in the GitHub repo go to Settings -> Pages -> Source: "GitHub Actions", then run the "Deploy ChordAI web page" workflow from the Actions tab.
