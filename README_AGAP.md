# AGAP — Automated Guitar Player

**Project owner:** Zwe Zwe / Zwe Nyi Nyar  
**Status updated:** 7 October 2026  
**Current stage:** Chord-helper measurement and fretting prototype development

AGAP is a physical guitar automation and showcase project involving a guitar, Arduino control, solenoids, servos, power electronics, Python and serial communication. The current work focuses on redesigning the fretting mechanism: solenoids will press buttons on a purchased mechanical chord helper, which will press the guitar strings into chord shapes.

The project currently has a measured, simplified model of the helper base and an adjustable fixture for testing one solenoid on one helper button. It does **not** yet have a completed ten-channel fretting assembly or a physically validated actuator/button combination.

## 1. Intended operation

```text
Python chord selection / sequencing
                |
          Serial connection
                |
          Arduino controller
                |
       Individual driver channels
                |
      Solenoid(s) press helper button(s)
                |
      Mechanical helper frets the strings
                |
      Strumming mechanism plays the chord
```

This diagram describes the intended system, not a verified implementation in this workspace. Software timing, button motion, release, electrical current and strumming timing must be tested together.

The earlier design review described an arrangement of 18 individual fret/string actuators with nominal 2.5:1 mechanical force multiplication. The new direction replaces that fretting approach with helper-button actuation. Existing strumming and guitar-support parts may be reusable, subject to fit checks; the present redesign does not establish that they already work with the helper.

## 2. Hardware and known constraints

| Item | Current information | Verification status |
| --- | --- | --- |
| Guitar | Acoustic, nominal 40-inch overall size | User supplied; neck dimensions and string type not recorded |
| Chord helper | Purchased online; photographed actual unit | Actual photos supplied; mechanism not fully measured |
| Solenoid voltage | 12 V | User supplied specification |
| Solenoid stroke | 4 mm nominal | Actual loaded travel not tested |
| Solenoid mass | 7 g | User supplied specification |
| Advertised force | “Initial repulsion 0.2 N” | Force curve and holding force unknown |
| Intended pressing points | 10 | User target; individual functions still need mapping |
| Actuator count | One per independently controlled pressing point in the proposed direct-actuation layout | Final channel count awaits button mapping |

