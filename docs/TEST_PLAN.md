# AGAP test plan and test report

Two phases. Phase 1 is automated and runs on a PC with no hardware; it has been
done and is summarised below, with the bugs it found. Phase 2 needs the real
robot and has **not been started**: it is written as an ordered checklist with
pass criteria and a log sheet.

## Phase 1: automated testing (done)

Run everything: `python agap.py selftest` (or `python agap.py` and pick "Run all
tests"). CI runs the same suites on every push.

| Layer | What it checks | Count |
|---|---|---|
| Logic unit tests (C++) | Channel state machine, masks, six-coil limit, strum order and muting, saved-tuning encoding, strict number parsing | 232 checks (shared logic) + 256 (fretboard mapping) |
| Chord search vs ChordAI | The C++ search on the board gives the same fingering and cost as `ChordAI/chord_ai.py`, for every root and quality, at 3 and 5 frets, plus chord-name parsing | 704 checks (528 solves, 38 name parses) |
| **Firmware simulation** | The **real sketches** (`AGAP_Fretboard.ino`, `AGAP_HelperButton.ino`) compiled on the PC against a mock Arduino: serial, clock, ports, EEPROM, servos, buttons. Scripted scenarios plus random-command fuzzing, with safety rules checked on every simulated millisecond | Fretboard 31 scenarios (fuzz: 4,000 commands); helper 12 (fuzz: 3,000) |
| ChordAI | Python solver; the JavaScript on the web page matches it | 26 tests + 572 parity checks |
| Launcher, bridge | Doctor, bring-up, console, flash command, remote bridge (auth, injection attempts, limits, STOP priority) | 64 + 40 tests |
| **Whole chain on the real firmware logic** | The phone page's logic, the bridge, and the **real `AGAP_Fretboard.ino`** (compiled for the PC against a mock Arduino): a chord sent over HTTP is solved by the firmware's own code, the page reads its reply, and the simulated coil pins are counted and compared with what the page would draw. See Phase 1c | 14 tests |
| **C++ control tool** | `tools/agap_control.cpp` (fretboard commands, strict arguments, drawn fingering, exit codes): 212 native unit checks of its planner, plus 15 tests that run the **compiled tool** against the real firmware logic through its `--stdio` transport. See Phase 1c | 212 checks + 15 tests |
| **Phone page** | What the bridge serves and forbids, the page source (no outside code), page rules identical to the bridge's, the page reading every simulated board reply, and the whole flow over HTTP. See Phase 1b | 31 Python tests + 44 JavaScript checks |
| Part measuring | G-code / STL / SVG inspectors, fit checker, deck drawing | 4 suites |
| **Design-file claims** | Facts in `HARDWARE_SPECS.md` checked against the real `.hvs` / `.stl` files: rails identical, jaws the same shape, clamps different, plate sockets match their STLs, rail slot pitch follows the fret rule, bed overrun flagged, each file's SHA recorded in the docs. Needs the files (not committed): set `AGAP_PARTS_DIR`. Without them the suite reports **SKIP**, not PASS | 20 tests (CI skips them) |
| Both sketches compile for the Mega 2560 | `arduino-cli` (also in CI) | 3 sketches |

**Safety rules the simulation enforces on every simulated millisecond** (including
inside the blocking `SEQUENCE` and `CALIB` loops): never more than 6 coils on at once
(fretboard); a kicking coil is always also marked on; no channel beyond 29 and
none of the unused PORTK pins is ever driven. After 4,000 random commands (valid,
invalid, garbage bytes, over-long lines, STOP presses at random moments) the board
must still accept and play a chord.

### What Phase 1 cannot show

The simulation runs the sketches' logic, not the hardware. It does **not** reproduce:

- the AVR's speed (the chord search time on the Mega is an estimate, not measured),
- real interrupt timing or PWM electrical behaviour,
- the solenoids, drivers, supply, heat, or any mechanical fit,
- the real servos or the sound of a chord,
- the USB serial link to a real computer.

That is what Phase 2 is for.

### Bugs found and fixed in Phase 1

Every one is guarded by a test that fails without the fix (checked by putting the bug back).

| # | Bug | Found by | Effect | Fix |
|---|---|---|---|---|
| 1 | **A serial `STOP` sent during `SEQUENCE` or `CALIB` was never seen.** The command was still in the shared line buffer, so the incoming STOP was glued onto its end and the command's own text was corrupted | Firmware simulation | The remote/serial stop did not work mid-run; only the panel button did (both sketches) | The command is copied out and the buffer freed before it runs |
| 2 | **Re-pressing a held coil released it immediately.** `loop()` reads the clock once, a command re-pressed the coil with a later reading, and the unsigned `now - refresh` wrapped to a huge number so the 8 s safety timeout fired at once | Firmware simulation | A chord change that shares a fingering with the previous one dropped the shared solenoids (a chord with missing notes); same in the helper build | Signed elapsed time; also tested across the 49-day clock wrap |
| 3 | **`CALIB` counts wrapped silently**: 300 repeats ran as 44, `-5` ran as 251 (about 6 minutes), 70000 ms became 4464 | Firmware simulation | A typo could exercise a coil for minutes | Strict limits: hold 10-7000 ms, repeats 1-100, gap 0-10000 ms; out-of-range is an error |
| 4 | **Non-numeric arguments were accepted**: `KICK abc` set a 10 ms kick, `TEMPO fast` set 20 bpm, `PICK 6 A x` set angle 10 | Firmware simulation | Silent bad settings | Strict number parsing; a non-number is an error and changes nothing |
| 5 | **`HOLD` with no argument reset the hold to 30%**, and `HOLD 0` let go of the string right after the kick | Firmware simulation | A query changed a setting; a 0% hold fails to hold | `HOLD` alone reports; the range is 10-100%, also enforced on saved tuning |
| 6 | **An over-long line was cut at 95 characters and the stub executed** | Firmware simulation | A long line could run as a different command than typed | The whole line is refused (`ERR line too long`) |
| 7 | **`PROG` with more than 12 chords was silently cut to 12** | Firmware simulation | Surprising | Refused with an error, progression unchanged |
| 8 | **Helper build: `SEQUENCE` moved the first buttons before noticing a bad label later in the list** | Firmware simulation | Buttons left pressed | The whole list is checked first (the fretboard already did this) |
| 9 | `CM7` parsed as C minor 7 and `CM` as C minor (chord names matched case-insensitively) | Code review during the fretboard port | Wrong chords from the Python tool and the web page | Exact spelling first; bare capital `M` rejected |
| 10 | `KICK` changed a printed number but not the real kick time (an earlier refactor copied the value once) | Fine-tuning review | The pulse could not be tuned | `setKickMs`, with a test |
| 11 | The fretboard doctor check expected the 3-fret answer for Bm, so it would fail on a healthy board | Self-review | False alarm on first power-up | Expects the 5-fret answer; pinned in the C++ test |
| 12 | A sketch-defined type used before the first function broke the real Arduino build though the PC build passed | Compiling for the board | Sketch did not build | Type declared above the first function |
| 13 | The specs said the rails and lower jaws were "true mirror pairs"; the rail STLs are in fact byte-identical and the jaws the same shape | Comparing the new rail print file with the STLs | Wrong assumption when mounting the rails | Text corrected; `stl_inspect.py --compare` and a test pin it |
| 14 | The rail print file moves to X = 155 mm, outside its own 150 mm slicer profile; nothing flagged it. The superseded clamp bed did the same (X = 155.1 mm) | Reading the new rail file, then re-checking the older one | A print that clips or collides if the bed really is 150 mm | `hvs_inspect.py` now warns on every run; test pins it; manual says to check the bed. The finalized clamp bed fits (X <= 122 mm) |
| 15 | The phone page's fretboard diagram was **blank** until a chord was played: the first draw compared an empty key with an empty starting value | Looking at the page in a browser (no unit test could see it) | A blank picture on first load | `boardKey` never returns an empty key; test pins it |
| 16 | After the bridge died the page kept saying Ready or Locked for about 8 s (two failed polls, plus a slow refused connection), and blamed the USB cable when the network was the problem | Killing the bridge while the page was open | A stale "Ready" on a robot nobody can see | One failed poll shows Offline; `offlineReason` names bridge or board; banner states what the board does on its own |
| 17 | An edit to `app.js` left a stray brace and killed the whole page; no test noticed | The same browser session (console: `Uncaught SyntaxError`) | A dead page after any bad edit | `node --check` of both scripts is now a test |
| 18 | The page showed **"Now playing: F"** before anything was played: at power-up the firmware prints its default progression's fingerings and the page took the last one for a chord | Running the page against the real firmware (the stand-in never printed them) | A wrong chord displayed on a robot nobody can see | Everything before the boot banner is ignored; test pins it, plus an end-to-end test on the real boot |
| 19 | Typed **Bb**, the real firmware answers **A#**. The page compared the two names as text, so the pressed key and the lit setlist chip would never light for flats | End-to-end test with the real firmware | Silent loss of the highlight for every flat chord | `sameChord` compares roots by pitch and keeps the quality exact (so `CM7` and `Cm7` stay different) |
| 20 | Starting the bridge on a busy port (8080 is not free on this PC) crashed with a raw traceback | Trying `agap.py demo` for real | A first-time user is stuck with no advice | Plain message with `--http-port N` advice; the launcher passes the option through |
| 21 | The bridge's token line did not appear when its output was captured (Python buffers piped output) | The same try-out, with output redirected | A launcher or app that captures output never shows the token | Line-buffered output in the bridge and launcher; test starts the bridge piped and waits for the token |
| 22 | The bridge and the page accepted sequences of 13-16 chords, but the real firmware refuses anything over 12 (`ERR too many chords`); the bridge would then report "busy" for a sequence that never played | Driving the real firmware (found while planning the C++ tool) | A progression silently not playing | Per-mode limit (12 fretboard, 16 helper) reported in `/api/status` and used by the page; a test ties it to `MAX_PROG` in the sketch; end-to-end test with 12 and 13 |
| 23 | The C++ tool turned a non-number into 0 (`atoi`): `--bpm abc` sent `TEMPO 0` and divided by zero for its wait time. It also spoke only the earlier helper protocol | Code review while updating the tool | A wrong or undefined command on a real board | Strict whole-number parsing with the firmware's ranges, in both modes; 212 unit checks and 15 end-to-end tests |

## Phase 1b: the phone page ("AGAP Stage")

Automated (run by `python agap.py selftest`, and in CI):

| Check | Why |
|---|---|
| The bridge serves exactly four files without a token and nothing else (path tricks such as `/../`, `%2e%2e`, `/app.js%00.py` included) | The robot's computer must not leak its files |
| A strict Content-Security-Policy on every response; the page has no external URL, inline script, inline style, inline handler, `eval` or `innerHTML` | A page-injection bug could not load or run anything |
| Both scripts are valid JavaScript | Bug 17 |
| The page's chord-name filter gives the same answer as the bridge's for 33 tricky names; sequence length and tempo limits equal the bridge's | The page must not offer what the bridge refuses, or accept what it should not |
| Every action the page sends is in the bridge's list; it never sends `calib` or `press` | No hidden power |
| The page reads every fingering the simulated board prints (12 roots x 7 qualities) exactly as ChordAI solved it | The picture must match the board |
| Locked, busy and offline: what the page disables is also what the bridge refuses (403 / 409), and STOP is never disabled | The page's rules and the bridge's must agree |
| Whole flow over real HTTP: chord, log, fingering, drawn model | End to end without a browser |

Mutation-checked: loosening the page's chord rule, letting `locked` allow chords, and removing
the policy header each fail tests (caught by 2, 2 and 1 tests).

By hand in the desktop app's built-in browser, against `--simulate` (done; found bugs 15-17):

| Check | Result |
|---|---|
| Login with the token; wrong token message | Pass |
| Play `Bm`: name, "3 coils press, 5 of 6 strings ring", EASY badge, diagram matches `x 2 0 4 0 2` | Pass |
| Phone width (375 px), dark and light themes | Pass (nut was invisible in light: fixed) |
| Play a progression: busy state disables controls, chip lights, STOP clears it | Pass |
| Locked bridge: banner; all chord keys, strum and Play disabled; Release and STOP enabled | Pass |
| Kill the bridge mid-session: Offline within a few seconds with the right banner | Pass after bug 16 |
| Browser console: no errors | Pass |

**Not done:** a real phone (touch, Safari, on-screen keyboard, screen lock), over a tunnel,
screen-reader pass, and anything against a real board. Add these to the bench checklist
below when the board exists.

## Phase 1c: the whole chain on the real firmware logic

Until now the bridge and page were tested against **stand-ins** that make up the board's replies.
`AGAP_Fretboard/sim/sim_serve.cpp` runs the **real sketch** (its command parser, chord search, state
machines, blocking SEQUENCE / CALIB loops, saved tuning and pin masks) on this computer against a
mock Arduino, as a program that speaks the serial protocol on stdin/stdout. The bridge talks to it
through `ProcessLink`, so the tests drive: page logic -> HTTP -> bridge -> **real firmware code** ->
simulated coil pins.

| Check (`python -m unittest test_agap_e2e`, in `selftest` and CI) | Result |
|---|---|
| The real firmware boots through the bridge | Pass |
| `chord Bm` over HTTP: the firmware prints `Bm -> x 2 0 4 0 2  (cost 9)` and exactly the three right coils switch on (channels worked out independently of the firmware) | Pass |
| `STOP` releases every coil; `release ALL` lets go | Pass |
| A chord the firmware does not know is refused by the firmware, and no coil moves | Pass |
| Seven presses: the real firmware refuses the one beyond six coils | Pass |
| **STOP during a running SEQUENCE** (the blocking loop): the firmware aborts (`ABORTED (STOP)`), every coil releases, the bridge is no longer busy | Pass |
| **A coil nobody refreshes releases itself between 5 and 10 s later** (the 8 s firmware timeout): a dead phone, bridge or cable cannot leave a solenoid energised | Pass |
| The page's parser reads the real fingerings of 84 chords and agrees with ChordAI on frets and cost | Pass |
| For 9 chords, **the number and the channels of coils that switch on equal what the page would draw** | Pass |
| A freshly booted board reads as "nothing played yet" | Pass (bug 18) |
| 12-chord sequences play; 13 are refused by the bridge before the firmware sees them | Pass (bug 22) |
| A locked bridge refuses chord / strum / sequence and the coils stay off | Pass |
| If the firmware dies, the bridge marks itself disconnected and refuses commands (503) | Pass |
| Across all of the above, the firmware's own safety checks (never more than 6 coils, kick subset of on, no channel beyond 29) were never violated | Pass |

**The C++ control tool on the same real firmware** (`python -m unittest test_control_e2e` in `tools/tests`):
the compiled tool runs in `--stdio` mode (commands on stdout, board lines on stdin) wired to `sim_serve`.
It sends the exact lines (`CHORD Bm`, `PRESS 6 1`, `TEMPO 200` + `SEQUENCE C G`), prints and draws
`Bm -> x 2 0 4 0 2  (cost 9)` with one `@` per coil, and the pins that follow are the right channels. It
refuses 16 kinds of bad command with exit code 2 and sends nothing to the board, refuses a typo before the
2-second reset, returns 1 when the board answers `ERR`, warns about the heat of `calib`, and warns when
`--helper` is used on the fretboard firmware. Its limits are tied to the sketch's own constants and
CALIB usage text, and it compiles with `-Wall -Wextra -Werror`.

**Mutation-checked on the firmware itself:** raising `MAX_COILS` to 7 fails 2 of these tests; making
the 8 s coil timeout effectively infinite fails the timeout test. (Both edits were reverted.)

**Design note:** the tests run in real time on purpose. The firmware's 8 s auto-release is a safety
feature, and racing the simulated clock made coils let go before a test could look at them.

Run it yourself with no hardware: `python agap.py demo` builds the real sketch for this computer and
starts the bridge on it, then open the page and paste the token. If port 8080 is taken, add
`--http-port 8777`.

**What this still does not show:** real serial timing and framing, USB, the AVR's speed and interrupt
timing, drivers, solenoids, a real phone, a tunnel. It proves the software chain, not the robot.

## Phase 2: bench testing on the real hardware (not started)

Work down the list in order; stop at the first failure and fix it before going on.
Record each result in [`bench_test_log.csv`](bench_test_log.csv). Keep failures too.
The pass criteria marked *(unmeasured)* depend on figures nobody has yet (solenoid
datasheet, real gap to the string); fill them in from your first measurements.

| ID | Test | How | Pass when |
|---|---|---|---|
| B0 | Power-off checks | Multimeter on every driver: coil polarity, flyback diode direction, grounds common, no short between 12 V and ground | All correct before any power |
| B1 | Fit coupon | Print `00-PRINT-FIRST-solenoid-fit-coupon`; put the solenoid in | Body fits the 7.5 x 10.5 mm pocket, plunger moves freely through the 4.8 mm hole |
| B2 | Connection | `python agap.py doctor` | No `[FAIL]` lines (it moves nothing) |
| B3 | Pick arms | `python agap.py bringup`, pick step; `PICKS`, `PLUCK 6`... | Each of the 6 arms swings across **its own** string on both sides without touching the next; note which pod serves which string |
| B4 | One solenoid | Bring-up step 3 (`PRESS 6 1`) | It clicks and releases cleanly; **no heat** |
| B5 | Endurance | `CALIB 6 1 1000 5 800` | 5/5 presses seat; coil and driver stay cool enough to hold a finger on for 5 s |
| B6 | All 30 channels | Bring-up "test the other 29" | Each socket's solenoid moves; the string order matches (fix wrong order by swapping wires) |
| B7 | Kick tuning | `docs/TUNING_GUIDE.md` step 2 | Smallest `KICK` that seats the plunger on every try, plus margin |
| B8 | Hold tuning | Guide step 3 | Lowest `HOLD` that keeps the string pressed through the longest hold; no creep, not hot |
| B9 | Six coils at once | `CHORD` of a 5-6 coil shape (e.g. `F`), measure the 12 V rail at the supply and at the drivers | Rail does not sag below the solenoid's working voltage *(unmeasured)*; supply stays cool |
| B10 | Chord shapes | `CHORD` each of C D E F G A B Am Dm Em Bm, then strum | Every string that should ring does, no buzz from a half-pressed fret |
| B11 | Mute handling | A chord with a muted string (`C`) | The muted string's pick does not swing |
| B12 | Chord change | `SEQUENCE C G Am F` at several tempos; raise `TEMPO` until it fails | The tempo where changes start to sound late or muted is recorded as the limit |
| B13 | Serial STOP | `SEQUENCE` running, send `STOP` | Everything releases within about a beat; the board accepts the next command |
| B14 | Panel STOP | `SEQUENCE` running, press the panel STOP button | Same |
| B15 | Auto-release | `PRESS 6 1`, do nothing for 8 s | Releases itself at about 8 s |
| B16 | Saved tuning | `KICK`/`HOLD` changed, `SAVE`, power-cycle | Values come back (the boot banner says "Loaded saved tuning"); check after re-flashing too |
| B17 | USB unplug | Pull the USB cable mid-hold | Everything releases within 8 s (the board has no other way to know) |
| B18 | Search time | `SHOW D9` (the slowest chord), time the reply | Reply in well under a second (estimate: about 0.1 s); record the real figure |
| B19 | Remote bridge | `python agap.py bridge`, drive it from a phone on the same network | Chord plays; STOP stops it; a wrong token is refused |
| B20 | Soak | `SEQUENCE C G Am F` repeating for 30 minutes | No missed chords, no drift, nothing hot; supply and drivers stay cool |

## How to add a test

- Logic that does not touch pins: add to `AGAP_HelperButton/tests/test_agap_logic.cpp`
  or `AGAP_Fretboard/tests/`.
- Anything about command handling, timing, STOP, tuning: add a scenario to
  `AGAP_Fretboard/sim/sim_fretboard.cpp` (or `AGAP_HelperButton/sim/sim_helper.cpp`).
  Write the failing scenario first.
- If a bug is found on the real board, **reproduce it in a simulation scenario first**
  where possible, then fix it, then add a row to the table above.
