# AGAP Helper-Button System — Step-by-Step User Manual

This walks through getting [`AGAP_HelperButton.ino`](AGAP_HelperButton.ino)
running on real hardware, from an empty Arduino Mega to playing a chord.
It assumes you've read the "what's confirmed vs. placeholder" warning in
[`README.md`](README.md) and `../README_AGAP.md` — **most timing and pin
values below are starting points, not measured specs.** Steps that need a
real physical measurement first are marked **⚠ TBD**.

## Can I run this with just an Arduino Mega?

Two different questions, two different answers:

- **Does it need a PC connected while running?** No. Once flashed, the
  board runs on its own — the panel buttons (Step 6 below) press and strum
  without any computer attached. A PC is only needed to flash the firmware
  (Step 2) and, optionally, to drive it over serial instead of the buttons
  (Step 7).
- **Can the Mega's pins power the solenoids and servos directly?** No.
  Never wire a solenoid coil straight to a Mega pin — it isn't rated for
  that current and you'll likely damage the pin or the board. You need,
  per solenoid channel, a MOSFET or relay driver plus a flyback diode
  (Step 3), and separate power supplies for the solenoids and servos
  (Step 4). The Mega is the brain; it switches those drivers, it doesn't
  power the coils itself.

So: **one Mega, no PC at runtime, but not Mega-alone** — you still need
driver electronics and external power between the board and the hardware.

## What you need

- Arduino Mega 2560 (not an Uno — see `../README.md` for why)
- USB cable (to flash it; a PC is not needed afterwards)
- 10× solenoid + MOSFET or relay driver + flyback diode, one set per
  button on the chord helper **⚠ TBD**: exact solenoid model depends on
  `README_AGAP.md` step 2 (not yet measured)
- 6× hobby servo (for the strummer — can reuse the `AGAP_Mega` build's
  strummer hardware if you have it)
- A 12 V supply for the solenoids, sized for your real measured coil
  current × number of channels that can be on at once **⚠ TBD**
- A separate 5–6 V, several-amp supply for the servos — don't share the
  solenoid supply, and tie all grounds together
- 4× momentary buttons: STRUM, NEXT, PREV, STOP
- The purchased mechanical chord helper itself, mounted on a tuned guitar
- A PC with the Arduino IDE or `arduino-cli`, for flashing only

## Step 1 — Get the firmware building

1. Install the Arduino IDE (or `arduino-cli`) and the AVR board package
   (`arduino:avr`), if you don't already have them.
2. Open [`AGAP_HelperButton.ino`](AGAP_HelperButton.ino). It needs
   [`agap_logic.h`](agap_logic.h) in the same folder — don't separate them.
