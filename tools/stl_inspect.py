#!/usr/bin/env python3
"""
Measure a binary STL part: overall size, volume, and the closed outlines you
get when you slice it at a height (holes and sockets show up as inner loops).

    python stl_inspect.py part.stl                 size + volume
    python stl_inspect.py part.stl --slice 7.0     outlines at z = 7.0 mm

STL is a true mesh (not print paths like .hvs), so sizes here are the design
sizes, not toolpath estimates. Stdlib only.
"""

import argparse
import math
import struct
from pathlib import Path


def load(path):
    """Triangles as ((x,y,z),(x,y,z),(x,y,z)). Binary STL only."""
    b = Path(path).read_bytes()
    if len(b) < 84:
        raise ValueError("too small to be a binary STL")
    n = struct.unpack("<I", b[80:84])[0]
    if 84 + 50 * n != len(b):
        raise ValueError("not a binary STL (size does not match its triangle count)")
    tris = []
    for i in range(n):
        v = struct.unpack("<12fH", b[84 + 50 * i:134 + 50 * i])
        tris.append(((v[3], v[4], v[5]), (v[6], v[7], v[8]), (v[9], v[10], v[11])))
    return tris


def bounds(tris):
    pts = [p for t in tris for p in t]
    return [(min(p[i] for p in pts), max(p[i] for p in pts)) for i in range(3)]


def volume(tris):
    v = 0.0
    for a, b, c in tris:
        v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
              + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0
    return abs(v)


def slice_loops(tris, z, tol=1e-3):
    """Closed outlines of the part at height z: list of point lists."""
    segs = []
    for tri in tris:
        pts = []
        for i in range(3):
            p, q = tri[i], tri[(i + 1) % 3]
            if (p[2] - z) * (q[2] - z) < 0:
                t = (z - p[2]) / (q[2] - p[2])
                pts.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
        if len(pts) == 2:
            segs.append(pts)
    key = lambda p: (round(p[0] / tol), round(p[1] / tol))
    ends = {}
    for i, (a, b) in enumerate(segs):
        ends.setdefault(key(a), []).append(i)
        ends.setdefault(key(b), []).append(i)
    used, loops = set(), []
    for start in range(len(segs)):
        if start in used:
            continue
        used.add(start)
        loop = [segs[start][0], segs[start][1]]
        while True:
            nxt = None
            for j in ends.get(key(loop[-1]), []):
                if j not in used:
                    nxt = j
                    break
            if nxt is None:
                break
            used.add(nxt)
            a, b = segs[nxt]
            loop.append(b if key(a) == key(loop[-1]) else a)
        if len(loop) > 3 and key(loop[0]) == key(loop[-1]):
            loops.append(loop[:-1])
    return loops


def loop_info(loop):
    xs, ys = [p[0] for p in loop], [p[1] for p in loop]
    area = sum(loop[i][0] * loop[(i + 1) % len(loop)][1] - loop[(i + 1) % len(loop)][0] * loop[i][1]
               for i in range(len(loop))) / 2.0
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    return {"w": w, "h": h, "cx": (min(xs) + max(xs)) / 2, "cy": (min(ys) + max(ys)) / 2,
            "area": abs(area), "fill": abs(area) / (w * h) if w * h else 0.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--slice", type=float, help="z height (mm) to list outlines at")
    a = ap.parse_args()
    t = load(a.file)
    (x0, x1), (y0, y1), (z0, z1) = bounds(t)
    print("%s: %d triangles, %.2f x %.2f x %.2f mm, %.2f cm3" % (a.file.name, len(t), x1 - x0, y1 - y0, z1 - z0, volume(t) / 1000))
    print("  x %.2f..%.2f  y %.2f..%.2f  z %.2f..%.2f" % (x0, x1, y0, y1, z0, z1))
    if a.slice is not None:
        infos = sorted((loop_info(l) for l in slice_loops(t, a.slice)), key=lambda i: -i["area"])
        print("  slice z=%.2f: %d closed outlines" % (a.slice, len(infos)))
        for i in infos:
            print("    %6.2f x %6.2f at (%.2f, %.2f)  area %.1f  fill %.2f" % (i["w"], i["h"], i["cx"], i["cy"], i["area"], i["fill"]))


if __name__ == "__main__":
    main()
