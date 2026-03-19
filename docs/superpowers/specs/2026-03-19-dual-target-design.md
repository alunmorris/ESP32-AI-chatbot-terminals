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
  hal.h             — HAL interface (InputEvent, halInit, halPollInput, halClickSound, halSetLed)
  hal_cyd28.cpp     — XPT2046 touch → InputEvent, LEDC RGB LED, LEDC speaker
  hal_c3.cpp        — NimBLE BLE HID host → InputEvent queue; no LED; no speaker
  fonts/            — unchanged
  images/           — unchanged
  secrets.h         — unchanged
docs/
  superpowers/specs/
    2026-03-19-dual-target-design.md  — this file
```

---

## HAL Interface (`hal.h`)

```cpp
enum InputEventType {
    INPUT_NONE,
    INPUT_CHAR,         // printable character; ch field is valid
    INPUT_BACKSPACE,
    INPUT_ENTER,        // Send / More — main.cpp checks moreMode
    INPUT_SCROLL_UP,    // CYD28: swipe up;   C3: ↑ arrow key
    INPUT_SCROLL_DOWN,  // CYD28: swipe down; C3: ↓ arrow key
    INPUT_NEW_CONV,     // CYD28: New button tap; C3: Ctrl+N or type "new"+Enter
    INPUT_MORE,         // CYD28: More button tap; C3: Ctrl+M or type "more"+Enter
};

struct InputEvent {
    InputEventType type;
    char           ch;   // valid when type == INPUT_CHAR
};

void halInit();
bool halPollInput(InputEvent* ev);   // non-blocking; returns true if event available
void halClickSound();                // no-op on C3
void halSetLed(uint8_t r, uint8_t g, uint8_t b);  // no-op on C3
```

`halPollInput()` is non-blocking and safe to call from the main loop on every iteration.

---

## hal_cyd28.cpp

Wraps all CYD28-specific hardware:

- **Touch input**: XPT2046 on HSPI (SCLK=25, MISO=39, MOSI=32, CS=33, IRQ=36). `halPollInput()` reads touch, maps coordinates via existing `mapTouch()` logic, and emits `InputEvent`s for key taps, swipe gestures, and button taps.
- **RGB LED**: LEDC channels 0–2 on GPIOs 4/16/17 (active LOW). Updated to new `ledcAttach(pin, freq, bits)` / `ledcWrite(pin, duty)` API (fixes current build failure).
- **Speaker**: LEDC channel 3 on GPIO 26. Triangle-envelope click sound. Updated to new LEDC API.
- `halSetLed(r, g, b)` inverts channels (active LOW) and writes.
- `halClickSound()` plays the two-tone triangle-envelope click.

---

## hal_c3.cpp

Wraps ESP32-C3-specific hardware:

- **BLE HID host** via NimBLE-Arduino (`h2zero/NimBLE-Arduino@^1.4.0`):
  1. `halInit()` starts NimBLE, scans for a device advertising HID service UUID `0x1812`
  2. On connect: pair/bond, discover HID Report characteristic (`0x2A4D`), subscribe to notifications
  3. BLE notify callback parses 8-byte boot-protocol report (byte 0 = modifiers, byte 1 = reserved, bytes 2–7 = keycodes) → pushes `InputEvent` onto a 16-entry ring buffer
  4. Modifier byte handling: Ctrl+N → `INPUT_NEW_CONV`, Ctrl+M → `INPUT_MORE`, Shift applied for correct case
  5. Arrow keys: ↑ → `INPUT_SCROLL_UP`, ↓ → `INPUT_SCROLL_DOWN`
  6. Enter → `INPUT_ENTER`, Backspace → `INPUT_BACKSPACE`
  7. On disconnect: silently attempts reconnect to bonded device address
- `halPollInput()` dequeues from the ring buffer (called from main loop; BLE callback runs on a different task)
- `halClickSound()` — no-op
- `halSetLed()` — no-op
- No speaker pin; no LED pins

**BLE keyboard assumption:** Windows mode on the cheap 7-inch Chinese BLE keyboard uses standard HID boot protocol. Use Windows mode on the keyboard.

---

## platformio.ini

```ini
[env:base]
platform = espressif32
framework = arduino
monitor_speed = 115200
upload_speed = 921600
lib_deps =
    bodmer/TFT_eSPI@2.5.43
    bblanchon/ArduinoJson@^7.0.0
    marian-craciunescu/ESP32Ping
