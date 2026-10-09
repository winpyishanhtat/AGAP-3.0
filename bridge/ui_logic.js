// Logic behind the AGAP phone page, kept free of the DOM so it can be tested with Node
// (ui_logic_test.js) and used unchanged in the browser (index.html loads it as a script).
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.AgapUI = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  var STRING_NAMES = ["E", "A", "D", "G", "B", "e"];   // low E first, as the firmware numbers them
  var MAX_FRET = 5;                                    // the plates cover frets 1-5
  var MAX_SEQUENCE = 16, BPM_MIN = 20, BPM_MAX = 200, BPM_DEFAULT = 50;
  // The same rule as CHORD_NAME_RE in agap_bridge.py (the bridge re-checks it; this is for early feedback).
  var CHORD_RE = /^[A-Ga-g][#b]?[A-Za-z0-9+\-]{0,10}$/;
  // Every action the page can send; the bridge accepts exactly these (plus press/calib, which the page never uses).
  var ACTIONS = ["chord", "strum", "sequence", "release", "stop"];

  var CHORD_GROUPS = [
    { name: "Major", chords: ["C", "D", "E", "F", "G", "A", "B"] },
    { name: "Minor", chords: ["Am", "Bm", "Dm", "Em", "Fm", "Gm"] },
    { name: "Sevenths", chords: ["C7", "D7", "E7", "G7", "A7", "Am7", "Em7", "Dm7"] }
  ];
  var PRESETS = [
    { name: "Pop", chords: ["C", "G", "Am", "F"] },
    { name: "Folk", chords: ["G", "C", "D", "G"] },
    { name: "Blues", chords: ["A7", "D7", "A7", "E7"] },
    { name: "Ballad", chords: ["Am", "F", "C", "G"] }
  ];

  // "Bm -> x 2 0 4 0 2  (cost 9)"  ->  { name: "Bm", frets: [-1,2,0,4,0,2], cost: 9 }
  function parseFingering(line) {
    if (typeof line !== "string") return null;
    var m = /^(\S+) -> ((?:[x0-9] ){5}[x0-9])(?: +\(cost (-?\d+)\))? *$/.exec(line);
    if (!m) return null;
    var frets = m[2].split(" ").map(function (c) { return c === "x" ? -1 : parseInt(c, 10); });
    for (var i = 0; i < 6; i++) if (frets[i] > MAX_FRET) return null;
    return { name: m[1], frets: frets, cost: m[3] === undefined ? null : parseInt(m[3], 10) };
  }

  // The newest fingering the board printed since it last powered up. At power-up the firmware prints
  // its default progression's fingerings just before its "ready" banner; those are not chords being
  // played, so anything up to the last banner is ignored.
  function latestFingering(entries) {
    entries = entries || [];
    var start = 0;
    for (var i = entries.length - 1; i >= 0; i--) {
      var line = entries[i] && entries[i].line;
      if (typeof line === "string" && /^AGAP\b.*\bready\b/.test(line)) { start = i + 1; break; }
    }
    for (var j = entries.length - 1; j >= start; j--) {
      var f = parseFingering(entries[j] && entries[j].line);
      if (f) return f;
    }
    return null;
  }

  // What to draw: one mark per string, and whether a barre is implied.
  function fretboardModel(frets) {
    if (!Array.isArray(frets) || frets.length !== 6) return null;
    var marks = [], pressed = 0, sounding = 0;
    for (var s = 0; s < 6; s++) {
      var f = frets[s];
      if (f < 0) marks.push({ string: s, kind: "mute", fret: -1 });
      else if (f === 0) { marks.push({ string: s, kind: "open", fret: 0 }); sounding++; }
      else { marks.push({ string: s, kind: "press", fret: f }); pressed++; sounding++; }
    }
    return { marks: marks, pressed: pressed, sounding: sounding, barre: findBarre(frets) };
  }

  function findBarre(frets) {
    var low = 99, s;
    for (s = 0; s < 6; s++) if (frets[s] > 0 && frets[s] < low) low = frets[s];
    if (low === 99) return null;
    var idx = [];
    for (s = 0; s < 6; s++) if (frets[s] === low) idx.push(s);
    if (idx.length < 3) return null;
    for (s = idx[0]; s <= idx[idx.length - 1]; s++) if (frets[s] < low) return null;
    return { fret: low, from: idx[0], to: idx[idx.length - 1] };
  }

  // What the diagram currently shows; never empty, so "nothing played yet" is drawn too.
  function boardKey(fingering) {
    return fingering ? fingering.name + "|" + fingering.frets.join(",") : "none";
  }

  // Is the chord the board reports the one that was asked for? The board may spell the root
  // differently (typed "Bb", reported "A#"), so roots are compared by pitch; the rest of the name
  // (quality) must match exactly, because "CM7" and "Cm7" are different chords.
  var PITCH = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };
  function splitChord(name) {
    var m = typeof name === "string" ? /^([A-Ga-g])([#b]?)(.*)$/.exec(name) : null;
    if (!m) return null;
    var pc = PITCH[m[1].toUpperCase()] + (m[2] === "#" ? 1 : m[2] === "b" ? -1 : 0);
    return { pc: (pc + 12) % 12, rest: m[3] };
  }
  function sameChord(a, b) {
    var x = splitChord(a), y = splitChord(b);
    return !!x && !!y && x.pc === y.pc && x.rest === y.rest;
  }

  function validChordName(s) { return typeof s === "string" && CHORD_RE.test(s); }

  function parseProgression(text) {
    var names = String(text || "").split(/[\s,]+/).filter(Boolean);
    if (!names.length) return { ok: false, error: "Add at least one chord." };
    if (names.length > MAX_SEQUENCE) return { ok: false, error: "At most " + MAX_SEQUENCE + " chords." };
    for (var i = 0; i < names.length; i++)
      if (!validChordName(names[i])) return { ok: false, error: "'" + names[i] + "' is not a chord name." };
    return { ok: true, chords: names };
  }

  function clampBpm(v) {
    var n = Math.round(Number(v));
    if (!isFinite(n)) return BPM_DEFAULT;
    return Math.max(BPM_MIN, Math.min(BPM_MAX, n));
  }
  function beatMs(bpm) { return Math.round(60000 / clampBpm(bpm)); }

  // ready | busy | locked | offline, from /api/status
  function uiState(status) {
    if (!status || !status.connected) return "offline";
    if (status.fretboard && !status.allow_unconfirmed) return "locked";
    if (status.busy) return "busy";
    return "ready";
  }

  // Who to blame when the page is offline: "bridge" (it did not answer), "board" (it answered but the
  // serial link is down), or null.
  function offlineReason(status, unreachable) {
    if (unreachable) return "bridge";
    if (status && !status.connected) return "board";
    return null;
  }

  // The last known status, marked as not connected, after a poll failed. Returns a copy.
  function markUnreachable(status) {
    if (!status) return status;
    var copy = {};
    Object.keys(status).forEach(function (k) { copy[k] = status[k]; });
    copy.connected = false;
    return copy;
  }

  function canSend(state, action) {
    if (action === "stop") return true;                       // never disabled
    if (state === "ready") return ACTIONS.indexOf(action) >= 0;
    if (state === "locked") return action === "release";      // letting go is always safe
    return false;
  }

  function difficulty(cost) {
    if (cost === null || cost === undefined) return "";
    return cost <= 10 ? "easy" : cost <= 40 ? "moderate" : "hard";
  }

  return {
    STRING_NAMES: STRING_NAMES, MAX_FRET: MAX_FRET, ACTIONS: ACTIONS,
    CHORD_GROUPS: CHORD_GROUPS, PRESETS: PRESETS,
    parseFingering: parseFingering, latestFingering: latestFingering, fretboardModel: fretboardModel, boardKey: boardKey, sameChord: sameChord,
    validChordName: validChordName, parseProgression: parseProgression,
    clampBpm: clampBpm, beatMs: beatMs, uiState: uiState, offlineReason: offlineReason, markUnreachable: markUnreachable, canSend: canSend, difficulty: difficulty
  };
});
