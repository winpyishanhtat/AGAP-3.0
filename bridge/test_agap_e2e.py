"""End-to-end tests: the phone page's logic -> the bridge -> the REAL AGAP_Fretboard.ino -> simulated coil pins.

Until now the bridge and page were only tested against stand-ins that make up the board's replies.
Here the bridge talks (through ProcessLink) to AGAP_Fretboard/sim/sim_serve.cpp, which runs the
sketch's real command parser, chord search, state machines and pin masks on this computer against
a mock Arduino. So a chord sent over HTTP is solved by the firmware's own code, its printed reply
is read back by the page's own parser, and the coils that switch on are counted and compared with
what the page would draw.

What it does NOT show: real serial timing, real electronics, or a real board (see docs/TEST_PLAN.md).

Run:  python -m unittest test_agap_e2e -v       (from the bridge folder; needs g++, Node for some tests)
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

import agap_bridge as ab

sys.path.insert(0, str(ab.HERE.parent / "ChordAI"))
import chord_ai  # noqa: E402

ROOT = ab.HERE.parent
GPP = shutil.which("g++")
NODE = shutil.which("node")
TOKEN = "e" * 24
SPEED = 1       # simulated ms per real ms. Real time on purpose: the firmware releases any coil held 8 s
                # without a refresh, so racing the clock would make coils let go before a test can look.

need_gpp = unittest.skipUnless(GPP, "g++ not found")
need_node = unittest.skipUnless(NODE, "Node.js not found")

_exe_dir = None
_exe = None


def build_sim():
    """Compile the firmware simulation once per test run."""
    global _exe_dir, _exe
    if _exe:
        return _exe
    _exe_dir = tempfile.mkdtemp(prefix="agap_sim_")
    out = os.path.join(_exe_dir, "sim_serve.exe" if os.name == "nt" else "sim_serve")
    sim = ROOT / "AGAP_Fretboard" / "sim"
    cmd = [GPP, "-std=c++14", "-include", "Arduino.h", "-I" + str(ROOT / "sim" / "mock"), "-I" + str(ROOT / "AGAP_Fretboard"),
           str(sim / "sim_serve.cpp"), "-o", out]
    r = subprocess.run(cmd, cwd=str(sim), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("could not build sim_serve:\n" + r.stdout)
    _exe = out
    return out


def tearDownModule():
    if _exe_dir:
        shutil.rmtree(_exe_dir, ignore_errors=True)


def mask_for(fingering):
    """Channel mask for "x 2 0 4 0 2", worked out here independently of the firmware: channel = (fret-1)*6 + string."""
    m = 0
    for s, c in enumerate(fingering.split()):
        if c != "x" and c != "0":
            m |= 1 << ((int(c) - 1) * 6 + s)
    return m


def node(script, data=None):
    code = "const U=require('./ui_logic.js');const d=JSON.parse(require('fs').readFileSync(0,'utf8'));" + script
    r = subprocess.run([NODE, "-e", code], cwd=str(ab.HERE), input=json.dumps(data), text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if r.returncode != 0:
        raise AssertionError(r.stderr)
    return json.loads(r.stdout)


class Rig:
    """A bridge on the real firmware, plus an HTTP server in front of it."""

    def __init__(self, allow_unconfirmed=True):
        self.link = ab.ProcessLink([build_sim(), "--speed", str(SPEED)])
        self.bridge = ab.Bridge(self.link, [], ab.Config(allow_unconfirmed=allow_unconfirmed, fretboard=True))
        self.bridge.start()
        self.server = ab.make_server(self.bridge, TOKEN, "127.0.0.1", 0)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]
        self.wait_for(lambda lines: any("AGAP (fretboard) ready" in x for x in lines), "the firmware to boot")

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.bridge.shutdown()

    # ---- helpers
    def lines(self):
        return [e["line"] for e in self.bridge.recent_log(200)]

    def wait_for(self, predicate, what, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if predicate(self.lines()):
                return self.lines()
            time.sleep(0.02)
        raise AssertionError("timed out waiting for %s; log: %s" % (what, self.lines()[-8:]))

    def post(self, body):
        req = urllib.request.Request(self.base + "/api/command", data=json.dumps(body).encode(), method="POST")
        req.add_header("Authorization", "Bearer " + TOKEN)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def harness(self, word):
        """Ask the simulation itself (not the firmware) a question; returns the key=value pairs it answered."""
        before = len([x for x in self.lines() if x.startswith("#" + word.upper())])
        self.link.write_line("#" + word)
        lines = self.wait_for(lambda ls: len([x for x in ls if x.startswith("#" + word.upper())]) > before, "the #%s reply" % word)
        last = [x for x in lines if x.startswith("#" + word.upper())][-1]
        return {k: v for k, v in re.findall(r"(\w+)=(\S+)", last)}

    def coils(self):
        return int(self.harness("pins")["on"], 16)

    def wait_coils(self, predicate, what, timeout=6.0):
        """Poll the simulated pins until `predicate(mask)` holds (chord presses land a few simulated ms
        after the command, and a loaded machine can be slow); returns the mask or fails with what it saw."""
        end = time.time() + timeout
        mask = self.coils()
        while not predicate(mask) and time.time() < end:
            time.sleep(0.05)
            mask = self.coils()
        if not predicate(mask):
            raise AssertionError("coils never reached: %s (last mask %08x)" % (what, mask))
        return mask

    def settle(self):
        """Let the firmware finish what it is doing (chord presses are asynchronous)."""
        time.sleep(0.15)


@need_gpp
class TestRealFirmwareThroughTheBridge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rig = Rig()

    @classmethod
    def tearDownClass(cls):
        cls.rig.close()

    def setUp(self):
        self.rig.post({"action": "stop"})
        self.rig.settle()
        time.sleep(0.2)             # past the bridge's rate limit

    def test_the_real_firmware_boots(self):
        lines = self.rig.lines()
        self.assertTrue(any("AGAP (fretboard) ready" in x for x in lines))
        self.assertTrue(any("AGAP-fretboard fw" in x for x in lines))

    def test_chord_over_http_is_solved_by_the_firmware_and_switches_the_right_coils(self):
        self.assertEqual(self.rig.post({"action": "chord", "target": "Bm"})[0], 200)
        self.rig.wait_for(lambda ls: "Bm -> x 2 0 4 0 2  (cost 9)" in ls, "the Bm fingering")
        self.rig.wait_coils(lambda m: m == mask_for("x 2 0 4 0 2"), "exactly the three Bm coils")

    def test_stop_releases_every_coil(self):
        self.rig.post({"action": "chord", "target": "C"})
        self.rig.wait_coils(lambda m: m != 0, "the C chord pressed")
        self.assertEqual(self.rig.post({"action": "stop"})[0], 200)
        self.rig.wait_coils(lambda m: m == 0, "every coil released after STOP")

    def test_release_all_lets_go(self):
        self.rig.post({"action": "chord", "target": "Em"})
        self.rig.wait_coils(lambda m: m != 0, "the Em chord pressed")
        time.sleep(0.2)
        self.assertEqual(self.rig.post({"action": "release", "target": "ALL"})[0], 200)
        self.rig.wait_coils(lambda m: m == 0, "every coil released")

    def test_a_chord_the_firmware_does_not_know_is_refused_by_the_firmware(self):
        # the bridge's name filter lets "Czzz" through; the board's own answer is what the page shows
        self.assertEqual(self.rig.post({"action": "chord", "target": "Czzz"})[0], 200)
        self.rig.wait_for(lambda ls: "ERR unknown chord" in ls, "the board's error")
        self.rig.settle()
        self.assertEqual(self.rig.coils(), 0)

    def test_press_beyond_six_coils_is_refused_by_the_real_firmware(self):
        for string, fret in [(1, 1), (2, 1), (3, 1), (4, 1), (5, 1), (6, 1), (1, 2)]:
            self.assertEqual(self.rig.post({"action": "press", "string": string, "fret": fret})[0], 200)
            time.sleep(0.17)         # the bridge's own rate limit is 0.15 s
        self.rig.wait_for(lambda ls: any(x.startswith("ERR already 6 coils") for x in ls), "the seventh press to be refused")
        self.rig.wait_coils(lambda m: bin(m).count("1") == 6, "six coils held")
        time.sleep(0.3)
        self.assertEqual(bin(self.rig.coils()).count("1"), 6)       # still six: the seventh was refused, not added

    def test_zz_no_safety_rule_was_broken_by_any_test_above(self):
        st = self.rig.harness("status")
        self.assertEqual(st["violations"], "0")
        self.assertLessEqual(int(st["maxcoils"]), 6)


@need_gpp
class TestForgottenCoilLetsGo(unittest.TestCase):
    def test_a_coil_nobody_refreshes_releases_itself_after_8_seconds(self):
        """If the phone, the bridge or the USB cable dies while a chord is held, the board must not
        leave a solenoid energised. This is the firmware's own timeout, end to end."""
        rig = Rig()
        try:
            self.assertEqual(rig.post({"action": "press", "string": 3, "fret": 2})[0], 200)
            rig.wait_coils(lambda m: bin(m).count("1") == 1, "the one pressed coil")
            time.sleep(5)
            self.assertEqual(bin(rig.coils()).count("1"), 1, "released too early (before the 8 s timeout)")
            time.sleep(5)
            self.assertEqual(rig.coils(), 0, "still energised 10 s after the last command")
            self.assertEqual(rig.harness("status")["violations"], "0")
        finally:
            rig.close()


