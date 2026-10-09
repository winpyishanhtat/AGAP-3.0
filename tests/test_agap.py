"""Tests for agap.py (the launcher). No board needed: a fake board stands in
for the firmware and records everything sent to it.

Run from the repo root:  python -m unittest discover -s tests -v
"""

import csv
import re
import sys
import tempfile
import threading
import time
import unittest
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import agap  # noqa: E402

agap.FIRST_WAIT = 0.05
agap.QUIET = 0.03

LABELS = "EM AM D C F DM G BM X1 X2"


class FakeBoard:
    """Behaves like AGAP_HelperButton.ino's serial interface."""

    def __init__(self, mode="good", boot=True):
        self.written = []
        self._q = deque()
        self._lock = threading.Lock()
        self.mode = mode
        self.broken = False
        if mode == "wrong":
            self._push("Hello from some other sketch")
        elif mode.startswith("fret") and boot:
            self._push("AGAP (fretboard) ready - type HELP")
            self._push("AGAP-fretboard fw 0.1")
            self._push("No saved tuning - using compiled defaults.")
        elif mode != "silent" and boot:
            self._push("AGAP (helper-button) ready - type HELP")
            self._push("AGAP-helper-button fw 0.4")
            self._push("No saved tuning - using compiled defaults.")
            self._push(self.labels())

    def labels(self):
        return LABELS if self.mode != "badlabels" else "EM AM D C F DM G BM"

    def _push(self, line):
        with self._lock:
            self._q.append(line)

    def write_line(self, line):
        self.written.append(line)
        if self.mode in ("silent", "wrong"):
            return
        word = line.split(" ", 1)[0]
        if self.mode.startswith("fret") and word in ("VERSION", "FRETS", "SHOW"):
            if word == "VERSION":
                self._push("AGAP-fretboard fw 0.1 built test")
            elif word == "FRETS":
                self._push("6 strings x 5 frets = 20 channels, at most 6 coils at once" if self.mode == "fret20"
                           else "6 strings x 5 frets = 30 channels, at most 6 coils at once")
            else:
                # fretbadsolver answers like the older three-fret search: a wrong build for this board
                self._push("Bm -> x 2 0 x 0 2  (cost 18)" if self.mode == "fretbadsolver"
                           else "Bm -> x 2 0 4 0 2  (cost 9)")
            return
        if word == "VERSION":
            self._push("ERR unknown command, type HELP" if self.mode == "old" else "AGAP-helper-button fw 0.4 built test")
        elif word == "LABELS":
            self._push(self.labels())
        elif word == "STATUS":
            self._push("button none")
            self._push("bpm=50 kick=60 holdDuty=60 (23%)")
        elif word == "PICKS":
            for n in range(6, 0, -1):
                self._push("string %d  A=70  B=110" % n)
        elif word == "STOP":
            self._push("STOPPED")
        else:
            self._push("OK " + line)

    def read_line(self):
        if self.broken:
            raise OSError("device disconnected")
        with self._lock:
            if self._q:
                return self._q.popleft()
        time.sleep(0.005)
        return None

    def close(self):
        pass


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def buttons():
    return agap.load_buttons()


def session_for(board, path=None):
    return agap.Session(board, path)


class TestPorts(unittest.TestCase):
    P = agap.PortInfo

    def test_arduino_ranked_before_clone_before_unknown(self):
        ports = [self.P("COM9", "Bluetooth link", None, None),
                 self.P("COM4", "USB-SERIAL CH340", 0x1A86, 0x7523),
                 self.P("COM3", "Arduino Mega 2560", 0x2341, 0x0042)]
        likely, others = agap.rank_ports(ports)
        self.assertEqual([p.device for p in likely], ["COM3", "COM4"])
        self.assertEqual([p.device for p in others], ["COM9"])

    def test_single_likely_port_is_auto_selected(self):
        ports = [self.P("COM3", "Arduino Mega 2560", 0x2341, 0x42), self.P("COM9", "BT", None, None)]
        got = agap.choose_port(ports=ports, input_fn=lambda _: self.fail("should not ask"), print_fn=lambda *_: None)
        self.assertEqual(got, "COM3")

    def test_multiple_candidates_asks_user(self):
        ports = [self.P("COM3", "Arduino Mega", 0x2341, 1), self.P("COM5", "Arduino Uno", 0x2341, 2)]
        got = agap.choose_port(ports=ports, input_fn=lambda _: "2", print_fn=lambda *_: None)
        self.assertEqual(got, "COM5")

    def test_no_ports_returns_none_with_hint(self):
        said = []
        self.assertIsNone(agap.choose_port(ports=[], print_fn=said.append))
        self.assertIn("data USB cable", said[0])

    def test_explicit_port_wins(self):
        self.assertEqual(agap.choose_port("COM7", ports=[]), "COM7")

    def test_error_explanations(self):
        self.assertIn("in use", agap.explain_serial_error(PermissionError("Access is denied"), "COM3"))
        self.assertIn("not found", agap.explain_serial_error(OSError("could not open port 'COM3'"), "COM3"))
        self.assertIn("Could not open COM3", agap.explain_serial_error(ValueError("weird"), "COM3"))


