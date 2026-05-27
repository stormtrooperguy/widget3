#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <esp_system.h>
#include <math.h>
#include "config.h"

// ============================================================
// State
// ============================================================

enum GameState : uint8_t {
    IDLE,                   // no cartridge — hub strip flickers, modules off
    STARTUP_FLASH,          // cartridge inserted — strip flashes green
    ROUND_INTRO,            // blue sweep across hub strip before each round
    PROMPT_ACTIVE,          // active module ring depletes amber, awaiting press
    PROMPT_RESULT_HIT,      // active module solid green for HIT_HOLD_MS
    PROMPT_RESULT_MISS,     // active module flashes red then settles red
    PROMPT_RESULT_PHANTOM,  // final-round only: press acknowledged but rejected
    INTER_PROMPT_PAUSE,     // brief pause between prompts (modules off, scoreboard up)
    COMPLETE_FLASH,         // hub strip flashes green after final round
    COMPLETE_STEADY         // hub strip solid blue, awaiting cartridge removal
};

enum PromptResult : uint8_t {
    RESULT_PENDING = 0,
    RESULT_HIT     = 1,
    RESULT_MISS    = 2
};

struct Button {
    bool          stable;        // debounced level (HIGH = released, LOW = pressed)
    bool          lastRaw;       // last raw read
    unsigned long lastChangeMs;  // when raw last changed
};

struct IdleCell {
    uint32_t      color;         // current colour (may be COLOR_OFF)
    unsigned long nextChangeMs;  // millis() at which this cell repaints
};

enum IdleRingMode : uint8_t {
    RING_BREATHE,
    RING_ORBIT
};

struct IdleRing {
    IdleRingMode  mode;            // current animation mode
    unsigned long modeStartMs;     // millis() when this mode began
    uint16_t      breathCycleSeen; // count of completed breath cycles, used to roll once per cycle
};

// ============================================================
// Globals
// ============================================================

