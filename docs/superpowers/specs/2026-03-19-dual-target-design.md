# Dual-Target Build: CYD28 + ESP32-C3 Supermini

**Date:** 2026-03-19
**Project:** SLUG AI chatbot
**Scope:** Add ESP32-C3 Supermini + ST7789 320×240 + BLE keyboard target alongside existing CYD28 build, sharing one codebase via a Hardware Abstraction Layer (HAL).

---

## Goals

- Both targets built from the same `src/main.cpp` with no forking
- CYD28 behaviour unchanged
- C3 target: same chat UI, full 320×240 for history (no on-screen keyboard), BLE HID keyboard for input
- Minimal changes to `main.cpp` — hardware differences isolated in HAL files

---

## File Structure

```
src/
  main.cpp          — platform-agnostic application logic
  hal.h             — HAL interface
  hal_cyd28.cpp     — XPT2046 touch → InputEvent, LEDC RGB LED, LEDC speaker, touch calibration
  hal_c3.cpp        — NimBLE BLE HID host → InputEvent queue; no LED; no speaker
  fonts/            — unchanged (VLW font binaries compiled into flash on both targets via unconditional #include)
  images/           — unchanged
  secrets.h         — unchanged
docs/superpowers/specs/2026-03-19-dual-target-design.md
```

---

## HAL Interface (`hal.h`)

```cpp
enum InputEventType {
    INPUT_NONE,
    INPUT_CHAR,         // printable character; ch field is valid
    INPUT_BACKSPACE,
    INPUT_ENTER,        // Send / More — main.cpp checks moreMode; also for AP selection confirm
    INPUT_SCROLL_UP,    // CYD28: swipe up;   C3: ↑ arrow key
    INPUT_SCROLL_DOWN,  // CYD28: swipe down; C3: ↓ arrow key
    INPUT_NEW_CONV,     // CYD28: New button tap; C3: Ctrl+N
    INPUT_MORE,         // CYD28: More button tap; C3: Ctrl+M
};

struct InputEvent {
    InputEventType type;
    char           ch;   // valid when type == INPUT_CHAR
};

void halInit();
bool halPollInput(InputEvent* ev);      // non-blocking; returns true if event available
void halClickSound();                   // no-op on C3
void halSetLed(uint8_t r, uint8_t g, uint8_t b);  // no-op on C3
void halLoadTouchCal();                 // loads touch calibration from NVS; no-op on C3
```

`halPollInput()` is non-blocking and safe to call from the main loop.

**`INPUT_MORE` handler:** Calls `sendPrompt()` only if `moreMode == true`. Ignored when `moreMode == false`.

**`INPUT_NEW_CONV` handler:** Clears `inputBuf`, resets `historyCount`, redraws history and input bar.

**Word commands:** In `INPUT_ENTER` handler, check `strcmp(inputBuf, "new") == 0` → `INPUT_NEW_CONV` path; `strcmp(inputBuf, "more") == 0` → `INPUT_MORE` path (same moreMode check). Supplements Ctrl+N / Ctrl+M.

**NVS namespaces:** `"wifi"` (main.cpp, both targets), `"touch"` (hal_cyd28.cpp only), `"ble_kb"` (hal_c3.cpp only). No collisions.

---

## hal_cyd28.cpp

- **Touch input**: XPT2046 on HSPI (SCLK=25, MISO=39, MOSI=32, CS=33, IRQ=36). `halPollInput()` maps touch coordinates (via `mapTouch()` logic moved from `main.cpp`) to `InputEvent`s: key taps, swipe gestures, button taps (New → `INPUT_NEW_CONV`, More → `INPUT_MORE`, Send → `INPUT_ENTER`).
- **RGB LED**: LEDC on GPIOs 4/16/17 (active LOW). New LEDC API: `ledcAttach(pin, freq, bits)` / `ledcWrite(pin, duty)`. Fixes current build failure.
- **Speaker**: LEDC on GPIO 26. Triangle-envelope click. New LEDC API.
- **Touch calibration**: `calibrateTouch()`, `loadTouchCal()`, `saveTouchCal()`, cal globals — moved here from `main.cpp`. `halLoadTouchCal()` calls `loadTouchCal()`.
- **Backlight**: `TFT_BL` (GPIO 21) init in `halInit()`.

---

## hal_c3.cpp

### BLE HID Host

**Library:** `h2zero/NimBLE-Arduino@^2.0.0` (Arduino-ESP32 3.x / IDF 5.x). Used as BLE central (GATT client) only.

**WiFi + BLE coexistence:** Shared 2.4 GHz radio may cause brief BLE disconnects during WiFi API calls. Accepted trade-off; reconnect handles this.

**`halInit()` sequence** (called after `tft.init()` — display ready):

1. Check NVS `"ble_kb"` for bonded address. If found, connect directly (skip scan).
2. Else: display "Waiting for BLE keyboard..." on TFT. Scan in 10-second windows for HID service UUID `0x1812` until found.
3. On connect: pair/bond. Store bonded address in NVS `"ble_kb"`.
4. Discover HID Report characteristic (`0x2A4D`), call `subscribe(notifyCallback, nullptr, true)`.
5. On disconnect: reconnect task (`xTaskCreate`, 4096-byte stack, priority 1) retries every 2 seconds.

