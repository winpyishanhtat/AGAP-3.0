// Tests for ui_logic.js - the pure logic behind the phone page (no DOM needed).
// Run:  node ui_logic_test.js        (from the bridge folder)
"use strict";
const assert = require("assert");
const U = require("./ui_logic.js");

let n = 0;
function test(name, fn) {
  try { fn(); n++; }
  catch (e) { console.error("FAIL: " + name + "\n  " + e.message); process.exitCode = 1; }
}

// ---- reading the board's fingering line -------------------------------------------------
test("parses the line the firmware prints for CHORD / SHOW", () => {
  assert.deepStrictEqual(U.parseFingering("Bm -> x 2 0 4 0 2  (cost 9)"),
    { name: "Bm", frets: [-1, 2, 0, 4, 0, 2], cost: 9 });
});
test("parses a RAW line, which has no cost", () => {
  const f = U.parseFingering("RAW -> x 3 2 0 1 0 ");
  assert.strictEqual(f.name, "RAW"); assert.strictEqual(f.cost, null);
  assert.deepStrictEqual(f.frets, [-1, 3, 2, 0, 1, 0]);
});
test("parses sharps and extensions in the name", () => {
  assert.strictEqual(U.parseFingering("F#m7 -> 2 4 2 2 2 2  (cost 20)").name, "F#m7");
});
test("rejects errors, echoes and garbage", () => {
  ["ERR unknown chord", "CHORD Bm", "", "STOPPED", "Bm -> x 2 0 4 0", "Bm -> x 2 0 4 0 2 9", "Bm -> x 2 0 4 0 9",
   "Bm -> x 2 0 4 0 -3", "-> 1 2 3 4 5 5", null, undefined, 42].forEach((s) =>
    assert.strictEqual(U.parseFingering(s), null, String(s)));
});
test("a fret above 5 is not accepted (the plates only go to fret 5)", () => {
  assert.strictEqual(U.parseFingering("X -> 0 0 0 0 0 6"), null);
  assert.notStrictEqual(U.parseFingering("X -> 0 0 0 0 0 5"), null);
});
test("the latest fingering wins and errors after it do not erase it", () => {
  const log = [{ line: "C -> x 3 2 0 1 0  (cost 3)" }, { line: "STRUMMED" }, { line: "Am -> x 0 2 2 1 0  (cost 3)" },
               { line: "ERR unknown chord" }];
  assert.strictEqual(U.latestFingering(log).name, "Am");
  assert.strictEqual(U.latestFingering([]), null);
  assert.strictEqual(U.latestFingering([{ line: "PONG" }]), null);
});

// ---- knowing when to redraw ---------------------------------------------------------------
test("the empty board has a key, so it is drawn on first load (a blank diagram was a real bug)", () => {
  assert.ok(U.boardKey(null).length > 0);
  assert.notStrictEqual(U.boardKey(null), "");
});
test("board key changes with the chord and with the fingering, and is stable otherwise", () => {
  const a = { name: "C", frets: [-1, 3, 2, 0, 1, 0], cost: 3 };
  assert.strictEqual(U.boardKey(a), U.boardKey({ name: "C", frets: [-1, 3, 2, 0, 1, 0], cost: 99 }));
  assert.notStrictEqual(U.boardKey(a), U.boardKey({ name: "C", frets: [3, 3, 2, 0, 1, 0], cost: 3 }));
  assert.notStrictEqual(U.boardKey(a), U.boardKey({ name: "Cadd", frets: [-1, 3, 2, 0, 1, 0], cost: 3 }));
  assert.notStrictEqual(U.boardKey(a), U.boardKey(null));
});

// ---- the fretboard picture ---------------------------------------------------------------
test("string names run low E to high e", () => {
  assert.deepStrictEqual(U.STRING_NAMES, ["E", "A", "D", "G", "B", "e"]);
});
test("model marks muted, open and pressed strings and counts the pressed ones", () => {
  const m = U.fretboardModel([-1, 2, 0, 4, 0, 2]);
  assert.deepStrictEqual(m.marks.map((k) => k.kind), ["mute", "press", "open", "press", "open", "press"]);
  assert.strictEqual(m.pressed, 3);
  assert.strictEqual(m.sounding, 5);
  assert.deepStrictEqual(m.marks[3], { string: 3, kind: "press", fret: 4 });
});
test("model reports a barre when one fret presses three or more adjacent strings", () => {
  assert.deepStrictEqual(U.fretboardModel([1, 3, 3, 2, 1, 1]).barre, { fret: 1, from: 0, to: 5 });
  assert.strictEqual(U.fretboardModel([-1, 3, 2, 0, 1, 0]).barre, null);
});
test("model is never more than six coils", () => {
  assert.ok(U.fretboardModel([5, 5, 5, 5, 5, 5]).pressed <= 6);
});
test("model rejects a wrong-length shape", () => {
  assert.strictEqual(U.fretboardModel([1, 2, 3]), null);
  assert.strictEqual(U.fretboardModel(null), null);
});