MFRC522           reader(RC522_SS_PIN, UINT8_MAX);   // soft reset, no RST wire
Adafruit_NeoPixel hubStrip(HUB_NUM_LEDS, HUB_DATA_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel rings[NUM_MODULES];                // initialised in setup()
Button            buttons[NUM_MODULES];
AsyncWebServer    server(80);
AsyncEventSource  events("/events");

GameState     state            = IDLE;
unsigned long stateChangeTime  = 0;

bool          cardPresent      = false;
uint8_t       removalCount     = 0;
String        currentUID;

uint8_t       currentRound     = 0;     // 0..NUM_ROUNDS-1
uint8_t       currentPromptIdx = 0;     // 0..PROMPTS_PER_ROUND-1
int8_t        selectedModule   = -1;    // active module, -1 if none
int8_t        lastModule       = -1;    // for no-immediate-repeat constraint
PromptResult  roundResults[PROMPTS_PER_ROUND];
IdleCell      idleCells[HUB_NUM_LEDS];   // zero-init → all repaint on first IDLE frame
IdleRing      idleRings[NUM_MODULES];    // zero-init → all start in RING_BREATHE

// ============================================================
// Utility
// ============================================================

static String uidToHex(MFRC522::Uid &uid) {
    String s;
    for (byte i = 0; i < uid.size; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", uid.uidByte[i]);
        s += buf;
    }
    return s;
}

static const char *stateName(GameState s) {
    switch (s) {
        case IDLE:                  return "idle";
        case STARTUP_FLASH:         return "startup_flash";
        case ROUND_INTRO:           return "round_intro";
        case PROMPT_ACTIVE:         return "prompt_active";
        case PROMPT_RESULT_HIT:     return "prompt_hit";
        case PROMPT_RESULT_MISS:    return "prompt_miss";
        case PROMPT_RESULT_PHANTOM: return "prompt_phantom";
        case INTER_PROMPT_PAUSE:    return "inter_prompt";
        case COMPLETE_FLASH:        return "complete_flash";
        case COMPLETE_STEADY:       return "complete_steady";
    }
    return "?";
}

static const char *resultName(PromptResult r) {
    switch (r) {
        case RESULT_PENDING: return "pending";
        case RESULT_HIT:     return "hit";
        case RESULT_MISS:    return "miss";
    }
    return "?";
}

// ============================================================
// LED helpers
// ============================================================

static void hubFill(uint32_t color) {
    hubStrip.fill(color, 0, HUB_NUM_LEDS);
    hubStrip.show();
}

static void hubClear() {
    hubStrip.clear();
    hubStrip.show();
}

// Render the per-prompt scoreboard: cell N coloured by roundResults[N].
// Pending cells stay off.
static void hubShowScoreboard() {
    hubStrip.clear();
    for (uint8_t i = 0; i < PROMPTS_PER_ROUND; i++) {
        uint32_t c = COLOR_OFF;
        if (roundResults[i] == RESULT_HIT)  c = COLOR_GREEN;
        if (roundResults[i] == RESULT_MISS) c = COLOR_RED;
        hubStrip.setPixelColor(i, c);
    }
    hubStrip.show();
}

static void moduleFill(int i, uint32_t color) {
    rings[i].fill(color, 0, MODULE_NUM_LEDS);
    rings[i].show();
}

static void moduleClear(int i) {
    rings[i].clear();
    rings[i].show();
}

static void modulesAllOff() {
    for (int i = 0; i < NUM_MODULES; i++) moduleClear(i);
}

static void modulesAllFill(uint32_t color) {
    for (int i = 0; i < NUM_MODULES; i++) moduleFill(i, color);
}

// Weighted random colour for the idle "computer panel" flicker.
// Heavy on amber for the period-correct vibe; rare red, occasional green,
// generous off so the strip breathes instead of looking solid.
static uint32_t pickIdleColor() {
    uint8_t r = (uint8_t)random(100);
    if (r < 50) return COLOR_AMBER;
    if (r < 70) return COLOR_GREEN;
    if (r < 75) return COLOR_RED;
    return COLOR_OFF;
}

static void updateIdleAnimation(unsigned long now) {
    bool dirty = false;
    for (uint8_t i = 0; i < HUB_NUM_LEDS; i++) {
        if (now >= idleCells[i].nextChangeMs) {
            idleCells[i].color = pickIdleColor();
            unsigned long span = (unsigned long)IDLE_ANIM_MAX_MS - IDLE_ANIM_MIN_MS + 1;
            idleCells[i].nextChangeMs = now + IDLE_ANIM_MIN_MS + (unsigned long)random(span);
            dirty = true;
        }
    }
    if (dirty) {
        for (uint8_t i = 0; i < HUB_NUM_LEDS; i++) {
            hubStrip.setPixelColor(i, idleCells[i].color);
        }
        hubStrip.show();
    }
}

// Render module ring `i` filled with `baseColor` scaled by `scale` (0..1).
// Used for the breathe animation; cheap enough to call per loop iteration.
static void ringSolidScaled(int i, uint32_t baseColor, float scale) {
    if (scale < 0) scale = 0;
    if (scale > 1) scale = 1;
    uint8_t r = (uint8_t)(((baseColor >> 16) & 0xFF) * scale);
    uint8_t g = (uint8_t)(((baseColor >>  8) & 0xFF) * scale);
    uint8_t b = (uint8_t)(( baseColor        & 0xFF) * scale);
    rings[i].fill(rings[i].Color(r, g, b), 0, MODULE_NUM_LEDS);
    rings[i].show();
}

// Render module ring `i` as a comet of length `tailLen` at head position
// `headPos`, in `baseColor`. Each tail pixel is dimmed linearly.
static void ringComet(int i, int headPos, uint32_t baseColor, uint8_t tailLen) {
    rings[i].clear();
    uint8_t br = (baseColor >> 16) & 0xFF;
    uint8_t bg = (baseColor >>  8) & 0xFF;
    uint8_t bb =  baseColor        & 0xFF;
    for (uint8_t t = 0; t < tailLen; t++) {
        int p = (headPos - t + MODULE_NUM_LEDS) % MODULE_NUM_LEDS;
        float f = 1.0f - (float)t / tailLen;
        rings[i].setPixelColor(p, (uint8_t)(br * f), (uint8_t)(bg * f), (uint8_t)(bb * f));
    }
    rings[i].show();
}

static void updateIdleRings(unsigned long now) {
    const unsigned long orbitDurationMs =
        (unsigned long)IDLE_ORBIT_STEP_MS * MODULE_NUM_LEDS * IDLE_ORBIT_CYCLES;
    // Per-ring phase offset so the four rings don't pulse in lockstep.
    const unsigned long phaseStep = IDLE_BREATHE_PERIOD_MS / NUM_MODULES;

    for (int i = 0; i < NUM_MODULES; i++) {
        IdleRing &r = idleRings[i];

        if (r.mode == RING_BREATHE) {
            // Apparent time within this ring's offset breathe cycle
            unsigned long t = (now + (unsigned long)i * phaseStep) % IDLE_BREATHE_PERIOD_MS;
            float phase = (float)t / IDLE_BREATHE_PERIOD_MS;
            float s     = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * phase);
            float scale = IDLE_BREATHE_MIN + (IDLE_BREATHE_MAX - IDLE_BREATHE_MIN) * s;
            ringSolidScaled(i, COLOR_WHITE, scale);

            // Detect cycle boundaries — roll once per completed breath whether
            // to switch into ORBIT mode for a few revolutions.
            uint16_t cycle = (uint16_t)((now + (unsigned long)i * phaseStep) / IDLE_BREATHE_PERIOD_MS);
            if (cycle != r.breathCycleSeen) {
                r.breathCycleSeen = cycle;
                if ((long)random(100) < (long)IDLE_ORBIT_TRIGGER_PCT) {
                    r.mode        = RING_ORBIT;
                    r.modeStartMs = now;
                }
            }
        } else {  // RING_ORBIT
            unsigned long elapsed = now - r.modeStartMs;
            if (elapsed >= orbitDurationMs) {
                r.mode = RING_BREATHE;
                // Don't immediately re-roll — advance breathCycleSeen so the
                // next dice roll waits for the next genuine cycle boundary.
                r.breathCycleSeen = (uint16_t)((now + (unsigned long)i * phaseStep) / IDLE_BREATHE_PERIOD_MS);
            } else {
                int pos = (int)((elapsed / IDLE_ORBIT_STEP_MS) % MODULE_NUM_LEDS);
                ringComet(i, pos, COLOR_MAGENTA, IDLE_ORBIT_COMET_LENGTH);
            }
        }
    }
}

