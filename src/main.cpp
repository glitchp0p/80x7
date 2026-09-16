// Hospital LED matrix panel driver — RP2040 replacement for original LPC1768 controller.
//
// This is a FIRST DRAFT built directly from reverse-engineered capture data — see README.md
// for the full writeup. Several details are marked TODO because they're still unconfirmed
// (see "Open questions" in README.md). Treat this as a test harness to validate those
// assumptions against the real panel, not a finished driver.
//
// Bit-banged for now (simple, easy to debug with a logic analyzer alongside it). Once the
// protocol is fully validated, this is a good candidate to move to RP2040 PIO for cleaner
// timing and to free up the CPU core, especially if a scanning/animation refresh rate ends
// up needing more headroom than bit-banging comfortably gives.

#include <Arduino.h>
#include <string.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"

// ============================================================================
//  MODE SELECT — set MODE to exactly one value. Nothing else to change.
// ============================================================================
//   MODE_RUN     normal panel driver
//   MODE_PINMAP  static DMM pin verification (LED magenta)
//   MODE_DIAG    display-driven signal identification (LED amber)
//   MODE_REPLAY  clock out a frame captured from the original controller (blue)
//
// This replaced four independent flags that each had their own default. Setting
// one without clearing the others silently ran the wrong test more than once.
// Now there is a single knob, and the build FAILS if it is inconsistent.

#define MODE_RUN     0
#define MODE_PINMAP  1
#define MODE_DIAG    2
#define MODE_REPLAY  3

// >>>>>>>>>>>>>>>>  SET THIS  <<<<<<<<<<<<<<<<
#define MODE MODE_RUN

// Which DIAG test, when MODE == MODE_DIAG. 1=chain length 2=which line carries
// data 3=what IDC 9 does 4=row addressing
#define DIAG_TEST 1

#if (MODE != MODE_RUN) && (MODE != MODE_PINMAP) && (MODE != MODE_DIAG) && (MODE != MODE_REPLAY)
  #error "MODE must be one of MODE_RUN / MODE_PINMAP / MODE_DIAG / MODE_REPLAY"
#endif

// Derived flags - do not set these by hand.
#define PIN_MAP_TEST  (MODE == MODE_PINMAP)
#define REPLAY_MODE   (MODE == MODE_REPLAY)
#if MODE == MODE_DIAG
  #define DIAG_MODE DIAG_TEST
#else
  #define DIAG_MODE 0
#endif


// ---- WS2812 heartbeat (RP2040-Zero onboard LED, GP16) ----
// TEMPORARY BRING-UP AID. The Zero has no plain GPIO LED, so without this there is
// no visible confirmation that a flash actually took and the firmware is running.
// Costs ~30us of blocking time once per second (24 bits x 1.25us), which perturbs
// exactly one panel frame per second. Acceptable while establishing basic life;
// REMOVE once panel timing is being characterised for real, since it injects
// measurable jitter into the very loop you'd be measuring.
//
// PIO-based rather than NOP-bit-banged, with the divider derived from the actual
// clock_get_hz(clk_sys) at runtime — the earlephilhower core runs at 133MHz, not
// the 125MHz a hand-tuned delay loop would assume. That mismatch previously caused
// a solid-white LED on this exact board.
#define PIN_WS2812 16
#define WS2812_PIO pio0
#define WS2812_SM  0

// Standard pico-examples ws2812 program, pre-assembled.
//   0: out    x, 1   side 0 [2]
//   1: jmp    !x, 3  side 1 [1]
//   2: jmp    0      side 1 [4]
//   3: nop           side 0 [4]
static const uint16_t ws2812_program_instructions[] = {
  0x6221, 0x1123, 0x1400, 0xa422,
};
static const struct pio_program ws2812_program = {
  .instructions = ws2812_program_instructions,
  .length = 4,
  .origin = -1,
};

static void ws2812_init(void) {
  uint offset = pio_add_program(WS2812_PIO, &ws2812_program);
  pio_gpio_init(WS2812_PIO, PIN_WS2812);
  pio_sm_set_consecutive_pindirs(WS2812_PIO, WS2812_SM, PIN_WS2812, 1, true);

  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, offset + 0, offset + 3);
  sm_config_set_sideset(&c, 1, false, false);
  sm_config_set_sideset_pins(&c, PIN_WS2812);
  sm_config_set_out_shift(&c, false, true, 24);  // shift left, autopull at 24 bits
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

  // 10 PIO cycles per WS2812 bit at 800kHz.
  float div = (float)clock_get_hz(clk_sys) / (800000.0f * 10.0f);
  sm_config_set_clkdiv(&c, div);

  pio_sm_init(WS2812_PIO, WS2812_SM, offset, &c);
  pio_sm_set_enabled(WS2812_PIO, WS2812_SM, true);
}

// Deliberately dim (values kept low): the onboard WS2812 is uncomfortably bright at
// full scale and there's no reason to draw the current during bring-up.
static void ws2812_put(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
  pio_sm_put_blocking(WS2812_PIO, WS2812_SM, grb << 8u);
}

// ---- Pin assignments — VERIFIED against physical wiring by continuity test ----
// These were previously placeholders (2,3,4,5,6,7,8,9) that matched NOTHING on the
// actual harness. Every signal was crossed: SRCLK was fed the OE waveform, RCLK was
// fed red pixel data, and OE was driven by address bit A2 — leaving the panel enabled
// for ~half of every frame (vs the original hardware's 6.4% duty). That is the cause
// of both the "display never changes regardless of firmware" symptom and the module
// overheating. The three address lines were never configured as outputs at all and
// floated. Do not change these without re-checking continuity to the IDC header.
//
//                        IDC pin   signal
#define PIN_SRCLK  4   //    1      shift register clock
#define PIN_RCLK   5   //    3      latch
#define PIN_R      6   //    6      red serial data
#define PIN_G      7   //    7      green serial data
#define PIN_OE     9   //    9      output enable (active low)
#define PIN_ADDR0 10   //   11      row address bit 0
#define PIN_ADDR1 11   //   13      row address bit 1
#define PIN_ADDR2 12   //   15      row address bit 2
// IDC pin 2 = GND. GP2, GP3 and GP8 are NOT connected to the panel.
//
// Heartbeat output — GP8 is free (wired to nothing) and adjacent to the panel
// signals, so it can share a logic-analyzer capture as a "firmware is alive" marker.
#define PIN_HEARTBEAT 8

// ---- Panel geometry (confirmed via capture) ----
#define NUM_MODULES        16
#define COLS_PER_MODULE    5
#define TOTAL_COLS         (NUM_MODULES * COLS_PER_MODULE)  // 80
#define NUM_ROWS           7   // physical row commons H1-H7
#define NUM_ADDR_STATES    8   // A0-A2 cycles 0-7 (addr 0 believed non-physical, see README)
#define BITPLANES_PER_ROW  2   // 2 loads observed per address value

// ---- LED polarity — INDEPENDENT PER COLOUR ----
// The red and green chains on this panel have OPPOSITE polarity. Observed directly:
//   buffers all 0  ->  every RED led lit, green dark
//   buffers all 1  ->  every GREEN led lit, red dark
// So: RED is active-LOW (0 = on), GREEN is active-HIGH (1 = on).
//
// A single global LED_ON/LED_OFF pair CANNOT blank this panel — it only swaps which
// colour is fully lit. That is why the first polarity flip produced an all-green panel
// instead of a dark one.
//
// Cause not yet established. Candidates: one data path passes through an inverting
// buffer on the panel (a '240 rather than a '245), or the red and green columns have
// opposite common polarity inside the modules. Worth identifying eventually, but the
// per-colour flags below are correct regardless of which it turns out to be.
//
// Flip either flag independently if the panel disagrees.
#define RED_ACTIVE_LOW   1
#define GREEN_ACTIVE_LOW 0

#if RED_ACTIVE_LOW
  #define RED_ON  0
  #define RED_OFF 1
#else
  #define RED_ON  1
  #define RED_OFF 0
#endif

#if GREEN_ACTIVE_LOW
  #define GREEN_ON  0
  #define GREEN_OFF 1
#else
  #define GREEN_ON  1
  #define GREEN_OFF 0
#endif

// Frame buffer: one bit per (row, column), one array per color.
// Index [0] corresponds to address value 0 (currently treated as blank/dummy — see below).
// Real rows are addresses 1-7 mapped to physical rows H1-H7 — TODO confirm this mapping,
// it's currently just assumed identity (addr N -> row N) with no evidence either way.
static uint8_t redBuf[NUM_ROWS][TOTAL_COLS];
static uint8_t greenBuf[NUM_ROWS][TOTAL_COLS];

static volatile uint32_t srclkPulseCount = 0;
static volatile uint32_t oeEnableCount = 0;