3. Select **Board: Arduino Mega or Mega 2560** and the correct **Port**.
4. Compile (don't upload yet). It should report roughly 10 KB flash, under
   1 KB RAM used. If it doesn't compile, stop here — nothing past this
   point will work either.

   Command-line equivalent, if you're not using the IDE:
   ```bash
   arduino-cli compile --fqbn arduino:avr:mega AGAP_HelperButton
   ```

## Step 2 — Flash it, with nothing else connected yet

1. Connect the Mega to your PC by USB only — no solenoids, no servos, no
   external power yet.
2. Upload the sketch.
3. Open the Serial Monitor at **115200 baud**. You should see:
   ```
   AGAP (helper-button) ready - type HELP
   All button pins, labels and timings below are PLACEHOLDERS.
   Confirm them against README_AGAP.md steps 1, 2 and 5 before trusting any press.
   EM AM D C F DM G BM X1 X2
   ```
4. Type `HELP` and press Enter. If you get the command list back, the
   firmware is alive and the serial link works. Leave everything
   unplugged beyond USB until Step 5.

## Step 3 — Wire the solenoid drivers (power still off)

**⚠ Do this with the 12 V supply switched off.**

1. For each of the 10 channels, build a driver: MOSFET (or relay) gate/coil
   from the Mega pin (through a gate resistor if MOSFET), solenoid coil
   from +12 V to the driver's drain/collector, and a flyback diode across
   the coil, cathode to +12 V. Never skip the flyback diode — without it,
   the voltage spike when a coil turns off can damage the driver or the
   Mega pin driving it.
2. Wire channel indices 0–9 to Mega pins **D22 D23 D24 D25 D26 D27 D28 D29
   D37 D36** in that order — see the pin-map comment at the top of
   [`AGAP_HelperButton.ino`](AGAP_HelperButton.ino). This order is a
   **guess** from the helper's photographed layout, not yet confirmed
   (`README_AGAP.md` step 1) — it's fine to wire it this way for testing,
   but don't assume `BUTTON_LABEL[i]` is the right name for whichever
   physical button ends up on channel `i` until you've checked.
3. Double-check polarity and continuity with a multimeter before applying
   power. A reversed flyback diode looks fine until the first switch-off,
   then fails.

## Step 4 — Wire the servos, buttons, and power (still off)

1. Servos (strummer, 6th string to 1st): Mega pins **D2 D3 D4 D5 D6 D7**.
   Servo power comes from the separate 5–6 V supply, **not** from the
   Mega's 5 V pin — six servos can draw more than the Mega can supply.
2. Panel buttons, each one side to the pin, the other side to **GND**
   (the firmware uses internal pull-ups, so no external resistor needed):
   **A0 STRUM, A1 NEXT, A2 PREV, A3 STOP**.
3. Connect every ground together — Mega, 12 V supply, 5–6 V supply, all
   common.
4. Leave the chord helper and guitar disconnected from the solenoids for
   now; Step 5 tests one channel in open air first.

## Step 5 — ⚠ TBD: test and calibrate one channel before trusting any of them

This is `README_AGAP.md` step 5 ("run one powered channel"), and it comes
*before* wiring the rest, on purpose — find problems on one channel, not
all ten.

1. Power up the 12 V and 5–6 V supplies.
2. In the Serial Monitor, check one channel presses and releases cleanly,
   with nothing attached to the solenoid's plunger yet:
   ```
   PRESS EM
   RELEASE EM
   ```
3. Run the repeated-press test to check for heating, binding, or
   incomplete return:
   ```
   CALIB EM 1000 5
   ```
   This presses channel `EM`, holds 1000 ms, releases, waits, and repeats
   5 times — exactly what step 5 of `README_AGAP.md` asks for. Watch and
   feel the driver and coil; stop (see below) at the first sign of
   overheating.
4. If a run needs to be cut off early, either press the physical **STOP**
   button or send `STOP` — both work even mid-`CALIB`, immediately.
5. Only once one channel behaves safely and predictably should you wire
   and power the remaining nine the same way.
6. Mount the solenoid against the real chord-helper button and repeat the
   `CALIB` test loaded. Record travel, force behaviour, and temperature in
   [`../docs/measurement_log_template.csv`](../docs/measurement_log_template.csv)
   — keep failed attempts in the log too, not just the good ones.
7. Adjust timing from the measured results, not guesses:
   ```
   KICK 60       set the full-power seating pulse, in ms
   HOLD 60       set the hold-phase duty, 0-100 (percent)
   ```

## Step 6 — Standalone operation (no PC)

Once wiring and one-channel testing are done, the board runs on its own:

- **STRUM** button: strums the currently armed chord (alternates down/up
  each press).
- **NEXT** / **PREV**: step to the next/previous button in the compiled-in
  list (`EM AM D C F DM G BM X1 X2`) and press + strum it.
- **STOP**: releases every channel immediately, cancels any run in
  progress.

This needs nothing beyond the Mega, its drivers, and power — no USB cable,
no phone, no PC.

## Step 7 — Optional: control it from a PC with the Python sequencer

For testing full progressions or driving it from a laptop instead of the
panel buttons:

```bash
pip install pyserial

python ../tools/agap_control.py ports                      # find the COM port
python ../tools/agap_control.py --port COM5 labels
python ../tools/agap_control.py --port COM5 chord Em        # chord name or device label
python ../tools/agap_control.py --port COM5 sequence C G Am F --bpm 50
python ../tools/agap_control.py --port COM5 stop
```

See [`README.md`](README.md) for the full command reference (serial
protocol and Python CLI both).

## Step 8 — Confirm the real chord chart

Everything above runs with placeholder labels. Once `README_AGAP.md` step
1 is done (measuring the real helper and identifying the two unlabeled
positions):

1. Edit [`button_map.json`](button_map.json) so each `"chord"` field
   matches what that button actually does on the real device, and set
   `"confirmed": true` per button as you verify it.
2. If the physical wiring order doesn't match the firmware's assumed
   order (Step 3 above), reorder `BUTTON_LABEL[]` in
   [`AGAP_HelperButton.ino`](AGAP_HelperButton.ino) to match reality,
   rather than rewiring — whichever is less trouble for your enclosure.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Nothing in Serial Monitor at 115200 | Wrong baud rate, or the board reset mid-boot — reopen the monitor after upload finishes |
| `ERR unknown label, see LABELS` | Typo, or you're using a chord name instead of a device label (or vice versa) — `tools/agap_control.py` accepts either, raw serial commands need the exact label from `LABELS` |
| A channel won't release | Send `STOP`, or press the panel STOP button — both cut every channel immediately. If it still won't physically release, check the driver and flyback diode before applying power again |
| Solenoid or driver gets hot during `CALIB` | Lower `HOLD` duty, shorten `KICK`, or increase the gap between reps — then re-test before trusting it in a progression |
| `SEQUENCE`/`CALIB` seems stuck | Both poll for `STOP` continuously, including from the panel button — if truly unresponsive, cycle power and check the wiring from Step 3 |