// Light the first `litCount` pixels of module `i` with `color`, rest off.
// Used to draw the depleting countdown wedge.
static void moduleDeplete(int i, uint32_t color, uint8_t litCount) {
    if (litCount > MODULE_NUM_LEDS) litCount = MODULE_NUM_LEDS;
    rings[i].clear();
    for (uint8_t p = 0; p < litCount; p++) {
        rings[i].setPixelColor(p, color);
    }
    rings[i].show();
}

// ============================================================
// Game state transitions
// ============================================================

static void enterState(GameState s) {
    state = s;
    stateChangeTime = millis();
    Serial.printf("[%lu] state → %s  round=%u prompt=%u module=%d\n",
                  stateChangeTime, stateName(s),
                  currentRound + 1, currentPromptIdx + 1, selectedModule);
}

static void resetGameVars() {
    currentRound     = 0;
    currentPromptIdx = 0;
    selectedModule   = -1;
    lastModule       = -1;
    for (uint8_t i = 0; i < PROMPTS_PER_ROUND; i++) roundResults[i] = RESULT_PENDING;
    modulesAllOff();
}

static void selectNextModule() {
    int8_t pick;
    if (NUM_MODULES <= 1 || lastModule < 0) {
        pick = (int8_t)random(NUM_MODULES);
    } else {
        // Pick from the NUM_MODULES-1 modules that aren't `lastModule`
        int8_t r = (int8_t)random(NUM_MODULES - 1);
        pick = (r >= lastModule) ? (r + 1) : r;
    }
    selectedModule = pick;
}

static void startNextPrompt() {
    selectNextModule();
    enterState(PROMPT_ACTIVE);
}

// Called when one prompt's result hold completes — advances to the next
// prompt, the next round, or the completion state.
static void advanceAfterPrompt() {
    lastModule = selectedModule;
    selectedModule = -1;

    currentPromptIdx++;
    if (currentPromptIdx < PROMPTS_PER_ROUND) {
        enterState(INTER_PROMPT_PAUSE);
        return;
    }

    // Round done.
    currentRound++;
    currentPromptIdx = 0;
    for (uint8_t i = 0; i < PROMPTS_PER_ROUND; i++) roundResults[i] = RESULT_PENDING;
    lastModule = -1;   // fresh module pool for the new round

    if (currentRound < NUM_ROUNDS) {
        enterState(ROUND_INTRO);
    } else {
        enterState(COMPLETE_FLASH);
    }
}