**Key NimBLE-Arduino 2.x classes used:**
- `NimBLEDevice::init("")` — init stack
- `NimBLEDevice::setSecurityAuth(true, true, true)` — enable bonding (bonding, MITM, SC)
- `NimBLEDevice::setSecurityCallbacks(new MyCallbacks())` — handle passkey/pairing events
- `NimBLEScan* scan = NimBLEDevice::getScan()` — scan setup
- `NimBLEClient* client = NimBLEDevice::createClient()` — connection
- `client->connect(address)`, `client->getService(uuid)`, `service->getCharacteristic(uuid)`
- `characteristic->subscribe(notify_cb, nullptr, true)` — enable notifications

**HID report parsing (notify callback):**

8-byte USB HID boot-protocol report. Byte 0 = modifiers, byte 1 = reserved, bytes 2–7 = scan codes.

Modifier check: `(modifier & 0x11) != 0` detects either Ctrl key. `0x11` = `0x01` (Left Ctrl, bit 0) `|` `0x10` (Right Ctrl, bit 4) — intentionally OR'd, not compared for equality.

Modifier byte (byte 0) and scan code bytes (2–7) are evaluated independently:

| Condition | Event |
|-----------|-------|
| Ctrl + scan code `0x11` (N) | `INPUT_NEW_CONV` |
| Ctrl + scan code `0x10` (M) | `INPUT_MORE` |
| Scan code `0x52` (↑) | `INPUT_SCROLL_UP` |
| Scan code `0x51` (↓) | `INPUT_SCROLL_DOWN` |
| Scan code `0x28` (Enter) | `INPUT_ENTER` |
| Scan code `0x2A` (Backspace) | `INPUT_BACKSPACE` |
| All scan codes zero (key-up) | ignored |
| Other | HID→ASCII + Shift → `INPUT_CHAR` |

**Ring buffer:** 16 `InputEvent` entries, FreeRTOS mutex. BLE callback pushes; `halPollInput()` pops. Non-blocking.

**`halClickSound()`**, **`halSetLed()`**, **`halLoadTouchCal()`** — all no-op stubs.

### WiFi AP Selection on C3

AP list capped at 9 (existing code). Polling loops use `delay(10)` to yield to BLE FreeRTOS task.

- **AP selection**: spin `halPollInput()` + `delay(10)`. Accept `INPUT_CHAR` `'1'`–`'9'`; index = `ch - '1'`. Out-of-range digits (>= `apCount`) are silently ignored. Confirm immediately on valid digit.
- **Password entry**: `INPUT_CHAR` appends, `INPUT_BACKSPACE` deletes, `INPUT_ENTER` confirms. The `kbVisible = true` and `drawKeyboard()` calls at the top of `enterPassword()` are guarded `#ifndef TARGET_C3`.

---

## platformio.ini

Replace the existing `platformio.ini` entirely. Requires PlatformIO Core ≥ 6.0 for reliable `[base_config]` interpolation.

```ini
[platformio]
default_envs = cyd28, c3

[base_config]
platform = espressif32
framework = arduino
monitor_speed = 115200
lib_deps =
    bodmer/TFT_eSPI@2.5.43
    bblanchon/ArduinoJson@^7.0.0
build_flags =
    -DUSER_SETUP_LOADED
    -DLOAD_GLCD -DLOAD_FONT2 -DLOAD_FONT4 -DLOAD_GFXFF -DSMOOTH_FONT

[env:cyd28]
extends = base_config
board = esp32dev
upload_speed = 921600
build_src_filter = +<*> -<hal_c3.cpp>
lib_deps =
    ${base_config.lib_deps}
    https://github.com/PaulStoffregen/XPT2046_Touchscreen.git
    marian-craciunescu/ESP32Ping
build_flags =
    ${base_config.build_flags}
    -DILI9341_2_DRIVER
    -DTFT_MOSI=13 -DTFT_MISO=12 -DTFT_SCLK=14 -DTFT_CS=15 -DTFT_DC=2 -DTFT_RST=-1
    -DTOUCH_CS=-1 -DTFT_INVERSION_ON -DSPI_FREQUENCY=27000000

[env:c3]
extends = base_config
board = esp32-c3-devkitm-1
upload_speed = 460800
build_src_filter = +<*> -<hal_cyd28.cpp>
lib_deps =
    ${base_config.lib_deps}
    h2zero/NimBLE-Arduino@^2.0.0
build_flags =
    ${base_config.build_flags}
    -DTARGET_C3
    -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1
    -DST7789_DRIVER -DTFT_WIDTH=240 -DTFT_HEIGHT=320
    -DTFT_MOSI=10 -DTFT_MISO=-1 -DTFT_SCLK=8 -DTFT_CS=3 -DTFT_DC=2 -DTFT_RST=9
    -DTFT_BL=-1 -DSPI_FREQUENCY=40000000
```

