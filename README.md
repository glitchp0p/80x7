# READ THIS FIRST — confirmed working state

The panel now renders correctly: four colours, full per-pixel control, working
blanking, brightness control, and a running 4-digit counter.

Everything below the horizontal rule was written incrementally during bring-up.
Several of its conclusions were later DISPROVED. Where it conflicts with this
block, this block wins.

## Connector pinout — CORRECTED

**The even pins are all GND or +5V. Signals live on the ODD pins.**
An earlier version of this file had R data on IDC 6, which is a GROUND pin. That
single error caused nearly every colour anomaly in the project.

| IDC | Signal |
|-----|--------|
| 1  | SRCLK |
| 2  | GND |
| 3  | RCLK |
| 4  | GND |
| **5**  | **GREEN data** |
| 6  | GND |
| **7**  | **RED data** |
| 8  | GND |
| 9  | OE |
| 10, 12, 14 | +5V |
| 11 | A0 |
| 13 | A1 |
| 15 | A2 |

Note R and G are the opposite way round from the original assumption: IDC 5 is
green, IDC 7 is red.

## Firmware pin map (RP2040-Zero, contiguous GP4-GP11 into 245 A1-A8)

| Signal | Zero pad | firmware macro | 245 A-side (in) | 245 B-side (out) | Panel IDC |
|--------|----------|----------------|-----------------|------------------|-----------|
| SRCLK  | GP4  | `PIN_SRCLK` | A1 — pin 2 | B1 — pin 18 | 1 |
| RCLK   | GP5  | `PIN_RCLK`  | A2 — pin 3 | B2 — pin 17 | 3 |
| G data | GP6  | `PIN_G`     | A3 — pin 4 | B3 — pin 16 | **5** |
| R data | GP7  | `PIN_R`     | A4 — pin 5 | B4 — pin 15 | **7** |
| OE     | GP8  | `PIN_OE`    | A5 — pin 6 | B5 — pin 14 | 9 |
| A0     | GP9  | `PIN_ADDR0` | A6 — pin 7 | B6 — pin 13 | 11 |
| A1     | GP10 | `PIN_ADDR1` | A7 — pin 8 | B7 — pin 12 | 13 |
| A2     | GP11 | `PIN_ADDR2` | A8 — pin 9 | B8 — pin 11 | 15 |
| GND    | GND  | —           | pin 10     | —                | 2 |

G and R must stay on consecutive GPIOs (G = GP6, R = GP7): the PIO driver sets
both with one `out pins, 2` instruction.

245 fixed pins: 1 (DIR) and 20 (VCC) to 5V; 10 (GND) and 19 (/OE) to GND; 100nF
X7R across 20 and 10. B-side runs backwards (B1 = pin 18 ... B8 = pin 11).
GP12 is free, used as a heartbeat output.

## Colour table — CONFIRMED

**Both data lines are ACTIVE-LOW.**

| colour | red line | green line |
|--------|----------|------------|
| dark   | OFF (1)  | OFF (1)    |
| red    | ON  (0)  | OFF (1)    |
| green  | OFF (1)  | ON  (0)    |
| amber  | ON  (0)  | ON  (0)    |

## Geometry — inverted on ALL THREE axes

Confirmed empirically while getting the counter to read correctly:

- Buffer row 0 is at the **BOTTOM** of the panel -> use `NUM_ROWS-1-row`
- Column 0 within a module is on the **RIGHT** -> read glyph bits `(1 << col)`,
  not `(0x10 >> col)`
- Module numbering runs **RIGHT-TO-LEFT** -> thousands digit goes to the last
  module of the group

## Driver structure

- **OE is ACTIVE-HIGH.** PIN_OE HIGH enables, LOW blanks. Anything below saying
  "active-low" is wrong. Measured: shifting is ~400us of every slot against an
  8us lit window, so whichever level the shift runs in dominates. Shift with OE
  LOW -> 0.143A, cool. Shift with OE HIGH -> 4.5A, row drivers heat.
- **Shift while BLANKED.** setOE(false) then shiftOutRow(), never the reverse.
- **PIPELINED.** Each address displays data shifted during the PREVIOUS slot.
  Confirmed from the capture: the second load at address N carries the same data
  as the first load at address N+1. The old "2-level BCM bit-planes" theory is
  wrong.
- **PIPELINE_PRIME 1** — the first address of a frame has no predecessor within
  it, so without priming it shows the previous frame's leftovers. That seam, not
  a hardware fault, is what made one row look permanently stuck on BOTH panels.
- **BLANK_ADDR -1** — every address gets real buffer data, addresses 0-6 mapping
  to rows 0-6.

## Brightness

`ROW_ON_TIME_US` sets the lit window per row. Duty is roughly that over the shift
time. **Refresh rate does not affect brightness** — every row still gets exactly
one slot per frame either way. Measured: 8us ~0.17A, 64us ~0.7A, scaling roughly
linearly, all thermally safe. Capped at `BRIGHTNESS_MAX_US` (64); values above
are silently clamped by the preprocessor.

Per-colour dimming via `RED_DUTY` / `GREEN_DUTY` out of `DUTY_STEPS` sub-frames.
Refresh divides by DUTY_STEPS. With the old bit-banged shift, 4 steps took ~186 fps
down to ~46, near flicker; the PIO driver (below) removes most of that cost.

## PIO shift driver

The row data is now clocked out by a PIO state machine instead of
`digitalWrite()`. No wiring change — same GPIOs. Set by `PIO_DRIVER 1` in
`main.cpp`; the bit-banged `shiftOutRowL()` is kept as the `#else` fallback.

- **State machine:** pio0 SM1 (SM0 is the WS2812 heartbeat on GP16).
- **Program**, 2 instructions, wrapping:
  `out pins, 2 side 0 [3]` (drive G,R, SRCLK low) then `nop side 1 [3]` (SRCLK
  high, 595 samples). SRCLK is side-set; G/R are the out pins.
- **Speed:** SRCLK = sys_clk / (`PIO_CLKDIV` × 8). At 133 MHz and clkdiv 4 that is
  ~4.2 MHz, against the original controller's 1.14 MHz. Raise `PIO_CLKDIV` if
  columns ever look shifted or garbled.
- **Data format:** 16 pixels per 32-bit word, 2 bits per pixel (bit 0 = G line,
  bit 1 = R line, raw active-low levels), pixel 0 first. 80 pixels = 5 words
  per row. OSR shifts right with autopull at 32.
- **Still on the CPU:** RCLK, OE and the address lines change once per row,
  not per bit, so they stay as plain GPIO writes. The CPU also packs each row
  into words and waits for the SM to go idle (`panelPioWaitIdle()`, which waits
  for the TX-stall flag, not just an empty FIFO) before latching.