class TestTranslate(unittest.TestCase):
    def setUp(self):
        self.b = buttons()

    def t(self, line):
        return agap.translate(line, self.b)

    def test_chord_name_and_label_both_work(self):
        self.assertEqual(self.t("chord Em"), (["CHORD EM"], None))
        self.assertEqual(self.t("chord em"), (["CHORD EM"], None))
        self.assertEqual(self.t("press bm"), (["PRESS BM"], None))

    def test_unknown_chord_is_caught_before_sending(self):
        lines, err = self.t("chord Zz")
        self.assertEqual(lines, [])
        self.assertIn("unknown chord/label 'Zz'", err)

    def test_release_all(self):
        self.assertEqual(self.t("release all"), (["RELEASE ALL"], None))

    def test_sequence_with_and_without_bpm(self):
        self.assertEqual(self.t("seq C G Am F"), (["SEQUENCE C G AM F"], None))
        self.assertEqual(self.t("seq C G bpm 70"), (["TEMPO 70", "SEQUENCE C G"], None))
        self.assertIn("usage", self.t("seq")[1])
        self.assertIn("unknown", self.t("seq C nope")[1])

    def test_other_commands_pass_through_uppercased(self):
        self.assertEqual(self.t("kick 80"), (["KICK 80"], None))
        self.assertEqual(self.t("pick 6 a 70"), (["PICK 6 a 70"], None))  # args untouched, firmware accepts a/A
        self.assertEqual(self.t("stop"), (["STOP"], None))

    def test_calib_label_resolved(self):
        self.assertEqual(self.t("calib em 1000 3 800"), (["CALIB EM 1000 3 800"], None))

    def test_blank_line(self):
        self.assertEqual(self.t("   "), ([], None))


class TestDoctor(unittest.TestCase):
    def run_doctor(self, board):
        s = session_for(board)
        try:
            boot = s.collect(first_wait=0.2, quiet=0.05)
            return agap.diagnose(boot, s, buttons()), board
        finally:
            s._done = True

    def statuses(self, checks):
        return {c.title: c.status for c in checks}

    def test_healthy_board_passes_and_is_read_only(self):
        checks, board = self.run_doctor(FakeBoard("good"))
        self.assertFalse([c for c in checks if c.status == "fail"])
        self.assertEqual(self.statuses(checks)["Board reply"], "ok")
        self.assertEqual(self.statuses(checks)["Button labels"], "ok")
        # The safety property: doctor must never energise anything.
        moving = [w for w in board.written if w.split()[0] in ("PRESS", "CHORD", "PLUCK", "CALIB", "SEQUENCE", "STRUM", "PICK", "SAVE")]
        self.assertEqual(moving, [])
        self.assertEqual(set(w.split()[0] for w in board.written), {"VERSION", "LABELS", "STATUS", "PICKS"})

    def test_silent_board_fails_with_actionable_hint(self):
        checks, _ = self.run_doctor(FakeBoard("silent"))
        self.assertEqual(checks[0].status, "fail")
        self.assertIn("115200", checks[0].hint)

    def test_other_sketch_detected(self):
        checks, _ = self.run_doctor(FakeBoard("wrong"))
        self.assertEqual(checks[0].status, "fail")
        self.assertIn("not running the AGAP firmware", checks[0].hint)

    def test_old_firmware_is_a_warning_not_a_failure(self):
        checks, _ = self.run_doctor(FakeBoard("old"))
        st = self.statuses(checks)
        self.assertEqual(st["Board reply"], "ok")
        self.assertEqual(st["Firmware version"], "warn")

    def test_label_mismatch_is_flagged(self):
        checks, _ = self.run_doctor(FakeBoard("badlabels"))
        self.assertEqual(self.statuses(checks)["Button labels"], "fail")

    def test_unconfirmed_buttons_noted(self):
        checks, _ = self.run_doctor(FakeBoard("good"))
        self.assertEqual(self.statuses(checks)["Chord chart"], "warn")

    def test_print_checks_return_value(self):
        out = []
        ok = agap.print_checks([agap.Check("ok", "a", "fine", ""), agap.Check("fail", "b", "bad", "do x")], out.append)
        self.assertFalse(ok)
        self.assertTrue(any("do x" in l for l in out))


