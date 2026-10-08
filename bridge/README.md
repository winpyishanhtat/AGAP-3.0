# AGAP remote-control bridge

Lets you drive the robot from a phone or another computer.

```
browser / phone --HTTP--> bridge (Pi or PC next to the robot) --USB serial--> Mega firmware
```

The bridge runs on the machine plugged into the Mega. There is no cloud
server to build or pay for: to reach the bridge from outside your network,
put it behind a tunnel or VPN (Tailscale, Cloudflare Tunnel, SSH) instead of
opening a router port. The bridge has no TLS of its own.

## Run it

```bash
pip install pyserial
python agap_bridge.py --port COM5            # Windows; /dev/ttyACM0 on a Pi
```

It prints a token. Open `http://localhost:8080`, paste the token, and you get
a STOP button, one button per chord, strum controls and a sequence box.

No board yet? `python agap_bridge.py --simulate --allow-unconfirmed` runs
against a stand-in so you can try the page. The stand-in only echoes
replies; it does not emulate the firmware's timing or state.

Options: `--host` (default 127.0.0.1), `--http-port`, `--map`, `--token`
(or `AGAP_BRIDGE_TOKEN`; 16+ characters), `--allow-calib`,
`--allow-unconfirmed`.

## What it enforces

| Rule | Why |
|---|---|
| Token on every API call | Anyone who can send a command moves hardware |
| Fixed list of actions (chord, press, release, strum, sequence, calib, stop); no raw passthrough | A client can't inject a firmware command. Only labels read from `button_map.json` are ever written to the port |
| STOP always accepted at once, even mid-sequence | Nobody can watch the robot remotely, so cancelling must never be blocked |
| Other commands refused (409) while a sequence runs; rate-limited (429) | Prevents queueing up presses on a robot you can't see |
| Buttons not `"confirmed"` in `button_map.json` refused (403) unless `--allow-unconfirmed` | The chord labels are still unverified guesses (README_AGAP.md step 1) |
| `calib` off unless `--allow-calib`, then capped (hold <= 2 s, reps <= 10, gap >= 0.5 s) | Repeated presses can heat a coil |
| Sequence <= 16 chords, 20-200 bpm; body <= 2 KB | Bounds a bad request |
| STOP sent when the bridge exits; 401 replies delayed | Fail safe; slows token guessing |

The firmware's own 8-second auto-release still applies underneath all of
this, so a dropped connection mid-chord does not leave a coil on.

## API

All calls need `Authorization: Bearer <token>`.

- `GET /api/status`: connection, busy flag, button list
- `GET /api/log`: the last board replies
- `POST /api/command` with a JSON body, e.g.
  `{"action":"chord","target":"Em"}`,
  `{"action":"sequence","targets":["C","G","Am","F"],"bpm":50}`,
  `{"action":"stop"}`

## Tests

```bash
python -m unittest test_agap_bridge -v      # 28 tests, no board needed
```

Covers auth, command-injection attempts, the allowlist, the unconfirmed and
calib gates, limits, busy/STOP priority, rate limiting, STOP on shutdown, and
the HTTP layer. I also broke the auth check and the STOP priority on purpose
to confirm the tests fail when they should.

## Not verified

None of this has run against a real board. The serial link to the Mega
(`SerialLink`) has not been exercised, and the remote path has not been
tested over a real tunnel. Do the bench steps in `README_AGAP.md` first.
