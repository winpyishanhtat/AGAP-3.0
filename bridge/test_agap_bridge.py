"""Tests for agap_bridge.py. No serial port or board needed.

Run:  python -m unittest test_agap_bridge -v      (from the bridge folder)
"""

import json
import threading
import time
import unittest
import urllib.error
import urllib.request

import agap_bridge as ab

TOKEN = "t" * 24


def buttons():
    return [
        {"label": "EM", "chord": "Em", "confirmed": True},
        {"label": "AM", "chord": "Am", "confirmed": True},
        {"label": "C", "chord": "C", "confirmed": True},
        {"label": "G", "chord": "G", "confirmed": True},
        {"label": "F", "chord": "F", "confirmed": False},
        {"label": "X1", "chord": None, "confirmed": False},
    ]


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t

    def advance(self, s):
        self.t += s


def make(cfg=None):
    link = ab.SimLink()
    clock = FakeClock()
    bridge = ab.Bridge(link, buttons(), cfg, clock=clock)
    return bridge, link, clock


class TestValidation(unittest.TestCase):
    def test_chord_by_name_and_by_label(self):
        b, link, clock = make()
        b.execute({"action": "chord", "target": "Em"})
        clock.advance(1)
        b.execute({"action": "chord", "target": "am"})
        self.assertEqual(link.written, ["CHORD EM", "CHORD AM"])

    def test_unknown_target_rejected(self):
        b, link, _ = make()
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "chord", "target": "Zz"})
        self.assertEqual(cm.exception.status, 400)
        self.assertEqual(link.written, [])

    def test_command_injection_attempts_never_reach_serial(self):
        b, link, clock = make()
        for evil in ["EM\nSTOP", "EM; HOLD 100", "EM\r\nKICK 300", "../x", "E" * 100, "", None, 5, ["EM"]]:
            clock.advance(1)
            with self.assertRaises(ab.CommandError):
                b.execute({"action": "chord", "target": evil})
        self.assertEqual(link.written, [])

    def test_raw_and_unknown_actions_rejected(self):
        b, link, _ = make()
        for action in ["raw", "KICK", "hold", None, 7, "chord; STOP"]:
            with self.assertRaises(ab.CommandError):
                b.execute({"action": action, "line": "HOLD 100"})
        self.assertEqual(link.written, [])

    def test_non_object_body_rejected(self):
        b, _, _ = make()
        for body in ([], "chord", 5, None):
            with self.assertRaises(ab.CommandError):
                b.execute(body)

    def test_unconfirmed_button_refused_by_default(self):
        b, link, _ = make()
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "chord", "target": "F"})
        self.assertEqual(cm.exception.status, 403)
        self.assertEqual(link.written, [])

    def test_unconfirmed_allowed_with_flag(self):
        b, link, _ = make(ab.Config(allow_unconfirmed=True))
        b.execute({"action": "chord", "target": "F"})
        self.assertEqual(link.written, ["CHORD F"])

    def test_sequence_builds_tempo_then_sequence(self):
        b, link, _ = make()
        b.execute({"action": "sequence", "targets": ["C", "G", "Am"], "bpm": 60})
        self.assertEqual(link.written, ["TEMPO 60", "SEQUENCE C G AM"])

    def test_sequence_limits(self):
        b, link, clock = make()
        bad = [
            {"targets": []},
            {"targets": "C G"},
            {"targets": ["C"] * 17},
            {"targets": ["C"], "bpm": 5},
            {"targets": ["C"], "bpm": 999},
            {"targets": ["C"], "bpm": "50"},
            {"targets": ["C"], "bpm": True},
            {"targets": ["C", "nope"]},
        ]
        for body in bad:
            clock.advance(1)
            with self.assertRaises(ab.CommandError):
                b.execute({"action": "sequence", **body})
        self.assertEqual(link.written, [])

    def test_strum_direction(self):
        b, link, clock = make()
        b.execute({"action": "strum", "direction": "u"})
        clock.advance(1)
        with self.assertRaises(ab.CommandError):
            b.execute({"action": "strum", "direction": "sideways"})
        self.assertEqual(link.written, ["STRUM U"])

    def test_release_all_and_one(self):
        b, link, clock = make()
        b.execute({"action": "release", "target": "all"})
        clock.advance(1)
        b.execute({"action": "release", "target": "EM"})
        self.assertEqual(link.written, ["RELEASE ALL", "RELEASE EM"])


