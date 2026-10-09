#!/usr/bin/env python3
"""
AGAP remote-control bridge.

Runs on the machine plugged into the Arduino Mega (a Raspberry Pi or the
PC next to the robot) and exposes a small, locked-down HTTP API plus a
phone-friendly page, so the robot can be driven from another device.

    browser / phone --HTTP--> this bridge --USB serial--> Mega firmware

Safety and security rules this enforces (the robot can't be watched
remotely, so the bridge is deliberately strict):

  * Token required on every API call (Authorization: Bearer <token>).
  * Allowlist only. There is no raw passthrough: the client picks an action
    from a fixed list, and the only strings ever written to the serial port
    are built here from labels read out of button_map.json. A client can't
    inject a firmware command.
  * STOP is always accepted immediately, even while a sequence is running,
    and is written straight to the port (the firmware checks for it mid-run).
  * Other commands are refused with 409 while a sequence/calibration is in
    progress, and rate-limited.
  * Buttons not marked "confirmed" in button_map.json are refused unless the
    operator starts the bridge with --allow-unconfirmed.
  * CALIB (repeated presses; can heat a coil) is off unless --allow-calib,
    and then capped.
  * Binds to 127.0.0.1 by default. There is no TLS: put it behind a tunnel
    or VPN (Tailscale, Cloudflare Tunnel, SSH) rather than exposing it.
  * STOP is sent when the bridge exits.

This only relays commands. It can't tell whether a button physically
pressed or a chord sounded right, and nothing here has been run against a
real board.

Requires: pip install pyserial   (not needed with --simulate)
"""

import argparse
import hmac
import json
import re
import secrets
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_MAP = HERE.parent / "AGAP_HelperButton" / "button_map.json"
BAUD = 115200

MAX_BODY = 2048
MAX_SEQUENCE = 16
# AGAP_Fretboard.ino keeps a sequence in a 12-step array (MAX_PROG) and answers "ERR too many chords" beyond it.
# The earlier helper build streams its sequence and has no such array.
MAX_SEQUENCE_FRETBOARD = 12
BPM_RANGE = (20, 200)
MIN_COMMAND_GAP_S = 0.15
CALIB_MAX_HOLD_MS = 2000
CALIB_MAX_REPS = 10
CALIB_MIN_GAP_MS = 500
LABEL_RE = re.compile(r"^[A-Za-z0-9]{1,8}$")
# Fretboard mode: the board solves chord names itself, so a name is sent as typed
# (CM7 is not Cm7). Only characters a chord name can contain are allowed, ASCII only.
# fullmatch, not "^...$": "$" also matches just before a trailing newline.
CHORD_NAME_RE = re.compile(r"[A-Ga-g][#b]?[A-Za-z0-9+\-]{0,10}")