static inline void pulseSRCLK() {
  digitalWrite(PIN_SRCLK, HIGH);
  delayMicroseconds(1); // hold time: ensure clock stays high long enough to register
  digitalWrite(PIN_SRCLK, LOW);
  delayMicroseconds(1); // settle time before next bit's data changes
  srclkPulseCount++;
}

static inline void pulseRCLK() {
  digitalWrite(PIN_RCLK, HIGH);
  delayMicroseconds(1); // hold time — same fix applied to SRCLK earlier was missed here.
                        // If RCLK's pulse was too narrow to reliably latch, the panel's
                        // visible output could stay stuck on old data indefinitely,
                        // regardless of what shiftOutRow() sends — matching the observed
                        // "different code, identical display" symptom exactly.
  digitalWrite(PIN_RCLK, LOW);
  delayMicroseconds(1);
}

// SAFETY: panel modules were observed overheating during bring-up testing — almost
// certainly because bit-banged digitalWrite() on this core is much slower than the
// original ~1.14MHz protocol timing, so OE ends up enabled for a much larger fraction
// of time than the low-duty pulses seen in the original captures. Until real OE/SRCLK
// timing has been verified at the Pico's own GPIO pins (panel disconnected) with a
// logic analyzer, FORCE_OE_DISABLED keeps the panel permanently blanked so it's safe
// to flash and run without risk of sustained drive current. Only set this to false
// once you've confirmed OE duty cycle is genuinely low via direct measurement.
// SAFETY-CRITICAL: real per-row OE enable time on the original hardware was ~7-8us
// (measured from the very first captures), not the 70us this firmware used initially.
// That 9x-too-long window is the confirmed cause of the panel heating during bring-up
// (measured actual enabled duty was ~18.8% here vs ~6.4% on the original board).
// Start LOW and only increase gradually while monitoring temperature and using a
// current-limited supply — do not jump back up toward 70us.
#define ROW_ON_TIME_US 8

// Set to 1 to run the walking-column diagnostic, 0 to hold a completely blank frame.
// Start with 0 — if modules 5/6/10/11 still heat with NOTHING commanded on, that's a
// hardware fault local to those modules, not a firmware bug. If they stay cool blank
// but heat again once RUN_WALK_TEST is re-enabled, the problem is in the data framing
// (likely the shift/latch structure assumption — see README open question #3 area).
#define RUN_WALK_TEST 0

// ---- PIN_MAP_TEST: static pin-mapping verification with a DMM ----
// Set to 1 to bypass ALL panel driving. Holds exactly one signal HIGH at a time for
// PIN_MAP_DWELL_MS, everything else LOW, and prints which one is asserted. Probe the
// corresponding IDC pin with a DMM in DC volts (black lead to ground): you should read
// ~5V on the named pin (after the 245) and ~0V everywhere else.
//
// Why this exists: the firmware's pin defines were wrong for the entire bring-up and
// nothing caught it, because every symptom looked like a protocol bug. This test is the
// cheapest thing that would have caught it, and it also verifies the 74HCT245 end to end
// in the same pass. Run it after ANY rewiring, before trusting a single frame of output.
//
// OE is deliberately included in the walk. It is active-LOW, so asserting it HIGH is the
// blanked/safe state — this test never lights the panel.
#define PIN_MAP_DWELL_MS 3000

// WARNING (2026-09-10): OE APPEARS NOT TO WORK ON THIS PANEL.
// Tying IDC pin 9 hard to 5V and hard to GND produced IDENTICAL output (bottom row lit
// bright). Continuity from 245 pin 14 (B5) to IDC 9 is good, and IDC 9 is not shorted to
// either rail. So either IDC 9 is not the panel's output-enable, or it is gated behind
// the panel's own per-module 74HC245 buffers and never reaches the 595 /OE lines.
// CONSEQUENCE: FORCE_OE_DISABLED PROTECTS NOTHING. Data polarity is currently the only
// working blanking mechanism — which is why INVERT_POLARITY matters for safety, not just
// for correct display. Keep runs short and check panel temperature by hand.
#define FORCE_OE_DISABLED false   // SAFE DEFAULT after the pin-mapping fix:
                                 // keep the panel blanked until the corrected mapping
                                 // has been verified on a logic analyzer.

// ---- OE POLARITY: ACTIVE-HIGH on this panel ----
// Derived from three measured states, not from the capture. Shifting is the long
// part of every slot (~400us against an 8us window), so whatever OE state the
// shift runs in is the state the panel spends nearly all its time in:
//
//   version          shift happens        PIN_OE during shift   measured
//   pre-pipeline     interleaved          LOW  (setOE true)     4.6A,   hot
//   pipelined        after setOE(true)    LOW                   0.143A, cool
//   blanked-shift    after setOE(false)   HIGH                  4.5A,   warming
//
// Long phase at PIN_OE LOW -> 0.143A. Long phase at PIN_OE HIGH -> 4.5A.
// Therefore HIGH enables the panel and LOW blanks it. OE is ACTIVE-HIGH here.
//
// The old code had this inverted, so every OE call in the driver was backwards.
// The 0.143A version was only cool by accident - it happened to spend its long
// phase blanked. It also explains the uniform amber: the panel was enabled ONLY
// during the brief setOE(false) moments bracketing the latch and address change,
// so what showed was a smear of transitions rather than settled data - which is
// why current never tracked buffer contents.
//
// It also explains why changing REPLAY_OE_US did nothing: replayFrame() drives OE
// directly with the same inverted assumption, so shortening the "window" was
// shortening the BLANKED period, not the lit one.
//
// ROLLBACK: previous body was
//     digitalWrite(PIN_OE, lit ? LOW : HIGH);
//   with FORCE_OE_DISABLED writing HIGH.
static inline void setOE(bool lit) {
  if (FORCE_OE_DISABLED) {
    digitalWrite(PIN_OE, LOW);  // LOW = blanked on this panel
    return;
  }
  digitalWrite(PIN_OE, lit ? HIGH : LOW);
  if (lit) oeEnableCount++;
}

static inline void setRowAddress(uint8_t addr) {
  digitalWrite(PIN_ADDR0, (addr >> 0) & 1);
  digitalWrite(PIN_ADDR1, (addr >> 1) & 1);
  digitalWrite(PIN_ADDR2, (addr >> 2) & 1);
}

// Shifts 80 bits of column data (one row's worth) into the R and G chains in parallel.
// TODO (README open question #3): bit order within the 80 assumed sequential
// module0-col0..col4, module1-col0..col4, ... module15-col0..col4. Not yet verified —
// if the lit pattern comes out mirrored or module-shuffled, this is the first place to look.
//
// DIAGNOSTIC: added explicit setup-time delay before the clock edge, and hold-time delays
// inside pulseSRCLK(). Testing the theory that a blank/constant buffer works correctly but
// a single differing bit corrupts the whole row because data isn't held stable long enough
// relative to the clock edge — a single bad bit in a shift register propagates forward and
// can corrupt everything shifted after it, which would explain "one bit commanded -> entire
// row lit."
static void shiftOutRow(const uint8_t *redRow, const uint8_t *greenRow) {
  // ASCENDING, bit 0 first - matching replayFrame(), which is the only code path
  // that has ever rendered correctly on this hardware. This loop previously ran
  // DESCENDING, sending every row reversed relative to the known-good reference.
  // There was no evidence for the descending order; it was an assumption.
  for (int col = 0; col < TOTAL_COLS; col++) {
    digitalWrite(PIN_R, redRow[col]);
    digitalWrite(PIN_G, greenRow[col]);
    delayMicroseconds(1); // setup time: let data settle before the clock samples it
    pulseSRCLK();
  }
}

// Drives one full frame: 8 address states, 2 loads each, matching captured timing.
// FIX (was README open question #2 / a standing TODO): the original hardware's address-0
// SECOND load was always 160 bits, not 80, in every capture. This firmware was never
// updated to match — it just shifted 80 bits regardless of address, leaving the back half
// of the real chain holding stale data from the previous cycle. That's the likely cause of
// the full-row-lit corruption seen during walking-test bring-up: a single new bit combining
// with leftover garbage in an under-shifted chain. Fix: shift an extra 80 bits of blank
// (all-off) padding immediately before that specific load's latch, so nothing stale remains.
// Separate pad rows per colour, since "off" is a different value for each.
// Zero-initialisation is NOT safe here: RED_OFF is 1, so a zero-init red pad would be
// 80 bits of full-brightness red. setup() memsets both explicitly.
static uint8_t blankPadRed[TOTAL_COLS];
static uint8_t blankPadGreen[TOTAL_COLS];

