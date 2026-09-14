
Prior work: ~/pi_pico/andys_matrix (protocol captures in logic_probes/,
2MHz ones superseded by 24MHz — see README open questions).


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

| Pin | Signal | Notes |
|-----|--------|-------|
| 1   | **SRCLK** | Shift register clock. ~1.14MHz, narrow active pulses (~1-2 samples @ 24MHz, ~42-83ns) |
| 2   | **GND** | Confirmed via continuity test to GND pad, board unpowered |
| 3   | **RCLK / LATCH** | Narrow active-high strobe. Fires once per row-load |
| 6   | **R data (SER)** | Red column serial data, sampled on SRCLK rising edges |
| 7   | **G data (SER)** | Green column serial data, sampled on SRCLK rising edges |
| 9   | **OE** | Output enable, active-low. Mostly high (blanked) with narrow low windows for brightness timing. Rising edge lands exactly 1 sample after RCLK's rising edge — tightly coupled latch→enable sequence |
| 11  | **A0** | Row address bit 0 (LSB) |
| 13  | **A1** | Row address bit 1 |
| 15  | **A2** | Row address bit 2 (MSB) |

Pins 4, 5, 8, 10, 12, 14, 16–20 are **not yet identified** — likely GND,
+5V, a second OE/LAT for a split panel, or spares. Not required for basic
operation but worth mapping eventually.

## Protocol / frame structure

- **SRCLK frequency:** ~1.14MHz (measured via 24MHz-sample-rate capture,
  period ≈ 20-23 samples).
- **80 bits shifted per row-load** = 16 modules × 5 columns. Confirmed two
  independent ways: (1) directly counting SRCLK edges between RCLK pulses,
  (2) matches the known module/column count.
- **Row address (A0-A2) cycles 7→0→7→0…**, i.e. counts *down*, wrapping at 0.
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

| Signal | RP2040 GPIO | firmware macro | 245 A-side (in) | 245 B-side (out) | Panel IDC |
|--------|-------------|----------------|-----------------|------------------|-----------|
| SRCLK  | GP4  | `PIN_SRCLK` | A1 — pin 2 | B1 — pin 18 | pin 1 |
| RCLK   | GP5  | `PIN_RCLK`  | A2 — pin 3 | B2 — pin 17 | pin 3 |
| R data | GP6  | `PIN_R`     | A3 — pin 4 | B3 — pin 16 | pin 6 |
| G data | GP7  | `PIN_G`     | A4 — pin 5 | B4 — pin 15 | pin 7 |
| OE     | GP9  | `PIN_OE`    | A5 — pin 6 | B5 — pin 14 | pin 9 |
| A0     | GP10 | `PIN_ADDR0` | A6 — pin 7 | B6 — pin 13 | pin 11 |
| A1     | GP11 | `PIN_ADDR1` | A7 — pin 8 | B7 — pin 12 | pin 13 |
| A2     | GP12 | `PIN_ADDR2` | A8 — pin 9 | B8 — pin 11 | pin 15 |

GP2, GP3 and GP8 are **not** connected to the panel.

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
| Raspberry Pi Pico (RP2040) | 1 | Replacement controller | In hand |
| 20-pin IDC header + cable | 1 | Panel connection | — |
| DIP socket, 20-pin | 1 | For the 245 (don't solder it in directly) | — |

On the panel itself (not purchased — salvaged): 74HC595D ×16, 74HC245D ×16,
APM4953 dual P-ch MOSFET, HR1–HR7 resistor networks, 16 × 5×7 bi-color
modules. **Fixed 5V supply only — no current-limited bench supply yet.**

## Root cause found: firmware pin defines never matched the harness

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

## Known firmware bugs (found by cross-checking `main.cpp` against captures)

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

**Still open from bring-up:** specific modules (5th, 6th, 10th, 11th from
left) warmed faster than the rest during live testing, at a point when
firmware data provably had no effect on the display. More likely a
hardware-side fault local to those modules (bad joint, weak driver) than a
data problem — revisit after the level-shifted bring-up.

## Open questions / not yet confirmed

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