class TestCalib(unittest.TestCase):
    def test_calib_disabled_by_default(self):
        b, link, _ = make()
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "calib", "target": "EM"})
        self.assertEqual(cm.exception.status, 403)
        self.assertEqual(link.written, [])

    def test_calib_enabled_builds_command(self):
        b, link, _ = make(ab.Config(allow_calib=True))
        b.execute({"action": "calib", "target": "EM", "hold_ms": 1000, "reps": 5, "gap_ms": 800})
        self.assertEqual(link.written, ["CALIB EM 1000 5 800"])

    def test_calib_caps(self):
        b, link, clock = make(ab.Config(allow_calib=True))
        for body in [{"hold_ms": 5000}, {"reps": 50}, {"gap_ms": 100}, {"hold_ms": 0}, {"reps": 0}]:
            clock.advance(100)
            with self.assertRaises(ab.CommandError):
                b.execute({"action": "calib", "target": "EM", **body})
        self.assertEqual(link.written, [])


class TestBusyStopAndRate(unittest.TestCase):
    def test_busy_blocks_other_commands_until_it_passes(self):
        b, link, clock = make()
        b.execute({"action": "sequence", "targets": ["C", "G"], "bpm": 60})  # ~4s busy
        clock.advance(1)
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "chord", "target": "Em"})
        self.assertEqual(cm.exception.status, 409)
        clock.advance(10)
        b.execute({"action": "chord", "target": "Em"})
        self.assertEqual(link.written[-1], "CHORD EM")

    def test_stop_always_goes_through_and_clears_busy(self):
        b, link, clock = make()
        b.execute({"action": "sequence", "targets": ["C", "G"], "bpm": 60})
        b.execute({"action": "stop"})  # same instant: no rate limit, not blocked by busy
        self.assertEqual(link.written[-1], "STOP")
        clock.advance(0.2)
        b.execute({"action": "chord", "target": "Em"})  # busy was cleared
        self.assertEqual(link.written[-1], "CHORD EM")

    def test_stop_is_not_rate_limited(self):
        b, link, _ = make()
        for _ in range(5):
            b.execute({"action": "stop"})
        self.assertEqual(link.written, ["STOP"] * 5)

    def test_rate_limit(self):
        b, _, clock = make()
        b.execute({"action": "chord", "target": "Em"})
        clock.advance(0.05)
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "chord", "target": "Am"})
        self.assertEqual(cm.exception.status, 429)
        clock.advance(0.2)
        b.execute({"action": "chord", "target": "Am"})

    def test_serial_down_refuses_commands_but_not_silently(self):
        b, link, _ = make()
        b.connected = False
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "chord", "target": "Em"})
        self.assertEqual(cm.exception.status, 503)

    def test_shutdown_sends_stop(self):
        b, link, _ = make()
        b.shutdown()
        self.assertEqual(link.written[-1], "STOP")

    def test_reader_collects_replies(self):
        b, link, _ = make()
        b.start()
        b.execute({"action": "chord", "target": "Em"})
        for _ in range(50):
            if b.recent_log():
                break
            time.sleep(0.02)
        self.assertEqual([e["line"] for e in b.recent_log()], ["CHORD EM"])
        b.shutdown()


class TestButtonMapLoading(unittest.TestCase):
    def test_real_button_map_loads_and_labels_are_safe(self):
        bs = ab.load_buttons(ab.DEFAULT_MAP)
        self.assertEqual([x["label"] for x in bs], ["EM", "AM", "D", "C", "F", "DM", "G", "BM", "X1", "X2"])

    def test_unsafe_label_in_map_rejected(self):
        import tempfile, os
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"buttons": [{"label": "EM\nSTOP", "chord": "Em"}]}, f)
        try:
            with self.assertRaises(ValueError):
                ab.load_buttons(f.name)
        finally:
            os.unlink(f.name)


