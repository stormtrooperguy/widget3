#pragma once

#include <stdint.h>

// ============================================================
// WiFi mode — uncomment exactly one
// ============================================================
#define WIFI_MODE_AP       // device creates its own hotspot
// #define WIFI_MODE_STA   // device joins an existing network

// AP mode credentials
#define WIFI_AP_SSID      "SimonSays"
#define WIFI_AP_PASSWORD  "escape123"

// STA mode credentials (only used when WIFI_MODE_STA is active)
#define WIFI_STA_SSID        "YourNetworkName"
#define WIFI_STA_PASSWORD    "YourNetworkPassword"
#define WIFI_STA_TIMEOUT_MS  15000   // ms to wait for connection before giving up

// Static IP for STA mode — comment out to use DHCP instead
// #define WIFI_STA_STATIC_IP
#define WIFI_STA_IP       "192.168.1.100"
#define WIFI_STA_GATEWAY  "192.168.1.1"
#define WIFI_STA_SUBNET   "255.255.255.0"
#define WIFI_STA_DNS1     "8.8.8.8"
#define WIFI_STA_DNS2     "8.8.4.4"

// ============================================================
// Hardware counts
// ============================================================
#define NUM_MODULES        4    // remote button/ring units
#define MODULE_NUM_LEDS   16    // LEDs per module ring
#define HUB_NUM_LEDS      15    // LEDs in the hub progress strip

// One prompt per hub LED — the strip is the round scoreboard.
#define PROMPTS_PER_ROUND HUB_NUM_LEDS

// ============================================================
// SPI bus (RC522 cartridge reader, hub-local — short hookup wire)
// ============================================================
#define SPI_SCK_PIN   18
#define SPI_MISO_PIN  19
#define SPI_MOSI_PIN  23

// Chip-select. RST is handled in software (UINT8_MAX) — no RST wire needed.
#define RC522_SS_PIN   5

// ============================================================
// Hub NeoPixel strip data pin (10-LED progress strip)
// ============================================================
#define HUB_DATA_PIN   4    // 300–500 Ω series resistor at this pin

// ============================================================
// Per-module pins
// ============================================================
// Each module gets one LED data line and one button line through its Cat6.
// Both ends of the cable use identical RJ45 pinouts (straight-through).
//
// LED data pins are normal output GPIOs.
// Buttons use INPUT_PULLUP and tie to GND when pressed — wired through
// the Cat6 to a momentary switch on the module.
//
// Indices below match the physical jack labelling on the hub face.
static const uint8_t MODULE_LED_PINS[NUM_MODULES] = { 32, 33, 25, 26 };
static const uint8_t MODULE_BTN_PINS[NUM_MODULES] = { 13, 14, 27, 22 };

// ============================================================
// Game timing
// ============================================================
// Three rounds, each shorter than the last. Index = round number.
#define NUM_ROUNDS              3
static const uint16_t PROMPT_TIMEOUTS_MS[NUM_ROUNDS] = { 3000, 2000, 1000 };

// Pause between consecutive prompts. Scales down per round so the
// final round feels noticeably more frantic — not just because the
// timeout is shorter, but because the dead air between prompts is too.
static const uint16_t INTER_PROMPT_PAUSE_MS[NUM_ROUNDS] = { 700, 500, 300 };

// Final-round phantom-press mechanic.
// In the final (fastest) round, each successful press has this percent
// chance of being silently rejected by the firmware: the module ring
// flashes cyan to acknowledge the press, but the scoreboard does NOT
// advance and a fresh prompt fires. Misses (timeouts) always count
// — otherwise the round could run indefinitely.
//   0  = disabled (every hit counts, like rounds 1 and 2)
//   30 = ~30% rejection → final round needs ~21 presses to fill 15 slots
//   50 = ~50% rejection → ~30 presses on average — extremely punishing
#define FINAL_ROUND_PHANTOM_PCT 30
#define PHANTOM_FLASH_MS        250   // ms the cyan acknowledgement holds

// ============================================================
// Animation timing
// ============================================================

// Startup flash — green strip flashes when cartridge is inserted
#define STARTUP_FLASH_COUNT       3
#define STARTUP_FLASH_INTERVAL_MS 250

// Round intro — blue sweep across the hub strip before each round begins
#define ROUND_INTRO_STEP_MS       60   // ms per LED (×HUB_NUM_LEDS = 600 ms total)

// Hit hold — module ring stays solid green for this long after a successful press
#define HIT_HOLD_MS               700

// Miss flash — module ring blinks red, then settles to steady red briefly
#define MISS_FLASH_COUNT          4
#define MISS_FLASH_INTERVAL_MS    150
#define MISS_HOLD_MS              400