class CommandError(Exception):
    """A rejected request. `status` is the HTTP status to return."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


class Config:
    def __init__(self, allow_calib=False, allow_unconfirmed=False, fretboard=False):
        self.allow_calib = allow_calib
        # Helper build: allow buttons not marked confirmed. Fretboard build: there are no
        # per-button confirmations, so this is "I accept driving hardware that has not
        # been checked locally"; without it nothing but STOP and RELEASE is accepted.
        self.allow_unconfirmed = allow_unconfirmed
        self.fretboard = fretboard


def load_buttons(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    buttons = []
    for b in data["buttons"]:
        label = b["label"]
        if not LABEL_RE.match(label):
            raise ValueError(f"unsafe label in button map: {label!r}")
        buttons.append({"label": label, "chord": b.get("chord"), "confirmed": bool(b.get("confirmed"))})
    return buttons


# ------------------------------- links -------------------------------

class SerialLink:
    """The real Mega, over USB serial."""

    def __init__(self, port, baud=BAUD):
        try:
            import serial
        except ImportError:
            sys.exit("Missing dependency. Install it with:  pip install pyserial")
        self.ser = serial.Serial(port, baud, timeout=0.2)
        time.sleep(2.0)  # the Mega resets when the port opens

    def write_line(self, line):
        self.ser.write((line + "\n").encode("ascii"))
        self.ser.flush()

    def read_line(self):
        raw = self.ser.readline()
        return raw.decode(errors="replace").rstrip() if raw else None

    def close(self):
        self.ser.close()


class ProcessLink:
    """Talks to a program on its stdin / stdout as if it were the board's serial port. Used with
    AGAP_Fretboard/sim/sim_serve.cpp, which runs the REAL sketch on this computer against a mock
    Arduino, so the bridge (and the phone page) can be tried against the firmware's own code
    without hardware. Same limits as any simulation: no real timing, no electronics."""

    def __init__(self, argv):
        import subprocess
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, bufsize=0)
        self._wlock = threading.Lock()

    def write_line(self, line):
        with self._wlock:
            try:
                self.proc.stdin.write((line + "\n").encode())
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError) as e:
                raise ConnectionError("simulated firmware is not running") from e

    def read_line(self):
        raw = self.proc.stdout.readline()
        if not raw:
            raise ConnectionError("simulated firmware exited")
        return raw.decode("utf-8", "replace").strip()

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        try:
            self.proc.stdout.close()
        except Exception:
            pass


class SimLink:
    """A stand-in for development without a board. It is NOT an emulation of the firmware's
    timing or state. With fretboard=True it answers CHORD / SHOW / SEQUENCE with a fingering line in
    the firmware's own format ('Bm -> x 2 0 4 0 2  (cost 9)'), solved by ChordAI over frets 0-5,
    so the phone page has something real to draw. Otherwise it just echoes."""

    def __init__(self, fretboard=False):
        self.written = []
        self._replies = deque()
        self._cv = threading.Condition()
        self._closed = False
        self._solve = _load_solver() if fretboard else None

    def _fingering(self, name):
        try:
            v = self._solve(name, 5)
        except (ValueError, KeyError):
            return "ERR unknown chord"
        shown = " ".join("x" if f < 0 else str(f) for f in v.frets)
        return "%s -> %s (cost %d)" % (name, shown + " ", v.cost)   # frets end with a space, as the firmware prints them

    def write_line(self, line):
        self.written.append(line)
        parts = line.split()
        word = parts[0] if parts else ""
        if self._solve and word in ("CHORD", "SHOW") and len(parts) == 2:
            replies = [self._fingering(parts[1])]
        elif self._solve and word == "SEQUENCE":
            replies = [self._fingering(n) for n in parts[1:]]
        else:
            replies = [{"STOP": "STOPPED", "PRESS": "PRESSED " + line[6:], "RELEASE": "RELEASED " + line[8:],
                        "CHORD": line, "SEQUENCE": "STEP " + line[9:], "CALIB": line}.get(word, "OK " + line)]
        with self._cv:
            self._replies.extend(replies)
            self._cv.notify()

    def read_line(self):
        with self._cv:
            if not self._replies and not self._closed:
                self._cv.wait(0.2)
            return self._replies.popleft() if self._replies else None

    def close(self):
        self._closed = True


def _load_solver():
    """ChordAI's solver, or None if the ChordAI folder is not next to this one (then SimLink echoes)."""
    sys.path.insert(0, str(HERE.parent / "ChordAI"))
    try:
        import chord_ai
    except ImportError:
        return None
    return chord_ai.solve


# ------------------------------- bridge -------------------------------

