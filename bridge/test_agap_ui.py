"""Tests for the phone page: what the bridge serves, what the page's logic does with real
board replies, and that the page's rules agree with the bridge's.

Run:  python -m unittest test_agap_ui -v      (from the bridge folder)
The JavaScript parts need Node (they are skipped without it; CI has it).
"""

import json
import re
import shutil
import subprocess
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request

import agap_bridge as ab

sys.path.insert(0, str(ab.HERE.parent / "ChordAI"))
import chord_ai  # noqa: E402

TOKEN = "u" * 24
NODE = shutil.which("node")
need_node = unittest.skipUnless(NODE, "Node.js not found")


def node(script, data=None):
    """Run a snippet against ui_logic.js and return its parsed JSON output."""
    code = "const U=require('./ui_logic.js');const d=JSON.parse(require('fs').readFileSync(0,'utf8'));" + script
    r = subprocess.run([NODE, "-e", code], cwd=str(ab.HERE), input=json.dumps(data), text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if r.returncode != 0:
        raise AssertionError(r.stderr)
    return json.loads(r.stdout)


def start(fretboard=True, allow_unconfirmed=True):
    link = ab.SimLink(fretboard=fretboard)
    clock = lambda: time.monotonic()
    buttons = [] if fretboard else [{"label": "C", "chord": "C", "confirmed": True}]
    bridge = ab.Bridge(link, buttons, ab.Config(allow_unconfirmed=allow_unconfirmed, fretboard=fretboard), clock=clock)
    bridge.start()
    server = ab.make_server(bridge, TOKEN, "127.0.0.1", 0)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return bridge, server, "http://127.0.0.1:%d" % server.server_address[1]


def get(url, token=None):
    req = urllib.request.Request(url)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def post(base, body):
    req = urllib.request.Request(base + "/api/command", data=json.dumps(body).encode(), method="POST")
    req.add_header("Authorization", "Bearer " + TOKEN)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


class TestSimulatorFingerings(unittest.TestCase):
    """--simulate used to echo commands, which gave the page nothing to draw. In fretboard mode it
    now answers the way the firmware does: 'Bm -> x 2 0 4 0 2  (cost 9)'."""

    def reply(self, line):
        link = ab.SimLink(fretboard=True)
        link.write_line(line)
        return link.read_line()

    def test_chord_reply_has_the_firmware_format(self):
        self.assertEqual(self.reply("CHORD Bm"), "Bm -> x 2 0 4 0 2  (cost 9)")

    def test_show_replies_the_same_way(self):
        self.assertEqual(self.reply("SHOW Bm"), "Bm -> x 2 0 4 0 2  (cost 9)")

    def test_unknown_chord_is_an_error_line(self):
        self.assertEqual(self.reply("CHORD Zz"), "ERR unknown chord")

    def test_stop_and_other_commands_keep_their_simple_replies(self):
        self.assertEqual(self.reply("STOP"), "STOPPED")
        self.assertEqual(self.reply("STRUM D"), "OK STRUM D")

    def test_sequence_replies_with_each_chord_in_turn(self):
        link = ab.SimLink(fretboard=True)
        link.write_line("SEQUENCE C G")
        lines = []
        for _ in range(10):
            ln = link.read_line()
            if ln is None:
                break
            lines.append(ln)
        fingerings = [ln for ln in lines if " -> " in ln]
        self.assertEqual([f.split(" ")[0] for f in fingerings], ["C", "G"])

    def test_earlier_simulator_behaviour_is_unchanged_without_the_flag(self):
        link = ab.SimLink()
        link.write_line("CHORD Bm")
        self.assertEqual(link.read_line(), "CHORD Bm")


class TestServedFiles(unittest.TestCase):
    def setUp(self):
        self.bridge, self.server, self.base = start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.bridge.shutdown()

    def test_the_four_page_files_are_served_without_a_token(self):
        for path, ctype in [("/", "text/html"), ("/ui.css", "text/css"), ("/ui_logic.js", "text/javascript"),
                            ("/app.js", "text/javascript")]:
            status, headers, body = get(self.base + path)
            self.assertEqual(status, 200, path)
            self.assertTrue(headers["Content-Type"].startswith(ctype), (path, headers["Content-Type"]))
            self.assertGreater(len(body), 200, path)

    def test_nothing_else_on_disk_is_reachable(self):
        for path in ["/agap_bridge.py", "/test_agap_bridge.py", "/README.md", "/ui_logic_test.js", "/index.html",
                     "/../AGAP_Fretboard/AGAP_Fretboard.ino", "/%2e%2e/agap.py", "/ui_logic.js/x", "/app.js%00.py",
                     "/ui.css/../agap_bridge.py"]:
            status, _, body = get(self.base + path)
            self.assertIn(status, (401, 404), path)
            self.assertNotIn(b"import ", body, path)

    def test_every_response_carries_a_strict_content_security_policy(self):
        for path in ["/", "/app.js", "/api/status"]:
            _, headers, _ = get(self.base + path, TOKEN)
            csp = headers["Content-Security-Policy"]
            self.assertIn("default-src 'none'", csp, path)
            self.assertIn("script-src 'self'", csp, path)
            self.assertIn("connect-src 'self'", csp, path)
            self.assertIn("frame-ancestors 'none'", csp, path)
            self.assertNotIn("unsafe-inline", csp.split("script-src")[1].split(";")[0], path)
            self.assertNotIn("unsafe-eval", csp, path)

    def test_the_api_still_needs_a_token(self):
        self.assertEqual(get(self.base + "/api/status")[0], 401)
        self.assertEqual(get(self.base + "/api/status", TOKEN)[0], 200)


class TestLimitsMatchTheFirmwareSource(unittest.TestCase):
    def test_bridge_sequence_limit_equals_the_sketchs_own_array_size(self):
        src = (ab.HERE.parent / "AGAP_Fretboard" / "AGAP_Fretboard.ino").read_text(encoding="utf-8")
        m = re.search(r"const uint8_t MAX_PROG = (\d+);", src)
        self.assertIsNotNone(m, "MAX_PROG not found in the sketch")
        self.assertEqual(ab.MAX_SEQUENCE_FRETBOARD, int(m.group(1)))

    @need_node
    def test_page_uses_the_limit_the_bridge_reports(self):
        got = node("console.log(JSON.stringify([U.sequenceLimit({max_sequence: d[0]}), U.sequenceLimit({max_sequence: d[1]})]))",
                   [ab.MAX_SEQUENCE_FRETBOARD, ab.MAX_SEQUENCE])
        self.assertEqual(got, [ab.MAX_SEQUENCE_FRETBOARD, ab.MAX_SEQUENCE])


class TestPageSource(unittest.TestCase):
    def read(self, name):
        return (ab.HERE / name).read_text(encoding="utf-8")

    def test_page_loads_only_its_own_files(self):
        html = self.read("index.html")
        for src in re.findall(r'(?:src|href)="([^"]+)"', html):
            self.assertFalse(re.match(r"^(https?:)?//", src), "external resource: " + src)
        self.assertIn('src="/ui_logic.js"', html)
        self.assertIn('src="/app.js"', html)
        self.assertIn('href="/ui.css"', html)

    def test_no_external_urls_anywhere_in_the_page_files(self):
        for name in ("index.html", "ui.css", "app.js", "ui_logic.js"):
            text = self.read(name)
            text = text.replace("http://www.w3.org/2000/svg", "")        # an XML namespace, not a request
            self.assertNotRegex(text, r"https?://", name)
            self.assertNotIn("@import", text, name)

    def test_nothing_inline_that_the_content_security_policy_would_block(self):
        html = self.read("index.html")
        self.assertNotRegex(html, r"<script(?![^>]*\bsrc=)[^>]*>", "inline <script>")
        self.assertNotRegex(html, r"\son[a-z]+\s*=", "inline event handler")
        self.assertNotRegex(html, r"\sstyle\s*=", "inline style attribute")
        self.assertNotRegex(html, r"<style", "inline <style>")

    def test_no_eval_or_dynamic_code(self):
        for name in ("app.js", "ui_logic.js"):
            code = re.sub(r"/\*.*?\*/", "", self.read(name), flags=re.S)       # comments may mention these words
            code = re.sub(r"(?m)^\s*//.*$|\s//\s.*$", "", code)
            self.assertNotRegex(code, r"\beval\s*\(|new Function|document\.write|innerHTML|insertAdjacentHTML", name)

    def test_the_stop_button_is_in_the_page_and_labelled(self):
        html = self.read("index.html")
        self.assertRegex(html, r'id="stop"')
        self.assertRegex(html, r'aria-label="[^"]*[Ss]top[^"]*"')

    def test_page_is_phone_ready(self):
        html = self.read("index.html")
        self.assertIn('name="viewport"', html)
        self.assertIn('lang="en"', html)
        css = self.read("ui.css")
        self.assertIn("prefers-reduced-motion", css)
        self.assertIn("prefers-color-scheme", css)
        self.assertIn(":focus-visible", css)

    @need_node
    def test_page_scripts_have_no_syntax_errors(self):
        # a broken app.js leaves a dead page and no other test notices
        for name in ("app.js", "ui_logic.js"):
            r = subprocess.run([NODE, "--check", name], cwd=str(ab.HERE), stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, timeout=30)
            self.assertEqual(r.returncode, 0, "%s: %s" % (name, r.stderr))

    def test_every_action_the_page_sends_is_one_the_bridge_accepts(self):
        sent = set(re.findall(r"action:\s*\"([a-z]+)\"", self.read("app.js")))
        self.assertTrue(sent, "no actions found in app.js")
        bridge_src = self.read("agap_bridge.py")
        for action in sent:
            self.assertIn('action == "%s"' % action, bridge_src, action)

    def test_the_page_never_sends_the_actions_that_need_extra_flags(self):
        sent = set(re.findall(r"action:\s*\"([a-z]+)\"", self.read("app.js")))
        self.assertFalse(sent & {"calib", "press"}, sent)


class TestPortInUse(unittest.TestCase):
    def test_a_busy_port_gives_advice_not_a_traceback(self):
        import socket
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        blocker.listen(1)
        port = blocker.getsockname()[1]
        try:
            r = subprocess.run([sys.executable, "agap_bridge.py", "--simulate", "--fretboard", "--http-port", str(port)],
                               cwd=str(ab.HERE), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
        finally:
            blocker.close()
        self.assertNotEqual(r.returncode, 0)
        out = r.stdout + r.stderr
        self.assertNotIn("Traceback", out)
        self.assertIn("--http-port", out)
        self.assertIn(str(port), out)


class TestTokenIsShownAtOnce(unittest.TestCase):
    def test_token_line_appears_immediately_even_when_output_is_piped(self):
        # Python buffers piped output; a launcher that captures it would not see the token until exit
        p = subprocess.Popen([sys.executable, "agap_bridge.py", "--simulate", "--fretboard", "--http-port", "0"],
                             cwd=str(ab.HERE), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        got = []
        reader = threading.Thread(target=lambda: [got.append(x) for x in iter(p.stdout.readline, "")], daemon=True)
        reader.start()
        try:
            deadline = time.time() + 6
            while time.time() < deadline and not any(x.startswith("Token:") for x in got):
                time.sleep(0.05)
            self.assertTrue(any(x.startswith("Token:") for x in got), "no token within 6 s; got %r" % got)
        finally:
            p.kill()
            p.wait(timeout=5)
            p.stdout.close()


@need_node
class TestNodeUnitTests(unittest.TestCase):
    def test_the_javascript_unit_tests_pass(self):
        r = subprocess.run([NODE, "ui_logic_test.js"], cwd=str(ab.HERE), stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertRegex(r.stdout, r"\d+ checks passed")


@need_node
class TestLogicAgreesWithTheBridge(unittest.TestCase):
    def test_chord_name_filter_is_identical_to_the_bridges(self):
        names = ["C", "Am", "F#m7", "Bb", "CM7", "Cmaj7", "G7", "Dsus4", "E-7", "a", "Cadd9", "C+", "C#b", "",
                 "H", "C 7", "C\n", " C", "C;rm", "C/G", "Cmaj7sus4extra", "C'", "Ä", "C" + "a" * 11,
                 "C" + "a" * 10, "g#", "Gb", "bb", "B#", "c-", "D+9", "A\t", "١"]
        js = node("console.log(JSON.stringify(d.map(s=>U.validChordName(s))))", names)
        py = [ab.CHORD_NAME_RE.fullmatch(n) is not None for n in names]
        self.assertEqual(js, py)

    def test_limits_are_the_bridges_limits(self):
        info = node("console.log(JSON.stringify({lo:U.clampBpm(-1e9),hi:U.clampBpm(1e9),"
                    "ok16:U.parseProgression(Array(16).fill('C').join(' ')).ok,"
                    "bad17:U.parseProgression(Array(17).fill('C').join(' ')).ok}))", None)
        self.assertEqual((info["lo"], info["hi"]), ab.BPM_RANGE)
        self.assertTrue(info["ok16"] and not info["bad17"])
        self.assertEqual(ab.MAX_SEQUENCE, 16)

    def test_every_library_chord_and_preset_is_solved_by_the_board_logic(self):
        lib = node("console.log(JSON.stringify({g:U.CHORD_GROUPS.flatMap(g=>g.chords),"
                   "p:U.PRESETS.flatMap(p=>p.chords)}))", None)
        for name in set(lib["g"] + lib["p"]):
            v = chord_ai.solve(name, 5)           # raises if the solver does not know it
            self.assertTrue(any(f >= 0 for f in v.frets), name)
            self.assertIsNotNone(ab.CHORD_NAME_RE.fullmatch(name), name)

    def test_page_reads_every_fingering_the_simulated_board_prints(self):
        link = ab.SimLink(fretboard=True)
        names, lines = [], []
        for root in chord_ai.NOTE_NAMES:
            for suffix in ("", "m", "7", "m7", "maj7", "sus4", "dim"):
                names.append(root + suffix)
        for n in names:
            link.write_line("CHORD " + n)
            lines.append(link.read_line())
        parsed = node("console.log(JSON.stringify(d.map(l=>U.parseFingering(l))))", lines)
        for n, p in zip(names, parsed):
            want = chord_ai.solve(n, 5)
            self.assertIsNotNone(p, n)
            self.assertEqual(p["frets"], list(want.frets), n)
            self.assertEqual(p["cost"], want.cost, n)

    def test_the_picture_never_shows_more_than_six_pressed_strings(self):
        shapes = [[f] * 6 for f in range(-1, 6)] + [[-1, 2, 0, 4, 0, 2], [1, 3, 3, 2, 1, 1]]
        models = node("console.log(JSON.stringify(d.map(f=>U.fretboardModel(f))))", shapes)
        for m in models:
            self.assertLessEqual(m["pressed"], 6)
            self.assertEqual(len(m["marks"]), 6)


@need_node
class TestWholeFlow(unittest.TestCase):
    """The page's path: send a chord through the real HTTP API, read the log back, draw it."""

    def setUp(self):
        self.bridge, self.server, self.base = start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.bridge.shutdown()

    def log_after(self, predicate):
        for _ in range(50):
            _, _, body = get(self.base + "/api/log", TOKEN)
            entries = json.loads(body)["log"]
            if predicate(entries):
                return entries
            time.sleep(0.05)
        self.fail("board output never appeared")

    def test_chord_to_drawn_fingering(self):
        self.assertEqual(post(self.base, {"action": "chord", "target": "Bm"})[0], 200)
        entries = self.log_after(lambda e: any(" -> " in x["line"] for x in e))
        f = node("console.log(JSON.stringify(U.latestFingering(d)))", entries)
        self.assertEqual(f["frets"], [-1, 2, 0, 4, 0, 2])
        m = node("console.log(JSON.stringify(U.fretboardModel(d)))", f["frets"])
        self.assertEqual(m["pressed"], 3)
        self.assertEqual(m["sounding"], 5)

    def test_state_in_the_page_follows_the_real_status(self):
        _, _, body = get(self.base + "/api/status", TOKEN)
        self.assertEqual(node("console.log(JSON.stringify(U.uiState(d)))", json.loads(body)), "ready")

    def test_locked_bridge_shows_as_locked_and_refuses_what_the_page_would_disable(self):
        self.server.shutdown()
        self.server.server_close()
        self.bridge.shutdown()
        self.bridge, self.server, self.base = start(allow_unconfirmed=False)
        _, _, body = get(self.base + "/api/status", TOKEN)
        status = json.loads(body)
        self.assertEqual(node("console.log(JSON.stringify(U.uiState(d)))", status), "locked")
        # the page disables chord/strum/sequence when locked; the bridge must refuse them too
        for action, extra in [("chord", {"target": "C"}), ("strum", {"direction": "D"}),
                              ("sequence", {"targets": ["C"], "bpm": 60})]:
            self.assertFalse(node("console.log(JSON.stringify(U.canSend('locked',d)))", action))
            self.assertEqual(post(self.base, dict(action=action, **extra))[0], 403, action)
            time.sleep(0.2)
        self.assertEqual(post(self.base, {"action": "stop"})[0], 200)

    def test_busy_bridge_shows_as_busy_and_stop_still_works(self):
        post(self.base, {"action": "sequence", "targets": ["C", "G"], "bpm": 60})
        _, _, body = get(self.base + "/api/status", TOKEN)
        status = json.loads(body)
        self.assertTrue(status["busy"])
        self.assertEqual(node("console.log(JSON.stringify(U.uiState(d)))", status), "busy")
        self.assertEqual(post(self.base, {"action": "stop"})[0], 200)
        _, _, body = get(self.base + "/api/status", TOKEN)
        self.assertFalse(json.loads(body)["busy"])


if __name__ == "__main__":
    unittest.main()