class Script:
    """Scripted answers for input(); fails loudly if the program asks more than expected."""

    def __init__(self, *answers):
        self.answers = deque(answers)
        self.asked = []

    def __call__(self, prompt=""):
        self.asked.append(prompt)
        if not self.answers:
            raise AssertionError("unexpected extra prompt: %r" % prompt)
        return self.answers.popleft()


class TestBringup(unittest.TestCase):
    def go(self, answers, board=None):
        board = board or FakeBoard("good")
        s = session_for(board)
        boot = s.collect(first_wait=0.2, quiet=0.05)
        out = []
        tmp = Path(tempfile.mkdtemp()) / "r.csv"
        script = Script(*answers)
        ok = agap.run_bringup(s, buttons(), boot, script, out.append, tmp)
        s._done = True
        return ok, board, out, tmp, script

    def sent(self, board):
        return [w for w in board.written if w.split()[0] in ("PRESS", "RELEASE", "PLUCK", "CALIB", "STOP")]

    def test_declining_the_safety_checklist_moves_nothing(self):
        ok, board, out, _, _ = self.go(["no"])
        self.assertFalse(ok)
        self.assertEqual(self.sent(board), [])

    def test_failed_doctor_moves_nothing(self):
        # A board that doesn't answer at all must stop before anything moves:
        ok2, board2, _, _, _ = self.go(["YES"], FakeBoard("silent"))
        self.assertFalse(ok2)
        self.assertEqual(self.sent(board2), [])

    def test_happy_path_one_channel_only(self):
        # YES, test picks=y, six pick confirmations, EM clicked=y, endurance=n, remaining channels=n
        ok, board, _, csv_path, _ = self.go(["YES", "y"] + ["y"] * 6 + ["y", "n", "n"])
        self.assertTrue(ok)
        self.assertEqual([w for w in board.written if w.startswith("PLUCK")], ["PLUCK %d" % n for n in range(6, 0, -1)])
        self.assertEqual([w for w in board.written if w.startswith("PRESS")], ["PRESS EM"])
        self.assertEqual([w for w in board.written if w.startswith("RELEASE")], ["RELEASE EM"])
        rows = list(csv.reader(read(csv_path).splitlines()))
        self.assertEqual(rows[0], ["time", "step", "item", "result", "note"])
        self.assertIn(["pick", "string 6", "pass"], [r[1:4] for r in rows])
        self.assertIn(["channel", "EM", "pass"], [r[1:4] for r in rows])

    def test_skipping_picks_is_recorded(self):
        ok, board, _, csv_path, _ = self.go(["YES", "n", "y", "n", "n"])
        self.assertTrue(ok)
        self.assertEqual([w for w in board.written if w.startswith("PLUCK")], [])
        self.assertIn("skipped", read(csv_path))

    def test_failed_pick_sends_stop_and_explains_pin(self):
        ok, board, out, _, _ = self.go(["YES", "y", "n", "n"])  # string 6 fails, don't continue
        self.assertFalse(ok)
        self.assertIn("STOP", board.written)
        self.assertTrue(any("D2" in l for l in out))  # string 6 -> D2

    def test_failed_channel_still_releases_it_then_stops_with_pin_hint(self):
        ok, board, out, csv_path, _ = self.go(["YES", "n", "n"])  # skip picks, EM did not click
        self.assertFalse(ok)
        written = board.written
        self.assertLess(written.index("PRESS EM"), written.index("RELEASE EM"))  # never left energised
        self.assertIn("STOP", written)
        self.assertTrue(any("D22" in l for l in out))
        self.assertIn("fail", read(csv_path))

    def test_hot_coil_after_endurance_test_stops(self):
        ok, board, out, _, _ = self.go(["YES", "n", "y", "y", "", "y"])  # EM ok, run calib, finished, too hot
        self.assertFalse(ok)
        self.assertTrue(any(w.startswith("CALIB EM") for w in board.written))
        self.assertIn("STOP", board.written)

    def test_all_channels_path_presses_each_once_and_releases_each(self):
        ok, board, _, _, _ = self.go(["YES", "n", "y", "n", "y"] + ["y"] * 9)
        self.assertTrue(ok)
        presses = [w for w in board.written if w.startswith("PRESS")]
        self.assertEqual(presses, ["PRESS " + b["label"] for b in buttons()])
        self.assertEqual(len([w for w in board.written if w.startswith("RELEASE")]), 10)


