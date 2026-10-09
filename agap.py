#!/usr/bin/env python3
"""
AGAP launcher - one place to check, test, flash and run the robot.

    python agap.py              menu (or double-click agap.bat on Windows)
    python agap.py doctor       read-only check of the whole connection
    python agap.py bringup      guided first power-up, one channel at a time
    python agap.py console      type commands and watch replies; Ctrl+C = STOP
    python agap.py bridge       start the phone/remote bridge
    python agap.py flash        compile and upload the firmware
    python agap.py selftest     run every test suite on this computer
    python agap.py ports        list serial ports

The board is found automatically; pass --port COMx to override.
Every session is recorded in logs/ (what was sent, what the board said), so
if something misbehaves you have an exact transcript to look at.

Needs Python 3.8+ and `pip install pyserial` (not needed for `selftest`).
Nothing here has been run against a real board yet.
"""

import argparse
import csv
import datetime
import json
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import namedtuple
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LOG_DIR = ROOT / "logs"
MAP_PATH = ROOT / "AGAP_HelperButton" / "button_map.json"
BAUD = 115200
FQBN = "arduino:avr:mega"
SKETCHES = {"fretboard": ROOT / "AGAP_Fretboard", "helper": ROOT / "AGAP_HelperButton"}
SKETCH = SKETCHES["fretboard"]  # the final design; "helper" is the earlier chord-helper build
FW_NAME = "AGAP-helper-button"
FRET_FW_NAME = "AGAP-fretboard"
FIRST_WAIT = 1.0   # seconds to wait for the first reply line
QUIET = 0.4        # then until the board has been silent this long

# Mega pins for button channels 0-9 and servos for strings 6..1. These mirror
# the pin-map comment at the top of AGAP_HelperButton.ino (a test checks it).
CHANNEL_PINS = ["D22", "D23", "D24", "D25", "D26", "D27", "D28", "D29", "D37", "D36"]


def fret_channel(string_number, fret):
    """Solenoid channel for a string (1 = high e ... 6 = low E) and fret 1-5.
    One plate (one fret) owns six consecutive channels; see AGAP_Fretboard/agap_fret.h."""
    return (fret - 1) * 6 + (6 - string_number)


def fret_channel_pin(channel):
    """The Mega pin for a fretboard channel (PORTA, PORTC, PORTL, then PORTK)."""
    if channel < 8:
        return "D%d" % (22 + channel)
    if channel < 16:
        return "D%d" % (37 - (channel - 8))
    if channel < 24:
        return "D%d" % (49 - (channel - 16))
    return "A%d" % (8 + channel - 24)


def firmware_kind(lines):
    """'fretboard', 'helper', or None, from what a board printed."""
    text = " ".join(lines)
    if FRET_FW_NAME in text or "(fretboard)" in text:
        return "fretboard"
    if FW_NAME in text or "(helper-button)" in text:
        return "helper"
    return None


def servo_pin(string_number):
    return "D%d" % (2 + (6 - string_number))  # string 6 -> D2 ... string 1 -> D7


# ------------------------------ button map ------------------------------

def load_buttons(path=MAP_PATH):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return [{"label": b["label"], "chord": b.get("chord"), "confirmed": bool(b.get("confirmed"))}
            for b in data["buttons"]]


def resolve(token, buttons):
    """'em' / 'Em' / 'EM' -> 'EM'. Labels match any case, chord names exactly."""
    up = token.strip().upper()
    for b in buttons:
        if b["label"].upper() == up:
            return b["label"]
    for b in buttons:
        if b["chord"] and b["chord"] == token.strip():
            return b["label"]
    return None


# ------------------------------ serial ports ------------------------------

PortInfo = namedtuple("PortInfo", "device description vid pid")
ARDUINO_VIDS = {0x2341, 0x2A03}   # Arduino LLC / Arduino.org
CLONE_VIDS = {0x1A86, 0x0403}      # CH340 and FTDI, common on Mega clones


def rank_ports(ports):
    """Returns (likely, others): likely = Arduino-looking ports, best first."""
    def score(p):
        desc = (p.description or "").lower()
        if p.vid in ARDUINO_VIDS or "arduino" in desc or "mega" in desc:
            return 2
        if p.vid in CLONE_VIDS or "ch340" in desc or "usb serial" in desc:
            return 1
        return 0
    ordered = sorted(ports, key=lambda p: (-score(p), p.device))
    likely = [p for p in ordered if score(p) > 0]
    others = [p for p in ordered if score(p) == 0]
    return likely, others


def list_system_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        raise SystemExit("pyserial is missing. Install it with:  pip install pyserial")
    return [PortInfo(p.device, p.description, p.vid, p.pid) for p in list_ports.comports()]