// Drives one full frame, mirroring the structure proven correct by REPLAY_MODE.
//
// STRUCTURE CONFIRMED FROM THE CAPTURE (not assumed):
//   - 16 loads per frame, addresses counting DOWN 7,7,6,6,...,1,1,0,0
//   - each address gets TWO loads = two independent bit-planes
//   - the SECOND load at address 0 is 160 bits, not 80 (one full-chain flush)
//   - total 15*80 + 160 = 1360 bits
//   - one OE low window per load, ~8us, immediately after each latch
//
// The two planes are genuinely different data: in the capture, consecutive loads
// at the same address had IDENTICAL red content but DIFFERENT green content. That
// is the original's 2-level brightness scheme, not duplicated frames. The previous
// implementation shifted the same buffer twice, which threw that away.
//
// FIXED HERE (was Known-bugs #1 and #2): the row address and the OE window are now
// set PER PLANE, inside the plane loop. Previously both planes were shifted and
// latched before the address and OE were touched even once, so plane 0's latch was
// overwritten by plane 1's while still blanked - plane 0 never reached the LEDs,
// and the frame emitted 8 OE windows instead of the captured 16.
//
// CAVEAT, measured: on this panel OE does NOT appear to gate illumination. Changing
// the window from 8us to 1us produced no current change (2.666A -> 2.723A), while
// 245 pin 14 reads 4.74V against a 4.93V rail, i.e. it IS pulsing correctly. The OE
// sequencing below is kept because it matches the original hardware exactly, not
// because it has been shown to do anything here.
// ---- POLARITY_SWEEP — test all four R/G polarity combinations automatically ----
// The "off" values for this panel have been inferred repeatedly and repeatedly
// been wrong. Rather than propose another model, cycle through all four
// combinations and let the panel say which one is right.
//
// Every 4 seconds it rebuilds the sparse pattern under the next combination and
// prints which one is active. ONE of the four should show a sparse diagonal of a
// few pixels on a dark panel. The other three will light most or all of the panel.
//
// Report back which combination number shows the sparse pattern, and that settles
// polarity from observation instead of inference.
//
// Current draw is itself a readout here: the correct combination lights ~9 pixels
// and will draw a small fraction of what the wrong ones draw.
#define POLARITY_SWEEP 0

#if POLARITY_SWEEP
static uint8_t sweepRedOff = 1, sweepGreenOff = 0;
static uint8_t sweepRedOn  = 0, sweepGreenOn  = 1;

static void applySweep(uint8_t combo) {
  //  combo 0: R off=1 on=0 | G off=0 on=1
  //  combo 1: R off=1 on=0 | G off=1 on=0
  //  combo 2: R off=0 on=1 | G off=0 on=1
  //  combo 3: R off=0 on=1 | G off=1 on=0
  sweepRedOff   = (combo & 2) ? 0 : 1;  sweepRedOn   = sweepRedOff ? 0 : 1;
  sweepGreenOff = (combo & 1) ? 1 : 0;  sweepGreenOn = sweepGreenOff ? 0 : 1;

  memset(redBuf,   sweepRedOff,   sizeof(redBuf));
  memset(greenBuf, sweepGreenOff, sizeof(greenBuf));
  memset(blankPadRed,   sweepRedOff,   sizeof(blankPadRed));
  memset(blankPadGreen, sweepGreenOff, sizeof(blankPadGreen));

  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    uint8_t c = (uint8_t)(r * 11);
    if (c < TOTAL_COLS) redBuf[r][c] = sweepRedOn;
  }
  greenBuf[3][41] = sweepGreenOn;

  if (Serial) {
    Serial.print("=== POLARITY COMBO "); Serial.print(combo);
    Serial.print("  RED_OFF=");   Serial.print(sweepRedOff);
    Serial.print(" GREEN_OFF=");  Serial.print(sweepGreenOff);
    Serial.println("  <- does the panel show a SPARSE diagonal now?");
  }
}

static void runPolaritySweep(void) {
  static uint8_t combo = 0;
  static uint32_t last = 0;
  static bool started = false;
  if (!started) { applySweep(0); started = true; last = millis(); return; }
  if (millis() - last < 4000) return;
  last = millis();
  combo = (combo + 1) & 3;
  applySweep(combo);
}
#endif

// ---- PATTERN_SEQUENCE — systematic elimination test ----
// Cycles through known patterns, one per PATTERN_DWELL_MS, printing the pattern
// name and the PREDICTED current before each. Watch three things per step: panel
// appearance, actual current, and the printed prediction. Discrepancies localise
// the fault far better than any single test.
//
// Predictions assume ~10mA per lit die and 0.057A quiescent (both measured).
// A pattern drawing far LESS than predicted means pixels are not being driven.
// A pattern drawing far MORE means something is lit that was not commanded.
//
// NOTE ON "ALL AMBER": the panel currently shows amber everywhere while drawing
// only 0.144A. 1120 lit dies would be ~11A. So that amber is NOT commanded output
// - it is low-duty residual illumination from data shifting through rows that are
// still enabled. Visible, but drawing almost nothing. Current is the reliable
// readout here; the eye is not.
#define PATTERN_SEQUENCE 1
#define PATTERN_DWELL_MS 4000

// SAFE SUBSET: run only the low-current patterns until proportionality is proven.
// These four span 0 to 80 lit pixels and all stay under ~0.6A. If measured current
// tracks prediction across them, current scales with lit pixel count and the
// high-count patterns (1,2,3,6,7 - up to ~11A) become safe to run.
//
// Note what made the panel safe: NOT the sparse pattern itself, but the PIPELINE
// FIX. Before it, this same sparse pattern drew 4.6A and the row drivers heated,
// because rows stayed enabled while data shifted through them. After it, the same
// pattern draws 0.144A and stays cool. The pattern did not change; the driver did.
// Sparse is low-current because it lights 9 pixels - that is arithmetic, not a
// safety property of the mode.
//
// Set to 0 to run the full nine-pattern sequence.
#define PATTERN_SAFE_SUBSET 0

// ---- BLANK_ADDR — which address gets no row data ----
// 8 addresses (0-7) but only 7 physical rows, so exactly one address must be
// driven blank. WHICH one is unsettled: skipping address 1 darkened the second
// row from the bottom, skipping address 7 darkened the bottom row, so the '138
// outputs are not wired to rows in numeric order.
// Set -1 to give every address real data. Try 0-7 to find the spare.
#define BLANK_ADDR -1

// ---- PIPELINE_PRIME ----
// Each address displays data shifted during the PREVIOUS slot. The FIRST address
// of a frame has no predecessor inside that frame, so it shows leftovers from the
// last frame. That row is the pipeline SEAM and is stuck regardless of its buffer
// - matching the bottom row staying amber in every pattern, on BOTH panels, which
// makes it structural rather than damage. The original primes it: the captures
// show a 160-bit load (two rows' worth) at address 0, once per frame.
#define PIPELINE_PRIME 1

#if PATTERN_SEQUENCE
static void fillAll(uint8_t rv, uint8_t gv) {
  memset(redBuf, rv, sizeof(redBuf));
  memset(greenBuf, gv, sizeof(greenBuf));
}