class TestSession(unittest.TestCase):
    def test_transcript_records_both_directions(self):
        path = Path(tempfile.mkdtemp()) / "t.log"
        board = FakeBoard("good")
        s = agap.Session(board, path)
        s.collect(first_wait=0.2, quiet=0.05)
        s.ask("STATUS")
        s.stop_and_close()
        text = path.read_text(encoding="utf-8")
        self.assertRegex(text, r"> STATUS")
        self.assertRegex(text, r"< bpm=50")
        self.assertRegex(text, r"> STOP")  # always sent on close

    def test_lost_connection_is_noticed(self):
        board = FakeBoard("good")
        s = agap.Session(board)
        board.broken = True
        for _ in range(100):
            if s.lost:
                break
            time.sleep(0.01)
        self.assertIn("disconnected", s.lost)

    def test_stop_sent_even_after_connection_loss(self):
        board = FakeBoard("good")
        s = agap.Session(board)
        board.broken = True
        time.sleep(0.05)
        s.stop_and_close()
        self.assertIn("STOP", board.written)


class TestFretboardFirmware(unittest.TestCase):
    def doctor(self, mode):
        board = FakeBoard(mode)
        s = session_for(board)
        try:
            boot = s.collect(first_wait=0.2, quiet=0.05)
            return agap.diagnose(boot, s, buttons()), board
        finally:
            s._done = True

    def test_firmware_kind_detection(self):
        self.assertEqual(agap.firmware_kind(["AGAP (fretboard) ready - type HELP"]), "fretboard")
        self.assertEqual(agap.firmware_kind(["AGAP-fretboard fw 0.1 built x"]), "fretboard")
        self.assertEqual(agap.firmware_kind(["AGAP-helper-button fw 0.4"]), "helper")
        self.assertIsNone(agap.firmware_kind(["Hello from some other sketch"]))
        self.assertIsNone(agap.firmware_kind([]))

    def test_healthy_fretboard_passes_and_is_read_only(self):
        checks, board = self.doctor("fretboard")
        self.assertEqual([c for c in checks if c.status == "fail"], [])
        titles = {c.title: c.status for c in checks}
        self.assertEqual(titles["Channels"], "ok")
        self.assertEqual(titles["On-board chord search"], "ok")
        # Never energise anything, and never ask for the helper's button labels.
        sent = {w.split()[0] for w in board.written}
        self.assertTrue(sent <= {"VERSION", "FRETS", "SHOW", "STATUS", "PICKS"}, sent)
        self.assertNotIn("LABELS", sent)

    def test_wrong_channel_count_is_flagged(self):
        checks, _ = self.doctor("fret20")
        self.assertEqual({c.title: c.status for c in checks}["Channels"], "fail")

    def test_on_board_solver_disagreeing_with_the_known_answer_is_flagged(self):
        checks, _ = self.doctor("fretbadsolver")
        self.assertEqual({c.title: c.status for c in checks}["On-board chord search"], "fail")

    def test_fretboard_does_not_complain_about_unconfirmed_helper_buttons(self):
        checks, _ = self.doctor("fretboard")
        self.assertNotIn("Chord chart", {c.title for c in checks})