class Bridge:
    def __init__(self, link, buttons, config=None, clock=time.monotonic):
        self.link = link
        self.buttons = buttons
        self.cfg = config or Config()
        self.clock = clock
        self.log = deque(maxlen=200)
        self.connected = True
        self._wlock = threading.Lock()
        self._busy_until = 0.0
        self._last_cmd = -1e9
        self._reader = threading.Thread(target=self._read_loop, daemon=True)

    def start(self):
        self._reader.start()

    def _read_loop(self):
        while self.connected:
            try:
                line = self.link.read_line()
            except Exception as e:  # port unplugged, etc.
                self.connected = False
                self.log.append((time.time(), f"! serial error: {e}"))
                return
            if line:
                self.log.append((time.time(), line))

    def _write(self, line):
        with self._wlock:
            self.link.write_line(line)

    # ---- request validation ----
    def resolve(self, token):
        t = str(token).strip()
        if not t or len(t) > 16:
            raise CommandError(400, "bad chord/label")
        up = t.upper()
        for b in self.buttons:
            if b["label"].upper() == up:
                return b
        for b in self.buttons:
            if b["chord"] and b["chord"] == t:
                return b
        raise CommandError(400, f"unknown chord/label '{t[:16]}'")

    def _check_confirmed(self, b):
        if not b["confirmed"] and not self.cfg.allow_unconfirmed:
            raise CommandError(403, f"button {b['label']} is not marked confirmed in button_map.json "
                                    f"(start the bridge with --allow-unconfirmed to override)")

    @staticmethod
    def _int(payload, key, default, lo, hi):
        v = payload.get(key, default)
        if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
            raise CommandError(400, f"{key} must be an integer from {lo} to {hi}")
        return v

    def execute(self, payload):
        """Validate and run one command. Returns the dict to send back."""
        if not isinstance(payload, dict):
            raise CommandError(400, "body must be a JSON object")
        action = payload.get("action")
        if not self.connected:
            raise CommandError(503, "serial link is down")

        if action == "stop":  # always allowed, never rate-limited or blocked by busy
            self._write("STOP")
            self._busy_until = 0.0
            return {"sent": ["STOP"]}

        now = self.clock()
        if now < self._busy_until:
            raise CommandError(409, f"busy for about {self._busy_until - now:.1f}s more; send stop to cancel")
        if now - self._last_cmd < MIN_COMMAND_GAP_S:
            raise CommandError(429, "too many commands, slow down")

        lines, busy_s = self._build(action, payload)
        for line in lines:
            self._write(line)
        self._last_cmd = now
        self._busy_until = now + busy_s
        return {"sent": lines}

    # ---- fretboard mode ----
    def _need_untested_ok(self):
        if not self.cfg.allow_unconfirmed:
            raise CommandError(403, "this hardware has not been checked locally yet "
                                    "(start the bridge with --allow-unconfirmed to override)")

    @staticmethod
    def _chord_name(v):
        if not isinstance(v, str) or not CHORD_NAME_RE.fullmatch(v):
            raise CommandError(400, "not a chord name (letters A-G, optional # or b, then letters/digits)")
        return v

    def _string_fret(self, p):
        return (self._int(p, "string", None, 1, 6), self._int(p, "fret", None, 1, 5))

    def _build_fret(self, action, p):
        if action == "chord":
            self._need_untested_ok()
            return ["CHORD " + self._chord_name(p.get("target"))], 0.0
        if action == "strum":
            self._need_untested_ok()
            d = str(p.get("direction", "D")).upper()
            if d not in ("D", "U"):
                raise CommandError(400, "direction must be D or U")
            return ["STRUM " + d], 0.0
        if action == "sequence":
            self._need_untested_ok()
            targets = p.get("targets")
            if not isinstance(targets, list) or not 1 <= len(targets) <= MAX_SEQUENCE_FRETBOARD:
                raise CommandError(400, "targets must be a list of 1 to %d chords" % MAX_SEQUENCE_FRETBOARD)
            bpm = self._int(p, "bpm", 50, *BPM_RANGE)
            names = [self._chord_name(t) for t in targets]
            return ["TEMPO %d" % bpm, "SEQUENCE " + " ".join(names)], len(names) * 60.0 / bpm + 2.0
        if action == "press":
            self._need_untested_ok()
            string, fret = self._string_fret(p)
            return ["PRESS %d %d" % (string, fret)], 0.0
        if action == "release":  # letting go is always allowed
            if str(p.get("target", "")).strip().upper() == "ALL":
                return ["RELEASE ALL"], 0.0
            string, fret = self._string_fret(p)
            return ["RELEASE %d %d" % (string, fret)], 0.0
        if action == "calib":
            if not self.cfg.allow_calib:
                raise CommandError(403, "calib is disabled (start the bridge with --allow-calib)")
            self._need_untested_ok()
            string, fret = self._string_fret(p)
            hold = self._int(p, "hold_ms", 1000, 100, CALIB_MAX_HOLD_MS)
            reps = self._int(p, "reps", 3, 1, CALIB_MAX_REPS)
            gap = self._int(p, "gap_ms", 800, CALIB_MIN_GAP_MS, 10000)
            return ["CALIB %d %d %d %d %d" % (string, fret, hold, reps, gap)], reps * (hold + gap) / 1000.0 + 2.0
        raise CommandError(400, "unknown action (allowed: chord, press, release, strum, sequence, calib, stop)")

    def _build(self, action, p):
        if self.cfg.fretboard:
            return self._build_fret(action, p)
        if action in ("chord", "press"):
            b = self.resolve(p.get("target", ""))
            self._check_confirmed(b)
            return [f"{action.upper()} {b['label']}"], 0.0
        if action == "release":
            if str(p.get("target", "")).strip().upper() == "ALL":
                return ["RELEASE ALL"], 0.0
            return [f"RELEASE {self.resolve(p.get('target', ''))['label']}"], 0.0
        if action == "strum":
            d = str(p.get("direction", "D")).upper()
            if d not in ("D", "U"):
                raise CommandError(400, "direction must be D or U")
            return [f"STRUM {d}"], 0.0
        if action == "sequence":
            targets = p.get("targets")
            if not isinstance(targets, list) or not 1 <= len(targets) <= MAX_SEQUENCE:
                raise CommandError(400, f"targets must be a list of 1 to {MAX_SEQUENCE} chords")
            bpm = self._int(p, "bpm", 50, *BPM_RANGE)
            labels = []
            for t in targets:
                b = self.resolve(t)
                self._check_confirmed(b)
                labels.append(b["label"])
            return [f"TEMPO {bpm}", "SEQUENCE " + " ".join(labels)], len(labels) * 60.0 / bpm + 2.0
        if action == "calib":
            if not self.cfg.allow_calib:
                raise CommandError(403, "calib is disabled (start the bridge with --allow-calib)")
            b = self.resolve(p.get("target", ""))
            hold = self._int(p, "hold_ms", 1000, 100, CALIB_MAX_HOLD_MS)
            reps = self._int(p, "reps", 3, 1, CALIB_MAX_REPS)
            gap = self._int(p, "gap_ms", 800, CALIB_MIN_GAP_MS, 10000)
            return [f"CALIB {b['label']} {hold} {reps} {gap}"], reps * (hold + gap) / 1000.0 + 2.0
        raise CommandError(400, "unknown action (allowed: chord, press, release, strum, sequence, calib, stop)")

    # ---- read side ----
    def status(self):
        now = self.clock()
        return {
            "connected": self.connected,
            "busy": now < self._busy_until,
            "allow_calib": self.cfg.allow_calib,
            "allow_unconfirmed": self.cfg.allow_unconfirmed,
            "fretboard": self.cfg.fretboard,
            "max_sequence": MAX_SEQUENCE_FRETBOARD if self.cfg.fretboard else MAX_SEQUENCE,
            "buttons": [{"label": b["label"], "chord": b["chord"], "confirmed": b["confirmed"]} for b in self.buttons],
        }

    def recent_log(self, n=50):
        return [{"t": t, "line": line} for t, line in list(self.log)[-n:]]

    def shutdown(self):
        try:
            self._write("STOP")
        except Exception:
            pass
        self.connected = False
        try:
            self.link.close()
        except Exception:
            pass