def choose_port(requested=None, ports=None, input_fn=input, print_fn=print):
    if requested:
        return requested
    ports = list_system_ports() if ports is None else ports
    if not ports:
        print_fn("No serial ports found. Is the Mega plugged in with a data USB cable?")
        return None
    likely, others = rank_ports(ports)
    if len(likely) == 1:
        print_fn("Using %s (%s)" % (likely[0].device, likely[0].description))
        return likely[0].device
    candidates = likely or others
    print_fn("Which port is the Mega?")
    for i, p in enumerate(candidates, 1):
        print_fn("  %d) %s  %s" % (i, p.device, p.description))
    ans = input_fn("Number [1]: ").strip() or "1"
    if ans.isdigit() and 1 <= int(ans) <= len(candidates):
        return candidates[int(ans) - 1].device
    print_fn("Not a valid choice.")
    return None


def explain_serial_error(exc, port):
    """Turns a low-level serial error into what to do about it."""
    text = str(exc).lower()
    if "access is denied" in text or "permission" in text or "busy" in text:
        return ("%s is in use by another program. Close the Arduino IDE Serial Monitor "
                "(and any other terminal or the bridge) and try again." % port)
    if "could not open port" in text or "no such file" in text or "cannot find" in text or "filenotfound" in text:
        return ("%s was not found. Check the USB cable (it must carry data, not just power), "
                "then run `python agap.py ports` to see what is connected." % port)
    return "Could not open %s: %s" % (port, exc)


class SerialLink:
    """The real board. read_line() waits up to ~0.2 s and returns None if idle."""

    def __init__(self, port, baud=BAUD):
        try:
            import serial
        except ImportError:
            raise SystemExit("pyserial is missing. Install it with:  pip install pyserial")
        try:
            self.ser = serial.Serial(port, baud, timeout=0.2)
        except Exception as e:  # SerialException, OSError ...
            raise ConnectionError(explain_serial_error(e, port)) from e

    def write_line(self, line):
        self.ser.write((line + "\n").encode("ascii", "replace"))
        self.ser.flush()

    def read_line(self):
        raw = self.ser.readline()
        return raw.decode(errors="replace").rstrip() if raw else None

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


# ------------------------------ session ------------------------------

class Session:
    """A link plus a background reader and a transcript file."""

    def __init__(self, link, log_path=None, echo=None):
        self.link = link
        self.echo = echo
        self.lines = queue.Queue()
        self.lost = None
        self._done = False
        self._log = open(log_path, "a", encoding="utf-8") if log_path else None
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _write_log(self, mark, text):
        if self._log:
            self._log.write("%s %s %s\n" % (datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3], mark, text))
            self._log.flush()

    def _read(self):
        while not self._done:
            try:
                line = self.link.read_line()
            except Exception as e:
                self.lost = str(e)
                self._write_log("!", "connection lost: %s" % e)
                if self.echo:
                    self.echo("!! connection to the board was lost: %s" % e)
                return
            if line:
                self._write_log("<", line)
                if self.echo:
                    self.echo(line)
                else:
                    self.lines.put(line)

    def send(self, line):
        self._write_log(">", line)
        self.link.write_line(line)

    def collect(self, first_wait=None, quiet=None):
        """Reply lines: waits up to first_wait for the first, then until quiet."""
        first_wait = FIRST_WAIT if first_wait is None else first_wait
        quiet = QUIET if quiet is None else quiet
        out = []
        wait = first_wait
        while True:
            try:
                out.append(self.lines.get(timeout=wait))
            except queue.Empty:
                return out
            wait = quiet

    def ask(self, line, first_wait=None, quiet=None):
        self.drain()
        self.send(line)
        return self.collect(first_wait, quiet)

    def drain(self):
        while True:
            try:
                self.lines.get_nowait()
            except queue.Empty:
                return

    def stop_and_close(self):
        try:
            self.send("STOP")
            time.sleep(0.1)
        except Exception:
            pass
        self._done = True
        try:
            self.link.close()
        except Exception:
            pass
        if self._log:
            self._log.close()


def new_log_path(kind):
    LOG_DIR.mkdir(exist_ok=True)
    return LOG_DIR / ("%s-%s.log" % (kind, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))


def open_session(port, kind, echo=None, boot_wait=2.0):
    """Opens the port. A Mega resets when the port opens, so wait out its boot."""
    link = SerialLink(port)
    session = Session(link, new_log_path(kind), echo)
    time.sleep(boot_wait)
    return session


# ------------------------------ doctor ------------------------------

Check = namedtuple("Check", "status title detail hint")  # status: ok / warn / fail