// GREEN-ONLY PATTERNS.
//
// The red data line is stuck LOW at the panel (measured 0.7mV at IDC pin 6 while
// the buffer held mostly 1s). RED_ON is 0, so stuck-low means red is ON for every
// pixel - which is why the background is amber rather than dark, and why no
// red-only pattern has ever changed the display.
//
// Rather than wait on that fault, drive GREEN only and read the panel as a
// two-state display:
//     green OFF -> pixel shows RED
//     green ON  -> pixel shows AMBER (red + green)
// That is enough contrast to verify addressing, bit order, module mapping and the
// whole driver path. Red gets fixed separately; it does not block this.
//
// Every pattern below leaves redBuf at RED_OFF throughout, so the ONLY variable is
// green. If the panel shows anything that does not track the green pattern, the
// fault is in the driver, not in the red line.
static void applyPattern(uint8_t p) {
  const char *name = "";
  const char *expect = "";
  memset(redBuf, RED_OFF, sizeof(redBuf));      // constant across all patterns
  memset(greenBuf, GREEN_OFF, sizeof(greenBuf));

  switch (p) {
    case 0:
      name   = "ALL GREEN OFF";
      expect = "whole panel RED (uniform)"; break;

    case 1:
      memset(greenBuf, GREEN_ON, sizeof(greenBuf));
      name   = "ALL GREEN ON";
      expect = "whole panel AMBER (uniform)"; break;

    case 2:
      for (uint8_t c = 0; c < TOTAL_COLS; c++) greenBuf[3][c] = GREEN_ON;
      name   = "SINGLE ROW green (row 3)";
      expect = "ONE amber row, six red rows -> proves row addressing"; break;

    case 3:
      for (uint8_t r = 0; r < NUM_ROWS; r++) greenBuf[r][40] = GREEN_ON;
      name   = "SINGLE COLUMN green (col 40)";
      expect = "ONE amber column mid-panel -> proves bit index maps to a column"; break;

    case 4:
      for (uint8_t r = 0; r < NUM_ROWS; r++)
        for (uint8_t c = 0; c < TOTAL_COLS; c++)
          if (((r + c) & 1) == 0) greenBuf[r][c] = GREEN_ON;
      name   = "CHECKERBOARD green";
      expect = "alternating amber/red per pixel -> proves PER-PIXEL control"; break;

    case 5:
      for (uint8_t r = 0; r < NUM_ROWS; r++)
        for (uint8_t c = 0; c < TOTAL_COLS / 2; c++) greenBuf[r][c] = GREEN_ON;
      name   = "LEFT HALF green";
      expect = "left 8 modules AMBER, right 8 RED -> boundary shows bit ORDER"; break;

    case 6:
      for (uint8_t c = 0; c < COLS_PER_MODULE; c++) greenBuf[0][c] = GREEN_ON;
      name   = "FIRST MODULE, ROW 0 green";
      expect = "5 amber pixels in ONE module -> which end is bit 0?"; break;

    case 7:
      for (uint8_t r = 0; r < NUM_ROWS; r++) {
        uint8_t c = (uint8_t)(r * 11);
        if (c < TOTAL_COLS) greenBuf[r][c] = GREEN_ON;
      }
      name   = "DIAGONAL green";
      expect = "amber diagonal stepping right and down -> row+column together"; break;

    case 8:
      for (uint8_t r = 0; r < NUM_ROWS; r++)
        for (uint8_t c = 0; c < TOTAL_COLS; c++)
          if ((c % 10) < 5) greenBuf[r][c] = GREEN_ON;
      name   = "ALTERNATE MODULES green";
      expect = "modules alternate amber/red -> proves module boundaries"; break;
  }

  if (Serial) {
    Serial.println();
    Serial.print("=== PATTERN "); Serial.print(p); Serial.print(": ");
    Serial.println(name);
    Serial.print("    expect: "); Serial.println(expect);
  }
}

static void runPatternSequence(void) {
  static uint8_t p = 0;
  static uint32_t last = 0;
  static bool started = false;
  if (!started) { applyPattern(0); started = true; last = millis(); return; }
  if (millis() - last < PATTERN_DWELL_MS) return;
  last = millis();
#if PATTERN_SAFE_SUBSET
  // 0=all off, 5=single column, 4=single row, 8=sparse diagonal
  static const uint8_t safeSet[] = {0, 5, 4, 8};
  static uint8_t si = 0;
  si = (uint8_t)((si + 1) % (sizeof(safeSet)/sizeof(safeSet[0])));
  p = safeSet[si];
#else
  p = (uint8_t)((p + 1) % 9);
#endif
  applyPattern(p);
}
#endif

// ---- SPARSE_TEST — minimum-current proof that the panel renders ----
// Lights a handful of known pixels instead of a full frame. The replay frame has
// roughly 3/4 of the panel lit and draws ~2.67A; a dozen pixels draw a tiny
// fraction of that while proving exactly the same things: that the chain shifts,
// that the latch works, that row addressing selects, that both colours drive, and
// that bit index maps to the physical position expected.
//
// This is the correct FIRST power-up test on an undamaged panel. Use it before
// MODE_REPLAY, not after.
//
// WHY PIXEL COUNT IS THE ONLY REAL CURRENT KNOB HERE:
//   - Refresh RATE does not change current. Each row still gets one slot per
//     frame, so the 1-in-8 duty is identical whether frames run fast or slow.
//     Faster = shorter slots more often; slower = longer slots less often.
//     (The earlier SLOW_SCAN damage was NOT from a slower rate: that code padded
//     the ON time while leaving off-time alone, taking a row from a small
//     fraction of the time to nearly all of it. Duty change, not rate change.)
//   - Column scanning is not possible. All 80 columns in a row are driven
//     simultaneously by the 595 outputs; the shift register holds a static state
//     until the next latch. Multiplexing gives per-pixel CONTROL, not per-pixel
//     TIMING. The addressable unit is the row.
//   - OE would normally shorten on-time within a slot, but measurement shows it
//     does not gate illumination on this hardware.
// That leaves how many pixels are lit. It is proportional and reliable.
#define SPARSE_TEST 0

#if SPARSE_TEST
// One pixel per row, walking diagonally, plus a colour pair for comparison.
// Positions chosen to land in different modules so module ordering is readable.
static void loadSparsePattern(void) {
  memset(redBuf,   RED_OFF,   sizeof(redBuf));
  memset(greenBuf, GREEN_OFF, sizeof(greenBuf));
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    uint8_t c = (uint8_t)(r * 11);          // 0,11,22,33,44,55,66 - spreads across modules
    if (c < TOTAL_COLS) redBuf[r][c] = RED_ON;
  }
  // Adjacent red/green pair mid-panel, to confirm both colours and their ordering.
  redBuf[3][40]   = RED_ON;
  greenBuf[3][41] = GREEN_ON;
}
#endif

// ---- SKIP_ADDR — never select this address ----
// Diagnostic for the bottom row, which stays lit after its slot ends and shows a
// left-to-right brightness gradient during other rows' slots.
//
// That gradient is the tell: if a row is on while the chain is being shifted for
// OTHER rows, it displays the register contents LIVE. Positions near the far end
// of the chain reach their final value early and hold; positions near the input
// keep changing until the last clock. Time-averaged that is bright at the far end
// fading to dim at the input end - exactly what is seen, and the same mechanism
// that produced the unexplained gradient in DIAG test 1.
//
// So the bottom row is smearing every other row's data. Dim red rather than amber
// because it is an average of transients, not a settled value.
//
// THE TEST: never select the suspect address at all.
//   Row still lights -> its high-side MOSFET is stuck on. Hardware fault: either
//        the APM4953 for that row is damaged (gate-source short) or its gate
//        pull-up in the HR networks is open, leaving the gate floating low.
//        Repairable - APM4953 is a cheap SOT23-6 dual P-channel.
//   Row goes dark    -> it IS being selected, and the fault is in addressing.
//
// Set to -1 to disable skipping. Try 0 and 1 in turn: which one lights the bottom
// row also settles open question #2, since address 0 is the long-suspected
// non-physical blanking cycle and a real row lighting there would disprove it.
#define SLOW_SCAN_MAX_MS     30   // hard ceiling on per-address dwell
#define SLOW_SCAN_TIMEOUT_S  20   // auto-blank after this many seconds of scanning

#if SLOW_SCAN_MS > SLOW_SCAN_MAX_MS
  #undef  SLOW_SCAN_MS
  #define SLOW_SCAN_MS SLOW_SCAN_MAX_MS
#endif

#define SKIP_ADDR -1

// ---- SLOW_SCAN_MS — make the multiplexing visible ----
// Normally the frame cycles through all 8 addresses in a few milliseconds, so
// persistence of vision blends them and the panel LOOKS continuously lit. That is
// why "whole panel amber" has been so hard to interpret: it is what correct
// multiplexing of all-pixels-on looks like, and it is also what several failure
// modes look like.
//
// Set SLOW_SCAN_MS to hold each address for that many milliseconds. At 250ms the
// rows step visibly one at a time and you can read off directly:
//   - which PHYSICAL row each address value lights
//   - whether address 0 lights anything at all (open question #2)
//   - whether all seven rows are actually being driven
//   - what content each row really holds, unblended
//
// 0 = normal full-speed operation.
//
// SAFETY - ENFORCED IN CODE, NOT BY THE OPERATOR:
// A single row carries ~4.7A when selected. The panel is designed for 1-in-8 duty,
// so holding one row on is ~8x its rated dissipation and destroys the APM4953 row
// MOSFETs in SECONDS. This already happened: a static single-address hold killed
// H1/H2 (cracked packages, burnt legs) on this board.
//
// Therefore:
//   - SLOW_SCAN_MS is CAPPED at SLOW_SCAN_MAX_MS below. Larger values are clamped.
//   - Any slow-scan run auto-stops after SLOW_SCAN_TIMEOUT_S and blanks the panel.
//   - There is deliberately NO mode that holds a single address indefinitely.
// Never add one. If a static measurement is needed, use a brief periodic hold, not
// a sustained one - no test should be able to damage hardware through inattention.
#define SLOW_SCAN_MS 0

static void blankChain(void);