Notes:
- `ESP32Ping` moved to `[env:cyd28]` only — avoids potential compile issues on C3 where it is unused.
- `[base_config]` (no `env:` prefix) is a shared settings section, not a buildable environment.
- `default_envs` prevents `pio run` building `base_config`.
- `build_src_filter` excludes the wrong HAL file per environment.
- `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` required for USB-CDC on C3 Supermini with Arduino-ESP32 3.x.
- `board = esp32-c3-devkitm-1` is user-verified working for the C3 Supermini variant in use.
- `TFT_DC=2` is user-verified working alongside USB-CDC on this Supermini.

**C3 SPI pins (user-verified):**

| Signal | GPIO |
|--------|------|
| MOSI   | 10   |
| SCLK   | 8    |
| CS     | 3    |
| DC     | 2    |
| RST    | 9    |
| BL     | —    |

**C3 backlight:** The ST7789 panel used has no backlight control pin — backlight is permanently powered. `TFT_BL=-1` tells TFT_eSPI not to drive a backlight GPIO.

GPIO 8 also drives the Supermini's onboard blue LED — flickers during SPI. Harmless.

---

## main.cpp Changes

1. **Includes / globals removed:**
   - `#include <XPT2046_Touchscreen.h>` — removed
   - `SPIClass touchSPI`, `XPT2046_Touchscreen ts` globals — removed
   - All touch, LED, speaker, `TFT_BL` pin `#define`s — removed
   - `calibrateTouch()`, `loadTouchCal()`, `saveTouchCal()`, `mapTouch()` — moved to `hal_cyd28.cpp`
   - `handleTouch()` — entire function removed (all write sites for `kbVisible` are inside it; removing it accounts for all assignments on C3)
   - `setupRGBLed()`, `setRGBLed()`, `setupSpeaker()`, `clickSound()` — moved/replaced
   - Add `#include "hal.h"`

2. **`kbVisible`:** Replace `bool kbVisible = true;` with:
   ```cpp
   #ifdef TARGET_C3
   constexpr bool kbVisible = false;
   #else
   bool kbVisible = true;
   #endif
   ```
   Write sites outside `handleTouch()` (which is removed entirely) that must also be guarded:
   - `enterPassword()` line ~848: `kbVisible = true` and `drawKeyboard()` call — guard both `#ifndef TARGET_C3`
   - `sendPrompt()` line ~1787: `kbVisible = false` — guard `#ifndef TARGET_C3`

3. **`setup()` init order:**
   - `tft.init()` + rotation + `tft.fillScreen()` — always first
   - `halInit()` — all remaining hardware init (backlight on CYD28, BLE scan on C3)
   - `halLoadTouchCal()` — no-op on C3
   - BOOT-button touch-cal reset path guarded `#ifndef TARGET_C3`

4. **On-screen keyboard:** `drawKeyboard()`, keyboard layout constants, and Show KB / New button rendering in `drawInputBar()` guarded `#ifndef TARGET_C3`. With `kbVisible = constexpr false` on C3, layout metrics always use full-screen dimensions; KB-visible branches are eliminated by the compiler.

5. **`loop()` dispatch:** Replace `handleTouch()` with `halPollInput()` loop. Switch on `InputEventType`. Include explicit cases for `INPUT_NEW_CONV` and `INPUT_MORE`. For `INPUT_ENTER`: guard with `if (inputLen == 0 && !moreMode) return;` before calling `sendPrompt()` — prevents sending empty messages when Enter is pressed with nothing typed.

6. **`selectAP()` / `enterPassword()`:** Replace `ts.touched()` / `ts.getPoint()` / `mapTouch()` with `halPollInput()` + `delay(10)` as specified above.

7. **LED:** `setRGBLed()` / `updateLedWifi()` → `halSetLed()`.

8. **Sound:** `clickSound()` → `halClickSound()`.

9. **WiFi health:** Guard `Ping.ping()` with `#ifndef TARGET_C3`.

10. **Word commands:** In `INPUT_ENTER` handler: `strcmp(inputBuf, "new") == 0` → `INPUT_NEW_CONV` path; `strcmp(inputBuf, "more") == 0` → `INPUT_MORE` path.

11. **LEDC API fix** (in `hal_cyd28.cpp`): `ledcSetup()`/`ledcAttachPin()` → `ledcAttach(pin, freq, bits)` / `ledcWrite(pin, duty)`.

---

## What Does NOT Change

- WiFi credential store (NVS `"wifi"`, `main.cpp`, both targets)
- AI API calls (Gemini, Grok, Groq)
- Message history, rendering, word-wrap, scroll
- Font rendering (TFT_eSPI VLW smooth fonts; `src/fonts/` compiled into flash on both targets)
- Screen dimensions (320×240)
- Boot splash, model selection flow
- `secrets.h` format

---

## Out of Scope

- Touch calibration UI — CYD28 only
- RGB LED WiFi indicator — CYD28 only
- Speaker click — CYD28 only
- BLE keyboard pairing UI — bonding silent; re-pair by erasing NVS `"ble_kb"` namespace
