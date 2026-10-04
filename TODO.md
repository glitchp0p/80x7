# TODO

## 1. PRIORITY: measure panel current under the PIO driver

All the current figures in `README.md` were measured with the old bit-banged
driver. With PIO, each row is lit for a much larger share of its slot, and
nothing has been measured since. So far it has only been checked by touch:
warm, not hot.

1. Put the DMM in series with the panel's 5V feed at JP1 (10A jack, DC amps).
2. Generate and flash the test card:
   `./animator levels ../src/frames.h`, then `pio run`, then flash.
3. In the second part (8–32 s), note the current at each 2 s step: red, green
   and amber at levels 0, 1, 2 and 3. The estimate for full-panel amber at
   level 3 is ~3 A.
4. Repeat at `ROW_ON_TIME_US 32` for comparison.
5. Optionally run 10+ minutes on full amber to confirm the current stays flat,
   as in the replay soak.
6. Update the "Power and safety" table and "Brightness" in `README.md`.
   If it comes in near the supply's rating, lower `ROW_ON_TIME_US`.

---

# Hospital panel — bring-up procedure

> **Status: completed.** The panel is fully working. This is the original
> bring-up plan, kept for reference. Some of its assumptions turned out wrong:
> OE is active-HIGH, both data lines are active-LOW, the two loads per address
> are pipelining rather than bit-planes, and address 0 is a real row. Test
> selection is now the single `MODE` switch in `main.cpp`, not separate flags.
> `README.md` describes the current behaviour, and `debug_log.md` has the full
> history. "Open question #N" below refers to the list at the end of
> `debug_log.md`.

Work top to bottom. Each stage has a pass condition; **do not move on until it
passes.** The project has already lost weeks to two independent faults that both
looked like protocol bugs (wrong pin defines, a heartbeat that silently did
nothing). Every stage below exists to catch one class of fault in isolation.

Flash cycle throughout: hold BOOTSEL, plug in, then

```
pio run
cp .pio/build/rp2040zero/firmware.uf2 /media/rich/RPI-RP2/
```

Serial monitor: `pio device monitor -b 115200`. **Open it before you care about
output** — on the mbed core a blocked print could stall the loop entirely. The
earlephilhower core in `platformio.ini` avoids this, but the habit is cheap.

---

## Stage 0 — Confirm the toolchain and that flashing works

**Wire up:** nothing. RP2040-Zero on USB only. Panel disconnected.

**Flags in `main.cpp`:**
```c
#define PIN_MAP_TEST       0
#define RUN_WALK_TEST      0
#define FORCE_OE_DISABLED  true
```

**Build and flash.** First build will pull the maxgerhardt platform — expect a
few minutes.

**Pass condition:**
- WS2812 goes **solid blue** briefly at boot, then **blinks green at 1Hz**.
- Serial prints `=== panel driver starting ===` and an `alive - frames/s:` line
  roughly once a second.

**If the LED stays dark:** the flash didn't take, or `board` is wrong in
`platformio.ini`. Do not proceed on the assumption it's "probably running" —
that assumption is exactly what cost this project weeks.

**If the LED is solid white:** the PIO clock divider is wrong. Check
`clock_get_hz(clk_sys)` is being used rather than a hardcoded 125MHz.

---

## Stage 1 — Build the level shifter

**Do not skip this and wire the panel direct.** The panel's shift registers are
74HC, not HCT — ~3.5V input threshold against the RP2040's 3.3V output. It will
appear to half-work, which is worse than not working.

**Parts:** 1 × 74HCT245, 20-pin DIP socket (socket it — don't solder the IC
directly), 1 × 100nF X7R ceramic.

**Fixed pins on the 245:**

| 245 pin | Connect to | Note |
|---------|-----------|------|
| 1 (DIR) | **5V** | A→B direction. Tie hard, never leave floating |
| 19 (/OE)| **GND** | outputs always enabled |
| 20 (VCC)| **5V** | |
| 10 (GND)| **GND** | |

