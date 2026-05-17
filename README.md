# ESP32 AI chatbot terminals
<img width="2043" height="1152" alt="c0770e99-15e4-4bde-a866-f58943980007" src="https://github.com/user-attachments/assets/cba7238e-035c-4b9a-b5a0-673266229c43" />

A portable AI chat terminal that runs on several ESP32-based hardware builds. All builds share the same application logic (`main.cpp`) with hardware differences isolated in target-specific HAL files.

Supports Gemini, Grok, and Groq APIs with on-device model selection, WiFi configuration, and message history.

---

## Build Targets

### `cyd28` — Cheap Yellow Display 2.8"

**Hardware:** ESP32-Dev + ILI9341 2.8" 320×240 TFT with XPT2046 touchscreen

The original build. Self-contained with no external keyboard — input is via an on-screen keyboard drawn in the lower 104px of the display. The remaining screen area shows the chat history and input line.

- **Input:** Touchscreen tap for keys; swipe up/down to scroll history
- **Display:** 320×240, split between chat area and on-screen keyboard
- **LED:** RGB LED on GPIO 4/16/17 shows WiFi signal strength (blue = strong, red = weak)
- **Speaker:** Click sound on keypress (GPIO 26, 4 kHz, 10 ms)
- **HAL:** `hal_cyd28.cpp`

---

### `c3` — ESP32-C3 Supermini + BLE Keyboard

**Hardware:** ESP32-C3 Supermini + ST7789 320×240 TFT + any Bluetooth HID keyboard

Full-screen chat — no on-screen keyboard, so the entire 320×240 display is used for the conversation. Pairs with a BLE keyboard; the bonded device address is stored in NVS so it reconnects automatically on power-up.

- **Input:** BLE HID keyboard (NimBLE stack); pairing is automatic on first boot
- **Display:** 320×240, full screen for chat
- **LED:** None
- **Speaker:** None
- **HAL:** `hal_c3.cpp`

> **Note:** WiFi and BLE share the C3's 2.4 GHz radio. WiFi reconnects automatically if BLE causes interference.

---

### `p3` — ESP32-C3 + ST7789P3 Compact Display
<img width="3700" height="1707" alt="IMG_20260515_075228541_HDR_AE" src="https://github.com/user-attachments/assets/c56a27a9-5c38-4304-a324-92fd9f2a173a" />

**Hardware:** ESP32-C3 + ST7789P3 284×76 landscape TFT + any Bluetooth HID keyboard

A compact variant of the `c3` build using a small landscape display module (284×76 px). Same BLE keyboard pairing behaviour; the reduced screen area means chat and input are presented in a condensed layout. Backlight is controlled via GPIO 5.

- **Input:** BLE HID keyboard (same as `c3`)
- **Display:** 284×76 landscape
- **LED:** None
- **Speaker:** None
- **HAL:** `hal_c3.cpp` (shares HAL with `c3`; `TARGET_P3` flag adjusts layout)

---

### `s2mini` — ESP32-S2 Mini + USB HID Keyboard

**Hardware:** ESP32-S2 Mini + ST7789 320×240 TFT + wired USB HID keyboard via USB-C OTG

Uses the S2's USB OTG peripheral in host mode to connect a standard wired USB keyboard. Because the OTG peripheral is occupied by the keyboard, USB-CDC serial is disabled — debug output requires a separate USB-UART adapter on GPIO 43.

- **Input:** USB HID keyboard via USB-C OTG host (GPIO 19/20)
- **Display:** 320×240, full screen for chat
- **LED:** Single GPIO 15 LED (on = WiFi connected)
- **Speaker:** None
- **HAL:** `hal_s2.cpp`
- **Extra keys:** PgUp/PgDn scroll history; Home opens model menu; Del for forward-delete; Caps Lock supported

> **Note:** `s2mini` is not in `default_envs` and must be built explicitly: `pio run -e s2mini`

---

### `epaper` — ESP32-C3 Supermini + B&W E-Paper
<img width="2507" height="2009" alt="IMG_20260515_075640335_HDR_AE" src="https://github.com/user-attachments/assets/362610c8-58a7-45e2-ac0e-ef710918d328" />

**Hardware:** ESP32-C3 Supermini + Waveshare GDEY0213B74 2.13" B&W e-paper (various sizes )+ any Bluetooth HID keyboard

Full-screen chat on an e-paper display. BLE keyboard pairing works the same as the `c3` build. Always uses the light theme (white background, black text); dark theme toggle has no effect.

- **Input:** BLE HID keyboard (NimBLE stack); same pairing behaviour as `c3`
- **Display:** 250×122 e-paper, full refresh ~1700 ms, partial (input bar) ~500 ms
- **LED:** None
- **Speaker:** None
- **HAL:** `hal_epaper.cpp`
- **SPI pins:** SCK=4, MOSI=6, CS=7, DC=1, RST=2, BUSY=3, MISO=5 (dummy)

> **Note:** AI response streaming triggers one full refresh (1700 ms) per update — visibly slow but functional.

---

## Project Structure

```
src/
  main.cpp          — platform-agnostic application (WiFi, API calls, UI)
  hal.h             — HAL interface shared by all builds
  hal_cyd28.cpp     — HAL for cyd28 (touchscreen, RGB LED, speaker)
  hal_c3.cpp        — HAL for c3 and p3 (BLE HID keyboard)
  hal_s2.cpp        — HAL for s2mini (USB HID keyboard)
  font.h            — font selection macros
  fonts/            — DejaVu Sans Bold VLW fonts (8 px, 12 px, 18 px)
  images/           — splash screen and logo assets
  secrets.h         — API keys (not in git — copy from secrets.h.example)
```

---

## Configuration

Copy `src/secrets.h.example` to `src/secrets.h` and fill in your API keys for Gemini, Grok, and/or Groq.

WiFi credentials are configured on-device via the WiFi setup menu and stored in NVS.

---

## Building

```bash
# Build all default targets (cyd28, c3, p3)
pio run

# Build a specific target
pio run -e cyd28
pio run -e c3
pio run -e p3
pio run -e s2mini

# Upload
pio run -e cyd28 --target upload
```