// ============================================================
// RFID polling
// ============================================================

static void pollReader() {
    bool   detected = false;
    String uid;

    if (reader.PICC_IsNewCardPresent() && reader.PICC_ReadCardSerial()) {
        uid      = uidToHex(reader.uid);
        detected = true;
        reader.PICC_HaltA();
        reader.PCD_StopCrypto1();
    } else if (cardPresent) {
        // Halted card — wake with WUPA to confirm it's still seated
        byte atqa[2];
        byte sz = sizeof(atqa);
        if (reader.PICC_WakeupA(atqa, &sz) == MFRC522::STATUS_OK
                && reader.PICC_ReadCardSerial()) {
            uid      = uidToHex(reader.uid);
            detected = true;
            reader.PICC_HaltA();
            reader.PCD_StopCrypto1();
        }
    }

    if (detected) {
        removalCount = 0;
        if (!cardPresent) {
            // Cartridge inserted — start a new game
            cardPresent = true;
            currentUID  = uid;
            resetGameVars();
            enterState(STARTUP_FLASH);
        } else if (currentUID != uid) {
            // Cartridge swapped without a clean removal — restart
            currentUID = uid;
            resetGameVars();
            enterState(STARTUP_FLASH);
        }
    } else if (cardPresent) {
        removalCount++;
        if (removalCount >= REMOVAL_DEBOUNCE) {
            cardPresent  = false;
            currentUID   = "";
            removalCount = 0;
            resetGameVars();
            enterState(IDLE);
        }
    }
}

// ============================================================
// Button polling (per-module debounce)
// ============================================================

// Returns the index of any module whose button registered a fresh press
// this loop, or -1. Only one press per call — multiple simultaneous presses
// are caught on subsequent calls (good enough at 1 ms loop cadence).
static int8_t pollButtons() {
    unsigned long now = millis();
    int8_t pressed = -1;

    for (int i = 0; i < NUM_MODULES; i++) {
        bool raw = digitalRead(MODULE_BTN_PINS[i]);
        if (raw != buttons[i].lastRaw) {
            buttons[i].lastRaw      = raw;
            buttons[i].lastChangeMs = now;
        }
        if (now - buttons[i].lastChangeMs >= BUTTON_DEBOUNCE_MS
                && raw != buttons[i].stable) {
            buttons[i].stable = raw;
            // Falling edge = press (INPUT_PULLUP, button to GND)
            if (raw == LOW && pressed < 0) {
                pressed = (int8_t)i;
            }
        }
    }
    return pressed;
}

// ============================================================
// LED update — runs every loop iteration, drives all NeoPixels
// based on the current state and elapsed time within that state.
// ============================================================