**100nF ceramic across pins 20 and 10, physically at the package, short leads.**
Not optional — eight outputs switching at once is exactly what it's there for.

**Signal wiring:**

| Signal | RP2040-Zero pad | 245 A-side (in) | 245 B-side (out) | Panel IDC |
|--------|-----------------|-----------------|------------------|-----------|
| SRCLK  | GP4  | pin 2 | pin 18 | 1  |
| RCLK   | GP5  | pin 3 | pin 17 | 3  |
| R data | GP6  | pin 4 | pin 16 | 6  |
| G data | GP7  | pin 5 | pin 15 | 7  |
| OE     | GP9  | pin 6 | pin 14 | 9  |
| A0     | GP10 | pin 7 | pin 13 | 11 |
| A1     | GP11 | pin 8 | pin 12 | 13 |
| A2     | GP12 | pin 9 | pin 11 | 15 |

Plus **GND: RP2040-Zero GND → 245 pin 10 → panel IDC pin 2.**

The Zero's silkscreen numbers **are** GP numbers — no physical-pin translation.

**Watch the B-side ordering: it runs backwards.** B1 is pin 18 and B8 is pin 11,
so the wiring crosses over rather than running straight across the package.

**Panel still disconnected at the end of this stage.** Power the 245 from your
5V supply; common ground with the Zero.

---

## Stage 2 — Verify the pin mapping with a DMM

**This is the stage that would have caught the original fault.** Fifteen minutes
now against the alternative already demonstrated.

**Wire up:** as Stage 1. Panel **still disconnected** — you're probing the free
end of the IDC cable, or the header itself.

**Flags:**
```c
#define PIN_MAP_TEST 1
```
Rebuild, flash.

**Pass condition:** LED goes **magenta**. Serial prints a line every 3 seconds:

```
HIGH: GP4  -> SRCLK  -> expect ~5V on IDC 1
```

DMM in **DC volts**, black lead on ground, red lead on the named IDC pin. Read
~5V on the named pin and ~0V on the others. Work through all eight.

Note the *actual* IDC pin that goes high for each named signal. If any
disagree, fix the `#define` block in `main.cpp` to match the wiring — or rewire —
and **repeat this stage until all eight agree.**

You are also implicitly testing the 245 here: reading ~5V (not ~3.3V) confirms
it's translating. If you read 3.3V, you've bypassed it somewhere.

Set `PIN_MAP_TEST` back to `0` before moving on.

---

## Stage 3 — Panel connected, deliberately blanked

**Wire up:** connect the panel. Power it from the 5V supply via its JP1 terminal
block.

**No current-limited bench supply available**, so: fit a **1Ω 5W resistor inline
in the panel's 5V feed**. Measuring across it gives you current (1V across = 1A),
and it soft-limits a dead short. Remove it only once duty cycle is verified.

**Flags:**
```c
#define PIN_MAP_TEST       0
#define RUN_WALK_TEST      0
#define FORCE_OE_DISABLED  true
```

**Pass condition:** panel **completely dark**. Nothing warm after 60 seconds —
check by hand, particularly modules 5, 6, 10 and 11, which ran hot previously.
LED blinking green, serial alive.

**If anything lights or warms with OE force-disabled**, stop. That's a genuine
hardware fault, not firmware — nothing is being commanded on.

---

## Stage 4 — First real drive, short bursts

**Flags:**
```c
#define FORCE_OE_DISABLED  false   // OE now live
#define ROW_ON_TIME_US     8       // leave at 8
#define RUN_WALK_TEST      0       // still blank frame
```

Buffers are still all-`LED_OFF`, so nothing is commanded on — but OE is now
pulsing, so the panel is being driven for real for the first time.

**Run for 10 seconds, then unplug.** Hand on the modules. Check the voltage
across the 1Ω shunt while it runs.

**Pass condition:** panel dark or nearly so, no module noticeably warming,
shunt voltage steady and low.