class TestFretboardConsole(unittest.TestCase):
    def t(self, line):
        return agap.translate(line, buttons(), "fretboard")

    def test_chord_names_pass_through_with_their_spelling(self):
        self.assertEqual(self.t("chord Em"), (["CHORD Em"], None))
        self.assertEqual(self.t("chord F#m7"), (["CHORD F#m7"], None))
        self.assertEqual(self.t("show Bm"), (["SHOW Bm"], None))

    def test_sequence_with_tempo(self):
        self.assertEqual(self.t("seq C G Am F bpm 60"), (["TEMPO 60", "SEQUENCE C G Am F"], None))
        self.assertIn("usage", self.t("seq")[1])

    def test_unsafe_chord_text_is_refused_before_sending(self):
        for bad in ("chord Em;STOP", "chord " + "C" * 40, "seq C G;HOLD"):
            lines, err = self.t(bad)
            self.assertEqual(lines, [], bad)
            self.assertTrue(err, bad)

    def test_other_commands_pass_through(self):
        self.assertEqual(self.t("raw x 3 2 0 1 0"), (["RAW x 3 2 0 1 0"], None))
        self.assertEqual(self.t("press 6 1"), (["PRESS 6 1"], None))
        self.assertEqual(self.t("release all"), (["RELEASE ALL"], None))
        self.assertEqual(self.t("stop"), (["STOP"], None))

    def test_helper_mode_still_resolves_labels(self):
        self.assertEqual(agap.translate("chord em", buttons()), (["CHORD EM"], None))


class TestFretboardBringup(unittest.TestCase):
    def go(self, answers, mode="fretboard"):
        board = FakeBoard(mode)
        s = session_for(board)
        boot = s.collect(first_wait=0.2, quiet=0.05)
        out = []
        tmp = Path(tempfile.mkdtemp()) / "r.csv"
        ok = agap.run_bringup(s, buttons(), boot, Script(*answers), out.append, tmp)
        s._done = True
        return ok, board, out, tmp

    def test_happy_path_tests_one_solenoid_only(self):
        # YES, skip picks, channel clicked, no endurance test, no more channels
        ok, board, _, csv_path = self.go(["YES", "n", "y", "n", "n"])
        self.assertTrue(ok)
        self.assertEqual([w for w in board.written if w.startswith("PRESS")], ["PRESS 6 1"])
        self.assertEqual([w for w in board.written if w.startswith("RELEASE")], ["RELEASE 6 1"])
        self.assertIn("string 6 fret 1", read(csv_path))

    def test_failed_channel_is_released_stopped_and_points_at_its_pin(self):
        ok, board, out, _ = self.go(["YES", "n", "n"])
        self.assertFalse(ok)
        w = board.written
        self.assertLess(w.index("PRESS 6 1"), w.index("RELEASE 6 1"))
        self.assertIn("STOP", w)
        self.assertTrue(any("D22" in line for line in out))  # channel 0 is D22

    def test_all_thirty_channels_when_asked(self):
        ok, board, _, _ = self.go(["YES", "n", "y", "n", "y"] + ["y"] * 29)
        self.assertTrue(ok)
        presses = [w for w in board.written if w.startswith("PRESS")]
        self.assertEqual(len(presses), 30)
        self.assertEqual(len(set(presses)), 30)
        self.assertEqual(presses[0], "PRESS 6 1")    # plate 1 first, low E first
        self.assertEqual(presses[6], "PRESS 6 2")    # then plate 2
        self.assertEqual(presses[-1], "PRESS 1 5")   # high e on plate 5 last
        self.assertEqual(len([w for w in board.written if w.startswith("RELEASE")]), 30)

    def test_never_more_than_one_coil_is_on_at_a_time_during_bringup(self):
        ok, board, _, _ = self.go(["YES", "n", "y", "n", "y"] + ["y"] * 29)
        on = 0
        for w in board.written:
            if w.startswith("PRESS"):
                on += 1
            elif w.startswith("RELEASE"):
                on -= 1
            self.assertLessEqual(on, 1)

    def test_pick_failure_points_at_the_servo_pin(self):
        ok, board, out, _ = self.go(["YES", "y", "n", "n"])
        self.assertFalse(ok)
        self.assertTrue(any("D2" in line for line in out))