class TestHttp(unittest.TestCase):
    def setUp(self):
        self.bridge, self.link, self.clock = make()
        self.bridge.start()
        self.server = ab.make_server(self.bridge, TOKEN, "127.0.0.1", 0)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.bridge.shutdown()

    def call(self, method, path, body=None, token=TOKEN, raw=None):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if token is not None:
            req.add_header("Authorization", "Bearer " + token)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read() or b"{}") if r.headers.get_content_type() == "application/json" else r.read()
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def test_page_loads_without_token_but_api_needs_one(self):
        status, _ = self.call("GET", "/", token=None)
        self.assertEqual(status, 200)
        self.assertEqual(self.call("GET", "/api/status", token=None)[0], 401)
        self.assertEqual(self.call("GET", "/api/status", token="wrong" * 6)[0], 401)
        self.assertEqual(self.call("POST", "/api/command", {"action": "stop"}, token=None)[0], 401)
        self.assertEqual(self.link.written, [])

    def test_status_and_command_with_token(self):
        status, body = self.call("GET", "/api/status")
        self.assertEqual(status, 200)
        self.assertTrue(body["connected"])
        status, body = self.call("POST", "/api/command", {"action": "chord", "target": "Em"})
        self.assertEqual((status, body), (200, {"sent": ["CHORD EM"]}))
        self.assertEqual(self.link.written, ["CHORD EM"])

    def test_errors_map_to_http_status(self):
        self.assertEqual(self.call("POST", "/api/command", {"action": "nope"})[0], 400)
        self.assertEqual(self.call("POST", "/api/command", {"action": "chord", "target": "F"})[0], 403)
        self.assertEqual(self.call("POST", "/api/command", raw=b"{not json")[0], 400)

    def test_oversize_body_rejected(self):
        status, _ = self.call("POST", "/api/command", raw=b"x" * (ab.MAX_BODY + 1))
        self.assertEqual(status, 413)
        self.assertEqual(self.link.written, [])

    def test_unknown_paths_404(self):
        self.assertEqual(self.call("GET", "/api/nope")[0], 404)
        self.assertEqual(self.call("POST", "/api/other", {})[0], 404)


def make_fret(allow_unconfirmed=True, allow_calib=False):
    link = ab.SimLink()
    clock = FakeClock()
    bridge = ab.Bridge(link, [], ab.Config(allow_calib=allow_calib, allow_unconfirmed=allow_unconfirmed,
                                           fretboard=True), clock=clock)
    return bridge, link, clock