**If it lights up with an all-off buffer**, polarity is inverted — swap `LED_ON`
and `LED_OFF` and repeat. (The previous "confirmation" of polarity was taken
through a miswired chain and should not be trusted.)

Once a 10-second run is clean, extend to 60 seconds and re-check temperature.

---

## Stage 5 — Walk test: the real payoff

**Flags:**
```c
#define RUN_WALK_TEST 1
```

A single pixel walks across all 80 columns of row 0, red then green, 600ms per
step, printing its bit index and derived module/column.

**Have the serial monitor and the panel in view together.** For each printed
line, note where the LED *actually* appears.

This one test resolves, at once:
- **Polarity** — does the commanded pixel light, or all the others?
- **Bit order** (open question #3) — sequential, reversed, or interleaved?
- **Module ordering** — does bit 0 land on the leftmost module?
- **Colour mapping** — does "RED" light red?

Write down the actual mapping. That's the deliverable of this stage.

> **Done:** see "Geometry" in `README.md`. Rows, columns and module order are
> all inverted.

---

## Stage 6 — Row addressing

Extend the walk test to step through rows rather than staying on row 0. Confirm
address N lights physical row H(N), and find out what address 0 actually does
(open question #2 — the theory is it's a non-physical blanking cycle, which is
why it gets the anomalous 160-bit load).

Note the known bug here: **address 0 currently displays row 0's real data**
because `rowIndex` maps both addr 0 and addr 1 to `redBuf[0]`. Fix that before
trusting row results.

> **Done:** address 0 is a real row. Addresses 0–6 map to rows 0–6
> (`BLANK_ADDR -1`), and the "stuck row" was the pipeline seam, fixed by
> `PIPELINE_PRIME`.

---

## Stage 7 — Bit-planes and brightness

> **Superseded:** the two loads per address turned out to be pipelining, not
> bit-planes (see "Driver structure" in `README.md`). Per-pixel brightness is
> now the firmware's own `levelBuf` scheme.

**Fix the bit-plane bug first** (`debug_log.md`, Known firmware bugs #1): both planes are
currently shifted and latched before the row address and OE window are set, so
plane 0 is overwritten and never displayed. Restructure `refreshFrame()` so each
plane gets its own address + OE window. This also fixes the OE-count mismatch
against the captures (should be 16 OE windows per frame, currently 8).

Then differentiate the planes and confirm whether the 2 loads per address are
genuine BCM brightness weighting or something else (open question #5).

---

## Stage 8 — Duty cycle and thermal

> **Still relevant, but now for the PIO driver:** re-measure panel current at
> `ROW_ON_TIME_US` 8 and 64 (see "Brightness" in `README.md`).

Re-measure OE duty at the GPIO. **The 18.8% / 6.4% / 4.1% figures in `debug_log.md`
are stale** — they come from an earlier code state, and the current bit-banged
loop has a much longer frame period, so 8µs is now a far smaller fraction of it.
Expect dim rather than hot.

Target the original hardware's **6.4%**. Raise `ROW_ON_TIME_US` gradually,
checking temperature at each step. Do not jump back toward 70µs.

Once stable, remove the 1Ω shunt.

---

## Stage 9 — Cleanup

- Remove the WS2812 heartbeat (~30µs blocking once per second perturbs one frame
  per second — fine for bring-up, not for timing work).
- Remove or keep the GP8 toggle as preferred.
- Consider moving the driver from bit-banged `digitalWrite` to PIO. The frame
  rate is currently far below the original's ~840Hz because of
  `delayMicroseconds` per bit. **Note the WS2812 heartbeat claims pio0 SM0** —
  free it first, or account for it in the SM budget.

---

## Standing cautions

- **No current-limited supply.** Fixed 5V only. Short runs, hand on the modules,
  1Ω shunt in place until Stage 8.
- **An identical RP2040-Zero is in service as the CMSIS-DAP debugprobe**
  (GP2=SWCLK, GP3=SWDIO, GP4/5=UART). Physically indistinguishable. Label them.
- **Re-run Stage 2 after any rewiring.** Breadboard connections move.
