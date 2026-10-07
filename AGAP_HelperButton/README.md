# AGAP — Helper-Button Control System

This is the control system for the **current** AGAP direction described in
`README_AGAP.md` (see the repo root): a purchased mechanical chord helper
sits on the guitar neck, and solenoids press the helper's own buttons
instead of pressing the strings directly. It replaces the earlier
[`../AGAP_Mega`](../AGAP_Mega) design (18 solenoids on individual
frets), kept in this repo for reference only.

## What's here vs. what's still physical work

`README_AGAP.md` section 9 lists 7 steps. This system is the **software and
firmware side of steps 5–7** — it has nothing to measure or verify with,
so it cannot do steps 1–4 for you:

| Step | Who/what does it |
|---|---|
| 1. Measure button layout | You, with calipers, on the real helper |
| 2. Measure the solenoid | You, with calipers/multimeter |
| 3. Prove one chord manually | You, by hand, on a tuned guitar |
| 4. Print a fit test | You, on a 3D printer |
| 5. Run one powered channel | **This firmware's `CALIB` command**, plus you watching/listening |
| 6. Design the shared frame | Depends on real numbers from steps 1–2 |
| 7. Integrate control + strumming | **This firmware + `../tools/agap_control.py`** |

Every pin assignment, label, and timing constant in
[`AGAP_HelperButton.ino`](AGAP_HelperButton.ino) is marked `TBD` in comments
and is a placeholder, not a measured value. [`button_map.json`](button_map.json)
is the editable source of truth for which label means which chord — edit
that file (not the firmware) once step 1 confirms the real chart, and set
`"confirmed": true` per button as each one is verified.

## Firmware (Arduino Mega 2560)

Flash [`AGAP_HelperButton.ino`](AGAP_HelperButton.ino). It compiles clean
against `arduino:avr:mega` (~10 KB flash, ~760 B RAM — verified in this repo).

- 10 driver channels (kick-and-hold PWM via Timer2, same scheme as the
  earlier sketch) press the helper's buttons.
- 6 servos (reused strummer hardware) strum all six strings together —
  unlike the old per-string solver, the helper frets every string at once,
  so there's no muting logic needed here.
- `STOP` (serial command or the panel button) drops every output
  immediately, even in the middle of a `SEQUENCE` or `CALIB` run.
- Any channel left pressed for more than `CMD_TIMEOUT_MS` (default 8 s)
  with no refresh auto-releases.

See the in-sketch `HELP` command, or the header comment, for the full
command list: `PRESS`, `RELEASE`, `CHORD`, `STRUM`, `SEQUENCE`, `CALIB`,
`TEMPO`, `KICK`, `HOLD`, `STATUS`, `STOP`, `LABELS`.

### Tested logic (`agap_logic.h`)

The channel state machine (off → pending → kick → hold, with mask
computation and the auto-release timeout) and the strum sequencer live in
[`agap_logic.h`](agap_logic.h) — plain C++, no Arduino/AVR dependency.
`AGAP_HelperButton.ino` includes it directly (`agap::ChannelDriver`,
`agap::Strummer`, `agap::findLabel`), so the firmware runs the exact code
the tests check, not a parallel copy.

```bash
cd tests
./run_tests.sh        # or: g++ -std=c++14 -I.. test_agap_logic.cpp -o t && ./t
```

74 checks: label lookup, every channel-state transition (including the
PORTA/PORTC register-boundary split at channel 8, the kick→hold duty
switch, and the timeout/refresh timing), and strum ordering in both
directions. Runs on any machine with g++ — no board, no Arduino IDE.
Change the state machine in `agap_logic.h` and its tests together; keep
the `.ino` itself limited to wiring that logic to real pins, `millis()`,
`Serial` and the Timer2 ISR.

**Wiring safety, restated from `README_AGAP.md` section 7:** never wire a
solenoid coil directly to a Mega pin. Each channel needs its own
appropriately rated MOSFET or relay driver and flyback suppression. Size
the 12 V supply from the real measured coil current (step 2) times the
number of channels that can be active together.

## Python sequencing layer (`../tools/agap_control.py`)

This is the "Python chord selection / sequencing" box in
`README_AGAP.md`'s architecture diagram. It's a thin serial client —
it sends text commands and prints what the board reports, nothing more.
It cannot confirm a chord actually sounded correctly; that's still you,
listening, per steps 3 and 5.

```bash
pip install pyserial

python ../tools/agap_control.py ports                 # find the COM port
python ../tools/agap_control.py --port COM5 labels
python ../tools/agap_control.py --port COM5 press EM
python ../tools/agap_control.py --port COM5 calib EM --hold-ms 1000 --reps 5
python ../tools/agap_control.py --port COM5 chord Em   # chord name or device label both work
python ../tools/agap_control.py --port COM5 sequence C G Am F --bpm 50
python ../tools/agap_control.py --port COM5 stop
```

`calib` is the direct tool for step 5 ("test one actual solenoid through
repeated loaded presses and the intended hold duration") — it repeats a
press/hold/release cycle on one button so you can watch for heating,
incomplete return, or binding before building the full 10-channel frame.

## Measurement log

[`../docs/measurement_log_template.csv`](../docs/measurement_log_template.csv)
has the columns `README_AGAP.md` section 10 asks you to keep: date, step,
button/chord, guitar setup, actuator model, coil voltage under load,
current, button travel, hold duration, press/release result, chord
quality notes, temperature notes, pass/fail. Keep failed attempts in it,
not just the successful ones.
