#!/usr/bin/env python3
"""
AGAP Chord AI - standalone barre-to-open chord simplifier.

This is the "AI Search Engine Software" feature from the original AGAP
spec: software that turns a difficult chord (usually meaning a barre
shape) into the easiest voicing that still sounds the same chord,
*without needing the robot or any hardware at all*. It's a reference
implementation of the same constraint-satisfaction + cost-function
search that AGAP_Mega/AGAP_Mega.ino runs on the Arduino - same idea,
same core chord table, plain Python so it also works standalone as a
practice/learning tool.

Two directions:
  1. solve(name)     chord name  -> easiest voicing      (the main feature)
  2. recognize(frets) fretted shape -> best-guess chord name
                      (then you can solve() that name for something easier)

No third-party dependencies - stdlib only.

CLI:
    python chord_ai.py simplify Bm
    python chord_ai.py simplify F#m7 --max-fret 4
    python chord_ai.py recognize x,4,6,6,5,4          # F# major barre at fret 4->6
    python chord_ai.py progression C G Am F
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field

MUTED = -1
NUM_STRINGS = 6
STRING_NAMES = ("E", "A", "D", "G", "B", "e")          # low E (6th) .. high e (1st)
OPEN_PC = (4, 9, 2, 7, 11, 4)                          # standard tuning, pitch class 0=C
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
LETTER_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

# Same chord table as the firmware's QUALITIES[] (AGAP_Mega.ino), so a name
# solved here matches what the board would compute. A few extra qualities
# are added for standalone use (marked below) - the firmware doesn't need
# them and has less flash to spend on them, but this tool isn't limited.
@dataclass(frozen=True)
class Quality:
    names: tuple        # e.g. ("", "maj") - first name is canonical/displayed
    intervals: tuple     # semitones from root
    weights: tuple        # cost of leaving that interval's tone out of the voicing


QUALITIES = [
    Quality(("", "maj"), (0, 4, 7), (40, 40, 8)),
    Quality(("m", "min"), (0, 3, 7), (40, 40, 8)),
    Quality(("7",), (0, 4, 7, 10), (40, 40, 6, 25)),
    Quality(("maj7", "M7"), (0, 4, 7, 11), (40, 40, 6, 25)),
    Quality(("m7",), (0, 3, 7, 10), (40, 40, 6, 25)),
    Quality(("6",), (0, 4, 7, 9), (40, 40, 6, 25)),
    Quality(("m6",), (0, 3, 7, 9), (40, 40, 6, 25)),
    Quality(("add9",), (0, 4, 7, 2), (40, 40, 6, 25)),
    Quality(("sus2",), (0, 2, 7), (40, 40, 8)),
    Quality(("sus4", "sus"), (0, 5, 7), (40, 40, 8)),
    Quality(("dim",), (0, 3, 6), (40, 40, 30)),
    Quality(("aug",), (0, 4, 8), (40, 40, 30)),
    Quality(("5",), (0, 7), (40, 40)),
    # Extras beyond the firmware's table - standalone-only, more flash than
    # an Uno/Mega sketch would want to spend on an unlikely-to-be-used entry.
    Quality(("9",), (0, 4, 7, 10, 2), (40, 40, 6, 20, 15)),
    Quality(("m9",), (0, 3, 7, 10, 2), (40, 40, 6, 20, 15)),
    Quality(("7sus4",), (0, 5, 7, 10), (40, 40, 8, 20)),
    Quality(("m7b5", "m7-5"), (0, 3, 6, 10), (40, 40, 20, 20)),
]

_BY_NAME = {alias.lower(): q for q in QUALITIES for alias in q.names}

# Cost weights for the voicing search itself (mirrors the firmware's
# COST_* constants in AGAP_Mega.ino).
COST_MUTE = 6
COST_INNER_MUTE = 4
COST_FRETTED = 1
COST_BASS_NOT_ROOT = 15
COST_THIN_PER_STRING = 60  # charged for each string below 3 sounded strings


@dataclass
class Voicing:
    frets: tuple          # length 6, MUTED/0/1/2/3...
    cost: int
    root: int
    quality: Quality

    @property
    def chord_name(self) -> str:
        return NOTE_NAMES[self.root] + self.quality.names[0]

    def as_compact(self) -> str:
        return " ".join("x" if f == MUTED else str(f) for f in self.frets)

    def diagram(self, max_fret: int | None = None) -> str:
        return render_diagram(self.frets, max_fret)


def parse_chord(text: str) -> tuple[int, Quality]:
    """'Bm' -> (11, QUALITIES[m]); 'F#7' -> (6, QUALITIES[7])."""
    text = text.strip()
    if not text:
        raise ValueError("empty chord name")
    letter = text[0].upper()
    if letter not in LETTER_PC:
        raise ValueError(f"'{text}': expected a note letter A-G first")
    pc = LETTER_PC[letter]
    rest = text[1:]
    if rest[:1] == "#":
        pc += 1
        rest = rest[1:]
    elif rest[:1] == "b":
        pc -= 1
        rest = rest[1:]
    quality = _BY_NAME.get(rest.lower())
    if quality is None:
        known = ", ".join(sorted({q.names[0] or "(major)" for q in QUALITIES}))
        raise ValueError(f"'{text}': unknown chord quality '{rest}'. Known: {known}")
    return pc % 12, quality


def solve(chord_name: str, max_fret: int = 3) -> Voicing:
    """The main feature: chord name -> easiest voicing within frets 0..max_fret."""
    root, quality = parse_chord(chord_name)
    best = _search(root, quality, max_fret)
    return Voicing(frets=best[0], cost=best[1], root=root, quality=quality)


def _search(root: int, quality: Quality, max_fret: int) -> tuple[tuple, int]:
    n = len(quality.intervals)
    best = {"frets": None, "cost": 10**9}
    cur = [MUTED] * NUM_STRINGS

    def tone_index(pc: int) -> int:
        iv = (pc - root) % 12
        for k, want in enumerate(quality.intervals):
            if want == iv:
                return k
        return -1

    def final_cost(covered: int) -> int:
        cost = sum(w for k, w in enumerate(quality.weights) if not (covered & (1 << k)))
        sounded = [s for s in range(NUM_STRINGS) if cur[s] != MUTED]
        if len(sounded) < 3:
            cost += COST_THIN_PER_STRING * (3 - len(sounded))
        if sounded:
            first, last = sounded[0], sounded[-1]
            if (OPEN_PC[first] + cur[first]) % 12 != root:
                cost += COST_BASS_NOT_ROOT
            for s in range(first + 1, last):
                if cur[s] == MUTED:
                    cost += COST_INNER_MUTE
        return cost

    def dfs(s: int, covered: int, partial: int):
        if partial >= best["cost"]:
            return  # branch-and-bound prune
        if s == NUM_STRINGS:
            total = partial + final_cost(covered)
            if total < best["cost"]:
                best["cost"] = total
                best["frets"] = tuple(cur)
            return
        for f in range(0, max_fret + 1):
            k = tone_index((OPEN_PC[s] + f) % 12)
            if k < 0:
                continue  # constraint: only chord tones allowed
            cur[s] = f
            dfs(s + 1, covered | (1 << k), partial + (COST_FRETTED if f else 0))
        cur[s] = MUTED
        dfs(s + 1, covered, partial + COST_MUTE)

    dfs(0, 0, 0)
    return best["frets"], best["cost"]


def difficulty_label(cost: int) -> str:
    if cost <= 10:
        return "easy (open chord)"
    if cost <= 40:
        return "moderate"
    if cost <= 90:
        return "hard (likely needs a barre or a stretch)"
    return "very hard"


def is_barre(frets: tuple) -> bool:
    """Heuristic: 3+ strings fretted at the same non-zero fret, spanning the
    lowest-to-highest sounded string, usually means one finger lays across
    several strings - the shape this whole tool exists to avoid."""
    fretted = [f for f in frets if f not in (MUTED, 0)]
    if len(fretted) < 3:
        return False
    low = min(fretted)
    return fretted.count(low) >= 3


def recognize(frets: tuple, max_candidates: int = 3) -> list[tuple[int, Quality, int]]:
    """Best-guess (root, quality, score) for an arbitrary fretted shape -
    the reverse direction: 'what chord is this, and can it be simplified?'
    Score = chord tones present - foreign tones present (higher is better).
    """
    sounded_pc = {(OPEN_PC[s] + f) % 12 for s, f in enumerate(frets) if f != MUTED}
    if not sounded_pc:
        raise ValueError("no strings sounded - nothing to recognize")
    scored = []
    for root in range(12):
        for quality in QUALITIES:
            chord_pcs = {(root + iv) % 12 for iv in quality.intervals}
            hits = len(sounded_pc & chord_pcs)
            foreign = len(sounded_pc - chord_pcs)
            score = hits * 2 - foreign * 3 - (len(quality.intervals) - hits)
            scored.append((score, root, quality))
    scored.sort(key=lambda t: -t[0])
    out, seen = [], set()
    for score, root, quality in scored:
        key = (root, quality.names[0])
        if key in seen:
            continue
        seen.add(key)
        out.append((root, quality, score))
        if len(out) >= max_candidates:
            break
    return out


def render_diagram(frets: tuple, max_fret: int | None = None) -> str:
    """Standard-orientation ASCII chord box: nut at top, 6th string on the
    left, open columns of letters under the fret grid."""
    shown_max = max_fret if max_fret is not None else max([f for f in frets if f > 0], default=3)
    shown_max = max(shown_max, 1)
    top = "  ".join("x" if f == MUTED else ("o" if f == 0 else " ") for f in frets)
    lines = ["  " + top, "  " + "+--" * NUM_STRINGS + "+"]
    for fret in range(1, shown_max + 1):
        row = "  ".join("@" if f == fret else " " for f in frets)
        lines.append(f"{fret} " + row + " ")
    lines.append("  " + "  ".join(STRING_NAMES))
    return "\n".join(lines)


# ================================== CLI ==================================

def _cmd_simplify(args):
    try:
        v = solve(args.chord, max_fret=args.max_fret)
    except ValueError as e:
        sys.exit(f"ERR {e}")
    print(f"{args.chord} -> {v.chord_name}: {v.as_compact()}  (cost {v.cost}, {difficulty_label(v.cost)})")
    if is_barre(v.frets):
        print("  note: even the simplified shape still looks like a barre - "
              "try --max-fret higher, or this chord may not have a true open voicing.")
    if not args.no_diagram:
        print(v.diagram())


def _parse_shape(text: str) -> tuple:
    toks = text.replace(",", " ").split()
    if len(toks) != NUM_STRINGS:
        raise ValueError(f"expected {NUM_STRINGS} values (6th..1st string), got {len(toks)}")
    frets = []
    for t in toks:
        frets.append(MUTED if t.lower() in ("x", "-1") else int(t))
    return tuple(frets)


def _cmd_recognize(args):
    try:
        frets = _parse_shape(args.shape)
        candidates = recognize(frets, max_candidates=args.top)
    except ValueError as e:
        sys.exit(f"ERR {e}")
    print(f"shape {' '.join('x' if f == MUTED else str(f) for f in frets)} looks like:")
    for rank, (root, quality, score) in enumerate(candidates, 1):
        name = NOTE_NAMES[root] + quality.names[0]
        print(f"  {rank}. {name}  (match score {score})")
    best_root, best_quality, _ = candidates[0]
    best_name = NOTE_NAMES[best_root] + best_quality.names[0]
    simplified = solve(best_name, max_fret=args.max_fret)
    print(f"\nSimplified {best_name} -> {simplified.as_compact()}  "
          f"(cost {simplified.cost}, {difficulty_label(simplified.cost)})")
    if not args.no_diagram:
        print(simplified.diagram())


def _cmd_progression(args):
    for name in args.chords:
        try:
            v = solve(name, max_fret=args.max_fret)
        except ValueError as e:
            print(f"{name}: ERR {e}")
            continue
        flag = "  <- still a barre" if is_barre(v.frets) else ""
        print(f"{name:>6} -> {v.as_compact():<14} cost {v.cost:>3}  {difficulty_label(v.cost)}{flag}")


def build_parser() -> argparse.ArgumentParser:
    # --max-fret/--no-diagram are shared by every subcommand. A parent
    # parser lets them be given either before or after the subcommand name
    # ("chord_ai.py simplify F --max-fret 4" and "chord_ai.py --max-fret 4
    # simplify F" both work) - argparse options placed only on the main
    # parser are rejected after the subcommand, which isn't how people type.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--max-fret", type=int, default=3, help="highest fret the search may use (default 3, matches the robot's reach)")
    common.add_argument("--no-diagram", action="store_true", help="skip the ASCII chord box")

    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, parents=[common])
    sub = p.add_subparsers(dest="action", required=True)

    sp = sub.add_parser("simplify", help="chord name -> easiest voicing", parents=[common])
    sp.add_argument("chord")
    sp.set_defaults(func=_cmd_simplify)

    sr = sub.add_parser("recognize", help="fretted shape -> best-guess chord name, then simplify it", parents=[common])
    sr.add_argument("shape", help='6 values, 6th..1st string, e.g. "x,4,6,6,5,4" or "x 4 6 6 5 4"')
    sr.add_argument("--top", type=int, default=3)
    sr.set_defaults(func=_cmd_recognize)

    sq = sub.add_parser("progression", help="simplify a whole chord progression at once", parents=[common])
    sq.add_argument("chords", nargs="+")
    sq.set_defaults(func=_cmd_progression)

    return p


def main():
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
