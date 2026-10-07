#!/usr/bin/env python3
"""
AGAP chord sequencer (helper-button architecture).

Implements the "Python chord selection / sequencing" layer from
README_AGAP.md section 1's intended-operation diagram:

    Python chord selection/sequencing -> Serial -> Arduino controller
    -> driver channels -> solenoids press helper buttons -> ...

Talks to ../AGAP_HelperButton/AGAP_HelperButton.ino over a plain serial
text protocol (one command per line, one reply per line, 115200 baud).

This only sends commands and prints what the board reports. It has no
way to confirm a button was actually pressed, a chord actually sounded,
or that nothing overheated - that's the physical verification in
README_AGAP.md steps 3 and 5, done by a person listening and watching.

Requires: pip install pyserial
"""

import argparse
import json
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Missing dependency. Install it with:  pip install pyserial")

DEFAULT_BAUD = 115200
DEFAULT_MAP = Path(__file__).resolve().parent.parent / "AGAP_HelperButton" / "button_map.json"


def load_button_map(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    by_label = {b["label"].upper(): b for b in data["buttons"]}
    by_chord = {b["chord"]: b for b in data["buttons"] if b.get("chord")}
    unconfirmed = [b["label"] for b in data["buttons"] if not b.get("confirmed")]
    return {"raw": data, "by_label": by_label, "by_chord": by_chord, "unconfirmed": unconfirmed}


class AgapLink:
    """Thin wrapper: send a line, read replies until a short quiet period."""

    def __init__(self, port: str, baud: int = DEFAULT_BAUD, timeout: float = 2.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        time.sleep(2.0)  # Mega resets on port open; wait for the boot banner
        self.drain()

    def drain(self) -> list[str]:
        lines = []
        while self.ser.in_waiting:
            line = self.ser.readline().decode(errors="replace").rstrip()
            if line:
                lines.append(line)
        return lines

    def send(self, line: str, wait: float = 0.15) -> list[str]:
        self.ser.write((line.strip() + "\n").encode())
        self.ser.flush()
        time.sleep(wait)
        return self.drain()

    def close(self):
        self.ser.close()


def cmd_labels(link: AgapLink, _args, _bmap):
    for line in link.send("LABELS"):
        print(line)


def cmd_press(link: AgapLink, args, bmap):
    label = resolve_label(args.target, bmap)
    for line in link.send(f"PRESS {label}"):
        print(line)


def cmd_release(link: AgapLink, args, bmap):
    label = "ALL" if args.target.upper() == "ALL" else resolve_label(args.target, bmap)
    for line in link.send(f"RELEASE {label}"):
        print(line)


def cmd_chord(link: AgapLink, args, bmap):
    label = resolve_label(args.target, bmap)
    warn_if_unconfirmed(label, bmap)
    for line in link.send(f"CHORD {label}", wait=0.4):
        print(line)


def cmd_sequence(link: AgapLink, args, bmap):
    labels = [resolve_label(tok, bmap) for tok in args.targets]
    for label in labels:
        warn_if_unconfirmed(label, bmap)
    line = "SEQUENCE " + " ".join(labels)
    beat_s = 60.0 / args.bpm
    link.send(f"TEMPO {args.bpm}")
    print(f"Sending: {line}  (~{beat_s * len(labels):.1f}s, Ctrl+C or the board's STOP button aborts)")
    link.ser.write((line + "\n").encode())
    link.ser.flush()
    deadline = time.time() + beat_s * len(labels) + 5
    try:
        while time.time() < deadline:
            for out in link.drain():
                print(out)
                if out.startswith("ABORTED") or out.startswith("ERR"):
                    return
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("Ctrl+C - sending STOP")
        for line in link.send("STOP"):
            print(line)


def cmd_calib(link: AgapLink, args, bmap):
    label = resolve_label(args.target, bmap)
    line = f"CALIB {label} {args.hold_ms} {args.reps} {args.gap_ms}"
    print(f"Sending: {line}")
    link.ser.write((line + "\n").encode())
    link.ser.flush()
    deadline = time.time() + (args.hold_ms + args.gap_ms) * args.reps / 1000.0 + 5
    try:
        while time.time() < deadline:
            for out in link.drain():
                print(out)
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("Ctrl+C - sending STOP")
        for line in link.send("STOP"):
            print(line)


def cmd_stop(link: AgapLink, _args, _bmap):
    for line in link.send("STOP"):
        print(line)


def cmd_status(link: AgapLink, _args, _bmap):
    for line in link.send("STATUS"):
        print(line)


def cmd_raw(link: AgapLink, args, _bmap):
    for line in link.send(" ".join(args.line), wait=0.3):
        print(line)


def cmd_ports(_link, _args, _bmap):
    for p in list_ports.comports():
        print(f"{p.device}  {p.description}")


def resolve_label(token: str, bmap: dict) -> str:
    """Accept either a device label (EM, AM, ...) or a chord name (Em, Am, ...)."""
    t = token.upper()
    if t in bmap["by_label"]:
        return bmap["by_label"][t]["label"]
    if token in bmap["by_chord"]:
        return bmap["by_chord"][token]["label"]
    known = ", ".join(sorted(bmap["by_label"]))
    sys.exit(f"Unknown chord/label '{token}'. Known labels: {known}")


def warn_if_unconfirmed(label: str, bmap: dict):
    if label in bmap["unconfirmed"]:
        print(
            f"  NOTE: '{label}' is not marked confirmed in button_map.json - "
            f"its chord/function hasn't been verified against the real device "
            f"(README_AGAP.md step 1/3).",
            file=sys.stderr,
        )


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="Serial port, e.g. COM5 (see `ports` subcommand)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument("--map", type=Path, default=DEFAULT_MAP, help="button_map.json path")
    sub = p.add_subparsers(dest="action", required=True)

    sub.add_parser("ports", help="list serial ports").set_defaults(func=cmd_ports)
    sub.add_parser("labels", help="list the board's button labels").set_defaults(func=cmd_labels)
    sub.add_parser("status", help="print board status").set_defaults(func=cmd_status)
    sub.add_parser("stop", help="release every channel immediately").set_defaults(func=cmd_stop)

    sp = sub.add_parser("press", help="seat one button, no strum")
    sp.add_argument("target")
    sp.set_defaults(func=cmd_press)

    sr = sub.add_parser("release", help="release one channel, or ALL")
    sr.add_argument("target")
    sr.set_defaults(func=cmd_release)

    sc = sub.add_parser("chord", help="press + strum one chord")
    sc.add_argument("target")
    sc.set_defaults(func=cmd_chord)

    sq = sub.add_parser("sequence", help="play a progression")
    sq.add_argument("targets", nargs="+")
    sq.add_argument("--bpm", type=int, default=50)
    sq.set_defaults(func=cmd_sequence)

    sb = sub.add_parser("calib", help="repeated loaded-press test (README_AGAP.md step 5)")
    sb.add_argument("target")
    sb.add_argument("--hold-ms", type=int, default=1000, dest="hold_ms")
    sb.add_argument("--reps", type=int, default=5)
    sb.add_argument("--gap-ms", type=int, default=800, dest="gap_ms")
    sb.set_defaults(func=cmd_calib)

    sx = sub.add_parser("raw", help="send a raw firmware command line")
    sx.add_argument("line", nargs="+")
    sx.set_defaults(func=cmd_raw)

    return p


def main():
    args = build_parser().parse_args()
    if args.action == "ports":
        cmd_ports(None, args, None)
        return

    bmap = load_button_map(args.map)
    if not args.port:
        sys.exit("--port is required (run `agap_control.py ports` to list them)")

    link = AgapLink(args.port, args.baud)
    try:
        for line in link.drain():  # print the boot banner
            print(line)
        args.func(link, args, bmap)
    finally:
        link.close()


if __name__ == "__main__":
    main()