# -------------------------------- HTTP --------------------------------

# The only files the bridge will serve without a token: the phone page and its three parts.
# Nothing else on disk is reachable, whatever path is asked for.
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/ui.css": ("ui.css", "text/css; charset=utf-8"),
    "/ui_logic.js": ("ui_logic.js", "text/javascript; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
}
# The page loads nothing from outside and runs no inline code, so the browser is told to refuse both.
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
       "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")


def make_handler(bridge, token):
    static = {}
    for url, (name, ctype) in STATIC_FILES.items():
        f = HERE / name
        static[url] = (f.read_bytes(), ctype) if f.exists() else None
    if static["/"] is None:
        static["/"] = (b"<h1>AGAP bridge</h1>", "text/html; charset=utf-8")

    class Handler(BaseHTTPRequestHandler):
        server_version = "AGAPBridge"

        def log_message(self, fmt, *args):  # keep the console quiet and token-free
            pass

        def _send(self, status, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", CSP)
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()
            self.wfile.write(data)

        def _authorized(self):
            header = self.headers.get("Authorization", "")
            given = header[7:] if header.startswith("Bearer ") else ""
            if hmac.compare_digest(given.encode(), token.encode()):
                return True
            time.sleep(0.5)  # slow down guessing
            self._send(401, {"error": "missing or wrong token"})
            return False

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in static and static[path] is not None:
                body, ctype = static[path]
                return self._send(200, body, ctype)
            if not self._authorized():
                return
            if path == "/api/status":
                return self._send(200, bridge.status())
            if path == "/api/log":
                return self._send(200, {"log": bridge.recent_log()})
            self._send(404, {"error": "not found"})

        def do_POST(self):
            if self.path.split("?", 1)[0] != "/api/command":
                return self._send(404, {"error": "not found"})
            if not self._authorized():
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                return self._send(400, {"error": "bad Content-Length"})
            if length > MAX_BODY:
                return self._send(413, {"error": "body too large"})
            try:
                payload = json.loads(self.rfile.read(length) or b"{}")
                self._send(200, bridge.execute(payload))
            except json.JSONDecodeError:
                self._send(400, {"error": "body is not valid JSON"})
            except CommandError as e:
                self._send(e.status, {"error": e.message})

    return Handler


def make_server(bridge, token, host="127.0.0.1", port=8080):
    return ThreadingHTTPServer((host, port), make_handler(bridge, token))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="serial port of the Mega, e.g. COM5 or /dev/ttyACM0")
    src.add_argument("--simulate", action="store_true", help="no board: use a stand-in link for development")
    src.add_argument("--sim-exe", type=Path,
                     help="no board: run the REAL sketch on this computer (build AGAP_Fretboard/sim/sim_serve.cpp; "
                          "see its header), so you try the page against the firmware's own code")
    ap.add_argument("--host", default="127.0.0.1", help="address to listen on (default 127.0.0.1)")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--map", type=Path, default=DEFAULT_MAP)
    ap.add_argument("--token", help="API token (or set AGAP_BRIDGE_TOKEN); generated if omitted")
    ap.add_argument("--fretboard", action="store_true",
                    help="drive the final 30-solenoid firmware (chord names), not the earlier chord-helper build")
    ap.add_argument("--allow-calib", action="store_true")
    ap.add_argument("--allow-unconfirmed", action="store_true",
                    help="allow buttons not marked confirmed in button_map.json (testing)")
    args = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)   # the token must show at once, even when output is piped

    import os
    token = args.token or os.environ.get("AGAP_BRIDGE_TOKEN") or secrets.token_urlsafe(24)
    if len(token) < 16:
        sys.exit("token must be at least 16 characters")

    if args.sim_exe:
        link = ProcessLink([str(args.sim_exe), "--speed", "1"])
    else:
        link = SimLink(fretboard=args.fretboard) if args.simulate else SerialLink(args.port)
    buttons = [] if args.fretboard else load_buttons(args.map)
    bridge = Bridge(link, buttons, Config(args.allow_calib, args.allow_unconfirmed, args.fretboard))
    bridge.start()
    try:
        server = make_server(bridge, token, args.host, args.http_port)
    except OSError as e:
        bridge.shutdown()
        sys.exit("Cannot listen on %s port %d (%s). Something else is probably using it. "
                 "Pick another port with --http-port, for example --http-port %d."
                 % (args.host, args.http_port, e.strerror or e, args.http_port + 1))

    where = "REAL FIRMWARE ON THIS PC (no hardware)" if args.sim_exe else ("SIMULATED" if args.simulate else args.port)
    print(f"AGAP bridge on http://{args.host}:{args.http_port}  ({where})")
    print(f"Token: {token}")
    if args.host not in ("127.0.0.1", "localhost"):
        print("WARNING: listening beyond localhost with no TLS. Put it behind a VPN/tunnel.")
    if args.fretboard and not args.allow_unconfirmed:
        print("Fretboard mode: only STOP and RELEASE are accepted until you pass --allow-unconfirmed "
              "(the hardware has not been checked locally).")
    elif not args.allow_unconfirmed:
        print("Only buttons marked confirmed in button_map.json will be accepted.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        print("Sending STOP and closing.")
        server.server_close()
        bridge.shutdown()


if __name__ == "__main__":
    main()