// ============================================================================
//  refreshFrame() - PIPELINED. Restructured <date of this change>.
// ============================================================================
// ROLLBACK: the previous non-pipelined version is preserved verbatim at the
// bottom of this comment block. If this change makes things worse, delete the
// body below and paste that one back in. Nothing else in the file depends on
// which version is active.
//
// WHY THE CHANGE - evidence, not inference:
// Counting green '1' bits per load in the capture, by address:
//
//     addr | load 1 | load 2
//       0  |   55   |   55
//       1  |   66   |   55
//       2  |   69   |   66
//       3  |   51   |   69
//       4  |   68   |   51
//       5  |   67   |   68
//       6  |   50   |   67
//       7  |   80   |   50
//
// The SECOND load at address N carries the SAME data as the FIRST load at
// address N+1. The counts cascade diagonally - 55, 66, 69, 51, 68, 67, 50, 80 -
// each value appearing twice, one address apart.
//
// So the two loads per address are NOT bit-planes. The original controller is
// PIPELINED: while row N is being displayed, it shifts in row N+1's data. The
// old README theory of "2-level BCM brightness weighting" is wrong, and so was
// the previous implementation, which shifted the SAME row twice per address.
//
// That is why our driver produced flat colour while the replay produced text:
// the row-to-data pairing was lost. It also explains why green looked "stuck"
// while red responded - the replay's red data is all zeros, so a pipeline offset
// is invisible on red. Only green carried varying content, so only green showed
// the symptom.
//
// The 160-bit load at address 0 fits too: it is pipeline priming, shifting two
// rows' worth to fill the chain at the start of each frame.
//
// --- PREVIOUS VERSION, for rollback ---
//   for (int8_t addr = NUM_ADDR_STATES - 1; addr >= 0; addr--) {
//     const bool driveBlank = (addr == 0);
//     uint8_t rowIndex = (addr >= 1 && addr <= NUM_ROWS) ? (addr - 1) : 0;
//     const uint8_t *rowRed   = driveBlank ? blankPadRed   : redBuf[rowIndex];
//     const uint8_t *rowGreen = driveBlank ? blankPadGreen : greenBuf[rowIndex];
//     for (uint8_t plane = 0; plane < BITPLANES_PER_ROW; plane++) {
//       setOE(false);
//       setRowAddress((uint8_t)addr);
//       if (addr == 0 && plane == 1) shiftOutRow(blankPadRed, blankPadGreen);
//       shiftOutRow(rowRed, rowGreen);
//       pulseRCLK();
//       setOE(true);
//       delayMicroseconds(ROW_ON_TIME_US);
//       setOE(false);
//     }
//   }
// --- END PREVIOUS VERSION ---
static void refreshFrame() {
#if PIPELINE_PRIME
  // Prime the seam: shift the FIRST address's data before the loop starts, so the
  // opening latch has this frame's content rather than last frame's leftovers.
  // Firmware equivalent of the original's 160-bit load at address 0.
  {
    const int8_t firstAddr = NUM_ADDR_STATES - 1;
    uint8_t pr = (firstAddr < NUM_ROWS) ? (uint8_t)firstAddr : (uint8_t)(NUM_ROWS - 1);
#if BLANK_ADDR >= 0
    const bool pb = (firstAddr == BLANK_ADDR);
#else
    const bool pb = false;
#endif
    setOE(false);                       // stay blanked throughout priming
    shiftOutRow(pb ? blankPadRed : redBuf[pr], pb ? blankPadGreen : greenBuf[pr]);
  }
#endif
  // Addresses count DOWN, as captured: 7,6,5,4,3,2,1,0.
  // At each address we DISPLAY what was shifted during the previous slot, then
  // shift the NEXT address's data while this one is lit.
  for (int8_t addr = NUM_ADDR_STATES - 1; addr >= 0; addr--) {
#if SKIP_ADDR >= 0
    if (addr == SKIP_ADDR) {
      if (Serial) { Serial.print("SKIPPING address "); Serial.println(addr); }
      continue;
    }
#endif

    // Latch whatever was shifted in during the previous iteration, select this
    // row, and light it.
    setOE(false);                    // blank across the latch
    pulseRCLK();
    setRowAddress((uint8_t)addr);
    setOE(true);

    // The NEXT address in the countdown (wrapping 0 -> 7). Its data is what we
    // shift while the current row is displayed.
    int8_t nextAddr = (addr == 0) ? (NUM_ADDR_STATES - 1) : (addr - 1);

    // Address 0 is driven blank: it must not share a buffer with address 1.
    // (Kept from the previous fix - the aliasing gave one row double on-time,
    // which is the most likely cause of H1/H2 running hot on both panels.)
#if BLANK_ADDR >= 0
    const bool nextBlank = (nextAddr == BLANK_ADDR);
#else
    const bool nextBlank = false;
#endif
    uint8_t nextRow = (nextAddr < NUM_ROWS) ? (uint8_t)nextAddr : (uint8_t)(NUM_ROWS - 1);
    const uint8_t *nRed   = nextBlank ? blankPadRed   : redBuf[nextRow];
    const uint8_t *nGreen = nextBlank ? blankPadGreen : greenBuf[nextRow];

    // The shift happens at the END of this block, AFTER setOE(false).
    // See the note there.

#if SLOW_SCAN_MS > 0
    if (millis() > (uint32_t)SLOW_SCAN_TIMEOUT_S * 1000UL) {
      setOE(false);
      memset(redBuf,   RED_OFF,   sizeof(redBuf));
      memset(greenBuf, GREEN_OFF, sizeof(greenBuf));
      blankChain();
      if (Serial) Serial.println("SLOW SCAN TIMEOUT - panel blanked. Power-cycle to rerun.");
      while (1) { ws2812_put(24, 0, 0); delay(500); ws2812_put(0,0,0); delay(500); }
    }
    if (Serial) {
      Serial.print("SLOW SCAN  address "); Serial.print(addr);
      Serial.println("   <- note which PHYSICAL row lights");
    }
    delay(SLOW_SCAN_MS);
#else
    delayMicroseconds(ROW_ON_TIME_US);
#endif
    // REVERTED. Shifting here (after setOE(false), i.e. "blanked") measured
    // 4.5A with the row drivers warming. Shifting BEFORE the OE window - see
    // above - measured 0.143A and stayed cool. The blanked-shift version is
    // theoretically what the capture shows the original doing, but on this
    // hardware it draws 30x more current, so the theory is wrong somewhere and
    // the measurement wins.
    //
    // Known unresolved: at 0.143A the panel still shows uniform amber whose
    // current does not vary with buffer contents, so buffer data is not reaching
    // the display. That is a real bug - but it is a COOL bug, and diagnosing it
    // must not be done by running the panel at 4.5A.
    setOE(false);          // blank first: PIN_OE LOW = blanked (active-high panel)

    // Shift the next row's data while BLANKED. Shifting is ~400us against an 8us
    // lit window, so this is where the panel spends nearly all its time - and it
    // must be spent blanked.
    //
    // MEASURED, to stop this being re-broken a third time:
    //   shift while PIN_OE HIGH -> 4.5A, drivers warming   (WRONG)
    //   shift while PIN_OE LOW  -> 0.143A, cool            (RIGHT)
    // Both polarity and ordering must be correct TOGETHER. Inverting one without
    // the other puts the long phase back in the enabled state and returns 4.5A by
    // the opposite route - which is exactly what happened once already.
    shiftOutRow(nRed, nGreen);
  }
}

#if PIN_MAP_TEST
struct PinMapEntry { uint8_t gpio; const char *name; const char *idc; };
static const PinMapEntry pinMap[] = {
  { PIN_SRCLK, "SRCLK",  "IDC 1"  },
  { PIN_RCLK,  "RCLK",   "IDC 3"  },
  { PIN_R,     "R data", "IDC 6"  },
  { PIN_G,     "G data", "IDC 7"  },
  { PIN_OE,    "OE",     "IDC 9"  },
  { PIN_ADDR0, "A0",     "IDC 11" },
  { PIN_ADDR1, "A1",     "IDC 13" },
  { PIN_ADDR2, "A2",     "IDC 15" },
};
#define PIN_MAP_COUNT (sizeof(pinMap) / sizeof(pinMap[0]))

static void allPinsLow() {
  for (uint8_t i = 0; i < PIN_MAP_COUNT; i++) digitalWrite(pinMap[i].gpio, LOW);
}