static void updateLEDs() {
    unsigned long now     = millis();
    unsigned long elapsed = now - stateChangeTime;

    switch (state) {

        case IDLE:
            updateIdleAnimation(now);
            updateIdleRings(now);
            break;

        case STARTUP_FLASH: {
            // Three on/off cycles in green across hub strip + all module rings,
            // then transition to ROUND_INTRO with everything dark.
            uint32_t halfCycles = STARTUP_FLASH_COUNT * 2;
            uint32_t phase      = elapsed / STARTUP_FLASH_INTERVAL_MS;
            if (phase >= halfCycles) {
                hubClear();
                modulesAllOff();
                enterState(ROUND_INTRO);
            } else {
                uint32_t c = (phase % 2 == 0) ? COLOR_GREEN : COLOR_OFF;
                hubFill(c);
                modulesAllFill(c);
            }
            break;
        }

        case ROUND_INTRO: {
            // Blue sweep across the hub strip, one LED at a time
            uint32_t step      = elapsed / ROUND_INTRO_STEP_MS;
            uint32_t totalStep = HUB_NUM_LEDS;
            if (step >= totalStep) {
                hubClear();
                startNextPrompt();
            } else {
                hubStrip.clear();
                hubStrip.setPixelColor((int)step, COLOR_BLUE);
                hubStrip.show();
            }
            break;
        }

        case PROMPT_ACTIVE: {
            unsigned long timeoutMs = PROMPT_TIMEOUTS_MS[currentRound];

            // Timeout?
            if (elapsed >= timeoutMs) {
                roundResults[currentPromptIdx] = RESULT_MISS;
                enterState(PROMPT_RESULT_MISS);
                break;
            }

            // Draw scoreboard on hub strip (does not change during the prompt)
            hubShowScoreboard();

            // Depleting wedge: 16 lit at start, 0 at end. Use ceil so the
            // last pixel stays lit until the very end of the timeout.
            unsigned long remaining = timeoutMs - elapsed;
            uint8_t litCount = (uint8_t)((remaining * MODULE_NUM_LEDS + timeoutMs - 1) / timeoutMs);
            moduleDeplete(selectedModule, COLOR_AMBER, litCount);
            break;
        }

        case PROMPT_RESULT_HIT: {
            moduleFill(selectedModule, COLOR_GREEN);
            hubShowScoreboard();    // cell currentPromptIdx already marked HIT
            if (elapsed >= HIT_HOLD_MS) {
                moduleClear(selectedModule);
                advanceAfterPrompt();
            }
            break;
        }

        case PROMPT_RESULT_MISS: {
            unsigned long flashTotal = (unsigned long)MISS_FLASH_COUNT * 2 * MISS_FLASH_INTERVAL_MS;
            unsigned long total      = flashTotal + MISS_HOLD_MS;
            hubShowScoreboard();
            if (elapsed >= total) {
                moduleClear(selectedModule);
                advanceAfterPrompt();
            } else if (elapsed >= flashTotal) {
                moduleFill(selectedModule, COLOR_RED);
            } else {
                uint32_t half = elapsed / MISS_FLASH_INTERVAL_MS;
                moduleFill(selectedModule, (half % 2 == 0) ? COLOR_RED : COLOR_OFF);
            }
            break;
        }

        case PROMPT_RESULT_PHANTOM: {
            // Brief cyan acknowledgement, then re-enter the prompt loop
            // WITHOUT recording a result or advancing currentPromptIdx.
            moduleFill(selectedModule, COLOR_CYAN);
            hubShowScoreboard();    // unchanged — slot stays pending
            if (elapsed >= PHANTOM_FLASH_MS) {
                moduleClear(selectedModule);
                lastModule     = selectedModule;
                selectedModule = -1;
                enterState(INTER_PROMPT_PAUSE);
            }
            break;
        }

        case INTER_PROMPT_PAUSE: {
            hubShowScoreboard();
            if (elapsed >= INTER_PROMPT_PAUSE_MS[currentRound]) {
                startNextPrompt();
            }
            break;
        }

        case COMPLETE_FLASH: {
            // Victory flash — blue on/off across hub strip + module rings,
            // then settle to solid blue everywhere in COMPLETE_STEADY.
            uint32_t halfCycles = COMPLETE_FLASH_COUNT * 2;
            uint32_t phase      = elapsed / COMPLETE_FLASH_INTERVAL_MS;
            if (phase >= halfCycles) {
                enterState(COMPLETE_STEADY);
            } else {
                uint32_t c = (phase % 2 == 0) ? COLOR_BLUE : COLOR_OFF;
                hubFill(c);
                modulesAllFill(c);
            }
            break;
        }

        case COMPLETE_STEADY:
            hubFill(COLOR_BLUE);
            modulesAllFill(COLOR_BLUE);
            break;
    }
}

// ============================================================
// Web — status JSON
// ============================================================

static void buildStatusJson(String &out) {
    unsigned long now       = millis();
    unsigned long elapsed   = now - stateChangeTime;
    unsigned long timeoutMs = PROMPT_TIMEOUTS_MS[currentRound];
    long          remaining = (state == PROMPT_ACTIVE)
                                ? (long)timeoutMs - (long)elapsed
                                : 0;
    if (remaining < 0) remaining = 0;

    out  = "{\"state\":\"";
    out += stateName(state);
    out += "\",\"uid\":\"";
    out += currentUID;
    out += "\",\"round\":";
    out += (currentRound + 1);
    out += ",\"totalRounds\":";
    out += NUM_ROUNDS;
    out += ",\"prompt\":";
    out += (currentPromptIdx + 1);
    out += ",\"promptsPerRound\":";
    out += PROMPTS_PER_ROUND;
    out += ",\"selectedModule\":";
    out += selectedModule;
    out += ",\"timeoutMs\":";
    out += timeoutMs;
    out += ",\"remainingMs\":";
    out += remaining;
    out += ",\"results\":[";
    for (uint8_t i = 0; i < PROMPTS_PER_ROUND; i++) {
        if (i) out += ',';
        out += '"';
        out += resultName(roundResults[i]);
        out += '"';
    }
    out += "]}";
}