// Game complete — hub strip celebrates on full success
#define COMPLETE_FLASH_COUNT      3
#define COMPLETE_FLASH_INTERVAL_MS 300

// Idle animation — late-70s mainframe panel flicker.
// In IDLE (no cartridge) each scoreboard cell independently picks a
// colour from a weighted set {amber, green, red, off} and holds it
// for a random interval in [IDLE_ANIM_MIN_MS, IDLE_ANIM_MAX_MS] before
// picking again. The asynchronous per-cell timing produces a continuous
// "thinking" shimmer without being frantic.
#define IDLE_ANIM_MIN_MS   250
#define IDLE_ANIM_MAX_MS  1000

// Idle module rings — slow breathe with occasional orbit excursions.
// While IDLE, each ring defaults to BREATHE: a dim white sine pulse
// between IDLE_BREATHE_MIN and IDLE_BREATHE_MAX brightness factors,
// period IDLE_BREATHE_PERIOD_MS. Rings are phase-offset from each
// other so they roll rather than pulse in sync.
//
// On each breath-cycle boundary, every ring independently rolls
// IDLE_ORBIT_TRIGGER_PCT % to switch into ORBIT mode for
// IDLE_ORBIT_CYCLES complete revolutions of a magenta comet, then
// returns to BREATHE. Probability is per-ring per-breath, so visual
// excursions are sparse and surprising rather than scheduled.
#define IDLE_BREATHE_PERIOD_MS   3000
#define IDLE_BREATHE_MIN         0.10f   // floor brightness factor
#define IDLE_BREATHE_MAX         0.50f   // peak brightness factor
#define IDLE_ORBIT_STEP_MS       80      // ms per pixel step (16 × 80 = 1280 ms / revolution)
#define IDLE_ORBIT_CYCLES        3       // complete revolutions per excursion
#define IDLE_ORBIT_TRIGGER_PCT   8       // % chance per breath cycle to switch to orbit
#define IDLE_ORBIT_COMET_LENGTH  5       // lit pixels in the trailing comet tail

// ============================================================
// LED brightness  (0–255)
// ============================================================
// At 150/255 brightness, four 16-LED rings + a 10-LED strip at full
// colour ≈ 1.4 A. Combined with ESP32 + RC522 well under a 3 A supply.
#define LED_BRIGHTNESS  150

// ============================================================
// Button debounce
// ============================================================
#define BUTTON_DEBOUNCE_MS  20

// ============================================================
// RFID polling and removal debounce
// ============================================================
#define RFID_POLL_INTERVAL_MS  40
#define REMOVAL_DEBOUNCE        3   // consecutive missed polls before removal

// ============================================================
// Wiring summary (print this out for assembly)
// ============================================================
//
//  Hub-internal:
//    RC522 SCK       ──► ESP32 GPIO 18
//    RC522 MISO      ──► ESP32 GPIO 19
//    RC522 MOSI      ──► ESP32 GPIO 23
//    RC522 SDA (SS)  ──► ESP32 GPIO  5
//    RC522 VCC       ──► ESP32 3.3 V (one reader, fits the onboard LDO)
//    RC522 GND       ──► Common GND
//    RC522 RST       ──► (leave unconnected — soft reset)
//    Hub strip DIN   ──► ESP32 GPIO 4  (300–500 Ω series resistor)
//    Hub strip +5 V  ──► 5 V terminal block
//    Hub strip GND   ──► Common GND
//
//  Each Cat6 cable to a module carries (4 conductors active, 4 spare GND):
//    LED Data        ──► hub GPIO 32 / 33 / 25 / 26
//    Button signal   ──► hub GPIO 13 / 14 / 27 / 22 (INPUT_PULLUP)
//    5 V             ──► 5 V terminal block
//    GND             ──► common GND
//
//  At each module:
//    LED Data        ──► NeoPixel ring DIN (300–500 Ω resistor recommended)
//    Button signal   ──► one terminal of the momentary switch
//    GND             ──► other terminal of the switch + ring GND
//    5 V             ──► ring +5 V
//
// ============================================================

// ============================================================
// Colours  (packed 0xRRGGBB)
// ============================================================
#define COLOR_OFF     0x000000UL
#define COLOR_AMBER   0xFF6000UL
#define COLOR_GREEN   0x00CC00UL
#define COLOR_RED     0xFF0000UL
#define COLOR_BLUE    0x0055FFUL
#define COLOR_CYAN    0x00DDFFUL    // phantom-press acknowledgement
#define COLOR_WHITE   0xFFFFFFUL    // idle-ring breathe base
#define COLOR_MAGENTA 0xFF00AAUL    // idle-ring orbit excursion