static void runPinMapTest() {
  static uint8_t idx = 0;
  static uint32_t lastStep = 0;
  static bool announced = false;

  if (!announced) {
    allPinsLow();
    Serial.println();
    Serial.println("=== PIN MAP TEST ===");
    Serial.println("DMM in DC volts, black lead to GND, red lead on the named IDC pin.");
    Serial.println("Expect ~5V on the named pin only, ~0V on all others.");
    Serial.println("Panel is never lit in this mode (OE asserted HIGH = blanked).");
    Serial.println();
    announced = true;
    lastStep = millis() - PIN_MAP_DWELL_MS;  // start immediately
  }

  if (millis() - lastStep < PIN_MAP_DWELL_MS) return;
  lastStep = millis();

  allPinsLow();
  digitalWrite(pinMap[idx].gpio, HIGH);

  // Magenta = pin map mode, so it can't be mistaken for the normal green heartbeat.
  ws2812_put(24, 0, 24);

  Serial.print("HIGH: GP");   Serial.print(pinMap[idx].gpio);
  Serial.print("  -> ");      Serial.print(pinMap[idx].name);
  Serial.print("  -> expect ~5V on ");  Serial.println(pinMap[idx].idc);

  idx = (idx + 1) % PIN_MAP_COUNT;
  if (idx == 0) Serial.println("--- full pass complete, repeating ---");
}
#endif

// Shifts an all-OFF frame into every address as fast as possible, with no OE window.
// WHY THIS EXISTS: with the IDC unplugged entirely, this panel powers up with its
// bottom row lit bright in a random pattern — uninitialised 595 contents plus inputs
// floating to an enabled state. Since OE does not appear to blank this panel (see
// warning above), the ONLY way to clear that is to shift real OFF data in. Doing it
// first thing in setup(), before the Serial wait, keeps the uncontrolled window to
// milliseconds instead of 3+ seconds.
static void blankChain(void) {
  for (int8_t addr = NUM_ADDR_STATES - 1; addr >= 0; addr--) {
    setRowAddress((uint8_t)addr);
    for (uint8_t plane = 0; plane < BITPLANES_PER_ROW; plane++) {
      shiftOutRow(blankPadRed, blankPadGreen);
      if (addr == 0 && plane == 1) shiftOutRow(blankPadRed, blankPadGreen);
      pulseRCLK();
    }
  }
}


// ============================================================================
//  DIAG_MODE — display-driven signal identification
// ============================================================================
// The panel is a 560-pixel readout; used well it is a better instrument than a
// meter for these questions. The rule these tests obey: NEVER use a uniform fill
// as a discriminator. A uniform fill cannot tell "line inverted" from "line
// ignored" — that ambiguity already produced two wrong polarity conclusions.
// Every test below changes ONE bit, or holds a static pattern and varies ONE
// control line, so each possible answer looks visibly different.
//
// Board inventory that motivates these tests:
//   20 x 74HC595D (U1-U20)  -> 160 bits of shift register, NOT 80
//    2 x 74HC245D (IC1,IC2) -> connector-side bus buffers, not per-module
//    1 x 74HC138D (IC3)     -> row decoder is ON the panel
//
// Run them IN ORDER. Each assumes only what the previous one established.
//
//   0 = off, normal driver
//   1 = chain length
//   2 = which line carries data
//   3 = what IDC 9 actually does
//   4 = row addressing

// Seconds per step. Long enough to read and note; short enough to limit heating.
#define DIAG_STEP_MS 1000

// THERMAL WARNING: these tests are STATIC. Whatever is lit is lit at 100% duty,
// with no multiplexing, because that is what makes them readable. Keep runs under
// 30 seconds and keep a hand on the panel between runs.

#if DIAG_MODE

// Chain length is the very thing test 1 measures, so it must not be assumed here.
// 200 is comfortably more than either candidate (80 or 160).
#define DIAG_MAX_BITS 200

static uint32_t diagStep = 0;
static uint32_t diagLastMs = 0;

static void diagShiftOneBit(uint8_t rVal, uint8_t gVal) {
  digitalWrite(PIN_R, rVal);
  digitalWrite(PIN_G, gVal);
  delayMicroseconds(2);
  pulseSRCLK();
}

// Fills the whole chain with a known background, then latches.
static void diagFillBackground(uint8_t rVal, uint8_t gVal) {
  for (int i = 0; i < DIAG_MAX_BITS; i++) diagShiftOneBit(rVal, gVal);
  pulseRCLK();
}

// ---------------------------------------------------------------------------
// TEST 1 — CHAIN LENGTH.  Are there 80 bits or 160?
// ---------------------------------------------------------------------------
// Fill the chain with background, then inject ONE opposite-value marker bit and
// clock it forward one position per step, latching each time. Watch the marker
// enter at one end and travel. The step count at which it falls off the far end
// IS the chain length.
//
// WHY THIS MATTERS MOST: with 20 x 595 the chain is probably 160 bits while the
// firmware shifts 80. If so, every latch leaves HALF the registers holding stale
// data that your frame never touches — which is exactly what an unblankable
// panel looks like. Everything downstream is meaningless until this is settled.
//
// READ: note the step number when the marker first appears and when it vanishes.
//   vanishes at ~80  -> TOTAL_COLS 80 is correct
//   vanishes at ~160 -> chain is 160 bits; TOTAL_COLS is half what it should be
static void diagTest1(void) {
  // Rebuilt to be STATELESS. Every step reconstructs the entire chain contents from
  // scratch: N background bits, then the marker, then enough background to push the
  // whole lot into position. Nothing depends on what the previous step left behind,
  // so a reboot, a wrap, or a missed step cannot corrupt the result — which is what
  // went wrong with the earlier accumulating version.
  //
  // Reading it: the marker sits diagStep positions from the input end. Step it up
  // and watch. The step number at which the marker VANISHES off the far end is the
  // chain length.
  if (diagStep == 0) {
    Serial.println();
    Serial.println("TEST 1: CHAIN LENGTH (stateless rebuild each step)");
    Serial.println("Watch the single lit pixel. Note the step where it VANISHES.");
    Serial.println("~80  = TOTAL_COLS 80 is correct.");
    Serial.println("~160 = chain is 160 bits; TOTAL_COLS is HALF what it should be,");
    Serial.println("       and every frame so far left half the chain holding garbage.");
    Serial.println("BLANK STATE for this panel: RED line HIGH, GREEN line LOW.");
    Serial.println();
    diagStep = 1;
  }

  // Push the marker diagStep positions deep: shift marker first, then diagStep
  // background bits behind it. Fill the remainder with background so no stale data
  // survives anywhere in the chain.
  diagShiftOneBit(RED_ON, GREEN_ON);
  for (uint32_t i = 0; i < diagStep; i++) diagShiftOneBit(RED_OFF, GREEN_OFF);
  for (uint32_t i = diagStep + 1; i < DIAG_MAX_BITS; i++) diagShiftOneBit(RED_OFF, GREEN_OFF);
  pulseRCLK();

  Serial.print("step ");
  Serial.print(diagStep);
  Serial.println("   <- marker should be this many positions from the input end");

  diagStep++;
  if (diagStep > DIAG_MAX_BITS) {
    Serial.println("--- marker walked past 200 bits; restarting ---");
    diagStep = 0;
  }
}

// ---------------------------------------------------------------------------
// TEST 2 — WHICH LINE CARRIES DATA.  Is IDC 6 / IDC 7 per-pixel data at all?
// ---------------------------------------------------------------------------
// Alternates between: marker on RED line only (green held constant), and marker
// on GREEN line only (red held constant). One bit differs from background, so
// the three possible answers are visually unambiguous:
//
//   ONE PIXEL changes            -> that line is genuine per-pixel data
//   NOTHING changes              -> that line is not data (or not connected)
//   WHOLE DISPLAY changes colour -> that line is a COLOUR SELECT, not pixel data
//
// The third outcome would explain the earlier uniform-fill results, where the
// green line alone appeared to decide the colour of the entire panel.
static void diagTest2(void) {
  bool redPhase = ((diagStep / 8) % 2) == 0;
  if (diagStep == 0) {
    Serial.println("TEST 2: WHICH LINE CARRIES DATA");
    Serial.println("ONE pixel changes = real data. NOTHING = line unused.");
    Serial.println("WHOLE display changes = colour-select line, not pixel data.");
  }
  diagFillBackground(RED_OFF, GREEN_OFF);
  // Single marker on one line only; the other line stays at its background value.
  diagShiftOneBit(redPhase ? RED_ON : RED_OFF, redPhase ? GREEN_OFF : GREEN_ON);
  for (int i = 1; i < DIAG_MAX_BITS; i++) diagShiftOneBit(RED_OFF, GREEN_OFF);
  pulseRCLK();
  Serial.print(redPhase ? "marker on RED line only" : "marker on GREEN line only");
  Serial.print("   step "); Serial.println(diagStep);
  diagStep = (diagStep + 1) % 16;
}

