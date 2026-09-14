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
#define MODE MODE_REPLAY

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
#define FORCE_OE_DISABLED true   // SAFE DEFAULT after the pin-mapping fix:
                                 // keep the panel blanked until the corrected mapping
                                 // has been verified on a logic analyzer.

static inline void setOE(bool lit) {
  // OE is active-low per capture data: LOW = panel driving/lit, HIGH = blanked.
  if (FORCE_OE_DISABLED) {
    digitalWrite(PIN_OE, HIGH); // always blanked
    return;
  }
  digitalWrite(PIN_OE, lit ? LOW : HIGH);
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
  for (int col = TOTAL_COLS - 1; col >= 0; col--) {
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

static void refreshFrame() {
  for (int8_t addr = NUM_ADDR_STATES - 1; addr >= 0; addr--) {
    setOE(false); // blank while shifting + latching, matches captured OE-high-during-load

    uint8_t rowIndex = (addr >= 1 && addr <= NUM_ROWS) ? (addr - 1) : 0;

    for (uint8_t plane = 0; plane < BITPLANES_PER_ROW; plane++) {
      // TODO: both planes currently shift the SAME data. Once the brightness/BCM meaning
      // of the 2 planes is confirmed (README open question #5), differentiate them here —
      // e.g. plane 0 = "on at all" mask, plane 1 = "bright" mask, or similar weighting.
      shiftOutRow(redBuf[rowIndex], greenBuf[rowIndex]);
      if (addr == 0 && plane == 1) {
        shiftOutRow(blankPadRed, blankPadGreen); // extra 80 bits, matching captured 160-bit load
      }
      pulseRCLK();
    }

    setRowAddress((uint8_t)addr);
    setOE(true);
    delayMicroseconds(ROW_ON_TIME_US);
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
static void blankChain() {
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
    digitalWrite(PIN_OE, LOW);
    delayMicroseconds(REPLAY_OE_US);
    digitalWrite(PIN_OE, HIGH);
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