def diagnose(boot_lines, session, buttons):
    """Read-only checks against a connected board. Never moves anything."""
    checks = []
    add = lambda *a: checks.append(Check(*a))

    banner = " ".join(boot_lines)
    ver_lines = session.ask("VERSION")
    version = next((l for l in ver_lines if FW_NAME in l or FRET_FW_NAME in l), None)
    is_agap = ("AGAP" in banner) or version is not None

    if not boot_lines and not ver_lines:
        add("fail", "Board reply", "Nothing came back.",
            "Wrong port, wrong baud (the firmware uses 115200), or no sketch flashed yet. "
            "Try `python agap.py flash`, or check the cable and port.")
        return checks
    if not is_agap:
        add("fail", "Board reply", "Got: %s" % (" | ".join((boot_lines + ver_lines)[:2])),
            "The board answers but is not running the AGAP firmware. Flash it with `python agap.py flash`.")
        return checks
    add("ok", "Board reply", "The AGAP firmware is answering.", "")

    if firmware_kind(boot_lines + ver_lines) == "fretboard":
        return checks + diagnose_fretboard(banner, ver_lines, version, session)

    if version:
        add("ok", "Firmware version", version, "")
    else:
        add("warn", "Firmware version", "VERSION was not understood.",
            "This is an older build. Re-flash with `python agap.py flash` to get the current one.")

    labels = session.ask("LABELS")
    fw_labels = labels[0].split() if labels else []
    want = [b["label"] for b in buttons]
    if fw_labels == want:
        add("ok", "Button labels", "Firmware and button_map.json agree (%d buttons)." % len(want), "")
    else:
        add("fail", "Button labels", "firmware: %s | button_map.json: %s" % (" ".join(fw_labels) or "(none)", " ".join(want)),
            "They must match in order. Edit button_map.json, or BUTTON_LABEL[] in the sketch, then re-flash.")

    status = session.ask("STATUS")
    if any("bpm=" in l for l in status):
        add("ok", "Status", " / ".join(status), "")
    else:
        add("warn", "Status", "STATUS gave: %s" % (" | ".join(status) or "nothing"), "")

    picks = session.ask("PICKS")
    if len([l for l in picks if l.startswith("string ")]) == 6:
        add("ok", "Pick arms", "All 6 pick angle pairs reported.", "")
    else:
        add("warn", "Pick arms", "Expected 6 lines, got %d." % len(picks), "Re-flash if this is an older build.")

    if "Loaded saved tuning" in banner:
        add("ok", "Tuning", "Saved tuning loaded from EEPROM.", "")
    elif "No saved tuning" in banner:
        add("ok", "Tuning", "No saved tuning yet - using the compiled placeholder values.", "")

    unconfirmed = [b["label"] for b in buttons if not b["confirmed"]]
    if unconfirmed:
        add("warn", "Chord chart", "%d buttons are not marked confirmed in button_map.json." % len(unconfirmed),
            "Fine for testing. Check each against the real helper, then set \"confirmed\": true.")
    return checks


def diagnose_fretboard(banner, ver_lines, version, session):
    """Read-only checks for the 30-solenoid fretboard firmware. Sends only
    VERSION, FRETS, SHOW, STATUS and PICKS, none of which moves anything."""
    checks = []
    add = lambda *a: checks.append(Check(*a))

    add("ok", "Firmware version", version or "VERSION answered", "")

    frets = session.ask("FRETS")
    if any("30 channels" in l for l in frets):
        add("ok", "Channels", "6 strings x 5 frets = 30 solenoid channels.", "")
    else:
        add("fail", "Channels", "firmware says: %s" % (" | ".join(frets) or "nothing"),
            "The printed design has 30 solenoids (five six-socket plates). Re-flash `python agap.py flash`.")

    shown = session.ask("SHOW Bm")
    if any("x 2 0 4 0 2" in l for l in shown):
        add("ok", "On-board chord search", "Bm gives x 2 0 4 0 2, the known answer with frets 0-5.", "")
    else:
        add("fail", "On-board chord search", "SHOW Bm gave: %s" % (" | ".join(shown) or "nothing"),
            "Expected x 2 0 4 0 2. The chord search on the board disagrees with ChordAI; re-flash, "
            "and if it persists send logs/ to whoever maintains the firmware.")

    status = session.ask("STATUS")
    if any("bpm=" in l for l in status):
        add("ok", "Status", " / ".join(status), "")
    else:
        add("warn", "Status", "STATUS gave: %s" % (" | ".join(status) or "nothing"), "")

    picks = session.ask("PICKS")
    if len([l for l in picks if l.startswith("string ")]) == 6:
        add("ok", "Pick arms", "All 6 pick angle pairs reported.", "")
    else:
        add("warn", "Pick arms", "Expected 6 lines, got %d." % len(picks), "Re-flash if this is an older build.")

    if "Loaded saved tuning" in banner:
        add("ok", "Tuning", "Saved tuning loaded from EEPROM.", "")
    elif "No saved tuning" in banner:
        add("ok", "Tuning", "No saved tuning yet - using the compiled placeholder values.", "")
    return checks


def print_checks(checks, print_fn=print):
    icons = {"ok": "[ OK ]", "warn": "[WARN]", "fail": "[FAIL]"}
    for c in checks:
        print_fn("%s %s - %s" % (icons[c.status], c.title, c.detail))
        if c.hint and c.status != "ok":
            print_fn("         -> %s" % c.hint)
    fails = sum(1 for c in checks if c.status == "fail")
    print_fn("")
    print_fn("Result: %s" % ("PROBLEMS FOUND" if fails else "looks good - nothing was moved by this check"))
    return fails == 0