- **Only used in `MODE_RUN`.** Diagnostic and replay modes still bit-bang.

The shift-time figures in "Driver structure" and "Brightness" (~400 µs per slot,
~186 fps) were measured with the bit-banged driver and are stale. Re-measure
before relying on them.

## Animation pipeline — `tools/animator.ml`

OCaml tool that generates `src/frames.h`. An animation is a function
`time -> x -> y -> pixel`; effects are functions from animation to animation,
so they compose. Frames only exist when it's sampled at a fixed rate and
written out.

Build (from `tools/`):

```
ocamlfind ocamlopt -package str -linkpkg animator.ml -o animator
# or without ocamlfind:
ocamlopt str.cmxa animator.ml -o animator
```

Commands (run `./animator` with no arguments for the full list):

| Command | Output |
|---------|--------|
| `demo frames.h` | built-in demo reel, 20 fps |
| `showcase frames.h` | showcase reel, 50 fps |
| `plasma frames.h [s]` / `plasma2 frames.h [s]` | plasma, 20 fps / 50 fps |
| `plasmatext frames.h [TEXT]` | text knocked out of solid plasma |
| `marquee frames.h "TEXT"` | scrolling text |
| `life frames.h [s]` | Game of Life |
| `pan in.pan frames.h` / `show in.pan` | hand-drawn `.pan` file / terminal preview |
| `ani-new` / `ani-show` / `ani` | `.ani` template, preview, build |
| `video clip.rgb frames.h` | raw rgb24 80×7 from ffmpeg |
| `slitscan clip.rgb frames.h SRCW [col] [px]` | slit-scan from a wider clip |

For video, convert first with ffmpeg, e.g.
`ffmpeg -i clip.mp4 -vf "crop=iw:ih/11.4,scale=80:7,fps=50" -f rawvideo -pix_fmt rgb24 clip.rgb`.

**`frames.h` format:** 560 bytes per frame (80 × 7), one byte per pixel, rows
top-first and columns left-first *as a viewer sees the panel*. Bits 3:2 = colour
(0 off, 1 red, 2 green, 3 amber), bits 1:0 = brightness level 0–3. The firmware's
`renderFrame()` applies all three geometry inversions, so frames are authored
the way they look.

**Playback:** `ANIM_MODE 1` in `main.cpp`. **`ANIM_FRAME_MS` must equal
1000 / the fps the frames were made at** (50 for 20 fps, 33 for 30 fps, 20 for
50 fps), or frames are dropped or repeated. The 4 brightness levels come from
`levelBuf` / `BCM_LEVELS 4`; the 50 fps effects rely on the PIO driver's faster
repaint.

## Hardware damage (panel 1)

APM4953 row drivers H1 and H2 destroyed by a firmware mode (`HOLD_ADDR`) that
held a single address statically. One row draws ~4.7A; the panel is designed for
1-in-8 duty, so a static hold is ~8x rated dissipation and kills the MOSFETs in
SECONDS. That mode has been removed and must never be reintroduced.
Safety is now enforced in firmware: `SLOW_SCAN_MS` capped at 30ms, slow scans
auto-blank after 20s.

## Method note, learned the hard way — TWICE

**When a signal behaves impossibly, check which physical pin it is actually on
before revising the model.** Two separate wiring errors in this project (the
original placeholder GPIO map, and red on a ground pin) both produced symptoms
that looked like protocol, polarity or driver bugs. Between them they cost most
of two sessions and several discarded colour models. Checking the pin is cheaper
than rebuilding the theory.

---

# Hospital LED Display — Reverse Engineering Notes

Salvaged 16-module bi-color (red/green) 5×7 LED dot-matrix display, originally
driven by an onboard NXP LPC1768FBD100 (ARM Cortex-M3). Goal: replace the
LPC1768 controller with an RP2040 driving the same panel electronics directly.

## Hardware overview

- **16 identical 5×7 bi-color LED matrix modules**, mounted in a single line
  on one long PCB.