class TestFretboardTables(unittest.TestCase):
    def test_channel_numbering_matches_the_firmware_header(self):
        self.assertEqual(agap.fret_channel(6, 1), 0)    # low E, fret 1
        self.assertEqual(agap.fret_channel(1, 1), 5)    # high e, fret 1
        self.assertEqual(agap.fret_channel(6, 2), 6)
        self.assertEqual(agap.fret_channel(1, 5), 29)

    def test_pins_are_all_distinct_and_match_the_header_table(self):
        pins = [agap.fret_channel_pin(c) for c in range(30)]
        self.assertEqual(len(set(pins)), 30)
        header = (ROOT / "AGAP_Fretboard" / "agap_fret.h").read_text(encoding="utf-8")
        rows = re.findall(r"channels\s+(\d+)-\s*(\d+)\s+PORT[ACLK]\s+bit \d-\d\s+((?:[DA]\d+\s*)+)", header)
        self.assertEqual(len(rows), 4)
        from_header = []
        for lo, hi, names in rows:
            listed = names.split()
            self.assertEqual(len(listed), int(hi) - int(lo) + 1)
            from_header += listed
        self.assertEqual(from_header, pins)

    def test_both_copies_of_the_shared_logic_header_are_identical(self):
        a = (ROOT / "AGAP_HelperButton" / "agap_logic.h").read_bytes()
        b = (ROOT / "AGAP_Fretboard" / "agap_logic.h").read_bytes()
        self.assertEqual(a, b, "re-copy AGAP_HelperButton/agap_logic.h into AGAP_Fretboard/")

    def test_fretboard_firmware_name_matches_the_sketch(self):
        src = (ROOT / "AGAP_Fretboard" / "AGAP_Fretboard.ino").read_text(encoding="utf-8")
        self.assertIn('#define FW_NAME "%s"' % agap.FRET_FW_NAME, src)

    def test_servo_pins_match_the_fretboard_sketch(self):
        src = (ROOT / "AGAP_Fretboard" / "AGAP_Fretboard.ino").read_text(encoding="utf-8")
        pins = re.search(r"SERVO_PIN\[6\]\s*=\s*\{([^}]*)\}", src).group(1)
        self.assertEqual(["D" + p.strip() for p in pins.split(",")], [agap.servo_pin(n) for n in range(6, 0, -1)])

    def test_flash_defaults_to_the_fretboard_sketch(self):
        cmds = agap.flash_commands("arduino-cli", "COM5")
        self.assertTrue(cmds[2][-1].endswith("AGAP_Fretboard"))
        helper = agap.flash_commands("arduino-cli", "COM5", "helper")
        self.assertTrue(helper[2][-1].endswith("AGAP_HelperButton"))
        self.assertEqual([c[1] for c in cmds], ["core", "lib", "compile", "upload"])

    def test_selftest_includes_the_fretboard_suites(self):
        names = [n for n, _, _, _ in agap.selftest_plan()]
        self.assertTrue(any("Fretboard" in n for n in names))
        self.assertTrue(any("STL" in n for n in names))
        self.assertTrue(any("simulation" in n for n in names))

    def test_every_command_the_fretboard_readme_names_exists_in_the_firmware(self):
        readme = (ROOT / "AGAP_Fretboard" / "README.md").read_text(encoding="utf-8")
        sketch = (ROOT / "AGAP_Fretboard" / "AGAP_Fretboard.ino").read_text(encoding="utf-8")
        in_table = readme.split("## Commands")[1].split("## Wiring")[0]
        words = set()
        for cell in re.findall(r"^\| `([^|]+)` \|", in_table, re.M):
            for part in re.split(r"[,/]| \| ", cell.replace("\\|", "|")):
                m = re.match(r"\s*([A-Z]{3,})\b", part.strip().strip("`"))
                if m:
                    words.add(m.group(1))
        self.assertGreater(len(words), 15)
        for w in sorted(words):
            self.assertTrue('eq(cmd, "%s")' % w in sketch or 'eq(a, "%s")' % w in sketch,
                            "README mentions %s but the sketch has no such command" % w)

    def test_every_simulation_scenario_has_a_runner(self):
        for folder in ("AGAP_Fretboard", "AGAP_HelperButton"):
            self.assertTrue((ROOT / folder / "sim" / "run_tests.sh").is_file(), folder)
        self.assertTrue((ROOT / "sim" / "mock" / "Arduino.h").is_file())