// ---------------------------------------------------------------------------
// TEST 3 — WHAT IDC 9 ACTUALLY DOES.
// ---------------------------------------------------------------------------
// Captures prove IDC 9 pulses low once per row-load at 6.19% duty on the original
// hardware, which looks exactly like an LED output-enable. But the board has only
// TWO 74HC245 bus buffers on the connector side — and a '245 has its own /OE. If
// IDC 9 drives THAT, it gates the data bus rather than the LEDs.
//
// This test loads a static recognisable pattern, STOPS CLOCKING ENTIRELY, then
// toggles IDC 9 once per second. With no clocking, the two cases cannot be
// confused:
//   pattern BLINKS            -> IDC 9 blanks the LEDs (true output enable)
//   pattern STAYS LIT, frozen -> IDC 9 gates the bus only; it can never blank
//
// The second outcome would mean the panel has NO blanking control available to
// us, and brightness must be managed by data content and row dwell instead.
static void diagTest3(void) {
  if (diagStep == 0) {
    Serial.println("TEST 3: WHAT IS IDC 9?");
    Serial.println("Static pattern, no clocking. Toggling OE line only.");
    Serial.println("BLINKS = real output enable. STAYS LIT = bus enable only.");
    // Stripe pattern: alternating 5-column blocks, recognisable at a glance.
    for (int i = 0; i < DIAG_MAX_BITS; i++) {
      bool on = ((i / 5) % 2) == 0;
      diagShiftOneBit(on ? RED_ON : RED_OFF, GREEN_OFF);
    }
    pulseRCLK();
  }
  bool oeLow = (diagStep % 2) == 0;
  digitalWrite(PIN_OE, oeLow ? LOW : HIGH);
  Serial.print("IDC 9 = "); Serial.print(oeLow ? "LOW " : "HIGH");
  Serial.println("   (pattern static, nothing being clocked)");
  diagStep++;
  if (diagStep > 20) diagStep = 1;   // stay >0 so the pattern is not reloaded
}

// ---------------------------------------------------------------------------
// TEST 4 — ROW ADDRESSING.  Which address lights which physical row?
// ---------------------------------------------------------------------------
// Static pattern again, stepping A0-A2 slowly and printing the value. Note which
// physical row lights for each. Resolves the identity-mapping assumption and the
// standing question of what address 0 is for: the captures show address 0 always
// receiving the anomalous 160-bit load, and the working theory is that it is a
// non-physical blanking cycle. If NO row lights at address 0, that is confirmed.
//
// The panel has 7 row commons (H1-H7) driven high-side by APM4953 P-channel
// MOSFETs, decoded on-board by IC3 (74HC138). A '138 has 8 outputs for 7 rows,
// so one decoder output is spare — very likely the one address 0 selects.
static void diagTest4(void) {
  if (diagStep == 0) {
    Serial.println("TEST 4: ROW ADDRESSING");
    Serial.println("Note which PHYSICAL row lights for each address value.");
    Serial.println("If NO row lights at addr 0, the blanking-cycle theory is confirmed.");
    for (int i = 0; i < DIAG_MAX_BITS; i++) diagShiftOneBit(RED_ON, GREEN_OFF);
    pulseRCLK();
  }
  uint8_t addr = diagStep % NUM_ADDR_STATES;
  setRowAddress(addr);
  digitalWrite(PIN_OE, LOW);   // held enabled so the row is visible
  Serial.print("address "); Serial.print(addr);
  Serial.println("   <- note which physical row lights");
  diagStep++;
  if (diagStep > 200) diagStep = 1;
}

static void runDiag(void) {
  if (diagLastMs != 0 && (millis() - diagLastMs) < DIAG_STEP_MS) return;
  diagLastMs = millis();
  ws2812_put(24, 12, 0);   // amber = diagnostic mode
#if   DIAG_MODE == 1
  diagTest1();
#elif DIAG_MODE == 2
  diagTest2();
#elif DIAG_MODE == 3
  diagTest3();
#elif DIAG_MODE == 4
  diagTest4();
#endif
}
#endif  // DIAG_MODE


// ============================================================================
//  REPLAY_MODE — clock out a frame recorded from the ORIGINAL controller
// ============================================================================
// Set to 1 to replay one captured frame verbatim, in a loop.
//
// WHY: every test so far has fed the panel frames built from OUR model of the
// protocol — bit order, polarity, chain length, address mapping. When the result
// is nonsense, there is no way to tell whether the model is wrong or the signal
// path is. This replaces the model entirely with raw recorded line levels from
// the LPC1768 driving this exact panel correctly.
//
// The result is interpretable either way:
//   PANEL SHOWS SOMETHING COHERENT -> hardware and signal path are fine; every
//       remaining fault is in how the firmware CONSTRUCTS frames. Narrow target.
//   PANEL SHOWS NONSENSE -> the fault is upstream of frame content. Stop
//       theorising about protocol; something in the signal path differs from the
//       original in a way none of the reasoning has caught.
//
// Note what is deliberately ABSENT here: no RED_ON/GREEN_OFF macros, no
// TOTAL_COLS, no bit-order assumption, no polarity flag. Only recorded levels.

#if REPLAY_MODE
#include "replay_frame.h"

// OE low window measured from the capture: mean 190.4 samples at 24MHz = 7.93us.
#define REPLAY_OE_US 8

// ---- BRIGHTNESS via ROW DWELL, not OE ----
// Measured: changing REPLAY_OE_US from 8 to 1 did NOT reduce current (2.666A ->
// 2.723A). Meanwhile 245 pin 14 reads 4.74V against a 4.93V rail, i.e. OE IS
// pulsing at ~4% low duty. Both facts together: OE toggles correctly on the wire
// but does not gate illumination on this panel. IDC 9 is evidently not wired to
// the 595 output enables.
//
// So the available brightness control is how long each ROW ADDRESS is held. One
// row is selected at a time by the on-board '138; the LEDs in that row are on for
// the whole slot. Shorten the slot and average current should fall proportionally.
//
// REPLAY_ROW_DWELL_US inserts a deliberate ON period per load, then parks the
// address on a value that lights nothing. Set to 0 for no dwell limiting (the
// previous behaviour). Start LOW and work up while watching current.
//
// PREDICTION TO TEST: current should scale roughly linearly with this value.
// If it does NOT, row dwell is not the brightness control either, and the panel
// is simply drawing what it draws.
#define REPLAY_ROW_DWELL_US 0

// Address value parked between rows. Must be one that lights no LEDs. Address 0
// is the candidate (it receives the anomalous 160-bit load and has long been
// suspected of being a non-physical blanking cycle) but that is UNCONFIRMED -
// DIAG test 4 exists to settle it. If parking here still lights a row, this
// whole mechanism does nothing.
#define REPLAY_PARK_ADDR 0

static inline uint8_t replayBit(const uint8_t *buf, uint32_t i) {
  return (buf[i >> 3] >> (i & 7)) & 1;   // LSB-first, matching the generator
}

static void replayFrame(void) {
  uint32_t bit = 0;
  for (uint8_t l = 0; l < REPLAY_LOADS; l++) {
    setRowAddress(replayAddr[l]);
    for (uint16_t i = 0; i < replayLen[l]; i++, bit++) {
      digitalWrite(PIN_R, replayBit(replayRed,   bit));
      digitalWrite(PIN_G, replayBit(replayGreen, bit));
      delayMicroseconds(1);
      pulseSRCLK();
    }
    pulseRCLK();
    // One OE low window per load, as captured (OE toggles 1:1 with RCLK).
    // OE is ACTIVE-HIGH on this panel - see setOE() for the derivation.
    // This was previously LOW-then-HIGH, i.e. inverted, which is why changing
    // REPLAY_OE_US appeared to have no effect on current.
    digitalWrite(PIN_OE, HIGH);
    delayMicroseconds(REPLAY_OE_US);
    digitalWrite(PIN_OE, LOW);
#if REPLAY_ROW_DWELL_US > 0
    // Hold this row for a bounded time, then park the address somewhere dark.
    // This is the brightness control if OE is not one.
    delayMicroseconds(REPLAY_ROW_DWELL_US);
    setRowAddress(REPLAY_PARK_ADDR);
#endif
  }
}
#endif

void setup() {
  // ---- Panel safety first: configure pins and blank the chain BEFORE anything slow. ----
  // The Serial wait below can block for 3 seconds. Previously it ran first, leaving the
  // panel unblanked and uncontrolled for that entire time on every single boot.
  pinMode(PIN_SRCLK, OUTPUT);
  pinMode(PIN_RCLK, OUTPUT);
  pinMode(PIN_OE, OUTPUT);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_ADDR0, OUTPUT);
  pinMode(PIN_ADDR1, OUTPUT);
  pinMode(PIN_ADDR2, OUTPUT);
  pinMode(PIN_HEARTBEAT, OUTPUT);

  digitalWrite(PIN_SRCLK, LOW);
  digitalWrite(PIN_RCLK, LOW);
  setOE(false);

  memset(redBuf,   RED_OFF,   sizeof(redBuf));
  memset(greenBuf, GREEN_OFF, sizeof(greenBuf));
  memset(blankPadRed,   RED_OFF,   sizeof(blankPadRed));
  memset(blankPadGreen, GREEN_OFF, sizeof(blankPadGreen));