def cmd_doctor(args):
    buttons = load_buttons()
    print("AGAP doctor (read-only: it never moves anything)")
    port = choose_port(args.port)
    if not port:
        return 1
    try:
        session = open_session(port, "doctor")
    except ConnectionError as e:
        print("[FAIL] Open port - %s" % e)
        return 1
    try:
        boot = session.collect(first_wait=1.5, quiet=0.8)
        ok = print_checks(diagnose(boot, session, buttons))
        print("Transcript: logs/ (latest doctor-*.log)")
        return 0 if ok else 1
    finally:
        session.stop_and_close()


# ------------------------------ console ------------------------------

VERBS_WITH_LABEL = {"CHORD", "PRESS", "RELEASE", "CALIB"}


CHORD_TEXT = re.compile(r"^[A-Za-z0-9#+\-]{1,12}$")


def translate_fretboard(words):
    """Fretboard console input. The board solves chord names itself, so names
    pass through with their spelling (Em vs EM matters, and CM7 is not Cm7);
    only characters a chord name can contain are allowed, so nothing else can
    be smuggled into the command line."""
    verb = words[0].upper()
    if verb in ("SEQ", "SEQUENCE"):
        bpm = None
        rest = words[1:]
        if len(rest) >= 2 and rest[-2].lower() == "bpm" and rest[-1].isdigit():
            bpm, rest = int(rest[-1]), rest[:-2]
        if not rest:
            return [], "usage: seq C G Am F [bpm 60]"
        for t in rest:
            if not CHORD_TEXT.match(t):
                return [], "'%s' is not a chord name" % t[:20]
        return (["TEMPO %d" % bpm] if bpm else []) + ["SEQUENCE " + " ".join(rest)], None
    if verb in ("CHORD", "SHOW") and len(words) >= 2:
        if len(words) != 2 or not CHORD_TEXT.match(words[1]):
            return [], "'%s' is not a chord name" % " ".join(words[1:])[:20]
        return ["%s %s" % (verb, words[1])], None
    if verb == "RELEASE" and len(words) == 2 and words[1].upper() == "ALL":
        return ["RELEASE ALL"], None
    if verb == "PROG":
        for t in words[1:]:
            if not CHORD_TEXT.match(t):
                return [], "'%s' is not a chord name" % t[:20]
    return [" ".join([verb] + words[1:])], None


def translate(line, buttons, kind="helper"):
    """Friendly input -> firmware lines. Returns (lines, error)."""
    words = line.strip().split()
    if not words:
        return [], None
    if kind == "fretboard":
        return translate_fretboard(words)
    verb = words[0].upper()
    if verb in ("SEQ", "SEQUENCE"):
        bpm = None
        rest = words[1:]
        if len(rest) >= 2 and rest[-2].lower() == "bpm" and rest[-1].isdigit():
            bpm, rest = int(rest[-1]), rest[:-2]
        if not rest:
            return [], "usage: seq C G Am F [bpm 60]"
        labels = []
        for t in rest:
            lab = resolve(t, buttons)
            if not lab:
                return [], "unknown chord/label '%s'" % t
            labels.append(lab)
        return (["TEMPO %d" % bpm] if bpm else []) + ["SEQUENCE " + " ".join(labels)], None
    if verb in VERBS_WITH_LABEL and len(words) >= 2:
        if verb == "RELEASE" and words[1].upper() == "ALL":
            return ["RELEASE ALL"], None
        lab = resolve(words[1], buttons)
        if not lab:
            return [], "unknown chord/label '%s' (labels: %s)" % (words[1], " ".join(b["label"] for b in buttons))
        return [" ".join([verb, lab] + words[2:])], None
    return [" ".join([verb] + words[1:])], None


CONSOLE_HELP = """Type a command and press Enter. Examples:
  chord Em            press the Em button and strum
  press EM / release EM / release all
  seq C G Am F bpm 60 play a progression
  stop                release everything now
  picks / pick 6 A 70 / pluck 6     pick-arm tuning     (see docs/TUNING_GUIDE.md)
  kick 80 / hold 30 / tempo 50 / save    tuning
  status / version / help             (anything else goes to the firmware as typed)
  quit or Ctrl+C      stop everything and leave"""


FRET_CONSOLE_HELP = """Type a command and press Enter. Examples:
  chord Em            solve the fingering, press the solenoids, strum
  show Bm             print the fingering only (moves nothing)
  seq C G Am F bpm 60 play a progression
  raw x 3 2 0 1 0     press an explicit fingering (frets 0-5, x = muted)
  press 6 1 / release 6 1 / release all    one solenoid: string 6 = low E, fret 1-5
  stop                release everything now
  picks / pick 6 A 70 / pluck 6     pick-arm tuning     (see docs/TUNING_GUIDE.md)
  kick 80 / hold 30 / tempo 50 / save    tuning
  status / version / frets / help     (anything else goes to the firmware as typed)
  quit or Ctrl+C      stop everything and leave"""


