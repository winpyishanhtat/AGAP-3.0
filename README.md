# AGAP — Arduino Guitar Auto Player (Arduino Mega 2560)

A robot that plays a real guitar: 18 solenoids press the strings on frets 1–3, and 6 servos pluck them. An on-board AI search swaps barre chords for open-chord voicings that stay within frets 0–3.

Sketch: [AGAP_Mega/AGAP_Mega.ino](AGAP_Mega/AGAP_Mega.ino)

## Can each feature run on an Arduino?

| Feature (from the project sheet) | Uno | Mega 2560 | How the sketch does it |
|---|---|---|---|
| 18 solenoids (6 strings × frets 1–3) | ✗ only 20 I/O pins in total | ✓ | D22–D29, D30–D37, D48–D49 via MOSFETs |
| 6 servos for plucking/strumming | ✗ not enough pins left | ✓ | D2–D7, `Servo` lib (Timer5) |
| Kick-and-hold PWM on all 18 coils | ✗ 6 PWM pins | ✓ | Timer2 ISR: 100 % kick, then ~30 % hold, at 7.8 kHz |
| AI constraint search + cost function (barre → open) | ⚠ only 2 KB RAM | ✓ | Branch-and-bound DFS, finishes in a few ms |
| Search engine usable without the robot | — | ✓ | `SHOW <chord>` prints the voicing without moving anything |
| Assistive mode (button / screen) | ⚠ | ✓ | A0–A4 buttons, Nextion on Serial2, phone over HC-05 on Serial1 |
| Teaching mode (slow, repeated finger placement) | ⚠ | ✓ | Places one finger per beat, plays an arpeggio, then strums 4 times |
| Real guitar sound, no guitar modification | ✓ | ✓ | Mechanical (cradle), so the board choice doesn't matter |

An Uno would need 2× PCA9685 or shift registers, and it has only one UART, which the PC and Bluetooth would have to share. Use the **Mega 2560**.

## Hardware notes
- **Solenoid drivers:** for each coil, a logic-level MOSFET (IRLZ44N or IRLB8721), a 220 Ω gate resistor, a 10 k pull-down and a fast flyback diode (SS34 or 1N5819) across the coil. Don't use a ULN2803 if a kick draws more than 0.5 A.
- **Power:** use a 12 V supply for the solenoids, sized for 6 kicks at once. Kicks are staggered by 4 ms. Power the servos from a separate 5–6 V, ≥3 A buck converter. Connect all grounds together.
- **Don't use `tone()` or `analogWrite` on pins 9/10.** The solenoid PWM uses Timer2.

## Serial commands (USB 115200, Bluetooth/Nextion 9600)
```
SHOW Bm          -> Bm -> x 2 0 x 0 2   (AI voicing only)
CHORD F#m7       press + strum
RAW x 3 2 0 1 0  explicit frets 6th..1st
PROG C G Am F    load progression
MODE TEACH | MODE ASSIST, NEXT, PREV, STRUM [D|U], STOP, STATUS
TEMPO 40, HOLD 30, KICK 40, PLUCK 1-6 (calibration)
```