- **Driver ICs per module (repeated ×16 down the board):**
  - `74HC595D` — 8-bit serial-in/parallel-out shift register w/ latch. Forms
    a long cascaded chain carrying column data.
  - `74HC245D` — octal bus transceiver/buffer. Re-buffers clock/data between
    connector and the shift-register chain (likely needed due to the fan-out
    load of driving 16 modules' worth of 595s off one signal).
  - `APM4953` — dual P-channel MOSFET (SOT23-6). Believed to be the row
    driver: high-side switch pulling one row-common (H1–H7) to +5V at a time.
  - `HR1`–`HR7` resistor networks — gate pull-up / current-limit resistors
    for the row switches.
- **Original controller:** NXP LPC1768FBD100 (Cortex-M3 @ 100MHz), running
  proprietary firmware, on a **plug-in daughterboard** — now removed
  entirely. All protocol info below was extracted by sniffing the connector
  while the original daughterboard still drove the panel. With it removed,
  the main panel board is powered independently via its **JP1 terminal
  block**.
  - SWD flash-dump of the LPC1768 was attempted and **parked** — most likely
    CRP3 lockout. An RP2040-Zero clone is confirmed working as a CMSIS-DAP
    debugprobe (`ID 2e8a:000c`, GP2=SWCLK, GP3=SWDIO, GP4=UART TX,
    GP5=UART RX) if this is ever revisited. `mdw 0x2FC` would read the CRP
    word.
- **Connector:** 20-pin IDC header between the controller and the driver
  chain (pin 1 = first pin per IDC pin-1 marking/silkscreen pairing).

## LED module pinout (from silkscreen on back of each 5×7 module)

Two rows of pins per module:

```
Row 1: G1  R1  G2  G3  H4  H2  H1  H3  R5
Row 2: NL  H5  R2  H7  H6  R3  R4  G4  G5
```

17 unique electrical signals:
- **H1–H7** — 7 row commons (one per physical row)
- **R1–R5** — 5 red column drives
- **G1–G5** — 5 green column drives
- **NL** — "no lead" mechanical keying pin (not electrical — marks pin-1
  orientation, standard on these packages)

Amber/orange pixels are produced by driving R+G together on the same dot —
there is no separate third color die.

## Confirmed 20-pin IDC connector pinout

> **Superseded (see top block):** IDC 6 is GND, not R data. G data is on IDC 5 and R data on IDC 7. Pins 4 and 8 are GND. OE is active-HIGH, not active-low. Pins 16–20 are still unmapped.

| Pin | Signal | Notes |
|-----|--------|-------|
| 1   | **SRCLK** | Shift register clock. ~1.14MHz, narrow active pulses (~1-2 samples @ 24MHz, ~42-83ns) |
| 2   | **GND** | Confirmed via continuity test to GND pad, board unpowered |
| 10  | **+5V** | Confirmed: reads 4.93V, identical to panel input, vs 4.95V at PSU |
| 12  | **+5V** | As pin 10 — three conductors paralleled for current sharing |
| 14  | **+5V** | As pin 10 |
| 3   | **RCLK / LATCH** | Narrow active-high strobe. Fires once per row-load |
| 6   | **R data (SER)** | Red column serial data, sampled on SRCLK rising edges |
| 7   | **G data (SER)** | Green column serial data, sampled on SRCLK rising edges |
| 9   | **OE** | Output enable, active-low. Mostly high (blanked) with narrow low windows for brightness timing. Rising edge lands exactly 1 sample after RCLK's rising edge — tightly coupled latch→enable sequence |
| 11  | **A0** | Row address bit 0 (LSB) |
| 13  | **A1** | Row address bit 1 |
| 15  | **A2** | Row address bit 2 (MSB) |

Pins 4, 5, 8, 16–20 are **not yet identified** — likely GND,
a second OE/LAT for a split panel, or spares. Not required for basic
operation but worth mapping eventually.

**Emerging pattern:** signals occupy odd pins (1, 3, 9, 11, 13, 15) plus 6 and 7;
power occupies even pins (2 = GND, 10/12/14 = +5V). Standard ribbon practice —
supplies and grounds interleaved between signals for return paths and crosstalk
isolation. Suggests the remaining unknowns (4, 8, 16, 18, 20) are probably
further grounds. Quick to confirm with a meter next time the board is powered.

**Measurement note:** the first DMM used read these pins at 4.5–4.8V, low enough
to suggest a pull-up rather than a rail. A second meter read 4.93V against 4.95V
at the PSU — a 0.02V wire drop, i.e. a direct tie. Worth remembering that meter
disagreed; re-check anything marginal on the second one.

## Protocol / frame structure

- **SRCLK frequency:** ~1.14MHz (measured via 24MHz-sample-rate capture,
  period ≈ 20-23 samples).
- **80 bits shifted per row-load** = 16 modules × 5 columns. Confirmed two
  independent ways: (1) directly counting SRCLK edges between RCLK pulses,
  (2) matches the known module/column count.
- **Row address (A0-A2) cycles 7→0→7→0…**, i.e. counts *down*, wrapping at 0.
> **Superseded (see top block):** the two loads per address are a pipeline (each address shows the data shifted during the previous slot), not 2-level BCM. See "Driver structure".

- **Each address value is held for exactly 2 consecutive RCLK loads** — a
  2-level brightness/BCM scheme (dim/bright weighting), not full grayscale.
  So: 8 address states × 2 loads = 16 loads per full frame.
- **One load per cycle is double-length (160 bits instead of 80)** —
  consistently the *second* load at address value 0. Total bits per frame:
  15 × 80 + 160 = **1360 bits**. This exact number was independently
  confirmed via autocorrelation on a raw data capture (perfect 1.0
  correlation at 1360-bit offset) *and* by directly counting bits between
  RCLK pulses across one full address-wrap cycle — two unrelated methods
  landing on the same figure.
- **OE toggle count matches RCLK toggle count exactly** in every capture —
  reinforcing that OE pulses once per row-load, immediately after latch.
- Approx. refresh rate: 1360 bits / 1.14MHz ≈ 1.19ms per frame ≈ **~840Hz**
  row-cycle rate (fast, consistent with flicker-free multiplexing).

## Level shifting (3.3V → 5V) — REQUIRED

The panel's logic is **74HC-series, not HCT**. HC inputs are referenced to
VCC (~0.7 × 5V ≈ 3.5V threshold), so a 3.3V RP2040 GPIO **never reliably
reaches a logic high**. This is the root cause of the long-running symptom
where the panel showed the same fixed pattern regardless of firmware
content — the data/clock lines were not being read as valid highs at all.

**Do not drive this panel directly from RP2040 GPIO.**

### Chosen part: 74HCT245 (octal bus transceiver)

HCT inputs use TTL thresholds (~1.2–1.5V), so 3.3V is a solid high.
One package covers all 8 required signals:

All pin numbers below are **package pin numbers on the 74HCT245** except
where the column says otherwise.

| Signal | Zero pad | firmware macro | 245 A-side (in) | 245 B-side (out) | Panel IDC |
|--------|----------|----------------|-----------------|------------------|-----------|
| SRCLK  | GP4  | `PIN_SRCLK` | A1 — pin 2 | B1 — pin 18 | pin 1 |
| RCLK   | GP5  | `PIN_RCLK`  | A2 — pin 3 | B2 — pin 17 | pin 3 |
| G data | GP6  | `PIN_G`     | A3 — pin 4 | B3 — pin 16 | pin 5 |
| R data | GP7  | `PIN_R`     | A4 — pin 5 | B4 — pin 15 | pin 7 |
| OE     | GP8  | `PIN_OE`    | A5 — pin 6 | B5 — pin 14 | pin 9 |
| A0     | GP9  | `PIN_ADDR0` | A6 — pin 7 | B6 — pin 13 | pin 11 |
| A1     | GP10 | `PIN_ADDR1` | A7 — pin 8 | B7 — pin 12 | pin 13 |
| A2     | GP11 | `PIN_ADDR2` | A8 — pin 9 | B8 — pin 11 | pin 15 |

**REVISED for the soldered stripboard build.** GP4-GP11 now run contiguously into
A1-A8, matching the Zero's pad spacing. OE moved GP9 -> GP8, address lines each
shifted back one, heartbeat moved GP8 -> GP12. Wiring convenience only.

Fixed pins on the 245: pin 1 (DIR) -> 5V, pin 19 (/OE) -> GND, pin 20 -> 5V,
pin 10 -> GND, 100nF X7R across pins 20 and 10 with short leads. Ground common
between Zero, 245 and panel. Panel IDC pin 2 = GND; pins 10, 12, 14 = +5V.

**The B-side runs backwards** (B1 = pin 18 down to B8 = pin 11), so the signal
wiring crosses over rather than running straight across the package.

Socket the 245 rather than soldering it down - spare 244s and the second bank are
available if a channel fails.



GND: RP2040 GND → 245 pin 10 → panel IDC pin 2.

Note the B-side pin order **runs backwards** (B1 = pin 18 … B8 = pin 11),
so the physical wiring crosses over rather than running straight across.

> **GPIO column verified by continuity test against the physical harness**,
> and `main.cpp` has been corrected to match. The earlier values (GP2–GP9)
> were placeholders that matched nothing — see "Root cause found" below.

Note also: GP2–GP5 are the pins used for the CMSIS-DAP debugprobe role on
the RP2040-Zero. Not a conflict (different board, different job) but don't
confuse the two mappings.

**Fixed connections:**
- `DIR` (pin 1) → **5V** (A→B direction). Must never float — tie it hard,
  not via a removable jumper.
- `/OE` (pin 19) → **GND** (outputs always enabled).
- `VCC` (pin 20) → 5V, `GND` (pin 10) → GND.
- Common ground between RP2040, 245, and the panel's 5V supply.

**Decoupling:** 100nF **X7R/X5R ceramic** across VCC/GND, physically at the
package with short leads. Ceramic specifically — low ESR/ESL is what lets it
service the nanosecond-scale transient when 8 outputs switch at once.
Tantalum/electrolytic/polyester are all too slow (too much ESL) for this
role. Optionally add 10–100µF bulk (electrolytic or tantalum is fine there)
at the panel's 5V entry to cover the slower row-to-row current steps.

**Spares:** 2 × 74HCT244 also on hand. Either would work, but the 244's two
banks are interleaved across the package, which is more awkward to wire than
the 245's contiguous runs. 74HCT125 is reserved for the MIDI clock project.

### Parts inventory (no separate BOM — kept here)

| Part | Qty | Role | Status |
|------|-----|------|--------|
| 74HCT245 | — | Level shifter, 3.3V→5V, all 8 panel signals | **In hand** |
| 74HCT244 | 2 | Spare level shifters | In hand |
| 74HCT125 | — | Reserved for MIDI clock output buffer | In hand |
| 100nF X7R ceramic | 1+ | Decoupling at 245 VCC/GND | In hand |
| 10–100µF electrolytic/tant | 1 | Optional bulk at panel 5V entry | — |
| 1Ω 5W resistor | 1 | Inline current-sense shunt in 5V feed | Needed |
| RP2040-Zero (V1083 clone) | 1 | Replacement controller | In hand |
| 20-pin IDC header + cable | 1 | Panel connection | — |
| DIP socket, 20-pin | 1 | For the 245 (don't solder it in directly) | — |

On the panel itself (not purchased — salvaged): 74HC595D ×16, 74HC245D ×16,
APM4953 dual P-ch MOSFET, HR1–HR7 resistor networks, 16 × 5×7 bi-color
modules. **Fixed 5V supply only — no current-limited bench supply yet.**

## Root cause found: firmware pin defines never matched the harness

> **Note:** the GPIO numbers in the table below are the harness as it was *then*. The IDC 6/7 data assignments are also wrong (IDC 6 is GND). The current wiring is in the top block. `FORCE_OE_DISABLED` is now `false`, and OE is active-HIGH, not active-low.

**This, not level shifting, is why the panel never responded to firmware.**
The `#define` block in `main.cpp` used placeholder GPIOs 2–9. Continuity
testing the actual harness gave a completely different mapping. Every signal
was crossed:

| IDC | Signal | Wired to | Firmware drove that GPIO as |
|-----|--------|----------|------------------------------|
| 1  | SRCLK | GP4  | `PIN_OE` |
| 3  | RCLK  | GP5  | `PIN_R` |
| 6  | R data| GP6  | `PIN_G` |
| 7  | G data| GP7  | `PIN_ADDR0` |
| 9  | OE    | GP9  | `PIN_ADDR2` |
| 11 | A0    | GP10 | *never configured — floating* |
| 13 | A1    | GP11 | *never configured — floating* |
| 15 | A2    | GP12 | *never configured — floating* |

Firmware was meanwhile driving GP2, GP3 and GP8, which go nowhere.

**Explains the "identical display regardless of firmware" symptom.** There
was no real shift clock, the latch line carried pixel data, and all three
address lines floated. The visible pattern was an artefact of that, entirely
independent of frame content — so rebuilding with different data could not
change it. The protocol reverse-engineering was never the problem.

**Explains the overheating.** OE (active-low) was driven by address bit A2,
which is low for addresses 3–2–1–0 — so OE sat *enabled* for roughly half of
every frame, in windows hundreds of µs long, against the original hardware's
6.4% duty. The localised module heating was almost certainly just whichever
rows the garbage latched data happened to light, not a hardware fault.

**Also invalidates the polarity "confirmation"** (open question #1) — that
test was reading noise from a miswired chain.

`main.cpp` has been corrected. `FORCE_OE_DISABLED` has been flipped to
`true` as the safe default until the corrected mapping is verified on a
logic analyzer.

**Level shifting is still required** — HC thresholds remain a real problem
and the 74HCT245 still goes in. But it was a second, independent fault
masked by this one, and fixing it alone would not have made the panel work.

## Target board: RP2040-Zero (V1083 clone), not a Pico

> **Note:** the logic-analyzer heartbeat has moved from GP8 to GP12 (`PIN_HEARTBEAT`). GP8 is now OE.

Confirmed — the silkscreen numbers on this board **are GP numbers**, since the
Zero labels its castellated pads with GPIO directly. (The physical-vs-GP
mismatch that trips people up is a Pico-with-40-pin-header issue; it does not
apply here. Worth stating explicitly because it briefly cast doubt on the
verified pin mapping above.)

Two consequences, both now fixed in source:

- **No `LED_BUILTIN`.** The Zero's onboard LED is a WS2812 on GP16 and needs a
  protocol driver. The old `pinMode`/`digitalWrite` heartbeat compiled fine and
  did nothing. A dark LED was therefore *not* evidence of firmware not running —
  a dangerous false signal during bring-up. Two heartbeats now exist:
  - **WS2812 on GP16** (PIO, pio0 SM0, divider from runtime
    `clock_get_hz(clk_sys)`). Solid blue at end of `setup()` = flashed and
    booted; green blink at 1Hz = loop running. **Temporary** — costs ~30µs
    blocking once per second, perturbing one panel frame per second. Remove
    before characterising panel timing for real.
  - **GP8 digital toggle** (drives nothing, no LED attached) — visible only on
    a logic analyzer, but lands in the same capture as the panel signals so
    timing relationships can be read directly. Keep this one.
- **2MB flash, not 4MB.** `platformio.ini` now targets
  `board = waveshare_rp2040_zero` with `board_build.core = earlephilhower` —
  the same toolchain as the MIDI clock project, which also avoids the mbed
  core's `Serial.printf` gap and its blocking-print trap.

**Keep the boards labelled.** An identical Zero is in service as the CMSIS-DAP
debugprobe (GP2=SWCLK, GP3=SWDIO, GP4/5=UART). No electrical conflict — different
board, different job — but they are physically indistinguishable.

## Observation log — display states vs. driven signals

> **Superseded (see top block):** this whole log was recorded while red data was wired to a GROUND pin (IDC 6) and before OE's polarity was known. Its polarity, blank-state and "OE does not blank" conclusions are all invalid. Dark is now BOTH lines HIGH, and OE does blank (active-HIGH).

Recorded live during bring-up. Each row pairs a known set of driven line values
with the observed display. Treated as a dataset, these constrain the hardware
more tightly than any single test — several long-held assumptions are
contradicted by it.

Notation: values are what the firmware WROTE to the R and G data lines. "R=1"
means the red serial line was held high for every bit of the fill.

| # | Mode / flags | R line | G line | Rows scanned? | Observed |
|---|---|---|---|---|---|
| 1 | PIN_MAP_TEST, 245 fitted | static | static | no | Bottom row only, random pattern, advancing one position per 24s cycle |
| 2 | Normal driver, `LED_OFF`=0 | 0 | 0 | yes | **Whole panel red**, bottom row brighter + warm |
| 3 | IDC 9 jumpered to 5V | — | — | no | Bottom row only, bright |
| 4 | IDC 9 jumpered to GND | — | — | no | Bottom row only, bright — *identical to 5V* |
| 5 | IDC cable fully unplugged | — | — | no | Bottom row only, bright, frozen random mix of red/green/amber/off |
| 6 | Normal driver, `LED_OFF`=1 | 1 | 1 | yes | **Whole panel green**, bottom row brighter (less bright than red case) |
| 7 | Per-colour: RED_OFF=1, GREEN_OFF=0 | 1 | 0 | yes | **Whole panel red**, bottom row extra bright |
| 8 | Per-colour: RED_OFF=0, GREEN_OFF=1 | 0 | 1 | yes | **Whole panel green**, bottom row bright, warm |
| 9 | DIAG 1 (accumulating), 200-bit fills | 1 | 0 bg | no | Bottom row red, one green added per step from LEFT, then blank |
| 10 | DIAG 1, after wrap | 1 | 0 | no | **Fully blank**, step counter still running |
| 11 | BOOTSEL (no firmware) | — | — | no | Bottom row, mostly-but-not-all red |
| 12 | Normal boot, `blankChain()` in setup | 1 | 0 | yes | **Fully blank** — first reliable off state achieved |
| 13 | DIAG 1 (stateless), 200-bit fills | 1 | 0 | no | Whole panel amber; bottom row red, gradient dark→bright left→right |

### What the dataset establishes

**Bits enter at the LEFT.** Row 9: the marker appeared at the left and advanced
rightward. Resolves open question #3's direction, and supports the sequential
module-ordering assumption.

**A genuine blank state exists: R line HIGH, G line LOW** (rows 10, 12). This is
the only blanking mechanism currently available, since OE does not respond. It
is what `blankChain()` writes at boot.

**Both data lines carry real per-pixel data** (row 9 — individual pixels changed
one at a time, not the whole display).

### What the dataset CONTRADICTS

**Uniform fills cannot determine polarity.** Rows 2, 6, 7, 8 cover all four R/G
combinations and *every one of them lit the panel* — red or green, never off. A
test whose every outcome is "lit" carries no polarity information. Two wrong
polarity conclusions were drawn from exactly this before the pattern was noticed.
**Do not use uniform fills as discriminators.**

**OE (IDC 9) does not blank.** Rows 3 and 4 are identical at 5V and at GND. The
original-hardware captures show IDC 9 pulsing low once per row-load at 6.19%
duty, which looks exactly like an output enable — but on this panel, driven from
the RP2040, it gates nothing. Unexplained. Candidate: it drives the /OE of the
connector-side 74HC245 bus buffers (IC1/IC2) rather than the 595 output enables,
in which case it would gate data transfer and could never blank LEDs.

**Address 0 appears to light physical rows.** The DIAG tests never call
`setRowAddress()`, so A0–A2 sit LOW at address 0 throughout. Yet row 13 shows
*every row lit*. The long-standing theory that address 0 is a non-physical
blanking cycle (open question #2) does not survive this. Either address 0 selects
a real row, or the row decoder is not gating as assumed.

**The gradient in row 13 should be impossible.** The stateless version rewrites
all 200 bits every step, so no position accumulates more display time than any
other, and shift-register contents are binary — a smooth brightness ramp cannot
come from data. Something other than chain contents is modulating brightness.

### Open anomalies

1. Why do static tests (rows 1, 3, 4, 5, 9, 11) light **only** the bottom row,
   while scanning tests (2, 6, 7, 8) light all rows — yet row 13, which is also
   static, lights all rows?
2. Chain length still unmeasured. Note that 16 modules × 5 cols × 2 colours = 160
   outputs = exactly 20 × 595, which suggests **two 80-bit chains, one per
   colour** — i.e. `TOTAL_COLS 80` is correct and the 200-bit fills push markers
   straight out the far end. This would explain why no marker is visible in rows
   10 and 13.
3. The bottom row is involved in *every single observation*. No other individual
   row has ever appeared alone.

## BREAKTHROUGH: capture replay produces correct output

Replaying one frame extracted verbatim from `andy_matrix_200k_24MHz_pins1_3_6_7_9_11_13_15.csv`
(16 loads, 1360 bits, address countdown, 160-bit load at address 0, OE 8us per
load) rendered a **legible number — red digits on a green field**.

### What this proves

The entire signal path is sound: 74HCT245 level shifting, harness wiring, pin
mapping, SRCLK, RCLK, both data lines, row addressing via A0-A2, and OE. None of
these is the fault. **Every remaining problem is in how the firmware CONSTRUCTS
frames**, which is a far smaller space than what was being searched.

Confirmed simultaneously: the 1360-bit frame structure, the address countdown
with two loads per value, the 160-bit load at address 0, and that OE genuinely
does gate illumination at ~8us per load.

### Corrections to earlier conclusions

**"OE does nothing" was WRONG.** Observation-log rows 3 and 4 recorded identical
results with IDC 9 at 5V and at GND. The 245's B5 output was almost certainly
still connected and driving the pin, so the jumper was fighting it and the test
measured nothing. OE works.

> **Superseded (see top block):** this result was still taken with red on a ground pin. BOTH lines are active-LOW (`RED_ACTIVE_LOW 1`, `GREEN_ACTIVE_LOW 1`).

**Polarity, finally.** The replayed red channel is all zeros across the entire
frame and red digits appeared, while green carried 1052 ones out of 1360 and
formed the background field. So: **RED is active-LOW (0 = lit), GREEN is
active-HIGH (1 = lit).** The per-colour asymmetry was real; both earlier
attempts had the values inverted.

### New observations from the working display

> **Superseded (see top block):** the "stuck" row was the pipeline seam at the first address of each frame, fixed by `PIPELINE_PRIME 1`, and was seen on both panels. It was not a hardware fault in that row.

**Bottom row is faulty.** Red is markedly dimmer there and green does not light
at all. The bottom row has been anomalous in *every* observation this session.
Two possibilities, not currently distinguishable: a pre-existing hardware fault
in that row's drivers, or damage from the sustained-DC events earlier in
bring-up (address held static with OE enabled, no multiplexing). Worth probing
that row's APM4953 and its 595s.

**Parasitic powering through the signal lines — REAL HAZARD.** With the panel's
5V OFF but the Pico still connected, red LEDs light dimly. This is current
flowing through the panel ICs' ESD protection diodes from the driven inputs,
powering the chip parasitically. It can damage those ICs over time.
**Never drive signals into an unpowered panel.** Power the panel BEFORE the
Pico's outputs go active, and remove power in the reverse order.

Note this also explains why only red lights in that state: red LEDs have a lower
forward voltage (~1.8-2.0V) than green (~2.1-2.6V), so a weak parasitic supply
reaches red's threshold and not green's. It is a Vf difference, not evidence of
overdriving.

**On the overdrive hypothesis — the duty-cycle arithmetic does not support it.**
The original ran 1360 bits at 1.14MHz = 1.19ms per frame. This bit-banged replay
takes roughly 6ms per frame, while using the same 8us OE window per load. Per-row
on-time is therefore about 0.26% here versus ~1.3% originally — this firmware
drives the panel LESS hard than the original did, not more. Peak current is set
by the panel's own series resistors and is identical either way. Red simply
appears brighter because red LEDs are more efficient at a given current.

The warmth is worth watching, but the likely cause is ordinary operation plus the
bottom-row fault, not overdrive. Measure before assuming: compare panel current
draw during replay against the idle/blank state.

## Power characterisation — panel is thermally safe

Measured with a DMM in series with the panel's 5V feed at JP1 (10A jack, DC amps).

| State | Current |
|---|---|
| BOOTSEL, nothing driven | **0.057 A** |
| REPLAY_MODE, ~3/4 of pixels lit | **2.67 A** (~13 W) |

### Ten/twenty-minute soak

2.722 A at start, 2.690 at 1.5 min, 2.682 at 3 min, 2.676 at 5 min, 2.674 at
8 min, 2.675 at 10 min, **2.666 A at 20 min with no further temperature rise.**

Current *falls* about 1.8% as the panel warms, then flattens. This is
self-limiting, not runaway: the rising series-resistor and MOSFET on-resistance
outweigh the LEDs' falling forward voltage. Red areas end up mildly warm, green
stays cool.

> **Note:** 2.67 A was measured in REPLAY_MODE, with the shift running while lit. The current driver blanks during the shift and draws ~0.17 A (8 µs) to ~0.7 A (64 µs). Treat 2.67 A as a replay-mode figure, not the normal operating point.

**Conclusion: 2.67 A at 5 V is this panel's normal operating point and it is
stable indefinitely.** No need for short runs, thermal watching, or current
limiting during testing. Judge anything anomalous against this baseline.

(An earlier prediction here was that current would *climb* with temperature. It
does not. Three successive predictions about this panel's power behaviour were
wrong in different directions — prefer measurement over reasoning on this board.)

### Brightness control: mechanism still UNKNOWN

> **Superseded (see top block):** brightness is set by `ROW_ON_TIME_US` (see "Brightness"). These tests ran with OE's polarity inverted, so the panel was lit during the ~400 µs shift and the OE window was a small fraction of on-time. That is consistent with changing it having no visible effect.

Two candidate controls were tested and neither affects current:

- **OE window** — `REPLAY_OE_US` 8 -> 1 changed nothing (2.666 A -> 2.723 A),
  despite 245 pin 14 reading 4.74 V against a 4.93 V rail, confirming it pulses
  correctly at ~4% low duty. OE reaches the panel and does not gate illumination.
- **Row dwell** — bounding how long each address is held, then parking on address
  0, left current at 2.7 A unchanged.

But brightness plainly *does* vary: the bottom row has appeared both brighter and
dimmer than the rest at different times, red reads brighter than green, and DIAG
test 1 produced a smooth left-to-right gradient. Uniform continuous illumination
cannot produce any of that.

Working hypothesis, untested: brightness tracks **how long each bit has been
sitting latched and visible**, i.e. the display shows register contents live
during shifting rather than only after latching. That would make frame rate and
shift/latch sequencing the real brightness control, and would explain why the
original ran its shift clock at 1.14 MHz — fast shifting minimises the time
intermediate states are visible. It would also explain the gradient: bits that
entered earlier have been visible longer within each frame.

### Frame structure confirmed from capture analysis

> **Superseded (see top block):** the two loads are pipelined row data, not bit-planes. The firmware's per-pixel brightness now comes from its own BCM (`levelBuf`, `BCM_LEVELS`), not from the original's load structure.

The two loads at each address carry **identical red data but DIFFERENT green
data**. They are genuine independent bit-planes — the original's 2-level
brightness weighting — not a duplicated frame. The previous `refreshFrame()`
shifted the same buffer twice and discarded that distinction.

## Row addressing — MEASURED, and it is not what was assumed

> **Superseded (see top block):** the firmware uses `BLANK_ADDR -1`, so addresses 0–6 map to rows 0–6 and every address gets real data. The bullets below also contradict each other (address 7 is called the spare, but skipping it darkened the bottom row). These measurements predate the pipeline fix and are probably confounded by it. The `rowIndex` paragraph is stale.

Established by selectively skipping addresses (`SKIP_ADDR`) during a slow scan:

- **Address 0 lights a real physical row.** The long-standing theory that it is a
  non-physical blanking cycle (former open question #2) is **WRONG**. Skipping it
  changed the bottom row's behaviour, which a dummy cycle could not do.
- **Address 7 appears to be the spare** '138 output, not address 0.
- **The decoder-output-to-row mapping is NOT sequential.** Skipping address 1
  darkened the second row from the bottom; skipping address 7 darkened the bottom
  row. Whatever order the '138 outputs are wired to the row drivers in, it is a
  routing convenience and must be measured, never assumed.
- This also invalidates the explanation for the anomalous 160-bit load at address
  0 (it was assumed to be padding for a dummy cycle). **No replacement explanation
  yet.** The load is real and reproducible in the captures; its purpose is unknown.

`rowIndex` in `refreshFrame()` is therefore still wrong: it maps address N to
`redBuf[N-1]` for 1-7 and aliases address 0 onto `redBuf[0]`. The correct mapping
cannot be written until the full address-to-physical-row table is measured.

## The brightness gradient — MECHANISM IDENTIFIED

A row that is on while the chain is being shifted **for other rows** displays the
shift register contents LIVE. Positions near the far end of the chain reach their
final value early and hold; positions near the input keep changing until the last
clock. Time-averaged: bright at the far end, fading to dim at the input end.

That is the left-to-right gradient seen on the bottom row, and the same mechanism
produced the unexplained gradient in DIAG test 1. It is a symptom of a row being
stuck on, not a data problem.

## HARDWARE DAMAGE — APM4953 row drivers H1 and H2

**H2 smoked and was powered down immediately. Both H1 and H2 show cracked
packages with puckering, heat discoloration, and burn marks on some of H1's legs.
H3 and H4 appear undamaged.**

### Cause

A firmware mode (`HOLD_ADDR`) that held a single row address statically. **One row
draws ~4.7A when selected.** The panel is designed for 1-in-8 multiplexed duty, so
holding one row on is roughly 8x its rated dissipation — enough to destroy the row
MOSFETs in SECONDS, not minutes. The mode shipped with no timeout and relied on
the operator to stop it in time. That was an unsafe test design.

Contributing history: earlier bring-up also ran sustained single-address drive for
extended periods during the polarity investigation, and slow scans at 250ms dwell.
Whether H1 was already damaged before today or failed the same way earlier cannot
now be determined.

### Electrical state after the failure

Resistance across the panel's 5V input and GND at JP1, unpowered: **~943 ohms,
symmetric in both probe directions.** No short across the rail — the MOSFETs did
not fail short. Symmetric reading indicates resistive loading (pull-ups, logic
inputs), not a conducting junction. **The panel is safe to leave on the bench.**

### Repair outlook

APM4953 is a commodity SOT23-6 dual P-channel, cheap and available. Desoldering
the part itself is routine (hot air ~300C, or drag-soldering). The risk is the
surroundings: a dense board with LED modules nearby that will not tolerate 300C.
Kapton the neighbours and keep airflow low.

**Consider whether repair is warranted at all.** Losing one row costs 1/7 of the
display. If the damage stops at the bottom row, a working 6-row panel may be
sufficient depending on the intended use.

## Safety rules now ENFORCED IN FIRMWARE

Following the H1/H2 failure, the code — not the operator — enforces these:

- **`HOLD_ADDR` has been REMOVED.** There is deliberately no mode that holds a
  single address indefinitely. **Never add one.**
- `SLOW_SCAN_MS` is capped at `SLOW_SCAN_MAX_MS` (30ms) by the preprocessor;
  larger values are clamped, not honoured.
- Any slow-scan run auto-blanks after `SLOW_SCAN_TIMEOUT_S` (20s) and halts with
  the LED flashing red. Power-cycle to rerun.
- Default state is `SLOW_SCAN_MS 0`, `SKIP_ADDR -1` — normal multiplexed operation
  at 2.67A, which the panel sustains indefinitely.

**Principle: no test on this panel should be able to damage it through
inattention.** If a static measurement is needed, use a brief periodic hold with a
hard timeout, never a sustained one. Prefer a gentler way to get a number over
making the panel work harder.

## Known firmware bugs (found by cross-checking `main.cpp` against captures)

> **Superseded (see top block):** bugs 1–3 assume the BCM bit-plane and dummy-address-0 theories, both now disproved. Bug 4 is fixed: separate `blankPadRed`/`blankPadGreen` arrays are memset explicitly. Bug 5's figures are bit-bang era, and the PIO driver has changed the timing again.

**1. Only one of the two bit-planes is ever displayed.**
In `refreshFrame()`, both planes are shifted and latched back-to-back
*before* `setRowAddress()` and `setOE(true)` are called. Plane 0's latch is
therefore overwritten by plane 1's latch while OE is still blanked — plane 0
never reaches the LEDs. Invisible today because both planes shift identical
data, but it breaks the moment BCM weighting is implemented (open question
#5). Fix: address + OE window per plane, inside the plane loop.

**2. OE pulse count doesn't match the captured hardware.**
Captures showed OE toggle count exactly equal to RCLK toggle count — one OE
window per row-load, 16 per frame. The firmware emits 2 RCLKs but only 1 OE
window per address, so 8 per frame. Same root cause as bug 1.

**3. Address 0 displays row 0's real data.**
`rowIndex = (addr >= 1 && addr <= NUM_ROWS) ? (addr - 1) : 0` maps address 0
to `redBuf[0]`/`greenBuf[0]` — the same buffer as address 1. If address 0 is
genuinely a blanking/dummy cycle (working theory, open question #2), it
should shift `blankPadRow`, not real row data. As written, row 0's content
is emitted twice per frame at two different addresses.

**4. `blankPadRow` depends on `LED_OFF == 0`.**
It's a zero-initialised static, so it's only "all off" while `LED_OFF` is 0.
If polarity is revised, the padding silently becomes all-**on** — 80 bits of
full-brightness data on the most heat-sensitive path. Set it explicitly in
`setup()` with `memset(blankPadRow, LED_OFF, TOTAL_COLS)`.

**5. `ROW_ON_TIME_US` duty figures in this README are stale.**
The 18.8% → 4.1% measurements came from an earlier code state. The current
bit-bang does ~4µs of `delayMicroseconds` per bit plus `digitalWrite`
overhead, across 17 loads per frame — so the frame period is now far longer
than the original's 1.19ms, and 8µs × 8 windows is a much *smaller* fraction
of it. Expect the panel to be noticeably dimmer than original, not hotter.
**Re-measure duty at the GPIO before adjusting `ROW_ON_TIME_US` upward** —
don't scale it from the old numbers.

## RP2040 bring-up progress

> **Superseded (see top block):** the toolchain is now `board = waveshare_rp2040_zero` with the earlephilhower core, so `Serial.printf` works. `LED_ON`/`LED_OFF` has been replaced by per-colour `RED_ON/OFF`, `GREEN_ON/OFF`, both active-LOW. `ROW_ON_TIME_US` is now 64. Test selection is the single `MODE` switch.

Toolchain: PlatformIO, Arduino/mbed framework, board `pico`.

**Bugs found and fixed:**
- `PIN_A0`/`A1`/`A2` macro collisions with the framework's analog pin
  definitions → renamed `PIN_ADDR0/1/2`.
- `Serial.printf` unavailable on this core → replaced with `Serial.print`
  chains. Note also that `Serial.print` on the mbed/USB-CDC core can stall
  the entire loop when no monitor is attached — keep it out of
  timing-critical paths.
- LED polarity confirmed **backwards** from initial assumption:
  `LED_ON = 1`, `LED_OFF = 0`.
- **OE enable window was ~9× too long** (70µs vs the original hardware's
  ~7–8µs) and caused measurable panel overheating. Measured **18.8% duty
  vs the original's 6.4%**; fixed with `ROW_ON_TIME_US = 8` giving ~4.1%
  duty, verified by logic-analyzer capture of the Pico alone with the panel
  disconnected.
- RCLK originally had no setup/hold margin (only SRCLK was fixed when
  timing delays were first added).

**Compile-time flags** (single codebase, safe iterative testing) — *current
values in `main.cpp` as of this writing:*
- `FORCE_OE_DISABLED` — **currently `false`**, i.e. OE is live and the panel
  will drive. It's a plain `bool` used in an `if`, not an `#ifdef`, so set it
  to `true` (not just "defined") to blank the panel.
- `RUN_WALK_TEST` — **currently `0`** (blank-frame test, nothing commanded on).
- `ROW_ON_TIME_US` — currently `8`.

Minor: `loop()` has a redundant `#if RUN_WALK_TEST` nested inside an
already-guarded `#if RUN_WALK_TEST` block, making its `#else` branch dead
code. Harmless, but tidy it up before it misleads someone.

**Bring-up order once the 245 is wired:**
1. **Set `FORCE_OE_DISABLED = true`** (it ships as `false`). Power on.
   Confirm panel dark, nothing warming. This proves clocking/addressing
   aren't doing damage.
2. `RUN_WALK_TEST` — single walking LED. Confirms module order, row
   addressing, colour mapping, and the 80-bit load layout. Everything
   downstream depends on this being right.
3. Enable the second bit-plane; check colour mixing (R+G = amber).
4. OE duty last — bring up from near-zero, target the original's 6.4%.

**No current-limited bench supply yet** (fixed 5V only). Mitigations:
- Always boot with `FORCE_OE_DISABLED`.
- Inline **1Ω 5W** in the panel's 5V feed — measure the voltage across it
  for current, and it soft-limits a dead short. Remove once duty cycle is
  verified.
- Start tests on a single module, not the full array.

> **Note:** "Root cause found" above attributes this heating to the miswired pins (garbage data, OE driven by A2). Probably no longer open.

**Still open from bring-up:** specific modules (5th, 6th, 10th, 11th from
left) warmed faster than the rest during live testing, at a point when
firmware data provably had no effect on the display. More likely a
hardware-side fault local to those modules (bad joint, weak driver) than a
data problem — revisit after the level-shifted bring-up.

## Open questions / not yet confirmed

> **Superseded (see top block):** Q1 resolved: both lines active-LOW. Q2 resolved: address 0 is a real row. Q3 resolved: bits enter at the left, and "Geometry" gives the inversions. Q4 is partly resolved: 4, 6 and 8 are GND, 5 is G data, 16–20 still unmapped. Q5 resolved: pipelining, not brightness planes.

1. **R/G active polarity ("on" = 0 or 1).** Evidence is mixed:
   - G data is mostly HIGH (~97-98% duty) with sparse LOW bits scattered
     through each frame — consistent with **active-low** (0 = LED on),
     matching a mostly-dark display with a few lit pixels.
   - R data was observed nearly constant LOW during a state with *no red
     LEDs lit at all* — which fits **active-high** (1 = on) just as well,
     since "no red content" would just mean the line sits at its idle/off
     level throughout.
   - These two observations are not necessarily contradictory (R may simply
     have had zero "on" bits to show, so its idle level tells us nothing
     about polarity either way) but it means **polarity is not yet proven**.
   - **Status:** firmware uses `LED_ON = 1`, `LED_OFF = 0`. The source
     records this as *confirmed by hardware test* — filling the buffer with
     the old `LED_OFF` (1) lit most of a row, while the single bit set to
     the old `LED_ON` (0) did not light.
   - **But treat that as suspect.** It contradicts the other standing
     observation from the same period: that the panel showed identical
     output regardless of firmware content. Both cannot be strictly true.
     The likely explanation is marginal HC thresholds giving intermittent,
     partly-random behaviour — which means the polarity "confirmation" may
     have been reading noise. Re-run the test through the 74HCT245 before
     trusting it.
2. **What address value 0 actually represents.** The module only has 7 row
   commons (H1-H7) but the address counter cycles through 8 states (0-7).
   Address 0 consistently gets the anomalous double-length (160-bit) load —
   working theory is address 0 is a blanking/dummy cycle that doesn't
   correspond to a real physical row, and the extra 80 bits are
   padding/flush data for chain timing. **Not confirmed** — would need a
   direct capture of H1-H7 (or continuity-mapped equivalent) alongside
   A0-A2 to see which address value never actually lights a row.
3. **Bit order within the 80-bit column load.** Currently assumed to be
   sequential (module 1 col 1-5, module 2 col 1-5, … module 16 col 1-5) but
   this has not been empirically verified — could be reversed, or
   interleaved differently depending on how the 595 chain is physically
   wired module-to-module.
4. **The 8 unidentified connector pins** (see table above).
5. Whether the 2 bit-planes per row are true PWM brightness weighting or
   serve some other purpose (e.g. double-buffering) — not yet tested with a
   display state that has visibly different brightness levels.

## Tools/method notes for future captures

- Cheap 8-channel logic analyzer clone; **24MHz sample rate, ~200k sample
  window** (~8.3ms) has been the reliable setting for this board — enough
  to catch several full 1360-bit frames without overrunning the analyzer's
  USB streaming.
- 2MHz sampling (used in early captures) aliases SRCLK badly — don't trust
  "clock-shaped" signals found at that rate without re-verifying at higher
  rate.
- Diode-mode continuity testing does **not** work through the 74HC595/245
  ICs — always test connector-to-IC continuity with the board fully
  powered OFF, and expect to need to trace through buffer stages rather
  than assuming a direct wire.
- Always label/photograph which physical IDC pin is on which analyzer
  channel before capturing — a pin-ordering mixup already cost one
  capture session's worth of correlation data.
- **Check the logic family before assuming 3.3V will drive it.** HC vs HCT
  is a ~3.5V vs ~1.3V input threshold, and 3.3V GPIO silently fails against
  HC. This cost a long debugging cycle here: the symptom (identical panel
  output regardless of firmware) looked like a protocol or code bug and was
  neither.
- A logic analyzer capture of the **MCU alone, panel disconnected** is a
  clean way to verify firmware timing (OE duty, pulse widths) without the
  panel's loading or the risk of driving it wrongly.
- Cheap CY7C68013A analyzer clone is unreliable (floating inputs,
  inconsistent captures). DSLogic U2 Basic is the intended upgrade.
