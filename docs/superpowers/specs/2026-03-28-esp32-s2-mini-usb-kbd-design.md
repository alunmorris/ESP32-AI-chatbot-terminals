# ESP32-S2 Mini + USB Keyboard — Design Spec
Date: 2026-03-28

## Overview

Add a new build target `env:s2mini` for the Wemos/LOLIN ESP32-S2 Mini with a wired USB HID keyboard, using the same ST7789 display and pin assignments as the existing `env:c3` target. Input handling moves from BLE HID (NimBLE) to native USB HID host via the ESP-IDF USB Host Library.

## Hardware Constraints

The ESP32-S2 has a single USB OTG peripheral (GPIO 19 = D−, GPIO 20 = D+). It cannot be both a USB CDC serial device and a USB host simultaneously. Consequences:
- `ARDUINO_USB_MODE=1` must NOT be set (would claim USB OTG as CDC device)
- Serial debug uses UART0 (hardware pins), not USB CDC
- A USB-A socket must be wired to D+/D− with 5V VBUS supplied to the keyboard

WS2812 RGB LED is on GPIO 15. Used for WiFi status: solid = connected, flashing = disconnected.

## platformio.ini changes

New `env:s2mini` extending `base_config`:
- `board = lolin_s2_mini`
- `build_src_filter = +<*> -<hal_cyd28.cpp> -<hal_c3.cpp>`
- `build_flags`: `-DTARGET_S2` + same ST7789/TFT flags as `env:c3` (240×320, same SPI pins)
- `lib_deps`: base + `bodmer/TFT_eSPI` + `adafruit/Adafruit NeoPixel`
- No NimBLE, no `ARDUINO_USB_MODE`

## hal_s2.cpp

Guarded by `#ifdef TARGET_S2`. Implements the full `hal.h` interface.

### Input — USB HID host

Ring buffer + mutex (identical pattern to `hal_c3.cpp`) feeding `halPollInput()`.

`hidToAscii()` copied verbatim from `hal_c3.cpp` — pure scan-code logic, no BLE dependency.

Two FreeRTOS tasks:
- **`usbHostTask`** — installs USB host lib, calls `usb_host_lib_handle_events()` in a tight loop
- **`usbClientTask`** — waits for device connection, opens HID interface, receives 8-byte keyboard reports, parses modifier + keycodes into `InputEvent`s, pushes to ring buffer

Key mappings identical to C3: Enter, Backspace, arrows (scroll/cursor), Ctrl+N, Ctrl+M, printable chars.

### Boot sequence

`halInit()` calls `usb_host_install()`, spawns both tasks, displays "USB keyboard..." boot row using the same sprite-based `bootRow` lambda pattern as `hal_c3.cpp`.

### LED — WS2812 on GPIO 15

`halSetLed(r, g, b)` drives the single NeoPixel via Adafruit NeoPixel library. `main.cpp`'s existing WiFi-status LED calls work unchanged.

### No-op stubs

`halClickSound`, `halLoadTouchCal`, `calibrateTouch`, `pollKBHide`, `halBeforeApiCall`, `halAfterApiCall` — all no-ops (no BLE coexistence, no touch, no speaker).

## main.cpp changes

None required. `TARGET_S2` guard in `hal_s2.cpp` isolates all new code. Existing `#ifndef TARGET_C3` guards around `ESP32Ping` remain correct (S2 will need the same exclusion — add `&& !defined(TARGET_S2)` or define `TARGET_C3`-equivalent).

Actually: `main.cpp` uses `#ifndef TARGET_C3` to exclude ESP32Ping. `TARGET_S2` is a new define so that guard needs updating to `#if !defined(TARGET_C3) && !defined(TARGET_S2)`.
