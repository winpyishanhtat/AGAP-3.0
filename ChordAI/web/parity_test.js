// Checks chord_ai.js against the Python solver on every chord/quality/fret-range.
// Usage: python dump_python_results.py > expected.json && node parity_test.js expected.json
const fs = require("fs");
const ChordAI = require("./chord_ai.js");
const expected = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
let total = 0, failures = 0;
function check(ok, msg) { total++; if (!ok) { failures++; console.log("FAIL " + msg); } }

for (const e of expected.solve) {
  const v = ChordAI.solve(e.name, e.maxFret);
  check(JSON.stringify(v.frets) === JSON.stringify(e.frets) && v.cost === e.cost,
        `solve ${e.name} maxFret=${e.maxFret}: js ${v.frets} (${v.cost}) vs py ${e.frets} (${e.cost})`);
}
for (const e of expected.recognize) {
  const got = ChordAI.recognize(e.frets).map(c => [ChordAI.NOTE_NAMES[c.root] + c.quality.names[0], c.score]);
  check(JSON.stringify(got) === JSON.stringify(e.top),
        `recognize ${e.frets}: js ${JSON.stringify(got)} vs py ${JSON.stringify(e.top)}`);
}
for (const e of expected.parse) {
  let got = null;
  try { const p = ChordAI.parseChord(e.text); got = ChordAI.NOTE_NAMES[p.root] + p.quality.names[0]; } catch (err) { got = null; }
  check(got === e.name, `parse "${e.text}": js ${got} vs py ${e.name}`);
}
check(ChordAI.isBarre([1,3,3,2,1,1]) === true, "isBarre full F");
check(ChordAI.isBarre([-1,3,2,0,1,0]) === false, "isBarre open C");
console.log(`${total - failures}/${total} parity checks passed`);
process.exit(failures ? 1 : 0);