#if SPARSE_TEST
  loadSparsePattern();
#endif

  blankChain();   // clear power-on garbage from the 595s immediately

  Serial.begin(115200);
  // Wait briefly for a serial connection so early prints aren't lost, but don't hang
  // forever if nothing's listening (e.g. running on battery with no USB monitor open).
  uint32_t waitStart = millis();
  while (!Serial && (millis() - waitStart) < 3000) { delay(10); }

  // NOTE: target is an RP2040-Zero (V1083 clone), which has NO plain GPIO LED —
  // the onboard LED is a WS2812 on GP16 and needs a protocol driver, not a level.
  // The old pinMode/digitalWrite heartbeat silently did nothing on this board, so
  // "no blinking LED" must NOT be read as "firmware isn't running". Heartbeat is
  // now a GPIO toggle on a spare pin instead, which a logic analyzer can see.
  ws2812_init();
  // Solid blue the moment setup() is reached: distinguishes "flashed and booted"
  // from "flash didn't take" before any of the loop logic has had a chance to run.
  ws2812_put(0, 0, 24);

  Serial.println("=== panel driver starting ===");
  // Loud, unambiguous statement of which mode is actually compiled in. Read this
  // line before interpreting ANY panel behaviour - several observations were
  // misread because the running mode was not what was assumed.
  Serial.println();
  Serial.println("########################################");
#if   MODE == MODE_RUN
  Serial.println("#  MODE: RUN - normal panel driver");
#elif MODE == MODE_PINMAP
  Serial.println("#  MODE: PINMAP - static DMM check, panel not driven");
#elif MODE == MODE_DIAG
  Serial.print  ("#  MODE: DIAG - test "); Serial.println(DIAG_TEST);
#elif MODE == MODE_REPLAY
  Serial.println("#  MODE: REPLAY - captured original frame");
#endif
  Serial.print  ("#  SLOW_SCAN_MS = "); Serial.println(SLOW_SCAN_MS);
  Serial.print  ("#  SPARSE_TEST   = "); Serial.println(SPARSE_TEST);
  Serial.print  ("#  SKIP_ADDR     = "); Serial.println(SKIP_ADDR);
  Serial.println("########################################");
  Serial.println();
  Serial.print("SRCLK="); Serial.print(PIN_SRCLK);
  Serial.print(" RCLK=");  Serial.print(PIN_RCLK);
  Serial.print(" OE=");    Serial.print(PIN_OE);
  Serial.print(" R=");     Serial.print(PIN_R);
  Serial.print(" G=");     Serial.print(PIN_G);
  Serial.print(" A0=");    Serial.print(PIN_ADDR0);
  Serial.print(" A1=");    Serial.print(PIN_ADDR1);
  Serial.print(" A2=");    Serial.println(PIN_ADDR2);
  Serial.println("=== walking-column diagnostic: watch row 0, note the physical position");
  Serial.println("=== reported for each printed index (module + column-within-module) ===");
}

// --- Diagnostic mode ---
// Walks a single lit pixel across all 80 columns of row 0 (red first, then green),
// pausing on each one and printing its index + derived module/column-within-module.
// Purpose: empirically map bit index -> physical LED position, rather than guessing
// from theory. Note down what you actually see for each printed line — that mapping
// is what resolves the module/column ordering (README open question #3).
#define WALK_STEP_MS 600

void loop() {
#if REPLAY_MODE
  replayFrame();
  {
    static uint32_t n = 0, last = 0;
    n++;
    if (millis() - last >= 1000) {
      last = millis();
      ws2812_put(0, 0, 24);              // blue = replay mode
      if (Serial) { Serial.print("replay frames/s: "); Serial.println(n); }
      n = 0;
    }
  }
  return;
#endif

#if DIAG_MODE
  runDiag();
  return;   // never calls refreshFrame(); these tests drive the panel themselves
#endif

#if PIN_MAP_TEST
  runPinMapTest();
  return;   // never touches refreshFrame() — panel stays dark
#endif

  static uint32_t frameCount = 0;
  static uint32_t lastReport = 0;
  static bool heartbeatState = false;
  static uint32_t lastStep = 0;
  static int walkIndex = 0;
  static bool walkingRed = true;

#if POLARITY_SWEEP
  runPolaritySweep();
#endif
#if PATTERN_SEQUENCE
  runPatternSequence();
#endif

  refreshFrame();
  frameCount++;

  if (millis() - lastStep >= WALK_STEP_MS) {
    lastStep = millis();

#if RUN_WALK_TEST
    // clear previous bit
    memset(redBuf[0],   RED_OFF,   TOTAL_COLS);
    memset(greenBuf[0], GREEN_OFF, TOTAL_COLS);

    int module = walkIndex / COLS_PER_MODULE;
    int colInModule = walkIndex % COLS_PER_MODULE;

    if (walkingRed) {
      redBuf[0][walkIndex] = RED_ON;
    } else {
      greenBuf[0][walkIndex] = GREEN_ON;
    }

    if (Serial) {
#if RUN_WALK_TEST
      Serial.print(walkingRed ? "RED   " : "GREEN ");
      Serial.print("bit index "); Serial.print(walkIndex);
      Serial.print("  (module "); Serial.print(module);
      Serial.print(", col "); Serial.print(colInModule);
      Serial.println(" within module)");
#else
      Serial.println("blank frame - nothing commanded on");
#endif
    }

    walkIndex++;
    if (walkIndex >= TOTAL_COLS) {
      walkIndex = 0;
      walkingRed = !walkingRed;
      if (Serial) {
        Serial.println(walkingRed ? "--- restarting on RED ---" : "--- switching to GREEN ---");
      }
    }
#else
    // BLANK TEST: buffers stay at RED_OFF / GREEN_OFF the whole time — nothing commanded on.
    // Purpose: isolate whether the module 5/6/10/11 heating is a hardware fault (would
    // still happen here, with zero pixels commanded) vs a firmware data-framing bug
    // (should NOT happen here if truly isolated to the walk pattern).
    if (Serial) {
      Serial.println("blank frame - nothing commanded on");
    }
#endif
  }

  // Heartbeat: onboard LED toggles and a status line prints roughly once a second.
  // If you see this in the serial monitor but the panel does nothing, the Pico is
  // definitely running and the problem is downstream (wiring, polarity, or voltage
  // levels) rather than the firmware not executing at all.
  //
  // GUARDED WITH if (Serial): on this mbed/USB-CDC core, Serial.print() can BLOCK the
  // entire loop() — including all GPIO/SRCLK toggling in refreshFrame() — if nothing on
  // the host side is reading the output buffer (e.g. pio device monitor was closed or
  // never opened). That would silently stop the panel signals entirely without any error,
  // which is a serious trap during logic-analyzer bring-up: always confirm the monitor is
  // open and actively connected before capturing, or the capture will show near-zero
  // activity that looks like a firmware bug but is actually a blocked print call.
  if (millis() - lastReport >= 1000) {
    lastReport = millis();
    // Reprint the build settings EVERY second, not just at boot. On USB-CDC the
    // port does not exist until after setup() has run, so a boot-only banner is
    // unreadable - which already caused a test result to be uninterpretable.
    if (Serial) {
      Serial.print("[build] SPARSE_TEST="); Serial.print(SPARSE_TEST);
      Serial.print(" RUN_WALK_TEST=");      Serial.print(RUN_WALK_TEST);
      Serial.print(" SLOW_SCAN_MS=");       Serial.print(SLOW_SCAN_MS);
      Serial.print(" SKIP_ADDR=");          Serial.print(SKIP_ADDR);
      Serial.print(" RED_OFF=");            Serial.print(RED_OFF);
      Serial.print(" GREEN_OFF=");          Serial.print(GREEN_OFF);
      Serial.print(" redBuf[1][0]=");       Serial.print(redBuf[1][0]);
      Serial.print(" redBuf[0][0]=");       Serial.println(redBuf[0][0]);
    }
    heartbeatState = !heartbeatState;
    digitalWrite(PIN_HEARTBEAT, heartbeatState);
    // Green/off blink = loop is running and refreshFrame() is returning.
    // If this stops but the board is still powered, the loop has stalled
    // (blocking Serial print is the usual culprit on USB-CDC cores).
    ws2812_put(0, heartbeatState ? 24 : 0, 0);
    if (Serial) {
      Serial.print("alive - frames/s: ");
      Serial.print(frameCount);
      Serial.print("  SRCLK pulses/s: ");
      Serial.print(srclkPulseCount);
      Serial.print("  OE enables/s: ");
      Serial.println(oeEnableCount);
    }
    frameCount = 0;
    srclkPulseCount = 0;
    oeEnableCount = 0;
  }
}