class TestFretboardMode(unittest.TestCase):
    def test_chord_names_are_sent_with_their_spelling(self):
        b, link, clock = make_fret()
        b.execute({"action": "chord", "target": "F#m7"})
        clock.advance(1)
        b.execute({"action": "chord", "target": "CM7"})   # capital M matters: CM7 is not Cm7
        self.assertEqual(link.written, ["CHORD F#m7", "CHORD CM7"])

    def test_refused_until_the_operator_allows_untested_hardware(self):
        b, link, _ = make_fret(allow_unconfirmed=False)
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "chord", "target": "Em"})
        self.assertEqual(cm.exception.status, 403)
        self.assertEqual(link.written, [])
        b.execute({"action": "stop"})  # STOP is never gated
        self.assertEqual(link.written, ["STOP"])

    def test_nothing_but_a_chord_name_can_reach_the_serial_port(self):
        b, link, clock = make_fret()
        for evil in ["Em;STOP", "Em STOP", "Em\nSTOP", "Em\r\nKICK 300", "../x", "E" * 40, "", None, 5, ["Em"],
                     "Em/../", "'Em'", "Em$(x)", "ＥＭ"]:
            clock.advance(1)
            with self.assertRaises(ab.CommandError, msg=repr(evil)):
                b.execute({"action": "chord", "target": evil})
        self.assertEqual(link.written, [])

    def test_sequence_sends_tempo_then_chord_names(self):
        b, link, _ = make_fret()
        b.execute({"action": "sequence", "targets": ["C", "G", "Am", "F"], "bpm": 70})
        self.assertEqual(link.written, ["TEMPO 70", "SEQUENCE C G Am F"])

    def test_sequence_limits_and_bad_names(self):
        b, link, clock = make_fret()
        for body in [{"targets": []}, {"targets": ["C"] * 17}, {"targets": ["C", "G;STOP"]},
                     {"targets": ["C"], "bpm": 5}, {"targets": "C G"}]:
            clock.advance(1)
            with self.assertRaises(ab.CommandError):
                b.execute({"action": "sequence", **body})
        self.assertEqual(link.written, [])

    def test_fretboard_sequence_is_capped_at_the_firmware_limit_of_12(self):
        # AGAP_Fretboard.ino answers "ERR too many chords" beyond MAX_PROG = 12; the bridge must not
        # accept 13-16 and then report "busy" for a sequence the board never plays.
        b, link, clock = make_fret()
        b.execute({"action": "sequence", "targets": ["C"] * 12, "bpm": 60})
        self.assertEqual(len(link.written[-1].split()) - 1, 12)
        clock.advance(100)
        for n in (13, 16):
            with self.assertRaises(ab.CommandError) as cm:
                b.execute({"action": "sequence", "targets": ["C"] * n, "bpm": 60})
            self.assertEqual(cm.exception.status, 400)
            self.assertIn("12", cm.exception.message)
        self.assertFalse(b.status()["busy"])

    def test_earlier_helper_build_keeps_its_limit_of_16(self):
        b, link, clock = make()
        b.execute({"action": "sequence", "targets": ["C"] * 16, "bpm": 60})
        clock.advance(100)
        with self.assertRaises(ab.CommandError):
            b.execute({"action": "sequence", "targets": ["C"] * 17, "bpm": 60})

    def test_status_tells_the_page_the_limit(self):
        self.assertEqual(make_fret()[0].status()["max_sequence"], 12)
        self.assertEqual(make()[0].status()["max_sequence"], 16)

    def test_press_and_release_take_only_a_string_and_a_fret(self):
        b, link, clock = make_fret()
        b.execute({"action": "press", "string": 6, "fret": 1})
        clock.advance(1)
        b.execute({"action": "release", "string": 6, "fret": 1})
        clock.advance(1)
        b.execute({"action": "release", "target": "ALL"})
        self.assertEqual(link.written, ["PRESS 6 1", "RELEASE 6 1", "RELEASE ALL"])
        for bad in [{"string": 0, "fret": 1}, {"string": 7, "fret": 1}, {"string": 6, "fret": 0},
                    {"string": 6, "fret": 6}, {"string": "6", "fret": 1}, {"string": True, "fret": 1}, {}]:
            clock.advance(1)
            with self.assertRaises(ab.CommandError, msg=repr(bad)):
                b.execute({"action": "press", **bad})
        self.assertEqual(len(link.written), 3)

    def test_calib_is_off_by_default_and_capped_when_on(self):
        b, link, _ = make_fret()
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "calib", "string": 6, "fret": 1})
        self.assertEqual(cm.exception.status, 403)
        b2, link2, clock2 = make_fret(allow_calib=True)
        b2.execute({"action": "calib", "string": 6, "fret": 1, "hold_ms": 1000, "reps": 5, "gap_ms": 800})
        self.assertEqual(link2.written, ["CALIB 6 1 1000 5 800"])
        for bad in [{"hold_ms": 5000}, {"reps": 50}, {"gap_ms": 100}]:
            clock2.advance(100)
            with self.assertRaises(ab.CommandError):
                b2.execute({"action": "calib", "string": 6, "fret": 1, **bad})

    def test_stop_still_wins_and_busy_still_blocks(self):
        b, link, clock = make_fret()
        b.execute({"action": "sequence", "targets": ["C", "G"], "bpm": 60})
        clock.advance(1)
        with self.assertRaises(ab.CommandError) as cm:
            b.execute({"action": "chord", "target": "Em"})
        self.assertEqual(cm.exception.status, 409)
        b.execute({"action": "stop"})
        self.assertEqual(link.written[-1], "STOP")

    def test_status_reports_fretboard_mode_and_no_buttons(self):
        b, _, _ = make_fret()
        st = b.status()
        self.assertTrue(st["fretboard"])
        self.assertEqual(st["buttons"], [])

    def test_helper_mode_is_unchanged(self):
        b, link, _ = make()
        self.assertFalse(b.status()["fretboard"])
        b.execute({"action": "chord", "target": "Em"})
        self.assertEqual(link.written, ["CHORD EM"])


class TestFretboardHttp(unittest.TestCase):
    def setUp(self):
        self.bridge, self.link, self.clock = make_fret()
        self.bridge.start()
        self.server = ab.make_server(self.bridge, TOKEN, "127.0.0.1", 0)
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.bridge.shutdown()

    def post(self, body):
        req = urllib.request.Request(self.base + "/api/command", data=json.dumps(body).encode(), method="POST")
        req.add_header("Authorization", "Bearer " + TOKEN)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_chord_over_http_and_bad_input_rejected(self):
        self.assertEqual(self.post({"action": "chord", "target": "Bm"}), (200, {"sent": ["CHORD Bm"]}))
        self.clock.advance(1)  # past the rate limit
        self.assertEqual(self.post({"action": "chord", "target": "Bm;STOP"})[0], 400)
        self.assertEqual(self.link.written, ["CHORD Bm"])


class TestFretboardFirmwareNames(unittest.TestCase):
    def test_every_chord_name_the_solver_knows_passes_the_name_filter(self):
        import sys
        sys.path.insert(0, str(ab.HERE.parent / "ChordAI"))
        import chord_ai
        b, link, clock = make_fret()
        for root in chord_ai.NOTE_NAMES:
            for q in chord_ai.QUALITIES:
                for alias in q.names:
                    clock.advance(1)
                    b.execute({"action": "chord", "target": root + alias})
        self.assertEqual(len(link.written), 12 * sum(len(q.names) for q in chord_ai.QUALITIES))


if __name__ == "__main__":
    unittest.main()
