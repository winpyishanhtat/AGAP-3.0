#!/usr/bin/env python3
"""
Measure a printed part from its .hvs file (G-code for the 3DP-210F printer,
sliced with CubiEngine2).

A .hvs file is toolpaths, not a drawing, so everything here is *measured
from the print paths*: sizes are the centre lines of the printed walls
(true edge-to-edge size is about one extrusion width, ~0.4 mm, larger on
outer edges). Treat results as +/- 0.5 mm, not as design dimensions.

    python hvs_inspect.py center.hvs
    python hvs_inspect.py servomount.hvs --layers 2 40 100
"""

import argparse
import hashlib
import math
import re
from pathlib import Path


def parse(path):
    """Returns (part_name, layer_height, layers) where layers maps a layer
    number to {"z": z, "loops": [bounding boxes of closed WALL-OUTER loops]}."""
    name, layer_h = "", 0.2
    layers, layer, typ = {}, None, None
    x = y = z = last_e = 0.0
    cur = None

    def flush():
        nonlocal cur
        if cur and len(cur) > 3 and layer is not None:
            xs, ys = [p[0] for p in cur], [p[1] for p in cur]
            if math.hypot(cur[0][0] - cur[-1][0], cur[0][1] - cur[-1][1]) < 0.8:
                layers.setdefault(layer, {"z": z, "loops": []})["loops"].append(
                    (min(xs), max(xs), min(ys), max(ys)))
        cur = None

    for line in Path(path).read_text(errors="replace").splitlines():
        line = line.strip()
        if line.startswith(";M902"):
            name = line[5:].strip()
        elif line.startswith("; tool H"):
            m = re.match(r"; tool H([\d.]+)", line)
            layer_h = float(m.group(1)) if m else layer_h
        elif line.startswith(";LAYER:"):
            flush()
            layer = int(line[7:])
        elif line.startswith(";TYPE:"):
            flush()
            typ = line[6:]
        elif line.startswith(("G0 ", "G1 ")):
            d = dict(re.findall(r"([XYZEF])(-?[\d.]+)", line))
            nx, ny, nz = float(d.get("X", x)), float(d.get("Y", y)), float(d.get("Z", z))
            e = float(d["E"]) if "E" in d else None
            extruding = e is not None and e > last_e
            if e is not None:
                last_e = e
            if extruding and typ == "WALL-OUTER" and layer is not None and layer >= 0:
                if cur is None:
                    cur = [(x, y)]
                cur.append((nx, ny))
            else:
                flush()
            if layer is not None and layer >= 0:
                layers.setdefault(layer, {"z": nz, "loops": []})["z"] = nz
            x, y, z = nx, ny, nz
    flush()
    return name, layer_h, layers


def summarize(path, sample_layers=None):
    name, layer_h, layers = parse(path)
    if not layers:
        raise ValueError("no part layers found - is this a CubiEngine2 .hvs file?")
    ordered = sorted(layers)
    all_loops = [lp for L in layers.values() for lp in L["loops"]]
    x0, x1 = min(l[0] for l in all_loops), max(l[1] for l in all_loops)
    y0, y1 = min(l[2] for l in all_loops), max(l[3] for l in all_loops)
    height = layers[ordered[-1]]["z"] - layers[ordered[0]]["z"] + layer_h
    out = {
        "name": name, "layer_height_mm": layer_h, "layers": len(ordered),
        "size_mm": (round(x1 - x0, 1), round(y1 - y0, 1), round(height, 1)),
        "samples": {},
    }
    for n in sample_layers or [ordered[len(ordered) // 10], ordered[len(ordered) // 2]]:
        if n in layers:
            loops = sorted(layers[n]["loops"], key=lambda b: -(b[1] - b[0]) * (b[3] - b[2]))
            out["samples"][n] = [(round(b[1] - b[0], 1), round(b[3] - b[2], 1),
                                  round((b[0] + b[1]) / 2, 1), round((b[2] + b[3]) / 2, 1)) for b in loops]
    return out


def census(layers, layer_h, min_layers=3):
    """What is on a print bed: for each distinct outline size, how many parts there are and how
    tall each is. Parts with the same outline but different heights (1 mm and 2 mm shims) are
    reported separately. Outlines seen in fewer than `min_layers` layers are slicer noise.

    How: for each outline size, take the number of copies present in every layer. Copy number k
    exists in the layers where that count is at least k; copies whose layer spans are identical
    are the same kind of part.

    Sizes are toolpath centre lines, so true edges are about one extrusion width (0.4 mm) further
    out on a solid outer edge and further in on a hole. Height is whole layers x layer height,
    so a 10 mm part at 0.15 mm layers prints 66 layers = 9.9 mm."""
    per_size = {}
    for n, layer in layers.items():
        for b in layer["loops"]:
            key = (round(b[1] - b[0], 1), round(b[3] - b[2], 1))
            per_size.setdefault(key, {})
            per_size[key][n] = per_size[key].get(n, 0) + 1
    rows = []
    for key, per in per_size.items():
        if len(per) < min_layers:
            continue
        spans = {}
        for k in range(1, max(per.values()) + 1):
            present = [n for n, c in per.items() if c >= k]
            span = (min(present), max(present))
            spans[span] = spans.get(span, 0) + 1
        for (lo, hi), count in spans.items():
            rows.append({"size": key, "count": count, "first_layer": lo, "last_layer": hi,
                         "height_mm": round((hi - lo + 1) * layer_h, 3)})
    rows.sort(key=lambda r: (-r["size"][0] * r["size"][1], r["size"], -r["height_mm"]))
    return rows


def format_census(rows):
    lines = []
    for r in rows:
        lines.append("  %d x %.1f x %.1f mm outline, %.2f mm tall (layers %d-%d)"
                     % (r["count"], r["size"][0], r["size"][1], r["height_mm"], r["first_layer"], r["last_layer"]))
    return "\n".join(lines)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--layers", type=int, nargs="*", help="layer numbers to list closed loops for")
    ap.add_argument("--sha", action="store_true", help="also print the file's SHA-256")
    ap.add_argument("--census", action="store_true", help="count the parts on a print bed and measure each one's height")
    a = ap.parse_args()
    r = summarize(a.file, a.layers)
    print(f"{r['name'] or a.file.name}: {r['size_mm'][0]} x {r['size_mm'][1]} x {r['size_mm'][2]} mm "
          f"(X x Y x height, measured), {r['layers']} layers of {r['layer_height_mm']} mm")
    for n, loops in r["samples"].items():
        print(f"  layer {n}: {len(loops)} closed outlines (w x d mm @ centre x,y)")
        for w, d, cx, cy in loops:
            print(f"    {w:6.1f} x {d:5.1f}   @ ({cx}, {cy})")
    if a.census:
        _, h, layers = parse(a.file)
        print("bed census (outline sizes are toolpath centre lines):")
        print(format_census(census(layers, h)))
    if a.sha:
        print("sha256", sha256(a.file))


if __name__ == "__main__":
    main()