Helper purchase reference: [eBay item 316981179115](https://www.ebay.co.uk/itm/316981179115).

Photos show labelled positions **EM, AM, D, C, F, DM, G and BM**, plus unlabelled raised features. The unlabelled features must be identified before assuming they are two additional independent chord buttons. Confirm the supplied chord chart and the meaning of each label; a separate A-major button has not been established from the photos.

Ten independent pressing points would normally require ten solenoids and ten driver channels with this architecture, mounted on a shared frame. That does not require ten complete gantries. The existing wide single-solenoid holder is a test fixture and has not been demonstrated to fit a densely packed ten-channel arrangement.

## 3. Measured helper base

All CAD dimensions below are in **millimetres**. The user measured the physical helper; these are not dimensions estimated from photographs.

| Dimension | Value | Notes |
| --- | ---: | --- |
| Overall base length | 155 | Includes end supports; excludes protruding screws |
| Overall base width | 64 | Excludes protruding clamp screws |
| Base height | 32 | Excludes levers and buttons |
| Inside neck-opening width | 53 | Between walls, excluding adjustable pads |
| Inside neck-opening height | 25 | Below the top section |
| Top thickness | 7 | Corrected by user from the initial 6 mm estimate |
| Side-wall thickness | 5.5 each | Inferred from (64 − 53) / 2; assumes symmetry |
| Gap between end supports | Approximately 116 | Omitted from the simplified model by request |

The base model is a continuous U-shaped section along its length. The side openings between the end supports have been filled, while the neck opening remains. This is a simplified reference model for locating later components, not a complete reproduction of the purchased mechanism.

**Included:** measured outer envelope, neck opening, top thickness and symmetric side walls.  
**Not yet included:** lever bars, button caps, pivot/retaining screws, screw holes, brass clamp screws, clamp pads, string-contact parts and internal linkages.

The simplification changes the geometry of the real support region. Do not treat this file as a qualified replacement clamp or assume it reproduces the real helper's stiffness, weight or adjustment.

## 4. Files available now

Paths below are relative to this README, which sits in the workspace's `outputs` directory.

| File or directory | Purpose |
| --- | --- |
| [Measured helper base — 3MF](AGAP-measured-chord-helper/Chord-helper-base.3mf) | Simplified measured base; open in 3D Builder |
| [Measured helper base — STL](AGAP-measured-chord-helper/Chord-helper-base.stl) | Same base geometry in mesh format |
| [Measurement record](AGAP-measured-chord-helper/measurements.json) | Dimensions, inferred side thickness and omitted features |
| [Adjustable fixture assembly](AGAP-adjustable-fretting-prototype/AGAP-Fretting-Prototype-ASSEMBLY.3mf) | Seven separately selectable rigid objects for one-button testing |
| [3D Builder fixture copy](AGAP-adjustable-fretting-prototype/AGAP-Fretting-Prototype-3DBuilder.3mf) | Alternate filename for the fixture assembly |
| [Fixture parts scene](AGAP-adjustable-fretting-prototype/AGAP-Fretting-Prototype-PARTS.3mf) | Spread-out inspection scene; not a single printer-bed layout |
| `AGAP-adjustable-fretting-prototype/STL-parts/` | Individual fixture meshes and optional contact-shoe variants |
| [Fixture instructions](AGAP-adjustable-fretting-prototype/READ-ME-FIRST.md) | Assembly, indicative hardware, print suggestions and first-test procedure |
| [Fixture dimensions](AGAP-adjustable-fretting-prototype/prototype-dimensions.json) | Nominal fixture dimensions and unknown hardware fields |
| [Fixture mesh validation](AGAP-adjustable-fretting-prototype/mesh-validation.json) | Recorded geometric checks; not physical test evidence |
| [Fixture preview](AGAP-adjustable-fretting-prototype/prototype-preview.png) | Previously captured 3D Builder assembly view |
| [Fixture ZIP](AGAP-Fretting-Prototype.zip) | Existing fixture package; does not include the later measured helper base or this README |

The measured base and adjustable fixture are **separate models**. They have not yet been combined into a checked assembly with the real solenoid, helper buttons and guitar.

Development scripts are in the workspace's `work` directory: `build_helper_base.py` generates the simplified base, and `build_agap_prototype.py` generates the adjustable fixture. They are artifact-building utilities, not Arduino firmware or guitar-control software. Regenerating a model can overwrite its generated outputs, so preserve manual CAD changes separately.

The original design pack was previously reviewed at `C:\Users\Zwe Nyi Nyar\Downloads\AGAP 3d prints`. Its `description.md` was not found at that path during this README update. Historical design observations are retained as context; the original pack's current location and completeness have not been re-established.

## 5. Adjustable one-button fixture

The existing fixture provides height, sideways and along-neck adjustment without assuming an unmeasured solenoid hole pattern. It consists of two feet, two uprights, a crossbeam, a sliding carriage and a strap-mounted actuator carrier.

| Feature | Nominal value |
| --- | ---: |
| Foot/upright centre spacing | 180 mm |
| Clear space between inner foot edges | 126 mm |
| Crossbeam length | 210 mm |
| Beam-centre height range above mounting surface | 55–153 mm |
| Sideways carriage centre travel | 132 mm |
| Along-neck carrier centre travel | 40 mm |
| Carrier backplate | 50 mm wide × 42 mm tall |
| Fastener clearance holes | 4.6 mm, intended for M4 |

These are fixture design dimensions, not measured guitar dimensions. Actual travel may be reduced by interference and fastener access. Secure both feet to an independent rigid support, with the guitar supported separately. The printed fixture is not intended to rest on the guitar or grip its neck.

The carrier uses cable ties because the solenoid body and mounting holes remain unknown. Optional contact shoes have 1.6, 2.1 or 3.1 mm blind bores; choose only after measuring the shaft. See the fixture instructions for hardware quantities and printing details.

## 6. Actuation feasibility: the main unresolved issue

The advertised 0.2 N initial force is approximately 20 gram-force. It does not establish the force available throughout the stroke or during a sustained hold. The helper's button force, travel and return behaviour have not been measured, so successful actuation cannot yet be claimed.

The 4 mm nominal stroke must cover required button travel and any release clearance. Alignment error, friction and a flexible mount reduce usable performance. Increasing force through a lever also reduces output travel; a lever does not solve both shortages automatically.

Before finalising mounts, measure one button while the helper is installed on the tuned guitar and manually produces a clean chord. Record the travel to a clean chord, force over that travel, and whether it returns completely when released. Then test one actual solenoid through repeated loaded presses and the intended hold duration.

## 7. Electrical and control plan

The following is a proposed implementation scope, not completed wiring or firmware:

- Use a suitably rated driver for each independently controlled solenoid. Do not power a coil from an Arduino GPIO pin.
- Provide appropriate flyback suppression for the chosen driver arrangement and a 12 V supply sized from measured coil current and the maximum number of simultaneous active coils.
- Check coil polarity/direction, current, permitted duty cycle and temperature before selecting pulse lengths or hold control.
- Define a chord-to-button mapping and separately record which strings the strummer should play for each chord.
- Coordinate press, settling time, strum and release using measured mechanical timing.
- Give the controller a defined startup state, command timeout and stop command that turn actuator outputs off.

Exact supply rating, driver selection, wiring diagram, PWM settings, firmware pinout, serial protocol and timing values are not yet settled. The files listed here do not demonstrate an implemented or tested control stack. Any later integration should inspect the actual codebase before introducing new structure.

## 8. Work completed and validation limits

| Work item | Current state |
| --- | --- |
| Review earlier fretting architecture | Previously completed; source pack not relocated in this update |
| Select helper-button redesign direction | Agreed |
| Obtain actual helper photos | Completed |
| Measure base envelope and neck opening | Completed; side-wall symmetry inferred |
| Create simplified base STL and 3MF | Completed |
| Check base mesh and 3MF dimensions | Passed: closed mesh, consistent winding, positive volume, 64 × 155 × 32 mm bounds |
| Create adjustable one-button fixture | Completed; native 3D Builder assembly previously opened and saved |
| Check fixture mesh geometry | Recorded checks passed; see validation file |
| Model actual buttons and lever layout | Pending measurements |
| Measure solenoid body and shaft | Pending |
| Confirm all ten pressing-point functions | Pending |
| Print and assemble fixture | No completed test reported |
| Verify clean chord with powered solenoid | Not tested |
| Validate loaded cycling, hold and heating | Not tested |
| Final ten-channel CAD and electronics | Not completed |
| End-to-end automated guitar performance | Not demonstrated in this work |

Mesh validation establishes geometric properties only. It does not prove printed fit, strength, thermal suitability, electrical safety or successful chord production. The base 3MF has been checked by file import and dimensions; a native 3D Builder visual inspection of that newer base has not been recorded.

## 9. Next steps, in order

1. **Measure and model the button layout.** For each pressing point, record its label, cap width/length, unpressed cap-top height above the base top, and centre position measured from the same end and side of the base. Identify both unlabelled features. Add lever geometry only where needed to establish clearance and motion.
2. **Measure one solenoid.** Record body width/length/height, shaft diameter, mounting details, released and extended tip positions, lead exit and actuation direction. Obtain or measure current and confirm its duty rating.
3. **Prove one chord manually.** Install the helper correctly, tune the guitar, press a selected button and confirm a clean chord and complete release. Record button force and travel.
4. **Print a small fit test.** Follow the fixture instructions to check one foot/upright joint and fastener clearances before printing the complete fixture.
5. **Run one powered channel.** Align the solenoid, set extension so it cannot overpress the helper, and verify loaded press/release, clean sound, repeatability and permitted hold duration. The current mount relies on correct height and the solenoid's stroke limit; it has no independent force limiter.
6. **Design the shared actuator frame.** Use actual cap positions and actuator envelopes to resolve packing, height adjustment, wiring access and interference. Replicate only after the single-channel test succeeds.
7. **Integrate control and strumming.** Implement the mapped commands, driver channels and measured timing, then test chord changes and full sequences.

The immediate deliverable is an accurate enough reference model and a working one-button test. Cosmetic detail and a complete replica of the helper's hidden mechanism are unnecessary unless they affect fit or operation.

## 10. Measurement and test log to maintain

For each future test, record the date, chord/button, guitar setup, actuator model, coil voltage under load, current, button travel, command duration, press/release result, chord quality and temperature observations. Keep failed tests as well as successful ones; they determine whether a mounting change or a different actuator is needed.

Update this README when measurements, physical tests or implementation change the status. Keep advertised specifications, measured values, design assumptions and demonstrated results distinct.