// ============================================================
// Web — HTML page
// ============================================================

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Simon Says</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d0d1a;color:#ccc;font-family:'Courier New',monospace;padding:24px 16px}
h1{text-align:center;letter-spacing:6px;font-size:1.3rem;color:#888;margin-bottom:24px}

#meta{text-align:center;color:#666;font-size:.78rem;letter-spacing:3px;margin-bottom:18px;min-height:1em}
#meta .uid{color:#7799cc;margin-left:14px}

#strip{display:flex;justify-content:center;gap:6px;margin-bottom:36px}
.cell{
  width:34px;height:34px;border-radius:6px;
  background:#1a1a2a;border:1px solid #252540;
  transition:background-color .25s,box-shadow .25s
}
.cell.green{background:#00cc00;box-shadow:0 0 12px #00cc0088}
.cell.red  {background:#ff0000;box-shadow:0 0 12px #ff000088}
.cell.amber{background:#ff6000;box-shadow:0 0 12px #ff600088}
.cell.blue {background:#0055ff;box-shadow:0 0 12px #0055ffaa}

#modules{display:flex;justify-content:center;flex-wrap:wrap;gap:36px;margin-bottom:24px}
.mod{text-align:center}
.mlabel{font-size:.74rem;color:#666;letter-spacing:2px;margin-bottom:6px}
.ring{
  width:96px;height:96px;border-radius:50%;
  background:#1a1a2a;
  border:14px solid #252525;
  margin:0 auto;
  transition:border-color .2s,box-shadow .25s
}
.ring.off    {border-color:#252525}
.ring.green  {border-color:#00cc00;box-shadow:0 0 22px #00cc0088}
.ring.red    {border-color:#ff0000;box-shadow:0 0 22px #ff000088;animation:flash .3s steps(2,jump-none) infinite}
.ring.solidred{border-color:#ff0000;box-shadow:0 0 22px #ff000088}
.ring.amber  {border-color:#ff6000;box-shadow:0 0 26px #ff6000aa}
.ring.cyan   {border-color:#00ddff;box-shadow:0 0 22px #00ddffaa}
.ring.blue   {border-color:#0055ff;box-shadow:0 0 24px #0055ffaa}
.ring.breathe{border-color:#888;animation:breathe 3s ease-in-out infinite}
@keyframes breathe{0%,100%{border-color:#333;box-shadow:0 0 6px #44444466}50%{border-color:#cccccc;box-shadow:0 0 22px #cccccccc}}
#m0.breathe{animation-delay:0s}
#m1.breathe{animation-delay:-.75s}
#m2.breathe{animation-delay:-1.5s}
#m3.breathe{animation-delay:-2.25s}
@keyframes flash{0%,49%{opacity:1}50%,100%{opacity:.15}}
.countdown{font-size:.72rem;color:#ffaa66;letter-spacing:2px;margin-top:6px;min-height:1em}
</style>
</head>
<body>

<h1>&#x25B6; SIMON SAYS</h1>

<div id="meta">
  <span id="round">ROUND &mdash;</span>
  <span id="prompt"></span>
  <span class="uid" id="uid"></span>
</div>

<div id="strip"></div>

<div id="modules">
  <div class="mod"><div class="ring off" id="m0"></div><div class="mlabel">MODULE 1</div><div class="countdown" id="t0"></div></div>
  <div class="mod"><div class="ring off" id="m1"></div><div class="mlabel">MODULE 2</div><div class="countdown" id="t1"></div></div>
  <div class="mod"><div class="ring off" id="m2"></div><div class="mlabel">MODULE 3</div><div class="countdown" id="t2"></div></div>
  <div class="mod"><div class="ring off" id="m3"></div><div class="mlabel">MODULE 4</div><div class="countdown" id="t3"></div></div>
</div>

<script>
// Build the hub strip cells dynamically — count comes from the device.
const stripEl = document.getElementById('strip');

// Idle "computer panel" flicker — independent per-cell timers, run only
// while state==='idle'. Mirrors the firmware's pickIdleColor distribution.
let idleTimer = null;
const idleNext = [];
function pickIdleClass() {
  const r = Math.random() * 100;
  if (r < 50) return 'cell amber';
  if (r < 70) return 'cell green';
  if (r < 75) return 'cell red';
  return 'cell';
}
function startIdleAnim() {
  if (idleTimer) return;
  for (let i = 0; i < stripEl.children.length; i++) idleNext[i] = 0;
  idleTimer = setInterval(() => {
    const now = performance.now();
    for (let i = 0; i < stripEl.children.length; i++) {
      if (now >= idleNext[i]) {
        stripEl.children[i].className = pickIdleClass();
        idleNext[i] = now + 250 + Math.random() * 750;
      }
    }
  }, 60);
}
function stopIdleAnim() {
  if (idleTimer) { clearInterval(idleTimer); idleTimer = null; }
}

function setStrip(state, results) {
  if (stripEl.children.length !== results.length) {
    stripEl.innerHTML = '';
    for (let i = 0; i < results.length; i++) {
      const d = document.createElement('div');
      d.className = 'cell';
      stripEl.appendChild(d);
    }
  }
  if (state === 'idle') {
    startIdleAnim();
    return;
  }
  stopIdleAnim();
  for (let i = 0; i < results.length; i++) {
    const c = stripEl.children[i];
    if (state === 'complete_steady' || state === 'complete_flash') {
      c.className = 'cell blue';
    } else if (state === 'startup_flash') {
      c.className = 'cell green';
    } else if (state === 'round_intro') {
      c.className = 'cell';   // sweep not mirrored — strip empty between
    } else {
      c.className = 'cell' + (results[i] === 'hit' ? ' green'
                            : results[i] === 'miss' ? ' red' : '');
    }
  }
}

function setModules(state, selected, remainingMs, timeoutMs) {
  // Several states paint all four rings the same colour — handle those first.
  let allRingsClass = null;
  if (state === 'idle')                                              allRingsClass = 'ring breathe';
  else if (state === 'startup_flash')                                allRingsClass = 'ring green';
  else if (state === 'complete_flash' || state === 'complete_steady') allRingsClass = 'ring blue';

  for (let i = 0; i < 4; i++) {
    const r = document.getElementById('m' + i);
    const t = document.getElementById('t' + i);
    let cls = 'ring off';
    let countdown = '';
    if (allRingsClass) {
      cls = allRingsClass;
    } else if (i === selected) {
      if (state === 'prompt_active') {
        cls = 'ring amber';
        countdown = (remainingMs / 1000).toFixed(1) + ' s';
      } else if (state === 'prompt_hit')     cls = 'ring green';
      else if  (state === 'prompt_miss')    cls = 'ring red';
      else if  (state === 'prompt_phantom') cls = 'ring cyan';
    }
    r.className = cls;
    t.textContent = countdown;
  }
}

const src = new EventSource('/events');
src.addEventListener('message', e => {
  const d = JSON.parse(e.data);
  document.getElementById('round').textContent =
    'ROUND ' + d.round + '/' + d.totalRounds;
  document.getElementById('prompt').textContent =
    (d.state === 'idle' || d.state === 'complete_steady')
      ? '' : ' · PROMPT ' + d.prompt + '/' + d.promptsPerRound;
  document.getElementById('uid').textContent = d.uid ? 'UID ' + d.uid : '';
  setStrip(d.state, d.results);
  setModules(d.state, d.selectedModule, d.remainingMs, d.timeoutMs);
});
</script>
</body>
</html>
)rawliteral";

// ============================================================
// Web server setup
// ============================================================

static void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });

    events.onConnect([](AsyncEventSourceClient *client) {
        String json;
        buildStatusJson(json);
        client->send(json.c_str(), "message", millis());
    });
    server.addHandler(&events);

    server.begin();
    Serial.println("Web server started.");
}

// ============================================================
// Setup
// ============================================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== Simon Says booting ===");

    // Hub strip
    hubStrip.begin();
    hubStrip.setBrightness(LED_BRIGHTNESS);
    hubStrip.clear();
    hubStrip.show();

    // Module rings
    for (int i = 0; i < NUM_MODULES; i++) {
        rings[i] = Adafruit_NeoPixel(MODULE_NUM_LEDS, MODULE_LED_PINS[i],
                                     NEO_GRB + NEO_KHZ800);
        rings[i].begin();
        rings[i].setBrightness(LED_BRIGHTNESS);
        rings[i].clear();
        rings[i].show();
    }

    // Buttons
    for (int i = 0; i < NUM_MODULES; i++) {
        pinMode(MODULE_BTN_PINS[i], INPUT_PULLUP);
        buttons[i].stable       = HIGH;
        buttons[i].lastRaw      = HIGH;
        buttons[i].lastChangeMs = 0;
    }

    // SPI + RC522
    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    reader.PCD_Init();
    delay(50);
    Serial.print("RC522: ");
    reader.PCD_DumpVersionToSerial();

    // Random seed from the ESP32 hardware RNG so module choices differ
    // between power cycles.
    randomSeed(esp_random());

    // WiFi — mode selected in config.h
#if defined(USE_WIFI_AP)
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    Serial.printf("AP mode  SSID=%s  IP=%s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

#elif defined(USE_WIFI_STA)
    WiFi.mode(WIFI_STA);
#if defined(WIFI_STA_STATIC_IP)
    {
        IPAddress ip, gw, sn, dns1, dns2;
        ip.fromString(WIFI_STA_IP);
        gw.fromString(WIFI_STA_GATEWAY);
        sn.fromString(WIFI_STA_SUBNET);
        dns1.fromString(WIFI_STA_DNS1);
        dns2.fromString(WIFI_STA_DNS2);
        if (!WiFi.config(ip, gw, sn, dns1, dns2)) {
            Serial.println("Static IP config failed — will use DHCP");
        }
    }
#endif
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);
    Serial.printf("STA mode  connecting to %s", WIFI_STA_SSID);
    unsigned long staStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - staStart > WIFI_STA_TIMEOUT_MS) {
            Serial.println("\nSTA connect timed out — falling back to AP mode");
            WiFi.mode(WIFI_AP);
            WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
            Serial.printf("AP fallback  IP=%s\n", WiFi.softAPIP().toString().c_str());
            break;
        }
        delay(250);
        Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nConnected  IP=%s\n", WiFi.localIP().toString().c_str());
    }

#else
#error "Define either USE_WIFI_AP or USE_WIFI_STA in config.h"
#endif

    setupWebServer();

    // Enter IDLE — first updateLEDs() call will paint the idle flicker
    enterState(IDLE);

    Serial.println("=== Boot complete ===\n");
}

// ============================================================
// Loop
// ============================================================

void loop() {
    unsigned long now = millis();

    static unsigned long lastPollTime = 0;
    if (now - lastPollTime >= RFID_POLL_INTERVAL_MS) {
        lastPollTime = now;
        pollReader();
    }

    // Buttons every loop iteration — debounce time runs on real ms anyway
    if (state == PROMPT_ACTIVE) {
        int8_t pressed = pollButtons();
        if (pressed == selectedModule) {
            // In the final round, a configurable percentage of correct
            // presses are silently rejected — see PROMPT_RESULT_PHANTOM
            // and FINAL_ROUND_PHANTOM_PCT.
            bool isFinalRound = (currentRound == NUM_ROUNDS - 1);
            bool phantom = isFinalRound
                           && FINAL_ROUND_PHANTOM_PCT > 0
                           && (long)random(100) < (long)FINAL_ROUND_PHANTOM_PCT;
            if (phantom) {
                enterState(PROMPT_RESULT_PHANTOM);
            } else {
                roundResults[currentPromptIdx] = RESULT_HIT;
                enterState(PROMPT_RESULT_HIT);
            }
        }
        // Wrong-module presses: silently ignored. The active module's ring
        // already tells players where to focus; punishing wrong presses
        // would discourage the cooperative pointing/coaching that makes
        // the prop fun in groups.
    } else {
        // Still drain raw reads so debounce doesn't lag when we re-enter
        // PROMPT_ACTIVE — but discard any edges.
        (void)pollButtons();
    }

    updateLEDs();

#if defined(USE_WIFI_STA)
    static unsigned long lastReconnectCheck = 0;
    if (now - lastReconnectCheck >= 5000) {
        lastReconnectCheck = now;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi lost — reconnecting...");
            WiFi.reconnect();
        }
    }
#endif

    // SSE heartbeat — 100 ms so the depleting ring in the web mirror
    // animates smoothly rather than ticking in coarse chunks.
    static unsigned long lastSseTime = 0;
    if (now - lastSseTime >= 100) {
        lastSseTime = now;
        String json;
        buildStatusJson(json);
        events.send(json.c_str(), "message", now);
    }
}
