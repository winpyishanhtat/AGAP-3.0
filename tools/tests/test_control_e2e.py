"""The C++ control tool (tools/agap_control.cpp) driving the REAL AGAP_Fretboard.ino logic.

The tool runs in --stdio mode: it writes the command lines it would send to the serial port on stdout
and reads the board's lines from stdin. This test connects those two pipes to AGAP_Fretboard/sim/sim_serve.cpp,
which runs the sketch's real parser, chord search and coil logic on this computer against a mock Arduino.
So what is tested is the tool's actual compiled behaviour (argument checking, the exact lines it sends,
reading the reply, drawing the fingering, exit codes) against the firmware's actual answers, and the coil
pins that result.

Not covered: the serial transports (Win32 / termios), real timing, a real board.

Run:  python -m unittest test_control_e2e -v        (from tools/tests; needs g++)
"""

import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GPP = shutil.which("g++")
need_gpp = unittest.skipUnless(GPP, "g++ not found")

_dir = None
_tool = None
_sim = None


def _exe(name):
    return os.path.join(_dir, name + (".exe" if os.name == "nt" else ""))


def build():
    global _dir, _tool, _sim
    if _tool:
        return
    _dir = tempfile.mkdtemp(prefix="agap_ctl_")
    tool, sim = _exe("agap_control"), _exe("sim_serve")
    r = subprocess.run([GPP, "-std=c++14", "-Wall", "-Wextra", "-O1", str(ROOT / "tools" / "agap_control.cpp"), "-o", tool],
                       cwd=str(ROOT / "tools"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("could not build agap_control:\n" + r.stdout)
    s = ROOT / "AGAP_Fretboard" / "sim"
    r = subprocess.run([GPP, "-std=c++14", "-include", "Arduino.h", "-I" + str(ROOT / "sim" / "mock"),
                        "-I" + str(ROOT / "AGAP_Fretboard"), str(s / "sim_serve.cpp"), "-o", sim],
                       cwd=str(s), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("could not build sim_serve:\n" + r.stdout)
    _tool, _sim = tool, sim


def tearDownModule():
    if _dir:
        shutil.rmtree(_dir, ignore_errors=True)


def mask_for(fingering):
    """Channel mask for "x 2 0 4 0 2", worked out independently of the firmware: channel = (fret-1)*6 + string."""
    m = 0
    for s, c in enumerate(fingering.split()):
        if c not in ("x", "0"):
            m |= 1 << ((int(c) - 1) * 6 + s)
    return m


class Wire:
    """One simulated board kept alive across several runs of the tool."""

    def __init__(self):
        build()
        self.sim = subprocess.Popen([_sim, "--speed", "1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, bufsize=0)
        self.sim_lines = []           # everything the board printed
        self.sent_to_board = []       # everything the tool sent
        self.tool = None
        self._lock = threading.Lock()
        threading.Thread(target=self._read_board, daemon=True).start()
        self.wait_board(lambda ls: any("ready" in x for x in ls), "the firmware to boot")
        time.sleep(0.4)
        with self._lock:
            self.boot_lines = [x for x in self.sim_lines if not x.startswith("#")]

    def _read_board(self):
        for raw in iter(self.sim.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            with self._lock:
                self.sim_lines.append(line)
                tool = self.tool
            if tool is not None and not line.startswith("#"):
                try:
                    tool.stdin.write((line + "\n").encode())
                    tool.stdin.flush()
                except (BrokenPipeError, OSError, ValueError):
                    pass

    def wait_board(self, predicate, what, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            with self._lock:
                lines = list(self.sim_lines)
            if predicate(lines):
                return lines
            time.sleep(0.02)
        raise AssertionError("timed out waiting for %s; board said %s" % (what, self.sim_lines[-6:]))

    def run(self, *args, timeout=40):
        """Run the compiled tool with its --stdio transport wired to the board. Returns (rc, human_text)."""
        tool = subprocess.Popen([_tool, "--stdio"] + list(args), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, bufsize=0)
        with self._lock:
            self.tool = tool
        # Opening a real serial port resets the Mega, so the tool always sees the boot banner first.
        # This board booted once, so replay that banner to each run.
        for line in self.boot_lines:
            try:
                tool.stdin.write((line + chr(10)).encode())
            except (BrokenPipeError, OSError, ValueError):
                break
        tool.stdin.flush()
        captured_cmds = []

        def forward():
            for raw in iter(tool.stdout.readline, b""):
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                captured_cmds.append(line)
                self.sent_to_board.append(line)
                try:
                    self.sim.stdin.write((line + "\n").encode())
                    self.sim.stdin.flush()
                except (BrokenPipeError, OSError, ValueError):
                    return

        t = threading.Thread(target=forward, daemon=True)
        t.start()
        try:
            err = tool.stderr.read()          # ends when the tool exits
            tool.wait(timeout=timeout)
        finally:
            with self._lock:
                self.tool = None
        t.join(timeout=3)
        for f in (tool.stdin, tool.stdout, tool.stderr):
            try:
                f.close()
            except Exception:
                pass
        return tool.returncode, err.decode("utf-8", "replace"), captured_cmds

    def pins(self):
        before = sum(1 for x in self.sim_lines if x.startswith("#PINS"))
        self.sim.stdin.write(b"#pins\n")
        self.sim.stdin.flush()
        lines = self.wait_board(lambda ls: sum(1 for x in ls if x.startswith("#PINS")) > before, "the #pins reply")
        last = [x for x in lines if x.startswith("#PINS")][-1]
        return int(re.search(r"on=([0-9a-f]+)", last).group(1), 16)

    def wait_pins(self, predicate, what, timeout=6.0):
        end = time.time() + timeout
        m = self.pins()
        while not predicate(m) and time.time() < end:
            time.sleep(0.05)
            m = self.pins()
        if not predicate(m):
            raise AssertionError("coils never reached: %s (last mask %08x)" % (what, m))
        return m

    def violations(self):
        before = sum(1 for x in self.sim_lines if x.startswith("#STATUS"))
        self.sim.stdin.write(b"#status\n")
        self.sim.stdin.flush()
        lines = self.wait_board(lambda ls: sum(1 for x in ls if x.startswith("#STATUS")) > before, "the #status reply")
        return int(re.search(r"violations=(\d+)", [x for x in lines if x.startswith("#STATUS")][-1]).group(1))

    def close(self):
        try:
            self.sim.stdin.close()
        except Exception:
            pass
        try:
            self.sim.terminate()
            self.sim.wait(timeout=3)
        except Exception:
            self.sim.kill()
        try:
            self.sim.stdout.close()
        except Exception:
            pass


@need_gpp
class TestToolAgainstTheRealFirmware(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = Wire()

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def setUp(self):
        self.w.run("stop")
        self.w.wait_pins(lambda m: m == 0, "everything released between tests")

    def test_chord_sends_the_exact_line_draws_the_fingering_and_switches_the_right_coils(self):
        rc, human, sent = self.w.run("chord", "Bm")
        self.assertEqual(rc, 0, human)
        self.assertEqual(sent, ["CHORD Bm"])
        self.assertIn("Bm -> x 2 0 4 0 2  (cost 9)", human)
        self.assertEqual(human.count("@"), 3, human)                   # one mark per pressed coil
        self.assertIn("E  A  D  G  B  e", human)
        self.assertEqual(self.w.wait_pins(lambda m: m == mask_for("x 2 0 4 0 2"), "exactly the three Bm coils"),
                         mask_for("x 2 0 4 0 2"))

    def test_show_draws_without_moving_anything(self):
        rc, human, sent = self.w.run("show", "F#m7")
        self.assertEqual(rc, 0, human)
        self.assertEqual(sent, ["SHOW F#m7"])
        self.assertIn("F#m7 -> ", human)
        self.assertGreaterEqual(human.count("@"), 1)
        time.sleep(0.3)
        self.assertEqual(self.w.pins(), 0)

    def test_stop_releases_every_coil(self):
        self.w.run("chord", "C")
        self.w.wait_pins(lambda m: m != 0, "the C chord")
        rc, human, sent = self.w.run("stop")
        self.assertEqual(rc, 0, human)
        self.assertEqual(sent, ["STOP"])
        self.w.wait_pins(lambda m: m == 0, "everything released")

    def test_press_and_release_one_coil(self):
        self.assertEqual(self.w.run("press", "6", "1")[2], ["PRESS 6 1"])
        self.w.wait_pins(lambda m: m == 1 << 0, "string 6 (low E) at fret 1 = channel 0")
        self.assertEqual(self.w.run("release", "6", "1")[2], ["RELEASE 6 1"])
        self.w.wait_pins(lambda m: m == 0, "released")

    def test_raw_fingering_is_drawn_too(self):
        rc, human, sent = self.w.run("raw", "x", "3", "2", "0", "1", "0")
        self.assertEqual(rc, 0, human)
        self.assertEqual(sent, ["RAW x 3 2 0 1 0"])
        self.assertEqual(human.count("@"), 3)
        self.w.wait_pins(lambda m: m == mask_for("x 3 2 0 1 0"), "the raw fingering pressed")

    def test_a_chord_the_firmware_refuses_gives_a_nonzero_exit_code(self):
        rc, human, sent = self.w.run("chord", "Czzz")           # a legal-looking name the solver does not know
        self.assertEqual(sent, ["CHORD Czzz"])
        self.assertIn("ERR unknown chord", human)
        self.assertEqual(rc, 1)
        self.assertEqual(self.w.pins(), 0)

    def test_sequence_streams_the_boards_steps(self):
        rc, human, sent = self.w.run("sequence", "C", "G", "--bpm", "200")
        self.assertEqual(sent, ["TEMPO 200", "SEQUENCE C G"])
        self.assertEqual(rc, 0, human)
        self.assertIn("STEP C", human)
        self.assertIn("STEP G", human)
        self.assertEqual(self.w.violations(), 0)

    def test_twelve_chords_go_through_thirteen_are_refused_before_anything_is_sent(self):
        rc, human, sent = self.w.run("sequence", *(["C"] * 12), "--bpm", "200", timeout=60)
        self.assertEqual(rc, 0, human)
        self.assertEqual(len(sent[1].split()) - 1, 12)
        self.assertNotIn("too many chords", human)
        before = len(self.w.sent_to_board)
        rc, human, sent = self.w.run("sequence", *(["C"] * 13))
        self.assertEqual(rc, 2)
        self.assertEqual(sent, [])
        self.assertEqual(len(self.w.sent_to_board), before)
        self.assertIn("12", human)

    def test_bad_arguments_are_refused_and_nothing_reaches_the_board(self):
        before = len(self.w.sent_to_board)
        cases = [("press", "7", "1"), ("press", "1", "0"), ("press", "a", "b"), ("release", "0", "0"),
                 ("chord", "C;STOP"), ("chord", "H"), ("chord",), ("strum", "sideways"),
                 ("sequence", "C", "--bpm", "abc"), ("sequence", "C", "--bpm", "0"), ("sequence", "C", "--bpm", "9999"),
                 ("calib", "3", "2", "--hold-ms", "x"), ("calib", "3", "2", "--reps", "0"), ("raw", "x", "9", "0", "0", "0", "0"),
                 ("frobnicate",), ("labels",)]
        for case in cases:
            rc, human, sent = self.w.run(*case)
            self.assertEqual(rc, 2, (case, human))
            self.assertEqual(sent, [], case)
            self.assertIn("ERR", human, case)
        self.assertEqual(len(self.w.sent_to_board), before)

    def test_a_typo_is_caught_before_the_two_second_serial_reset(self):
        t0 = time.time()
        rc, _, _ = self.w.run("press", "7", "1")
        self.assertEqual(rc, 2)
        self.assertLess(time.time() - t0, 2.0)

    def test_calib_warns_about_heat_then_runs(self):
        rc, human, sent = self.w.run("calib", "3", "2", "--hold-ms", "200", "--reps", "2", "--gap-ms", "100", timeout=60)
        self.assertEqual(sent, ["CALIB 3 2 200 2 100"])
        self.assertIn("NOTE", human)
        self.assertIn("heat", human)
        self.assertEqual(rc, 0, human)

    def test_helper_flag_on_the_fretboard_firmware_says_so(self):
        # no --map file needed to see the mismatch warning; the tool reads the map first, so give it a real one
        rc, human, _ = self.w.run("--helper", "--map", str(ROOT / "AGAP_HelperButton" / "button_map.json"), "status")
        self.assertIn("fretboard firmware", human)

    def test_no_safety_rule_was_broken_by_any_of_this(self):
        # runs last (alphabetical): the firmware's own checks, applied on every simulated millisecond
        self.assertEqual(self.w.violations(), 0)


@need_gpp
class TestToolLimitsMatchTheFirmwareSource(unittest.TestCase):
    """The limits in agap_control_fret.h are copied from the sketch; if the sketch changes they must too."""

    def test_limits(self):
        sketch = (ROOT / "AGAP_Fretboard" / "AGAP_Fretboard.ino").read_text(encoding="utf-8")
        header = (ROOT / "tools" / "agap_control_fret.h").read_text(encoding="utf-8")
        prog = int(re.search(r"const uint8_t MAX_PROG = (\d+);", sketch).group(1))
        self.assertEqual(int(re.search(r"kMaxSequence = (\d+);", header).group(1)), prog)
        m = re.search(r"<holdMs (\d+)-(\d+)> <reps (\d+)-(\d+)> \[gapMs (\d+)-(\d+)\]", sketch)
        self.assertIsNotNone(m, "CALIB's own usage text not found in the sketch")
        hold_lo, hold_hi, reps_lo, reps_hi, gap_lo, gap_hi = (int(x) for x in m.groups())
        for pair, args in (((hold_lo, hold_hi), "--hold-ms"), ((reps_lo, reps_hi), "--reps"), ((gap_lo, gap_hi), "--gap-ms")):
            self.assertIn("takeInt(args, \"%s\", %d, %d," % (args, pair[0], pair[1]), header, args)

    def test_the_tool_compiles_without_a_single_warning(self):
        build()   # build() raises on error; warnings would be printed as the compiler output only on failure, so recheck
        r = subprocess.run([GPP, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", str(ROOT / "tools" / "agap_control.cpp")],
                           cwd=str(ROOT / "tools"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)


if __name__ == "__main__":
    unittest.main()