def run_console(session, buttons, input_fn=input, print_fn=print, kind="helper"):
    print_fn(FRET_CONSOLE_HELP if kind == "fretboard" else CONSOLE_HELP)
    while True:
        try:
            line = input_fn("agap> ")
        except (EOFError, KeyboardInterrupt):
            print_fn("")
            return
        if line.strip().lower() in ("quit", "exit", "q"):
            return
        if line.strip().lower() == "?":
            print_fn(FRET_CONSOLE_HELP if kind == "fretboard" else CONSOLE_HELP)
            continue
        lines, err = translate(line, buttons, kind)
        if err:
            print_fn("  ! " + err)
            continue
        for l in lines:
            session.send(l)
            time.sleep(0.05)


def cmd_console(args):
    buttons = load_buttons()
    port = choose_port(args.port)
    if not port:
        return 1
    seen = []

    def echo(line):
        seen.append(line)
        print("\r< " + line + "\nagap> ", end="", flush=True)

    try:
        session = open_session(port, "console", echo=echo)
    except ConnectionError as e:
        print(e)
        return 1
    try:
        kind = firmware_kind(seen) or "helper"
        run_console(session, buttons, kind=kind)
    finally:
        print("Stopping and closing...")
        session.stop_and_close()
    return 0


# ------------------------------ guided bring-up ------------------------------

SAFETY = """Before any power: read this.
  * Solenoids must be wired through drivers (MOSFET/relay + flyback diode),
    never straight to the Mega's pins.
  * Start with ONE solenoid connected, not touching the helper yet.
  * Keep your fingers clear of the pick arms.
  * STOP (panel button, or this program on Ctrl+C) releases everything.
  * Each button press below energises a coil for about a second."""


def channel_hint(label, buttons):
    idx = next(i for i, b in enumerate(buttons) if b["label"] == label)
    pin = CHANNEL_PINS[idx] if idx < len(CHANNEL_PINS) else "?"
    return ("No click or movement: check the 12 V supply is on, the driver's gate/input is wired to %s, "
            "the flyback diode is the right way round, and all grounds are joined. "
            "If the board answered 'PRESSED' the firmware side worked." % pin)


def fret_channel_hint(string_number, fret):
    pin = fret_channel_pin(fret_channel(string_number, fret))
    return ("No click or movement: check the 12 V supply is on, the driver for string %d fret %d is wired to %s, "
            "the flyback diode is the right way round, and all grounds are joined. "
            "If the board answered 'PRESSED' the firmware side worked." % (string_number, fret, pin))


def pick_hint(n):
    return ("The arm did not move: check the servo's separate 5-6 V supply, that grounds are joined, "
            "and the signal wire on %s. If it moved but hit something, set a safer angle with "
            "`pick %d A <deg>` / `pick %d B <deg>`." % (servo_pin(n), n, n))


def yes_no(question, input_fn):
    while True:
        a = input_fn(question + " [y/n] ").strip().lower()
        if a in ("y", "yes"):
            return True
        if a in ("n", "no"):
            return False


def run_fret_channels(session, boot_lines, input_fn, print_fn, record, results_path):
    """Step 3 for the 30-solenoid fretboard: one coil at a time, never two."""
    print_fn("\nStep 3/3: solenoids. Start with just one: string 6 (low E), fret 1.")
    order = [(string, fret) for fret in range(1, 6) for string in range(6, 0, -1)]  # plate by plate
    todo = [order[0]]
    done_first = False
    while todo:
        string, fret = todo.pop(0)
        item = "string %d fret %d" % (string, fret)
        print_fn("\nChannel %s (%s): pressing for about a second."
                 % (item, fret_channel_pin(fret_channel(string, fret))))
        session.ask("PRESS %d %d" % (string, fret))
        ok = yes_no("Did the solenoid for %s click / extend?" % item, input_fn)
        session.ask("RELEASE %d %d" % (string, fret))
        record("channel", item, "pass" if ok else "fail")
        if not ok:
            print_fn("  -> " + fret_channel_hint(string, fret))
            session.send("STOP")
            return False
        if not done_first:
            done_first = True
            if yes_no("Run a 3-press endurance test on %s (watch for heat)?" % item, input_fn):
                session.send("CALIB %d %d 1000 3 800" % (string, fret))
                input_fn("Press Enter when it has finished (or Ctrl+C to stop): ")
                hot = yes_no("Is the coil/driver too hot to keep a finger on?", input_fn)
                record("calib", item, "fail" if hot else "pass", "hot" if hot else "")
                if hot:
                    session.send("STOP")
                    print_fn("  -> Lower HOLD or KICK (docs/TUNING_GUIDE.md) before more testing.")
                    return False
            if yes_no("Test the other %d solenoids one by one, plate by plate?" % (len(order) - 1), input_fn):
                todo = order[1:]
    print_fn("\nBring-up finished. Results saved%s." % (" to %s" % results_path if results_path else ""))
    print_fn("Next: docs/TUNING_GUIDE.md. Remember `save` stores tuned values.")
    return True


