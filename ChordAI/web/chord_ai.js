// JavaScript port of ChordAI/chord_ai.py (solve / recognize / isBarre).
// Runs in the browser (global `ChordAI`) and in Node (module.exports).
// Keep it in step with the Python: web/parity_test.js compares the two.
(function (root) {
  "use strict";
  var MUTED = -1, NUM_STRINGS = 6;
  var STRING_NAMES = ["E", "A", "D", "G", "B", "e"];
  var OPEN_PC = [4, 9, 2, 7, 11, 4];
  var NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  var LETTER_PC = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };

  function Q(names, intervals, weights) { return { names: names, intervals: intervals, weights: weights }; }
  var QUALITIES = [
    Q(["", "maj"], [0, 4, 7], [40, 40, 8]),
    Q(["m", "min"], [0, 3, 7], [40, 40, 8]),
    Q(["7"], [0, 4, 7, 10], [40, 40, 6, 25]),
    Q(["maj7", "M7"], [0, 4, 7, 11], [40, 40, 6, 25]),
    Q(["m7"], [0, 3, 7, 10], [40, 40, 6, 25]),
    Q(["6"], [0, 4, 7, 9], [40, 40, 6, 25]),
    Q(["m6"], [0, 3, 7, 9], [40, 40, 6, 25]),
    Q(["add9"], [0, 4, 7, 2], [40, 40, 6, 25]),
    Q(["sus2"], [0, 2, 7], [40, 40, 8]),
    Q(["sus4", "sus"], [0, 5, 7], [40, 40, 8]),
    Q(["dim"], [0, 3, 6], [40, 40, 30]),
    Q(["aug"], [0, 4, 8], [40, 40, 30]),
    Q(["5"], [0, 7], [40, 40]),
    Q(["9"], [0, 4, 7, 10, 2], [40, 40, 6, 20, 15]),
    Q(["m9"], [0, 3, 7, 10, 2], [40, 40, 6, 20, 15]),
    Q(["7sus4"], [0, 5, 7, 10], [40, 40, 8, 20]),
    Q(["m7b5", "m7-5"], [0, 3, 6, 10], [40, 40, 20, 20])
  ];
  var EXACT = {}, LOWER = {}, lowerOwners = {};
  QUALITIES.forEach(function (q) {
    q.names.forEach(function (a) {
      EXACT[a] = q;
      var lo = a.toLowerCase();
      (lowerOwners[lo] = lowerOwners[lo] || {})[q.names[0]] = true;
    });
  });
  Object.keys(lowerOwners).forEach(function (lo) {
    if (Object.keys(lowerOwners[lo]).length === 1) {
      Object.keys(EXACT).forEach(function (a) { if (a.toLowerCase() === lo) LOWER[lo] = EXACT[a]; });
    }
  });

  function lookupQuality(text) {
    if (Object.prototype.hasOwnProperty.call(EXACT, text)) return EXACT[text];
    if (text.length < 3 || (text[0] === "M" && /[0-9]/.test(text[1]))) return null;
    return Object.prototype.hasOwnProperty.call(LOWER, text.toLowerCase()) ? LOWER[text.toLowerCase()] : null;
  }

  var COST_MUTE = 6, COST_INNER_MUTE = 4, COST_FRETTED = 1, COST_BASS_NOT_ROOT = 15, COST_THIN = 60;

  function mod12(n) { return ((n % 12) + 12) % 12; }

  function parseChord(text) {
    text = String(text).trim();
    if (!text) throw new Error("empty chord name");
    var letter = text[0].toUpperCase();
    if (!(letter in LETTER_PC)) throw new Error("'" + text + "': expected a note letter A-G first");
    var pc = LETTER_PC[letter], rest = text.slice(1);
    if (rest[0] === "#") { pc += 1; rest = rest.slice(1); }
    else if (rest[0] === "b") { pc -= 1; rest = rest.slice(1); }
    var q = lookupQuality(rest);
    if (!q) throw new Error("'" + text + "': unknown chord quality '" + rest + "'");
    return { root: mod12(pc), quality: q };
  }

  function search(rootPc, quality, maxFret) {
    var best = { frets: null, cost: 1e9 };
    var cur = [MUTED, MUTED, MUTED, MUTED, MUTED, MUTED];

    function toneIndex(pc) {
      var iv = mod12(pc - rootPc);
      for (var k = 0; k < quality.intervals.length; k++) if (quality.intervals[k] === iv) return k;
      return -1;
    }
    function finalCost(covered) {
      var cost = 0, s;
      for (var k = 0; k < quality.weights.length; k++) if (!(covered & (1 << k))) cost += quality.weights[k];
      var sounded = [];
      for (s = 0; s < NUM_STRINGS; s++) if (cur[s] !== MUTED) sounded.push(s);
      if (sounded.length < 3) cost += COST_THIN * (3 - sounded.length);
      if (sounded.length) {
        var first = sounded[0], last = sounded[sounded.length - 1];
        if ((OPEN_PC[first] + cur[first]) % 12 !== rootPc) cost += COST_BASS_NOT_ROOT;
        for (s = first + 1; s < last; s++) if (cur[s] === MUTED) cost += COST_INNER_MUTE;
      }
      return cost;
    }
    function dfs(s, covered, partial) {
      if (partial >= best.cost) return;
      if (s === NUM_STRINGS) {
        var total = partial + finalCost(covered);
        if (total < best.cost) { best.cost = total; best.frets = cur.slice(); }
        return;
      }
      for (var f = 0; f <= maxFret; f++) {
        var k = toneIndex((OPEN_PC[s] + f) % 12);
        if (k < 0) continue;
        cur[s] = f;
        dfs(s + 1, covered | (1 << k), partial + (f ? COST_FRETTED : 0));
      }
      cur[s] = MUTED;
      dfs(s + 1, covered, partial + COST_MUTE);
    }
    dfs(0, 0, 0);
    return best;
  }

  function solve(name, maxFret) {
    if (maxFret === undefined) maxFret = 3;
    var p = parseChord(name), b = search(p.root, p.quality, maxFret);
    return { frets: b.frets, cost: b.cost, root: p.root, quality: p.quality,
             chordName: NOTE_NAMES[p.root] + p.quality.names[0] };
  }

  function isBarre(frets) {
    var fretted = frets.filter(function (f) { return f !== MUTED && f !== 0; });
    if (fretted.length < 3) return false;
    var low = Math.min.apply(null, fretted);
    return fretted.filter(function (f) { return f === low; }).length >= 3;
  }

  function recognize(frets, maxCandidates) {
    if (maxCandidates === undefined) maxCandidates = 3;
    var sounded = {}, n = 0;
    frets.forEach(function (f, s) { if (f !== MUTED) { sounded[(OPEN_PC[s] + f) % 12] = true; } });
    var soundedList = Object.keys(sounded).map(Number);
    if (!soundedList.length) throw new Error("no strings sounded - nothing to recognize");
    var scored = [];
    for (var r = 0; r < 12; r++) QUALITIES.forEach(function (q) {
      var chordPcs = q.intervals.map(function (iv) { return (r + iv) % 12; });
      var hits = soundedList.filter(function (p) { return chordPcs.indexOf(p) >= 0; }).length;
      var foreign = soundedList.length - hits;
      scored.push({ score: hits * 2 - foreign * 3 - (q.intervals.length - hits), root: r, quality: q });
    });
    scored.sort(function (a, b) { return b.score - a.score; });
    var out = [], seen = {};
    for (var i = 0; i < scored.length && out.length < maxCandidates; i++) {
      var key = scored[i].root + ":" + scored[i].quality.names[0];
      if (seen[key]) continue;
      seen[key] = true;
      out.push(scored[i]);
    }
    return out;
  }

  function difficultyLabel(cost) {
    if (cost <= 10) return "easy (open chord)";
    if (cost <= 40) return "moderate";
    if (cost <= 90) return "hard (likely needs a barre or a stretch)";
    return "very hard";
  }

  var api = { MUTED: MUTED, STRING_NAMES: STRING_NAMES, NOTE_NAMES: NOTE_NAMES, QUALITIES: QUALITIES,
              parseChord: parseChord, solve: solve, recognize: recognize, isBarre: isBarre,
              difficultyLabel: difficultyLabel };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.ChordAI = api;
})(typeof window !== "undefined" ? window : this);