class TestFlashAndRepo(unittest.TestCase):
    def test_flash_commands_compile_before_upload(self):
        cmds = agap.flash_commands("arduino-cli", "COM5")
        verbs = [c[1] for c in cmds]
        self.assertEqual(verbs, ["core", "lib", "compile", "upload"])
        self.assertLess(verbs.index("compile"), verbs.index("upload"))
        up = cmds[-1]
        self.assertEqual(up[up.index("-p") + 1], "COM5")
        self.assertIn("arduino:avr:mega", up)

    def test_channel_pins_match_the_sketch_header(self):
        header = (ROOT / "AGAP_HelperButton" / "AGAP_HelperButton.ino").read_text(encoding="utf-8").split("*/")[0]
        found = re.findall(r"\b(\d)\s+([A-Z][A-Z0-9]*)\s+(D\d+)", header)
        self.assertEqual(len(found), 10)
        want = [(str(i), b["label"], agap.CHANNEL_PINS[i]) for i, b in enumerate(buttons())]
        self.assertEqual(found, want)

    def test_servo_pins_match_the_sketch(self):
        src = (ROOT / "AGAP_HelperButton" / "AGAP_HelperButton.ino").read_text(encoding="utf-8")
        pins = re.search(r"SERVO_PIN\[6\]\s*=\s*\{([^}]*)\}", src).group(1)
        fw = ["D" + p.strip() for p in pins.split(",")]  # index 0 = low E (string 6)
        self.assertEqual(fw, [agap.servo_pin(n) for n in range(6, 0, -1)])

    def test_firmware_name_in_sketch(self):
        src = (ROOT / "AGAP_HelperButton" / "AGAP_HelperButton.ino").read_text(encoding="utf-8")
        self.assertIn('#define FW_NAME "%s"' % agap.FW_NAME, src)

    def test_selftest_plan_paths_exist(self):
        for name, cwd, module, cpp_src in agap.selftest_plan():
            self.assertTrue(Path(cwd).is_dir(), name)
            if cpp_src:
                self.assertTrue((Path(cwd) / cpp_src).is_file(), name)

    def test_a_suite_where_every_test_skipped_is_not_reported_as_a_pass(self):
        nl = chr(10)
        skipped = "Ran 14 tests in 0.000s" + nl + nl + "OK (skipped=14)" + nl
        self.assertTrue(agap.all_skipped(skipped))

    def test_a_suite_with_some_skips_but_real_passes_is_a_pass(self):
        nl = chr(10)
        self.assertFalse(agap.all_skipped("Ran 14 tests in 1.0s" + nl + nl + "OK (skipped=3)" + nl))
        self.assertFalse(agap.all_skipped("Ran 14 tests in 1.0s" + nl + nl + "OK" + nl))
        self.assertFalse(agap.all_skipped("Ran 3 tests in 1.0s" + nl + nl + "FAILED (failures=1, skipped=3)" + nl))

    def test_design_file_suite_is_in_the_plan(self):
        self.assertIn("test_real_parts", [m for _, _, m, _ in agap.selftest_plan()])

    def test_selftest_does_not_use_bash(self):
        # `bash` on Windows is often the WSL stub and fails without a distro.
        cmd = agap.cpp_compile_command("g++", "x.cpp", "t.exe")
        self.assertNotIn("bash", cmd)
        self.assertEqual(cmd[0], "g++")
        self.assertIn("-I..", cmd)

    def test_menu_quits_cleanly(self):
        out = []
        self.assertEqual(agap.menu(agap.argparse.Namespace(port=None), Script("0"), out.append), 0)
        self.assertTrue(any("Check the connection" in l for l in out))

    def test_bridge_command_defaults_to_the_fretboard_firmware(self):
        ns = agap.argparse.Namespace(simulate=False, allow_unconfirmed=False, helper=False)
        cmd = agap.bridge_command(ns, "COM5")
        self.assertIn("--fretboard", cmd)
        self.assertEqual(cmd[cmd.index("--port") + 1], "COM5")
        self.assertNotIn("--allow-unconfirmed", cmd)  # never added silently
        ns = agap.argparse.Namespace(simulate=False, allow_unconfirmed=False, helper=True)
        self.assertNotIn("--fretboard", agap.bridge_command(ns, "COM5"))
        ns = agap.argparse.Namespace(simulate=True, allow_unconfirmed=False, helper=False)
        sim = agap.bridge_command(ns)
        self.assertIn("--simulate", sim)
        self.assertNotIn("--port", sim)

    def test_parser_accepts_port_and_command(self):
        a = agap.build_parser().parse_args(["--port", "COM3", "doctor"])
        self.assertEqual((a.port, a.cmd), ("COM3", "doctor"))


if __name__ == "__main__":
    unittest.main()