def run_bringup(session, buttons, boot_lines, input_fn=input, print_fn=print, results_path=None):
    rows = []

    def record(step, item, result, note=""):
        rows.append([datetime.datetime.now().isoformat(timespec="seconds"), step, item, result, note])
        if results_path:
            with open(results_path, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["time", "step", "item", "result", "note"])
                w.writerows(rows)

    print_fn(SAFETY)
    if input_fn("\nType YES when the checklist above is true: ").strip().upper() != "YES":
        print_fn("Stopped. Nothing was moved.")
        return False

    print_fn("\nStep 1/3: connection check (read-only)")
    if not print_checks(diagnose(boot_lines, session, buttons), print_fn):
        record("doctor", "connection", "fail")
        print_fn("Fix the above first, then run bring-up again.")
        return False
    record("doctor", "connection", "pass")

    print_fn("\nStep 2/3: pick arms (strings 6 = low E ... 1 = high e)")
    if yes_no("Test the six pick arms now? Servos need their own 5-6 V supply on.", input_fn):
        for n in range(6, 0, -1):
            session.ask("PLUCK %d" % n)
            ok = yes_no("String %d: did the pick swing across its string and clear the next one?" % n, input_fn)
            record("pick", "string %d" % n, "pass" if ok else "fail")
            if not ok:
                session.send("STOP")
                print_fn("  -> " + pick_hint(n))
                if not yes_no("Continue with the remaining picks anyway?", input_fn):
                    return False
    else:
        record("pick", "all", "skipped")

    if firmware_kind(boot_lines) == "fretboard":
        return run_fret_channels(session, boot_lines, input_fn, print_fn, record, results_path)

    print_fn("\nStep 3/3: solenoid channels. Start with just the first one.")
    first = buttons[0]["label"]
    todo = [first]
    done_first = False
    while todo:
        label = todo.pop(0)
        print_fn("\nChannel %s: pressing for about a second." % label)
        session.ask("PRESS %s" % label)
        ok = yes_no("Did the solenoid for %s click / extend?" % label, input_fn)
        session.ask("RELEASE %s" % label)
        record("channel", label, "pass" if ok else "fail")
        if not ok:
            print_fn("  -> " + channel_hint(label, buttons))
            session.send("STOP")
            return False
        if not done_first:
            done_first = True
            if yes_no("Run a 3-press endurance test on %s (watch for heat)?" % label, input_fn):
                session.send("CALIB %s 1000 3 800" % label)
                input_fn("Press Enter when it has finished (or Ctrl+C to stop): ")
                hot = yes_no("Is the coil/driver too hot to keep a finger on?", input_fn)
                record("calib", label, "fail" if hot else "pass", "hot" if hot else "")
                if hot:
                    session.send("STOP")
                    print_fn("  -> Lower HOLD or KICK (docs/TUNING_GUIDE.md) before more testing.")
                    return False
            if yes_no("Test the remaining %d channels one by one?" % (len(buttons) - 1), input_fn):
                todo = [b["label"] for b in buttons[1:]]
    print_fn("\nBring-up finished. Results saved%s." % (" to %s" % results_path if results_path else ""))
    print_fn("Next: docs/TUNING_GUIDE.md. Remember `save` stores tuned values.")
    return True


def cmd_bringup(args):
    buttons = load_buttons()
    print("AGAP guided bring-up")
    port = choose_port(args.port)
    if not port:
        return 1
    try:
        session = open_session(port, "bringup")
    except ConnectionError as e:
        print(e)
        return 1
    try:
        boot = session.collect(first_wait=1.5, quiet=0.8)
        res = new_log_path("bringup-results").with_suffix(".csv")
        ok = run_bringup(session, buttons, boot, results_path=res)
        return 0 if ok else 1
    except KeyboardInterrupt:
        print("\nInterrupted - stopping everything.")
        return 1
    finally:
        session.stop_and_close()


# ------------------------------ flash / bridge / selftest ------------------------------

def find_arduino_cli():
    found = shutil.which("arduino-cli")
    if found:
        return found
    for p in (Path.home() / "arduino-cli" / "arduino-cli.exe", Path("C:/Program Files/Arduino CLI/arduino-cli.exe")):
        if p.exists():
            return str(p)
    return None


def flash_commands(cli, port, sketch="fretboard"):
    path = str(SKETCHES[sketch])
    return [
        [cli, "core", "install", "arduino:avr"],
        [cli, "lib", "install", "Servo"],
        [cli, "compile", "--fqbn", FQBN, path],
        [cli, "upload", "-p", port, "--fqbn", FQBN, path],
    ]


