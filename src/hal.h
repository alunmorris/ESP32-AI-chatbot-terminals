// hal.h — Hardware Abstraction Layer interface
// CYD28: implemented in hal_cyd28.cpp
// ESP32-C3: implemented in hal_c3.cpp
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
};

struct InputEvent {
    InputEventType type;
    char           ch;   // valid when type == INPUT_CHAR
};

// Called once from setup() after tft.init(). Initialises all hardware for this target.
void halInit();

// Non-blocking. Returns true and fills *ev if an input event is available.
bool halPollInput(InputEvent* ev);

// Play key-click sound. No-op on C3.
void halClickSound();

// Set RGB LED colour. No-op on C3 (no LED).
void halSetLed(uint8_t r, uint8_t g, uint8_t b);

// Load touch calibration from NVS. No-op on C3.
void halLoadTouchCal();
