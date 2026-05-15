# Simon Says — ESP32 Cooperative Reaction Prop

An ESP32-based escape room prop modelled on the Simon game. Four remote modules (each with a 16-LED ring and a button) are wired by Cat6 to a central hub. Players insert a cartridge to start: the hub picks a module at random, the chosen module's ring lights amber and depletes as a countdown, and a player must press that module's button before time runs out. One prompt per LED on the hub strip (15 by default), three rounds, faster each time. The hub strip doubles as the round scoreboard.

There is no fail-state — every miss simply lights a red cell on the scoreboard. The tension is "can the team be ready at every station, every round?"

In the **final round**, a configurable percentage of correct presses are silently rejected by the firmware (acknowledged with a brief cyan flash) — the scoreboard doesn't advance and a fresh prompt fires. This stretches the fastest, most frantic round and gives the team a "why isn't this working?!" moment to push through.

---

## Hardware

### Required

| Component | Qty | Notes |
|---|---|---|
| ESP32 dev board | 1 | NodeMCU-32S / ESP-WROOM-32 (38-pin variant recommended for breadboarding) |
| RC522 RFID reader | 1 | 13.56 MHz, SPI interface — lives inside the hub enclosure |
| NeoPixel strip | 1 | WS2812B, 5 V — hub progress strip; **length must equal `HUB_NUM_LEDS` (15 by default)** |
| NeoPixel ring (16 LED) | 4 | WS2812B, 5 V — one per remote module |
| Momentary push button | 4 | Normally-open, one per remote module — arcade button or similar |
| RJ45 keystone jack | 8 | 4 mounted on the hub face, 1 inside each module enclosure |
| Cat6 / Cat6e patch cable | 4 | RJ45 plug both ends, straight-through; length per install (see [cable length notes](#cable-length-notes)) |
| 5 V / 3 A DC adapter | 1 | Barrel jack or screw terminal — sized for 4 rings + strip + ESP32 |
| Terminal block | 1 | Distributes 5 V and GND inside the hub |
| NFC stickers / tags | 1+ | NTAG or MIFARE — any cartridge is accepted as the start key |
| 300–500 Ω resistor | 5 | One in series with each NeoPixel data line: 1 at the ESP32 pin (hub strip), 1 at each module's ring DIN |
| Hookup wire | — | 22–24 AWG for hub-internal connections (ESP32 ↔ RC522, terminal block, RJ45 jacks, hub strip) |

### Optional but recommended

| Component | Qty | Notes |
|---|---|---|
| 1000 µF / 6.3 V electrolytic cap | 1 | Across the 5 V rail at the terminal block — smooths NeoPixel current spikes |
| 74AHCT125 level shifter | 1 | For module cable runs ≳ 5 m — buffers ESP32's 3.3 V data line up to 5 V before it enters the cable. See [cable length notes](#cable-length-notes). |
| ESP32 enclosure / hub box | 1 | Houses ESP32, RC522, terminal block, hub strip, and the 4 RJ45 jacks |
| Module enclosure | 4 | Houses one ring + button + RJ45 jack each |

---

## Wiring

The hub holds the ESP32, RC522 reader, and the progress strip in a single enclosure, so those connections are short hookup wire. Each remote module connects to the hub via a single Cat6 patch cable.

### Topology

```
                       Hub enclosure
   ┌──────────────────────────────────────────────────┐
   │                                                  │
   │   ESP32 ── SPI ────── RC522 (cartridge reader)   │
   │     │                                            │
   │     ├─ GPIO 4 ─────── Hub progress strip         │
   │     │                  (15 × WS2812B)            │
   │     │                                            │
   │     ├─ GPIO 32 + 13 ── RJ45 jack #0 ──┐          │
   │     ├─ GPIO 33 + 14 ── RJ45 jack #1 ──┤          │
   │     ├─ GPIO 25 + 27 ── RJ45 jack #2 ──┤          │
   │     └─ GPIO 26 + 22 ── RJ45 jack #3 ──┤          │
   │                                       │          │
   │   5 V / 3 A ── terminal block ────────┤          │
   │                                       │          │
   └───────────────────────────────────────┼──────────┘
                                           │
                          4 × Cat6 patch cable (one per module)
                                           │
                  ┌────────────────────────┼─────────┐
                  │  Module N              │         │
                  │   RJ45 jack ───────────┘         │
                  │    │                             │
                  │    ├──► 16-LED NeoPixel ring     │
                  │    └──► Momentary button         │
                  └──────────────────────────────────┘

                  (Modules 1–4 are physically identical;
                   identity is determined by which hub
                   jack the cable is plugged into.)
```

### Power distribution

```
DC Adapter (5 V / 3 A)
  └── Terminal block
        ├── 5 V ──► Hub strip +5 V
        ├── 5 V ──► ESP32 VIN  (or feed the ESP32 separately via USB)
        ├── 5 V ──► pin 4 of each RJ45 jack (delivered to each module ring)
        └── GND ──► Common ground (ESP32, RC522, hub strip, all module GND returns)
```

The RC522 runs from the ESP32's onboard 3.3 V regulator — one reader is well within the LDO's headroom.

Do **not** feed 5 V directly into the ESP32's 3.3 V pin.

**Common ground is mandatory.** All NeoPixel GND, RC522 GND, ESP32 GND, button-return GND, and the DC adapter's negative terminal must meet at the same point (the terminal block). NeoPixel data lines do not work without a shared reference.

#### Current budget

At `LED_BRIGHTNESS = 150` (~60%) all colours are bounded:

| Source | Worst-case current |
|---|---|
| Hub progress strip (15 LEDs) | ~300 mA |
| 4 × module rings (16 LEDs each) | ~1.3 A |
| ESP32 + WiFi | ~250 mA |
| RC522 reader | ~30 mA |
| **Total** | **~1.9 A** |

A 3 A supply gives ~1 A of headroom. If you raise `LED_BRIGHTNESS` to 255 plan on a 5 A supply.

---

### Hub-internal signal wiring

```
RC522
  SCK   ── ESP32 GPIO 18
  MISO  ── ESP32 GPIO 19
  MOSI  ── ESP32 GPIO 23
  SDA   ── ESP32 GPIO  5    (chip-select)
  VCC   ── ESP32 3.3 V
  GND   ── Common GND
  RST   ── (leave unconnected — soft reset over SPI)

Hub progress strip (length = HUB_NUM_LEDS, default 15)
  +5 V  ── 5 V terminal block
  GND   ── Common GND
  DIN   ── ESP32 GPIO  4    (300–500 Ω series resistor at the ESP32 pin)
```

---

### Module cabling — Cat6 over RJ45

Each remote module needs four signals from the hub: 5 V, GND, NeoPixel data, and a button line back to the hub. Cat6 has four twisted pairs, so each active signal gets a dedicated GND return on the same twisted pair for noise immunity. There is no shared bus and no per-module addressing — modules are plug-interchangeable, and a module's identity is whatever jack it is plugged into on the hub.

#### RJ45 pinout per cable (568B pair assignment)

Both ends of each cable use **identical pinouts** — straight-through, not crossover.

| RJ45 pin | Wire colour (568B) | Signal | Direction | Pair |
|:---:|---|---|---|---|
| 1 | White/Orange | LED Data | hub → module | Pair 2 with pin 2 |
| 2 | Orange | GND (LED return) | shared return | Pair 2 with pin 1 |
| 3 | White/Green | Button signal | module → hub | Pair 3 with pin 6 |
| 4 | Blue | 5 V | hub → module | Pair 1 with pin 5 |
| 5 | White/Blue | GND (power return) | shared return | Pair 1 with pin 4 |
| 6 | Green | GND (button return) | shared return | Pair 3 with pin 3 |
| 7 | White/Brown | (spare — tie to GND) | — | Pair 4 with pin 8 |
| 8 | Brown | (spare — tie to GND) | — | Pair 4 with pin 7 |

The fourth twisted pair is unused — tying it to GND at both ends adds a little extra return capacity at no cost.

#### Hub end — RJ45 jacks to ESP32

Five pins are bused across all four jacks (5 V, both shared GND returns, button-return GND); two pins (LED data and button signal) run individually to a unique ESP32 GPIO per jack.

```
5 V terminal block ──► pin 4 of each RJ45 jack
GND terminal block ──► pin 2, pin 5, pin 6 of each RJ45 jack
                       (and pins 7/8 if tied to GND as recommended)

Pin 1, jack #0 (LED Data) ──► ESP32 GPIO 32  (Module 1)
Pin 1, jack #1 (LED Data) ──► ESP32 GPIO 33  (Module 2)
Pin 1, jack #2 (LED Data) ──► ESP32 GPIO 25  (Module 3)
Pin 1, jack #3 (LED Data) ──► ESP32 GPIO 26  (Module 4)

Pin 3, jack #0 (Button)   ──► ESP32 GPIO 13  (Module 1, INPUT_PULLUP)
Pin 3, jack #1 (Button)   ──► ESP32 GPIO 14  (Module 2, INPUT_PULLUP)
Pin 3, jack #2 (Button)   ──► ESP32 GPIO 27  (Module 3, INPUT_PULLUP)
Pin 3, jack #3 (Button)   ──► ESP32 GPIO 22  (Module 4, INPUT_PULLUP)
```

Buttons use the ESP32's internal pull-ups, so the button line in the cable rests at ~3.3 V and is pulled to GND when the switch closes.

#### Module end — RJ45 jack to ring + button

```
RJ45 pin 4 (5 V)        ──► NeoPixel ring +5 V
RJ45 pin 2 (GND)        ──► NeoPixel ring GND
RJ45 pin 1 (LED Data)   ──► NeoPixel ring DIN  (300–500 Ω series resistor)
RJ45 pin 3 (Button sig) ──► one terminal of the momentary switch
RJ45 pin 6 (Button GND) ──► other terminal of the switch
RJ45 pin 5 (GND)        ──► (already at module GND via the jack)
```

<a id="cable-length-notes"></a>
#### Cable length notes

The signals carried over the cable are LED data (800 kHz WS2812 protocol), a static button line, and 5 V power. None of these are SPI, so the long-run constraints differ from widget1.

| Run length | 5 V drop @ 320 mA / 24 AWG | LED data risk |
|---|---|---|
| 1 m | ~54 mV | none |
| 3 m | ~160 mV | none |
| 5 m | ~270 mV | low — generally clean |
| 8 m | ~430 mV (delivered ≈ 4.57 V — marginal for WS2812B) | borderline; consider local 5 V injection |

WS2812B requires a HIGH data input of at least ~3.5 V (≈ 0.7 × VDD). The ESP32 outputs 3.3 V, which is at or just below this threshold. With short runs (under ~1 m) it works reliably; over Cat6 the line capacitance and the slight droop after the series resistor can push it into the unreliable zone. If you see flicker on a long run, add a 74AHCT125 level shifter at the hub end of the data line, or fit a "sacrificial" first WS2812 right at the ESP32 to clean up the signal before it enters the cable.

The button line is essentially immune to noise problems at any practical run length — it's a slow DC level with software debounce.

---

### Full GPIO summary

| GPIO | Function |
|---:|---|
| 18 | SPI SCK (RC522) |
| 19 | SPI MISO (RC522) |
| 23 | SPI MOSI (RC522) |
| 5 | RC522 chip-select |
| 4 | Hub progress strip data |
| 32 | Module 1 ring data |
| 33 | Module 2 ring data |
| 25 | Module 3 ring data |
| 26 | Module 4 ring data |
| 13 | Module 1 button (INPUT_PULLUP) |
| 14 | Module 2 button (INPUT_PULLUP) |
| 27 | Module 3 button (INPUT_PULLUP) |
| 22 | Module 4 button (INPUT_PULLUP) |

All pins avoid the boot-strap-sensitive set (0, 2, 12, 15) and the SPI-flash range (6–11). Buttons are placed on regular IO pins rather than the input-only range (34–39) because those pins lack internal pull-ups — using normal IO saves a discrete pull-up resistor per cable.

---

## Firmware

Built with [PlatformIO](https://platformio.org/) targeting the `esp32dev` board.

### Dependencies (resolved automatically by PlatformIO)

| Library | Source | Purpose |
|---|---|---|
| `miguelbalboa/MFRC522` | PIO registry | RC522 driver |
| `adafruit/Adafruit NeoPixel` | PIO registry | LED ring + strip control |
| `me-no-dev/AsyncTCP` | GitHub (`https://github.com/me-no-dev/AsyncTCP.git`) | Async TCP layer used by the web server |
| `me-no-dev/ESPAsyncWebServer` | GitHub (`https://github.com/me-no-dev/ESPAsyncWebServer.git`) | Non-blocking web server + Server-Sent Events |

`AsyncTCP` and `ESPAsyncWebServer` are pulled directly from GitHub rather than the PlatformIO registry: the registry packages (`me-no-dev/AsyncTCP@^1.1.1`, `me-no-dev/ESP Async WebServer@^1.2.4`) no longer resolve, so `lib_deps` in `platformio.ini` uses the upstream Git URLs instead.

No `ArduinoJson` dependency — the SSE status payload is built directly with `String` concat, and the firmware has no inbound JSON endpoint to parse.

### Building and flashing

```bash
pio run --target upload
pio device monitor          # 115200 baud — prints IP address and reader version on boot
```

---

## Configuration

All user-tunable settings live in `src/config.h`. Edit this file before flashing.

### WiFi mode

Uncomment exactly one of:

```c
#define WIFI_MODE_AP    // ESP32 creates its own hotspot
// #define WIFI_MODE_STA // ESP32 joins an existing network
```

**AP mode** — connect your phone/laptop to the SSID defined by `WIFI_AP_SSID`. The web interface is at `http://192.168.4.1`.

**STA mode** — set `WIFI_STA_SSID` and `WIFI_STA_PASSWORD`. The assigned IP is printed to the serial monitor on boot. If the connection fails within `WIFI_STA_TIMEOUT_MS` (default 15 s) the firmware falls back to AP mode automatically. STA mode checks for disconnection every 5 s and reconnects automatically.

### Static IP (STA mode only)

Uncomment `WIFI_STA_STATIC_IP` and fill in the four address fields:

```c
#define WIFI_STA_STATIC_IP
#define WIFI_STA_IP       "192.168.1.100"
#define WIFI_STA_GATEWAY  "192.168.1.1"
#define WIFI_STA_SUBNET   "255.255.255.0"
#define WIFI_STA_DNS1     "8.8.8.8"
#define WIFI_STA_DNS2     "8.8.4.4"
```

Leave `WIFI_STA_STATIC_IP` commented out to use DHCP.

### Game tunables

| Define | Default | Description |
|---|---|---|
| `NUM_MODULES` | 4 | Number of remote button/ring units |
| `MODULE_NUM_LEDS` | 16 | LEDs per module ring |
| `HUB_NUM_LEDS` | 15 | LEDs in the hub progress strip — also the prompts-per-round count |
| `NUM_ROUNDS` | 3 | Total rounds per game |
| `PROMPT_TIMEOUTS_MS[]` | `{3000, 2000, 1000}` | Per-round timeout in ms (one entry per round) |
| `INTER_PROMPT_PAUSE_MS[]` | `{700, 500, 300}` | Per-round pause between consecutive prompts in ms — shrinks each round to amplify the speed-up |
| `FINAL_ROUND_PHANTOM_PCT` | 30 | Final-round only: % chance a correct press is silently rejected (cyan flash, no scoreboard advance). 0 disables. |
| `PHANTOM_FLASH_MS` | 250 | ms the cyan acknowledgement holds on a phantom press |
| `STARTUP_FLASH_COUNT` | 3 | Green flashes when a cartridge is inserted |
| `STARTUP_FLASH_INTERVAL_MS` | 250 | ms per startup-flash half-cycle |
| `ROUND_INTRO_STEP_MS` | 60 | ms per LED in the blue round-intro sweep |
| `HIT_HOLD_MS` | 700 | ms the active ring stays solid green after a hit |
| `MISS_FLASH_COUNT` | 4 | Red on/off cycles after a miss |
| `MISS_FLASH_INTERVAL_MS` | 150 | ms per miss-flash half-cycle |
| `MISS_HOLD_MS` | 400 | ms of solid red after the flash sequence |
| `COMPLETE_FLASH_COUNT` | 3 | Final green flashes on the hub strip |
| `COMPLETE_FLASH_INTERVAL_MS` | 300 | ms per complete-flash half-cycle |
| `IDLE_ANIM_MIN_MS` | 250 | Minimum hold time per cell in the idle hub-strip flicker |
| `IDLE_ANIM_MAX_MS` | 1000 | Maximum hold time per cell in the idle hub-strip flicker |
| `IDLE_BREATHE_PERIOD_MS` | 3000 | Sine-breathe period for idle module rings (rings phase-offset by ¼ period) |
| `IDLE_BREATHE_MIN` / `IDLE_BREATHE_MAX` | 0.10 / 0.50 | Floor and peak brightness factors for the breathe |
| `IDLE_ORBIT_TRIGGER_PCT` | 8 | % chance per breath cycle that a ring switches to orbit mode |
| `IDLE_ORBIT_CYCLES` | 3 | Magenta comet revolutions per orbit excursion before returning to breathe |
| `IDLE_ORBIT_STEP_MS` | 80 | ms per pixel step in the orbit comet (16 × 80 = 1.28 s / revolution) |
| `IDLE_ORBIT_COMET_LENGTH` | 5 | Lit pixels in the orbit comet's trailing tail |
| `LED_BRIGHTNESS` | 150 | 0–255; 150 ≈ 60 % — keeps total current under 3 A |
| `BUTTON_DEBOUNCE_MS` | 20 | Software debounce window per button |
| `RFID_POLL_INTERVAL_MS` | 40 | ms between RC522 polls |
| `REMOVAL_DEBOUNCE` | 3 | Consecutive missed polls before a cartridge is marked absent |

---

## Game flow

1. **Idle.** Hub strip flickers asynchronously between amber, green, red, and off — a late-1970s mainframe panel "thinking" effect. Module rings slowly breathe in dim white, each ring phase-offset by ¼ cycle so they pulse in a rolling wave; every so often a ring spontaneously switches into a magenta orbit excursion (a comet chasing around the ring) for a few revolutions before returning to breathe. Awaiting cartridge.
2. **Cartridge inserted.** Hub strip flashes green three times, then goes dark. Game begins.
3. **Round intro.** A blue dot sweeps once across the hub strip — the operator's "go" signal.
4. **Prompt.** The hub picks a random module (never repeating the immediately previous module within a round). That module's ring fills amber. As the timeout elapses, ring LEDs extinguish one by one until the ring is empty.
   - **Hit** — the player presses *that module's* button before time runs out. Ring goes solid green for ~700 ms, the matching cell on the hub strip lights green, then the ring goes dark.
   - **Miss** — the timeout expires. Ring flashes red four times then settles red briefly, the matching cell on the hub strip lights red, then the ring goes dark.
   - **Phantom** *(final round only)* — the player presses in time, but the firmware rolls against `FINAL_ROUND_PHANTOM_PCT` and silently rejects the press. The ring flashes solid cyan for ~250 ms to acknowledge that something happened, but the scoreboard does **not** advance. A new prompt fires next. Misses (timeouts) always count, so the round still ends in finite time.
   - Pressing a button on a non-active module is silently ignored — no penalty, no feedback. The active module's amber ring is the only thing that matters.
5. **Pause.** A round-scaled beat (700 ms / 500 ms / 300 ms in rounds 1 / 2 / 3) with the scoreboard visible on the hub strip.
6. **Repeat** until the scoreboard is full (one cell per `HUB_NUM_LEDS`). With phantom presses enabled in the final round, expect ~`HUB_NUM_LEDS / (1 − pct/100)` total presses to fill it.
7. **End of round.** Hub strip clears for the round-intro sweep of the next round.
8. **Repeat for 3 rounds**, with timeouts shrinking 3 s → 2 s → 1 s and inter-prompt pauses shrinking 0.7 s → 0.5 s → 0.3 s.
9. **Complete.** Hub strip flashes green three times, then goes solid blue. Module rings stay off.
10. **Reset.** Pulling the cartridge at any point — including mid-prompt and after completion — instantly returns the prop to idle. Re-inserting starts a fresh game.

### LED behaviour summary

| Condition | Hub strip | Module rings |
|---|---|---|
| Idle (no cartridge) | Per-cell random flicker — amber / green / red / off (1970s panel feel) | All breathe in dim white (rolling wave); occasional magenta orbit excursion on a single ring |
| Cartridge inserted (startup) | Flash green × 3 then off | All off |
| Round intro | Single blue LED sweeps across | All off |
| Prompt active | Scoreboard (prior results, current cell off) | Active module: depleting amber wedge. Others: off |
| Prompt hit | Scoreboard updates to green at current cell | Active module solid green for `HIT_HOLD_MS` |
| Prompt miss | Scoreboard updates to red at current cell | Active module flashes red then settles red |
| Prompt phantom (final round only) | Scoreboard unchanged | Active module solid cyan for `PHANTOM_FLASH_MS` |
| Inter-prompt pause | Scoreboard | All off |
| Game complete (flash) | Flash green × 3 | All off |
| Game complete (steady) | Solid blue | All off |
| Cartridge removed | Returns to idle flicker | All off |

---

## Web interface

Navigate to the device IP in any browser. The interface is a live mirror only — no operator-tunable settings (game tunables are compile-time in `src/config.h`).

- **Header line** — current round (`ROUND 2/3`), current prompt within the round (`PROMPT 7/15`), and the detected cartridge UID.
- **Hub strip** — `HUB_NUM_LEDS` cells matching the physical strip: green on a hit, red on a miss, blue on completion. When idle, the cells run an independent JS flicker that mirrors the firmware's per-cell amber/green/red/off animation (timing is browser-local, not synced cell-by-cell with the device).
- **Module rings** — four rings showing the current physical state. The active module's ring is amber with a numeric countdown (`1.2 s`) underneath; settled rings show solid green, solid red, or (during the final round only) cyan for a phantom press. While idle, all four rings run a slow CSS breathe (the firmware's occasional magenta orbit excursions are not mirrored — the prop is the source of truth for those).

State updates stream over Server-Sent Events at 10 Hz so the countdown and ring transitions feel live.

### REST endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/` | Web UI |
| GET | `/events` | Server-Sent Events stream — see schema below |

#### `/events` payload

```json
{
  "state": "prompt_active",
  "uid": "A1B2C3D4",
  "round": 2,
  "totalRounds": 3,
  "prompt": 7,
  "promptsPerRound": 15,
  "selectedModule": 2,
  "timeoutMs": 2000,
  "remainingMs": 1240,
  "results": ["hit","hit","miss","hit","hit","hit","pending","pending","pending","pending","pending","pending","pending","pending","pending"]
}
```

`state` is one of `idle`, `startup_flash`, `round_intro`, `prompt_active`, `prompt_hit`, `prompt_miss`, `prompt_phantom`, `inter_prompt`, `complete_flash`, `complete_steady`. `selectedModule` is `-1` when no module is currently active; otherwise `0`–`3`. `remainingMs` is meaningful only during `prompt_active`. The `results` array always has `promptsPerRound` entries.