def cmd_flash(args):
    cli = find_arduino_cli()
    if not cli:
        print("arduino-cli was not found.\n"
              "Install it from https://arduino.github.io/arduino-cli/ (or use the Arduino IDE:\n"
              "open AGAP_HelperButton/AGAP_HelperButton.ino, Board = Arduino Mega or Mega 2560, Upload).")
        return 1
    port = choose_port(args.port)
    if not port:
        return 1
    print("Close the Serial Monitor and any other program using %s first." % port)
    sketch = getattr(args, "sketch", "fretboard")
    print("Flashing the %s firmware." % ("final fretboard (30 solenoids)" if sketch == "fretboard"
                                         else "earlier chord-helper"))
    for cmd in flash_commands(cli, port, sketch):
        print("\n$ " + " ".join(cmd))
        if subprocess.call(cmd) != 0:
            print("\nThat step failed. The message above says why; a common cause is the wrong port "
                  "or another program holding it open.")
            return 1
    print("\nFlashed. Now run:  python agap.py doctor")
    return 0


def bridge_command(args, port=None):
    """The command that starts the remote bridge. It drives the final 30-solenoid
    firmware unless --helper asks for the earlier chord-helper protocol."""
    cmd = [sys.executable, str(ROOT / "bridge" / "agap_bridge.py")]
    if not getattr(args, "helper", False):
        cmd.append("--fretboard")
    if args.simulate:
        cmd += ["--simulate", "--allow-unconfirmed"]
    else:
        cmd += ["--port", port]
        if args.allow_unconfirmed:
            cmd.append("--allow-unconfirmed")
    return cmd


def cmd_bridge(args):
    port = None
    if not args.simulate:
        port = choose_port(args.port)
        if not port:
            return 1
    cmd = bridge_command(args, port)
    print("Starting the bridge (Ctrl+C stops it and sends STOP to the board).")
    print("Open http://localhost:8080 and paste the token it prints.\n")
    try:
        return subprocess.call(cmd)
    except KeyboardInterrupt:
        return 0


def selftest_plan():
    """(name, working dir, python-module to run | None, C++ test source | None)."""
    py = [
        ("ChordAI tests", ROOT / "ChordAI", "test_chord_ai"),
        ("Remote bridge tests", ROOT / "bridge", "test_agap_bridge"),
        ("Launcher tests", ROOT / "tests", "test_agap"),
        ("G-code inspector tests", ROOT / "tools" / "tests", "test_hvs_inspect"),
        ("LH base fit checker tests", ROOT / "tools" / "tests", "test_lh_fit"),
        ("Central deck drawing tests", ROOT / "tools" / "tests", "test_deck_drawing"),
        ("STL inspector tests", ROOT / "tools" / "tests", "test_stl_inspect"),
    ]
    plan = [(n, cwd, mod, None) for n, cwd, mod in py]
    plan += [
        ("Firmware logic tests (C++)", ROOT / "AGAP_HelperButton" / "tests", None, "test_agap_logic.cpp"),
        ("Control tool tests (C++)", ROOT / "tools" / "tests", None, "test_agap_control_logic.cpp"),
        ("Fretboard chord search vs ChordAI (C++)", ROOT / "AGAP_Fretboard" / "tests", None, "test_agap_chords.cpp"),
        ("Fretboard channel mapping (C++)", ROOT / "AGAP_Fretboard" / "tests", None, "test_agap_fret.cpp"),
        ("Fretboard firmware simulation", ROOT / "AGAP_Fretboard" / "sim", None, "sim_fretboard.cpp"),
        ("Helper firmware simulation", ROOT / "AGAP_HelperButton" / "sim", None, "sim_helper.cpp"),
    ]
    return plan


def cpp_compile_command(gpp, source, out_exe):
    # -I.. so the test can include the header one level up, as the repo's run_tests.sh does.
    return [gpp, "-std=c++14", "-Wall", "-Wextra", "-I..", source, "-o", str(out_exe)]


def sim_compile_command(gpp, source, out_exe):
    """The real sketch is compiled on this computer against the mock Arduino in sim/mock."""
    return [gpp, "-std=c++14", "-include", "Arduino.h", "-I../../sim/mock", "-I..", source, "-o", str(out_exe)]


def run_sim(cwd, source, gpp, run):
    """Runs every scenario of a firmware simulation, each in its own process (each needs a fresh
    power-up). Returns (passed, summary line, full output) like run_suite."""
    if not gpp:
        return None, "skipped (g++ not found)", ""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "sim.exe"
        c = run(sim_compile_command(gpp, source, exe))
        if c.returncode != 0:
            return False, "did not compile", c.stdout
        names = [n.strip() for n in run([str(exe), "--list"]).stdout.splitlines() if n.strip()]
        failed, log = [], []
        for n in names:
            r = run([str(exe), n])
            if r.returncode != 0:
                failed.append(n)
                log.append(r.stdout)
        summary = "%d/%d scenarios passed" % (len(names) - len(failed), len(names))
        if failed:
            summary += " (failed: %s)" % ", ".join(failed)
        return not failed, summary, "\n".join(log)