@need_gpp
class TestStopDuringASequenceEndToEnd(unittest.TestCase):
    def test_stop_from_the_page_ends_a_running_sequence_and_frees_the_coils(self):
        rig = Rig()
        try:
            status, body = rig.post({"action": "sequence", "targets": ["C", "G", "Am", "F", "C", "G", "Am", "F"], "bpm": 30})
            self.assertEqual(status, 200)
            rig.wait_for(lambda ls: any(x.startswith("C -> ") for x in ls), "the first chord of the sequence")
            time.sleep(0.4)                              # the firmware is now inside its blocking SEQUENCE loop
            self.assertTrue(rig.bridge.status()["busy"])
            rig.wait_coils(lambda m: m != 0, "coils pressing during the sequence")
            self.assertEqual(rig.post({"action": "stop"})[0], 200)
            rig.wait_for(lambda ls: "ABORTED (STOP)" in ls, "the firmware to abort the sequence on STOP", timeout=5)
            rig.wait_coils(lambda m: m == 0, "every coil released after the abort")
            self.assertFalse(rig.bridge.status()["busy"])
            self.assertEqual(rig.harness("status")["violations"], "0")
        finally:
            rig.close()


@need_gpp
@need_node
class TestPageReadsTheRealFirmware(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rig = Rig()

    @classmethod
    def tearDownClass(cls):
        cls.rig.close()

    def test_page_parser_reads_every_real_fingering_and_matches_chordai(self):
        names = [r + q for r in chord_ai.NOTE_NAMES for q in ("", "m", "7", "m7", "maj7", "sus4", "dim")]
        for n in names:
            self.rig.link.write_line("SHOW " + n)          # SHOW moves nothing; sent straight to the board
        fing = self.rig.wait_for(lambda ls: sum(1 for x in ls if " -> " in x) >= len(names), "all SHOW replies", timeout=20)
        fing = [x for x in fing if " -> " in x]
        parsed = node("console.log(JSON.stringify(d.map(l=>U.parseFingering(l))))", fing)
        checked = 0
        for p in parsed:
            if p is None:
                continue
            want = chord_ai.solve(p["name"], 5)
            self.assertEqual(p["frets"], list(want.frets), p["name"])
            self.assertEqual(p["cost"], want.cost, p["name"])
            checked += 1
        self.assertGreaterEqual(checked, len(names))

    def test_what_the_page_would_draw_matches_the_coils_that_switch_on(self):
        for name in ["C", "G", "Am", "F", "Bm", "Em", "D7", "F#m7", "Bb"]:
            time.sleep(0.2)
            self.rig.post({"action": "stop"})
            self.rig.settle()
            time.sleep(0.2)
            before = sum(1 for x in self.rig.lines() if " -> " in x and not x.startswith("#"))
            self.assertEqual(self.rig.post({"action": "chord", "target": name})[0], 200, name)
            self.rig.wait_for(lambda ls: sum(1 for x in ls if " -> " in x and not x.startswith("#")) > before, "fingering for " + name)
            fingering = node("console.log(JSON.stringify(U.latestFingering(d)))",
                             [{"line": x} for x in self.rig.lines()])
            # the board may spell the root differently (typed Bb, reported A#); the page must still know it is the same chord
            self.assertTrue(node("console.log(JSON.stringify(U.sameChord(d[0], d[1])))", [name, fingering["name"]]), (name, fingering["name"]))
            model = node("console.log(JSON.stringify(U.fretboardModel(d)))", fingering["frets"])
            on = self.rig.wait_coils(lambda m: bin(m).count("1") == model["pressed"], "as many coils as the picture shows for " + name)
            self.assertEqual(bin(on).count("1"), model["pressed"], name)       # picture == coils, by count
            self.assertEqual(on, mask_for(" ".join("x" if f < 0 else str(f) for f in fingering["frets"])), name)  # and by channel


@need_gpp
@need_node
class TestFreshBoot(unittest.TestCase):
    def test_a_freshly_booted_board_reads_as_nothing_played_yet(self):
        """The real firmware prints its default progression (C G Am F) at power-up. The page once showed
        the last of those as the chord now playing."""
        rig = Rig()
        try:
            lines = rig.lines()
            self.assertTrue(any(x.startswith("F -> ") for x in lines), "the boot-time fingerings are not in the log")
            self.assertIsNone(node("console.log(JSON.stringify(U.latestFingering(d)))", [{"line": x} for x in lines]))
        finally:
            rig.close()


@need_gpp
class TestLockedBridgeNeverTouchesTheFirmware(unittest.TestCase):
    def test_locked_bridge_refuses_and_the_coils_stay_off(self):
        rig = Rig(allow_unconfirmed=False)
        try:
            for body in ({"action": "chord", "target": "C"}, {"action": "strum", "direction": "D"},
                         {"action": "sequence", "targets": ["C"], "bpm": 60}):
                self.assertEqual(rig.post(body)[0], 403)
                time.sleep(0.2)
            rig.settle()
            self.assertEqual(rig.coils(), 0)
            self.assertEqual(rig.post({"action": "stop"})[0], 200)
        finally:
            rig.close()


@need_gpp
class TestProcessLink(unittest.TestCase):
    def test_a_dead_firmware_marks_the_bridge_disconnected(self):
        link = ab.ProcessLink([build_sim(), "--speed", str(SPEED)])
        bridge = ab.Bridge(link, [], ab.Config(fretboard=True, allow_unconfirmed=True))
        bridge.start()
        try:
            time.sleep(0.5)
            self.assertTrue(bridge.connected)
            link.write_line("#quit")
            deadline = time.time() + 5
            while bridge.connected and time.time() < deadline:
                time.sleep(0.05)
            self.assertFalse(bridge.connected)
            with self.assertRaises(ab.CommandError) as cm:
                bridge.execute({"action": "chord", "target": "C"})
            self.assertEqual(cm.exception.status, 503)
        finally:
            bridge.shutdown()


if __name__ == "__main__":
    unittest.main()