// ---- chord names and progressions: must agree with the bridge's own rules ----------------
test("chord name check", () => {
  ["C", "Am", "F#m7", "Bb", "CM7", "Cmaj7", "G7", "Dsus4", "E-7", "a", "Cadd9", "C+"].forEach((s) =>
    assert.ok(U.validChordName(s), s));
  ["", "H", "C 7", "C\n", " C", "C;rm", "C/G", "Cmaj7sus4extra", "C'", "Ä", "C" + "a".repeat(11)].forEach((s) =>
    assert.ok(!U.validChordName(s), JSON.stringify(s)));
});
test("a progression is split on spaces and commas", () => {
  assert.deepStrictEqual(U.parseProgression("C G, Am  F"), { ok: true, chords: ["C", "G", "Am", "F"] });
});
test("a progression with a bad name says which one", () => {
  const r = U.parseProgression("C G H7");
  assert.strictEqual(r.ok, false); assert.ok(r.error.indexOf("H7") >= 0);
});
test("empty and over-long progressions are refused", () => {
  assert.strictEqual(U.parseProgression("   ").ok, false);
  assert.strictEqual(U.parseProgression(new Array(17).fill("C").join(" ")).ok, false);
  assert.strictEqual(U.parseProgression(new Array(16).fill("C").join(" ")).ok, true);
});
test("tempo is clamped to the bridge's 20-200 range", () => {
  assert.strictEqual(U.clampBpm(5), 20); assert.strictEqual(U.clampBpm(500), 200);
  assert.strictEqual(U.clampBpm("90"), 90); assert.strictEqual(U.clampBpm("abc"), 50);
  assert.strictEqual(U.clampBpm(60.7), 61);
});
test("a beat lasts 60000 / bpm milliseconds", () => {
  assert.strictEqual(U.beatMs(60), 1000); assert.strictEqual(U.beatMs(120), 500);
});

// ---- what the page lets you do in each state ---------------------------------------------
const ok = { connected: true, busy: false, fretboard: true, allow_unconfirmed: true };
test("state follows the bridge status", () => {
  assert.strictEqual(U.uiState(ok), "ready");
  assert.strictEqual(U.uiState({ ...ok, busy: true }), "busy");
  assert.strictEqual(U.uiState({ ...ok, allow_unconfirmed: false }), "locked");
  assert.strictEqual(U.uiState({ ...ok, connected: false }), "offline");
  assert.strictEqual(U.uiState(null), "offline");
});
test("the earlier helper build is not locked as a whole (each button has its own flag)", () => {
  assert.strictEqual(U.uiState({ ...ok, fretboard: false, allow_unconfirmed: false }), "ready");
});
test("offline wins over everything", () => {
  assert.strictEqual(U.uiState({ ...ok, connected: false, busy: true, allow_unconfirmed: false }), "offline");
});
test("STOP is always offered", () => {
  ["ready", "busy", "locked", "offline"].forEach((s) => assert.ok(U.canSend(s, "stop"), s));
});
test("while busy only STOP is allowed", () => {
  ["chord", "strum", "sequence", "release", "press"].forEach((a) => assert.ok(!U.canSend("busy", a), a));
});
test("while locked only STOP and RELEASE are allowed", () => {
  assert.ok(U.canSend("locked", "release"));
  ["chord", "strum", "sequence", "press"].forEach((a) => assert.ok(!U.canSend("locked", a), a));
});
test("offline allows nothing but trying STOP", () => {
  ["chord", "strum", "sequence", "release"].forEach((a) => assert.ok(!U.canSend("offline", a), a));
});
test("ready allows the normal actions", () => {
  ["chord", "strum", "sequence", "release", "stop"].forEach((a) => assert.ok(U.canSend("ready", a), a));
});
test("every action the page can send is one the bridge knows", () => {
  const known = ["chord", "press", "release", "strum", "sequence", "calib", "stop"];
  U.ACTIONS.forEach((a) => assert.ok(known.indexOf(a) >= 0, a));
});

// ---- why the page is offline: the bridge vs the board -------------------------------------
test("an unreachable bridge is blamed on the bridge, not the USB cable", () => {
  assert.strictEqual(U.offlineReason(ok, true), "bridge");
  assert.strictEqual(U.offlineReason(null, true), "bridge");
});
test("a bridge that answers but lost the serial link is blamed on the board", () => {
  assert.strictEqual(U.offlineReason({ ...ok, connected: false }, false), "board");
});
test("a healthy connection has no offline reason", () => {
  assert.strictEqual(U.offlineReason(ok, false), null);
  assert.strictEqual(U.offlineReason(null, false), null);
});
test("one failed poll is enough to show offline (a stale Ready is the unsafe direction)", () => {
  assert.strictEqual(U.uiState(U.markUnreachable(ok)), "offline");
  assert.strictEqual(U.markUnreachable(null), null);
  assert.strictEqual(ok.connected, true);   // the original is not changed
});

// ---- the chord library -------------------------------------------------------------------
test("every chord in the library is a valid name and appears once", () => {
  const all = [];
  U.CHORD_GROUPS.forEach((g) => g.chords.forEach((c) => { assert.ok(U.validChordName(c), c); all.push(c); }));
  assert.strictEqual(new Set(all).size, all.length);
  assert.ok(all.length >= 16);
});
test("every preset progression is valid and within the sequence limit", () => {
  U.PRESETS.forEach((p) => {
    const r = U.parseProgression(p.chords.join(" "));
    assert.ok(r.ok, p.name); assert.ok(typeof p.name === "string" && p.name.length > 0);
  });
});
test("difficulty wording follows the cost the board reports", () => {
  assert.strictEqual(U.difficulty(3), "easy");
  assert.strictEqual(U.difficulty(null), "");
  assert.ok(["easy", "moderate", "hard"].indexOf(U.difficulty(40)) >= 0);
});

console.log(n + " checks passed" + (process.exitCode ? " (with failures)" : ""));