def run_suite(name, cwd, module, cpp_src, gpp):
    """Returns (passed, last line, full output). Calls g++ directly: on Windows
    `bash` on PATH is often the WSL stub, which fails without a Linux install."""
    run = lambda cmd: subprocess.run(cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if module:
        r = run([sys.executable, "-m", "unittest", module])
        out = r.stdout
    elif cpp_src.startswith("sim_"):
        return run_sim(cwd, cpp_src, gpp, run)
    else:
        if not gpp:
            return None, "skipped (g++ not found)", ""
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "t.exe"
            c = run(cpp_compile_command(gpp, cpp_src, exe))
            if c.returncode != 0:
                return False, "did not compile", c.stdout
            extra = []
            if cpp_src == "test_agap_chords.cpp":
                # The chord search is checked against the Python one: write its answers first.
                expected = Path(tmp) / "expected_chords.txt"
                d = run([sys.executable, "dump_python_chords.py"])
                if d.returncode != 0:
                    return False, "could not run the Python solver", d.stdout
                expected.write_text(d.stdout, encoding="utf-8")
                extra = [str(expected)]
            r = run([str(exe)] + extra)
            out = r.stdout
    lines = out.strip().splitlines() or [""]
    return r.returncode == 0, lines[-1], out


def cmd_selftest(args):
    gpp = shutil.which("g++")
    failed = []
    for name, cwd, module, cpp_src in selftest_plan():
        ok, tail, out = run_suite(name, cwd, module, cpp_src, gpp)
        print("%-32s %s   %s" % (name, {True: "PASS", False: "FAIL", None: "SKIP"}[ok], tail))
        if ok is False:
            failed.append((name, out))
    for name, out in failed:
        print("\n--- %s ---\n%s" % (name, out[-1500:]))
    print("\n%s" % ("All tests passed." if not failed else "%d suite(s) failed." % len(failed)))
    return 0 if not failed else 1


def cmd_ports(args):
    ports = list_system_ports()
    if not ports:
        print("No serial ports found.")
        return 1
    likely, others = rank_ports(ports)
    for p in likely:
        print("%-8s %s   <- looks like an Arduino" % (p.device, p.description))
    for p in others:
        print("%-8s %s" % (p.device, p.description))
    return 0


# ------------------------------ menu / main ------------------------------

MENU = [
    ("Check the connection (read-only)", "doctor"),
    ("Guided first power-up (bring-up)", "bringup"),
    ("Console: type commands, watch replies", "console"),
    ("Start the phone / remote bridge", "bridge"),
    ("Flash the firmware onto the board", "flash"),
    ("Run all tests on this computer", "selftest"),
    ("List serial ports", "ports"),
]


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. COM5 (auto-detected if omitted)")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("doctor", "bringup", "console", "selftest", "ports"):
        sub.add_parser(name)
    fl = sub.add_parser("flash")
    fl.add_argument("--sketch", choices=sorted(SKETCHES), default="fretboard",
                    help="fretboard = final 30-solenoid design (default); helper = earlier chord-helper build")
    b = sub.add_parser("bridge")
    b.add_argument("--simulate", action="store_true", help="no board: try the page against a stand-in")
    b.add_argument("--allow-unconfirmed", action="store_true",
                   help="accept hardware that has not been checked locally (the bridge refuses chords otherwise)")
    b.add_argument("--helper", action="store_true", help="the earlier chord-helper firmware instead of the fretboard")
    return ap


COMMANDS = {"doctor": cmd_doctor, "bringup": cmd_bringup, "console": cmd_console, "bridge": cmd_bridge,
            "flash": cmd_flash, "selftest": cmd_selftest, "ports": cmd_ports}


def menu(args, input_fn=input, print_fn=print):
    while True:
        print_fn("\nAGAP - robot guitar player")
        for i, (label, _) in enumerate(MENU, 1):
            print_fn("  %d) %s" % (i, label))
        print_fn("  0) Quit")
        a = input_fn("Choose: ").strip()
        if a in ("0", "q", ""):
            return 0
        if a.isdigit() and 1 <= int(a) <= len(MENU):
            ns = argparse.Namespace(port=args.port, simulate=False, allow_unconfirmed=False)
            try:
                COMMANDS[MENU[int(a) - 1][1]](ns)
            except SystemExit as e:
                if e.code not in (None, 0):
                    print_fn(str(e.code))
            except KeyboardInterrupt:
                print_fn("\nInterrupted.")


def main(argv=None):
    args = build_parser().parse_args(argv)
    if not args.cmd:
        return menu(args)
    for k, v in (("simulate", False), ("allow_unconfirmed", False)):
        if not hasattr(args, k):
            setattr(args, k, v)
    return COMMANDS[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
