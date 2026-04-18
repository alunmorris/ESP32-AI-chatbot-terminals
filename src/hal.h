// hal.h — Hardware Abstraction Layer interface
// CYD28: implemented in hal_cyd28.cpp
// ESP32-C3: implemented in hal_c3.cpp
/*
 * 060426 declare halDeepSleep(), halIsDeepSleepWake()
 * 030426 declare halSleepIdle()
 */
#pragma once
#include <stdint.h>

enum InputEventType {
    INPUT_NONE,
    INPUT_CHAR,         // printable character; ch field is valid
    INPUT_BACKSPACE,
    INPUT_ENTER,        // Send/More — main.cpp checks moreMode; also AP selection confirm
    INPUT_SCROLL_UP,    // CYD28: swipe up;    C3: ↑ arrow key
    INPUT_SCROLL_DOWN,  // CYD28: swipe down;  C3: ↓ arrow key
    INPUT_NEW_CONV,     // CYD28: New button;  C3: Ctrl+N
    INPUT_MORE,         // CYD28: More button; C3: Ctrl+M
    INPUT_CURSOR_LEFT,  // C3: ← arrow key
    INPUT_CURSOR_RIGHT, // C3: → arrow key
    INPUT_MODEL_MENU,   // S2: Home key — re-enter model selection
    INPUT_DELETE,       // S2: Del key — forward-delete character at cursor
    INPUT_CTRL_D,       // C3: Ctrl+D — exit MicroPython REPL
};

struct InputEvent {
    InputEventType type;
    char           ch;   // valid when type == INPUT_CHAR
};

// Called once from setup() after tft.init(). Initialises all hardware for this target.
void halInit();

// Non-blocking. Returns true and fills *ev if an input event is available.
bool halPollInput(InputEvent* ev);

// Non-destructive peek: returns true and fills *ev without consuming the event.
bool halPeekInput(InputEvent* ev);

// Play key-click sound. No-op on C3.
void halClickSound();

// Set RGB LED colour. No-op on C3 (no LED).
void halSetLed(uint8_t r, uint8_t g, uint8_t b);

// Load touch calibration from NVS. No-op on C3.
void halLoadTouchCal();

// CYD28-only: show touch calibration UI. No-op on C3.
void calibrateTouch();

// CYD28-only: poll touch during blocking waits to allow KB show/hide. No-op on C3.
void pollKBHide();

// C3-only: deinit NimBLE before HTTPS call to reclaim ~30KB heap for TLS buffers.
// CYD28: no-ops.
void halBeforeApiCall();
void halAfterApiCall();


//030426 for power saving (no-op on standard espressif32; pioarduino builds use tickless idle)
void halSleepIdle();

// 060426 Deep sleep: enter ESP32 deep sleep with GPIO 9 wakeup (TARGET_EPAPER only; no-op stub elsewhere).
void halDeepSleep();
// Returns true if the current boot was caused by waking from deep sleep.
bool halIsDeepSleepWake();
// Start BLE reconnect task after WiFi connects (deep sleep wake path — avoids radio contention).
void halStartBleReconnect();