build_flags =
    -DUSER_SETUP_LOADED
    -DLOAD_GLCD -DLOAD_FONT2 -DLOAD_FONT4 -DLOAD_GFXFF -DSMOOTH_FONT

[env:cyd28]
extends = env:base
board = esp32dev
lib_deps =
    ${env:base.lib_deps}
    https://github.com/PaulStoffregen/XPT2046_Touchscreen.git
build_flags =
    ${env:base.build_flags}
    -DILI9341_2_DRIVER
    -DTFT_MOSI=13 -DTFT_MISO=12 -DTFT_SCLK=14 -DTFT_CS=15 -DTFT_DC=2 -DTFT_RST=-1
    -DTOUCH_CS=-1 -DTFT_INVERSION_ON -DSPI_FREQUENCY=27000000

[env:c3]
extends = env:base
board = esp32-c3-devkitm-1
lib_deps =
    ${env:base.lib_deps}
    h2zero/NimBLE-Arduino@^1.4.0
build_flags =
    ${env:base.build_flags}
    -DTARGET_C3
    -DST7789_DRIVER -DTFT_WIDTH=240 -DTFT_HEIGHT=320
    -DTFT_MOSI=10 -DTFT_MISO=-1 -DTFT_SCLK=8 -DTFT_CS=3 -DTFT_DC=2 -DTFT_RST=9
    -DTFT_BL=-1 -DSPI_FREQUENCY=40000000
```

**C3 SPI pins (ESP32-C3 Supermini, user-verified):**

| Signal  | GPIO |
|---------|------|
| MOSI    | 10   |
| SCLK    | 8    |
| CS      | 3    |
| DC      | 2    |
| RST     | 9    |
| BL      | —    |

Note: GPIO 8 is also the Supermini's onboard blue LED — it will flicker during SPI transfers. Harmless.

---

## main.cpp Changes

Only these changes to `main.cpp`:

1. **Includes**: add `#include "hal.h"`; remove `#include <XPT2046_Touchscreen.h>`; remove `SPIClass touchSPI` and `XPT2046_Touchscreen ts` objects
2. **`setup()`**: replace `touchSPI.begin()`, `ts.begin()`, `setupRGBLed()`, `setupSpeaker()` with `halInit()`; backlight pin init guarded `#ifndef TARGET_C3`
3. **`loop()`**: replace `handleTouch()` call with `halPollInput()` dispatch loop
4. **On-screen keyboard**: `drawKeyboard()`, `kbVisible` toggle, Show KB / Hide button rendering guarded `#ifndef TARGET_C3`; on C3 keyboard area is permanently free giving full 320×240 to history
5. **`selectAP()` / `enterPassword()`**: replace `ts.touched()` / `ts.getPoint()` + `mapTouch()` with `halPollInput()` — UI and logic unchanged
6. **LED**: `setRGBLed()` / `updateLedWifi()` replaced with `halSetLed()` calls — no-op on C3
7. **Sound**: `clickSound()` calls replaced with `halClickSound()` — no-op on C3
8. **WiFi health**: `checkWiFiHealth()` guards the `Ping.ping()` call with `#ifndef TARGET_C3`; C3 relies on `WiFi.status()` only
9. **Word commands**: in the `INPUT_ENTER` handler, check `inputBuf == "new"` → new conversation, `inputBuf == "more"` → more response, before sending to API
10. **LEDC API fix** (CYD28 only, in `hal_cyd28.cpp`): replace deprecated `ledcSetup()` / `ledcAttachPin()` with `ledcAttach(pin, freq, bits)` / `ledcWrite(pin, duty)` — fixes current build failure

---

## What Does NOT Change

- WiFi credential store (NVS)
- AI API calls (Gemini, Grok, Groq)
- Message history, rendering, word-wrap, scroll
- Font rendering (TFT_eSPI VLW smooth fonts)
- Screen dimensions (320×240)
- Boot splash, model selection flow
- `secrets.h` format

---

## Out of Scope

- Touch calibration UI (`calibrateTouch()`) — CYD28 only, not compiled on C3
- RGB LED WiFi indicator — CYD28 only
- Speaker click sound — CYD28 only
- BLE keyboard pairing UI — bonding handled silently by NimBLE; re-pair by erasing NVS if needed
