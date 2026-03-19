# Dual-Target Build (CYD28 + ESP32-C3 Supermini) Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an ESP32-C3 Supermini + ST7789 + BLE keyboard build target that shares `src/main.cpp` with the existing CYD28 build, isolating all hardware differences in a HAL layer.

**Architecture:** A thin HAL (`hal.h` + `hal_cyd28.cpp` + `hal_c3.cpp`) provides `halInit()`, `halPollInput()`, `halClickSound()`, `halSetLed()`, and `halLoadTouchCal()`. `main.cpp` calls only HAL functions for hardware; platform-specific code is guarded `#ifndef TARGET_C3`. Two PlatformIO environments share `[base_config]` settings; `build_src_filter` excludes the wrong HAL file per target.

**Tech Stack:** Arduino-ESP32 3.x, PlatformIO, TFT_eSPI 2.5.43, NimBLE-Arduino 2.x (C3 only), XPT2046_Touchscreen (CYD28 only).

**Build commands:**
- `~/.platformio/penv/bin/pio run -e cyd28` — CYD28 build
- `~/.platformio/penv/bin/pio run -e c3` — C3 build

---

## Chunk 1: Build System + HAL Interface

### Task 1: Update platformio.ini

**Files:**
- Modify: `platformio.ini`

- [ ] **Step 1: Replace platformio.ini entirely**

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

- [ ] **Step 2: Verify it parses (no build yet)**

```bash
~/.platformio/penv/bin/pio project config
```
Expected: lists `cyd28` and `c3` environments, no errors.

- [ ] **Step 3: Commit**

```bash
git add platformio.ini
git commit -m "build: dual-target platformio.ini with CYD28 and C3 envs"
```

---

### Task 2: Create hal.h

**Files:**
- Create: `src/hal.h`

- [ ] **Step 1: Create src/hal.h**

```cpp
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
```

- [ ] **Step 2: Commit**

```bash
git add src/hal.h
git commit -m "hal: add HAL interface header"
```

---

## Chunk 2: hal_cyd28.cpp

### Task 3: Create hal_cyd28.cpp

Extract the CYD28-specific hardware code from `main.cpp` into a new file. Key changes from the original code:
- LEDC API updated from deprecated `ledcSetup()`/`ledcAttachPin()` to new `ledcAttach(pin, freq, bits)` / `ledcWrite(pin, duty)` — fixes the current build failure.
- `halPollInput()` encapsulates all of `handleTouch()` logic, emitting `InputEvent`s instead of calling app functions directly.
- Shift/Alt state and keyboard-UI state (`shiftOn`, `altOn`, `kbVisible`) are maintained in `main.cpp` as globals; `hal_cyd28.cpp` accesses them via `extern` declarations and calls `drawKeyboard()` / `drawHistory()` / `drawInputBar()` via forward declarations for purely-UI responses (shift toggle, KB hide/show).

**Files:**
- Create: `src/hal_cyd28.cpp`

- [ ] **Step 1: Create src/hal_cyd28.cpp**

```cpp
// hal_cyd28.cpp — CYD28 hardware: XPT2046 touch, LEDC LED+speaker, touch calibration
#include "hal.h"
#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

// --- Pin assignments ---
#define TFT_BL          21
#define TOUCH_SCLK      25
#define TOUCH_MISO      39
#define TOUCH_MOSI      32
#define TOUCH_CS_PIN    33
#define TOUCH_IRQ       36
#define LED_R_PIN        4
#define LED_G_PIN       16
#define LED_B_PIN       17
#define SPEAKER_PIN     26
#define SPK_FREQ1     4000
#define SPK_FREQ2      400
#define SPK_VOLUME1     64
#define SPK_VOLUME2    128
#define SPK_CLICK_MS    10

// --- Touch calibration (NVS namespace "touch") ---
#ifdef ROTATE_180
  #define CAL_ORIENT 3
#else
  #define CAL_ORIENT 0
#endif

static int calXmin = 200, calXmax = 3900;
static int calYmin = 200, calYmax = 3900;

void loadTouchCal() {
    Preferences p;
    p.begin("touch", true);
    if (p.getBool("valid", false) && p.getInt("orient", -1) == CAL_ORIENT) {
        calXmin = p.getInt("xmin", calXmin);
        calXmax = p.getInt("xmax", calXmax);
        calYmin = p.getInt("ymin", calYmin);
        calYmax = p.getInt("ymax", calYmax);
    }
    p.end();
}

void saveTouchCal() {
    Preferences p;
    p.begin("touch", false);
    p.putInt("xmin", calXmin); p.putInt("xmax", calXmax);
    p.putInt("ymin", calYmin); p.putInt("ymax", calYmax);
    p.putInt("orient", CAL_ORIENT);
    p.putBool("valid", true);
    p.end();
}

void halLoadTouchCal() { loadTouchCal(); }

// --- Touch objects ---
static SPIClass touchSPI(HSPI);
static XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ);
static unsigned long lastTouchMs = 0;
#define TOUCH_DEBOUNCE_MS 125
#define SWIPE_THRESHOLD    15

// --- External state from main.cpp ---
// hal_cyd28.cpp reads/writes these to keep keyboard UI in sync.
extern bool shiftOn;
extern bool altOn;
extern bool kbVisible;
extern TFT_eSPI tft;

// Forward declarations of main.cpp UI functions called for KB show/hide/shift/alt.
void drawKeyboard();
void drawHistory();
void drawInputBar();
void drawKey(int x, int y, int w, int h, const char* label, uint16_t face, uint16_t text);

// --- Touch coordinate mapping ---
static void mapTouch(TS_Point& tp, int& sx, int& sy) {
    sx = map(tp.x, calXmin, calXmax, 0, 319);
    sy = map(tp.y, calYmin, calYmax, 0, 239);
}

static bool inRect(int sx, int sy, int rx, int ry, int rw, int rh) {
    return sx >= rx && sx < rx + rw && sy >= ry && sy < ry + rh;
}

// --- LED (active LOW: invert each channel) ---
void halSetLed(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LED_R_PIN, 255 - r);
    ledcWrite(LED_G_PIN, 255 - g);
    ledcWrite(LED_B_PIN, 255 - b);
}

// --- Speaker ---
void halClickSound() {
    int steps = SPK_CLICK_MS / 2;
    if (steps < 1) steps = 1;
    ledcAttach(SPEAKER_PIN, SPK_FREQ1, 8);
    for (int i = 0; i <  steps; i++) { ledcWrite(SPEAKER_PIN, SPK_VOLUME1 * i / steps); delay(1); }
    for (int i = steps; i >= 0; i--) { ledcWrite(SPEAKER_PIN, SPK_VOLUME1 * i / steps); delay(1); }
    ledcAttach(SPEAKER_PIN, SPK_FREQ2, 8);
    for (int i = 0; i <  steps; i++) { ledcWrite(SPEAKER_PIN, SPK_VOLUME2 * i / steps); delay(1); }
    for (int i = steps; i >= 0; i--) { ledcWrite(SPEAKER_PIN, SPK_VOLUME2 * i / steps); delay(1); }
    ledcWrite(SPEAKER_PIN, 0);
}

// --- Init ---
void halInit() {
    // Backlight
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // Touch
    touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS_PIN);
    ts.begin(touchSPI);
#ifdef ROTATE_180
    ts.setRotation(3);
#else
    ts.setRotation(1);
#endif
    unsigned long t0 = millis();
    while (ts.touched() && millis() - t0 < 500) delay(10);

    // RGB LED — new ledcAttach API (Arduino-ESP32 3.x)
    ledcAttach(LED_R_PIN, 5000, 8);
    ledcAttach(LED_G_PIN, 5000, 8);
    ledcAttach(LED_B_PIN, 5000, 8);
    halSetLed(0, 0, 0);  // off

    // Speaker
    ledcAttach(SPEAKER_PIN, SPK_FREQ1, 8);
    ledcWrite(SPEAKER_PIN, 0);
}

// --- Touch calibration UI ---
// (Requires touch, so CYD28-only — called only under #ifndef TARGET_C3 in main.cpp)
void drawCrosshair(int x, int y);  // forward declared — defined in main.cpp

void calibrateTouch() {
    const int T1X = 20,           T1Y = 20;
    const int T2X = 300,          T2Y = 220;  // SCREEN_W-20, SCREEN_H-20

    tft.fillScreen(0x0841);  // COL_BG
    tft.setTextSize(1);
    tft.setTextColor(0xFFFF, 0x0841);

    while (ts.touched()) delay(5);
    delay(200);

    drawCrosshair(T1X, T1Y);
    tft.setCursor(60, 110); tft.print("Tap the crosshair");
    while (!ts.touched()) delay(5);
    long sumX = 0, sumY = 0; int n = 0;
    while (ts.touched()) { TS_Point p = ts.getPoint(); sumX += p.x; sumY += p.y; n++; delay(5); }
    int rx1 = sumX / n, ry1 = sumY / n;

    tft.fillScreen(0x0841);
    drawCrosshair(T2X, T2Y);
    tft.setCursor(60, 110); tft.print("Tap the crosshair");
    delay(300);
    while (!ts.touched()) delay(5);
    sumX = 0; sumY = 0; n = 0;
    while (ts.touched()) { TS_Point p = ts.getPoint(); sumX += p.x; sumY += p.y; n++; delay(5); }
    int rx2 = sumX / n, ry2 = sumY / n;

    long dRx = rx2-rx1, dTx = T2X-T1X, dRy = ry2-ry1, dTy = T2Y-T1Y;
    calXmin = (int)(rx1 - dRx * T1X / dTx);
    calXmax = (int)(rx2 + dRx * (319 - T2X) / dTx);
    calYmin = (int)(ry1 - dRy * T1Y / dTy);
    calYmax = (int)(ry2 + dRy * (239 - T2Y) / dTy);
    saveTouchCal();
    Serial.printf("[Cal] xmin=%d xmax=%d ymin=%d ymax=%d\n", calXmin, calXmax, calYmin, calYmax);
    tft.fillScreen(0x0841);
    tft.setTextColor(0x07E0, 0x0841);  // TFT_GREEN
    tft.setCursor(60, 110); tft.print("Calibration saved!");
    delay(1500);
}

// ============================================================
// halPollInput — translates touch events to InputEvent
//
// Keyboard UI concerns (shift/alt/KB show-hide) are handled
// here directly, modifying main.cpp globals via extern.
// Returns false for those — they don't produce app-level events.
// Returns true with a filled InputEvent for all other input.
// ============================================================

// Screen layout constants (must match main.cpp)
#define SCREEN_W 320
#define SCREEN_H 240
#define HIST_H_KB_SHOW  116
#define HIST_H_KB_HIDE  220
#define IBAR_Y_KB_SHOW  116
#define IBAR_H_KB_SHOW   20
#define IBAR_Y_KB_HIDE  220
#define IBAR_H_KB_HIDE   20
#define KB_Y            136
#define KB_H            104
#define KEY_W            32
#define KEY_H            20
#define KEY_GAP           1
#define BS_W             26
#define BS_X            294
#define BS_H             40
#define SHIFT_X           0
#define SHIFT_W          40
#define ALT_X            40
#define ALT_W            40
#define SPACE_X          80
#define SPACE_W         166
#define HIDE_X          246
#define HIDE_W           42
#define BTN_SEND_W       46
#define BTN_NEW_X        84
#define BTN_SHOWKB_X    144
#define BTN_SHOWKB_W     58
#define BTN_INSET         2
#define KEY_FLASH_MS    100

// Keyboard layout (must match main.cpp)
static const char* KB_ROW1          = "QWERTYUIOP";
static const char* KB_ROW2          = "ASDFGHJKL";
static const char* KB_ROW3          = "ZXCVBNM";
static const char* KB_NUM_UNSHIFTED = "1234567890";
static const char* KB_NUM_SHIFTED   = "!@#$%^&*()";
static const char* KB_NUM_ALT_TYPED[10] = { "|","\"",":","{"  ,"}","'","@","-","+","=" };

// Returns the character string for a tap at (sx,sy) on keyboard rows 0-3,
// or "" if not a character key. Flashes the key for visual feedback.
static String typeKBKey(int sx, int sy) {
    if (sy < KB_Y) return "";
    int rowStep = KEY_H + KEY_GAP;
    int rowIdx  = (sy - KB_Y) / rowStep;
    int rowY    = KB_Y + rowIdx * rowStep;
    if (sy >= rowY + KEY_H) return "";

    int    kx = -1, x;
    String typed;
    char   lbl[3] = {0};

    switch (rowIdx) {
        case 0: {
            x = 0;
            for (int i = 0; i < 10; i++, x += KEY_W) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    if (altOn) {
                        typed = KB_NUM_ALT_TYPED[i];
                        strncpy(lbl, KB_NUM_ALT_TYPED[i], 2); lbl[2] = '\0';
                    } else {
                        const char* nums = shiftOn ? KB_NUM_SHIFTED : KB_NUM_UNSHIFTED;
                        typed = String(nums[i]); lbl[0] = nums[i]; lbl[1] = '\0';
                    }
                    kx = x; break;
                }
            }
            break;
        }
        case 1: {
            x = 0;
            for (int i = 0; i < 10; i++, x += KEY_W) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW1[i] : (KB_ROW1[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            break;
        }
        case 2: {
            x = 0;
            for (int i = 0; i < 9; i++, x += KEY_W) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW2[i] : (KB_ROW2[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            if (typed.length() == 0 && inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                const char* s = altOn ? "\\" : (shiftOn ? "?" : "/");
                typed = String(s); strncpy(lbl, s, 2); kx = x;
            }
            break;
        }
        case 3: {
            x = 0;
            for (int i = 0; i < 7; i++, x += KEY_W) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW3[i] : (KB_ROW3[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            if (typed.length() == 0) {
                const char unsh[] = {',','.'}; const char sh[] = {'<','>'};
                const char alt_e[] = {'[',']'};
                const char* extras = altOn ? alt_e : (shiftOn ? sh : unsh);
                for (int i = 0; i < 2; i++, x += KEY_W) {
                    if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                        typed = String(extras[i]); lbl[0] = extras[i]; lbl[1] = '\0'; kx = x; break;
                    }
                }
            }
            break;
        }
    }

    if (kx >= 0 && typed.length() > 0) {
        halClickSound();
        drawKey(kx, rowY, KEY_W, KEY_H, lbl, 0xFFFF, 0x0000);
        delay(KEY_FLASH_MS);
        drawKey(kx, rowY, KEY_W, KEY_H, lbl, 0x4208, 0xFFFF);
    }
    return typed;
}

bool halPollInput(InputEvent* ev) {
    ev->type = INPUT_NONE;
    ev->ch   = 0;

    if (!ts.touched()) return false;
    if (millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) {
        while (ts.touched()) delay(10);
        return false;
    }

    TS_Point startPt = ts.getPoint();
    TS_Point endPt   = startPt;
    while (ts.touched()) { endPt = ts.getPoint(); delay(5); }
    lastTouchMs = millis();

    int sx, sy;
    mapTouch(startPt, sx, sy);
    if (sx < 0 || sy < 0 || sx >= SCREEN_W || sy >= SCREEN_H) return false;

    // History area: swipe gesture
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    if (sy < histH) {
        int ex, ey; mapTouch(endPt, ex, ey);
        int deltaY = ey - sy;
        if (abs(deltaY) >= SWIPE_THRESHOLD) {
            ev->type = (deltaY > 0) ? INPUT_SCROLL_DOWN : INPUT_SCROLL_UP;
            return true;
        }
        return false;
    }

    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;

    // Input bar
    if (sy >= barY && sy < barY + barH) {
        if (!kbVisible && sx >= SCREEN_W - BTN_NEW_X && sx < SCREEN_W - BTN_SEND_W + 4) {
            ev->type = INPUT_NEW_CONV;
            return true;
        }
        if (sx >= SCREEN_W - BTN_SEND_W) {
            ev->type = INPUT_ENTER;
            return true;
        }
        if (!kbVisible && sx >= SCREEN_W - BTN_SHOWKB_X &&
            sx < SCREEN_W - BTN_SHOWKB_X + BTN_SHOWKB_W) {
            // Show KB
            kbVisible = true;
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, 0x0841);
            drawHistory(); drawInputBar(); drawKeyboard();
            return false;
        }
        if (kbVisible) {
            // Tap text area with KB shown → backspace
            ev->type = INPUT_BACKSPACE;
            return true;
        }
        return false;
    }

    // Keyboard area
    if (kbVisible && sy >= KB_Y) {
        int rowStep = KEY_H + KEY_GAP;
        int row4Y   = KB_Y + 4 * rowStep + 1;
        int bsY     = SCREEN_H - BS_H;

        // Tall BS key
        if (inRect(sx, sy, BS_X, bsY, BS_W, BS_H)) {
            halClickSound();
            drawKey(BS_X, bsY, BS_W, BS_H, "<-", 0xFFFF, 0xFFFF);
            delay(KEY_FLASH_MS);
            drawKey(BS_X, bsY, BS_W, BS_H, "<-", 0x2945, 0xFFFF);
            ev->type = INPUT_BACKSPACE;
            return true;
        }

        // Row 4 special keys
        if (sy >= row4Y && sy < row4Y + KEY_H - 1) {
            if (inRect(sx, sy, SHIFT_X, row4Y, SHIFT_W, KEY_H - 1)) {
                halClickSound();
                shiftOn = !shiftOn;
                if (shiftOn) altOn = false;
                drawKeyboard();
                return false;
            }
            if (inRect(sx, sy, ALT_X, row4Y, ALT_W, KEY_H - 1)) {
                halClickSound();
                altOn = !altOn;
                if (altOn) shiftOn = false;
                drawKeyboard();
                return false;
            }
            if (inRect(sx, sy, SPACE_X, row4Y, SPACE_W, KEY_H - 1)) {
                halClickSound();
                ev->type = INPUT_CHAR;
                ev->ch   = ' ';
                return true;
            }
            if (inRect(sx, sy, HIDE_X, row4Y, HIDE_W, KEY_H - 1)) {
                kbVisible = false;
                tft.fillRect(0, 0, SCREEN_W, SCREEN_H, 0x0841);
                drawHistory(); drawInputBar();
                return false;
            }
            return false;
        }

        // Character keys
        String typed = typeKBKey(sx, sy);
        if (typed.length() > 0) {
            ev->type = INPUT_CHAR;
            ev->ch   = typed[0];  // single char (multi-char alt symbols handled separately)
            // For multi-char alt symbols, emit as INPUT_CHAR for each byte
            // (caller appends ev->ch; we only support single-char here)
            // Alt symbols are all single printable chars in this keyboard layout.
            return true;
        }
    }

    return false;
}

// pollKBHide: used during blocking API wait on CYD28 to allow KB toggle.
// Only called under #ifndef TARGET_C3 in main.cpp.
void pollKBHide() {
    if (!ts.touched()) return;
    TS_Point pt = ts.getPoint();
    while (ts.touched()) delay(5);
    lastTouchMs = millis();
    int sx, sy; mapTouch(pt, sx, sy);
    if (sx < 0 || sy < 0 || sx >= SCREEN_W || sy >= SCREEN_H) return;

    int row4Y = KB_Y + 4 * (KEY_H + KEY_GAP) + 1;
    if (kbVisible) {
        if (inRect(sx, sy, HIDE_X, row4Y, HIDE_W, KEY_H - 1)) {
            kbVisible = false;
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, 0x0841);
            drawHistory(); drawInputBar();
        }
    } else {
        int barY = IBAR_Y_KB_HIDE, barH = IBAR_H_KB_HIDE;
        if (inRect(sx, sy, SCREEN_W - BTN_SHOWKB_X, barY + BTN_INSET, BTN_SHOWKB_W, barH - BTN_INSET*2)) {
            kbVisible = true;
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, 0x0841);
            drawHistory(); drawInputBar(); drawKeyboard();
        }
    }
}
```

> **Note on multi-char alt symbols in `typeKBKey`:** The alt layer (`KB_NUM_ALT_TYPED`) has only single-char strings, so `typed[0]` is safe. If multi-char alt keys are ever added, the INPUT_CHAR approach needs extending. For now it handles all existing alt symbols correctly.

- [ ] **Step 2: Commit**

```bash
git add src/hal_cyd28.cpp src/hal.h
git commit -m "hal: add hal_cyd28.cpp with touch input, LED, speaker, touch cal"
```

---

## Chunk 3: main.cpp Refactor

### Task 4: Remove extracted code from main.cpp, add HAL include

These are surgical removals — touch the listed sections only.

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add `#include "hal.h"` after the existing includes block (around line 40)**

Add after `#include <Preferences.h>`:
```cpp
#include "hal.h"
```

- [ ] **Step 2: Remove the XPT2046 include and touch objects (around lines 35–36, 231–232)**

Remove:
```cpp
#include <XPT2046_Touchscreen.h>
```
Remove:
```cpp
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ);
```

- [ ] **Step 3: Remove hardware pin defines (lines ~174–196)**

Remove the entire block:
```cpp
// --- Hardware pins ---
#define TFT_BL          21
#define TOUCH_SCLK      25
#define TOUCH_MISO      39
#define TOUCH_MOSI      32
#define TOUCH_CS_PIN    33
#define TOUCH_IRQ       36

// --- RGB LED (active LOW, common anode to 3.3V) ---
#define LED_R_PIN       4
#define LED_G_PIN      16
#define LED_B_PIN      17
#define LED_R_CH        0   // LEDC channels
#define LED_G_CH        1
#define LED_B_CH        2

// --- Speaker ---
#define SPEAKER_PIN    26
#define SPK_CH          3    // LEDC channel for speaker
#define SPK_FREQ1      4000  // first click tone Hz
#define SPK_FREQ2      400   // second click tone Hz
#define SPK_VOLUME1    64   // first tone peak duty (0–255)
#define SPK_VOLUME2    128   // second tone peak duty (0–255)
#define SPK_CLICK_MS    10   // duration per tone ms (triangle envelope: ramp up then down)
void clickSound();           // forward declaration
```

- [ ] **Step 4: Remove these functions entirely from main.cpp**

- `mapTouch()` (lines ~737–740) — moved to hal_cyd28.cpp
- `inRect()` can STAY (also used by `calibrateTouch` display code and is harmless)
- `typeKBKey()` (lines ~753–837) — moved to hal_cyd28.cpp
- `handleTouch()` (lines ~917–1069) — replaced by halPollInput()
- `setRGBLed()` (lines ~1072–1077) — replaced by halSetLed()
- `setupRGBLed()` (lines ~1079–1087)
- `setupSpeaker()` (lines ~1090–1094)
- `clickSound()` (lines ~1097–1112) — replaced by halClickSound()
- `loadTouchCal()`, `saveTouchCal()` (lines ~73–95) — moved to hal_cyd28.cpp
- `calibrateTouch()` (lines ~1814–1869) — moved to hal_cyd28.cpp
- `pollKBHide()` (lines ~1343–1371) — moved to hal_cyd28.cpp

Also remove the cal globals near the top:
```cpp
static int calXmin = 200, calXmax = 3900;
static int calYmin = 200, calYmax = 3900;
```
And the `#ifdef ROTATE_180 / #define CAL_ORIENT` block.

Also remove `lastTouchMs` global and its `#define TOUCH_DEBOUNCE_MS` / `SWIPE_THRESHOLD` (now in hal_cyd28.cpp).

- [ ] **Step 5: Replace `setRGBLed` / `updateLedWifi` calls with `halSetLed`**

`updateLedWifi()` currently calls `setRGBLed(...)`. Replace those six calls with `halSetLed(r, g, b)` using the same values. The function signature of `updateLedWifi()` itself stays in main.cpp — only the inner calls change.

- [ ] **Step 6: Replace `clickSound()` calls with `halClickSound()`**

Global search-and-replace `clickSound()` → `halClickSound()` in main.cpp.

- [ ] **Step 7: Build test — cyd28 should compile (may have errors to fix)**

```bash
~/.platformio/penv/bin/pio run -e cyd28 2>&1 | tail -30
```

Fix any remaining reference errors before proceeding.

- [ ] **Step 8: Commit when cyd28 builds**

```bash
git add src/main.cpp
git commit -m "refactor: extract hardware code to hal_cyd28.cpp"
```

---

### Task 5: Add `#ifndef TARGET_C3` guards and kbVisible

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Replace `kbVisible` declaration (around line 235)**

Replace:
```cpp
bool kbVisible = true;
```
With:
```cpp
#ifdef TARGET_C3
constexpr bool kbVisible = false;
#else
bool kbVisible = true;
#endif
```

- [ ] **Step 2: Guard `drawKeyboard()` declaration (around line 331)**

The function body of `drawKeyboard()` and its call sites inside the keyboard area should already only run when `kbVisible` is true. But guard the entire function definition:
```cpp
#ifndef TARGET_C3
void drawKeyboard() {
    // ... existing body ...
}
#endif
```

- [ ] **Step 3: Guard Show KB / New buttons in `drawInputBar()` (around line 472)**

The `if (!kbVisible)` block that renders the Show KB and New buttons:
```cpp
#ifndef TARGET_C3
    if (!kbVisible) {
        // Show KB and New button rendering
    }
#endif
```

- [ ] **Step 4: Guard `enterPassword()` keyboard setup (around line 848)**

Wrap these two lines with `#ifndef TARGET_C3`:
```cpp
#ifndef TARGET_C3
    kbVisible   = true;
    // ...
    drawKeyboard();
#endif
```
The `drawInputBar()` call just before the while loop stays (shows password prompt input bar on C3 too).

- [ ] **Step 5: Guard `sendPrompt()` KB hide (around line 1787)**

```cpp
#ifndef TARGET_C3
    kbVisible = false;   // hide KB; drawHistory() covers the KB area
#endif
```

- [ ] **Step 6: Guard `pollKBHide()` call sites (lines ~1453, ~1565, ~1718 in API functions)**

Each call to `pollKBHide()` in the API wait loops:
```cpp
#ifndef TARGET_C3
    pollKBHide();
#endif
```

- [ ] **Step 7: Guard `calibrateTouch()` call and BOOT-button reset in `setup()` (around line 2107)**

```cpp
#ifndef TARGET_C3
    // Hold BOOT button (GPIO0) on power-on to wipe touch calibration back to defaults
    pinMode(0, INPUT_PULLUP);
    if (digitalRead(0) == LOW) {
        // ... existing reset code ...
    }
#endif
```

Also guard the `calibrateTouch()` call inside `selectModel()` (the 'C' key handler at boot):
```cpp
#ifndef TARGET_C3
    calibrateTouch();
    // ... redraw ...
#endif
```

- [ ] **Step 8: Guard `drawCrosshair()` function definition (used by calibrateTouch)**

```cpp
#ifndef TARGET_C3
void drawCrosshair(int x, int y) {
    // ... existing body ...
}
#endif
```

- [ ] **Step 9: Build test**

```bash
~/.platformio/penv/bin/pio run -e cyd28 2>&1 | tail -30
```
Expected: `[SUCCESS]`

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp
git commit -m "refactor: add TARGET_C3 guards for CYD28-only code"
```

---

### Task 6: Refactor `setup()` and input loops to use HAL

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Refactor `setup()` hardware init**

Replace the hardware init block in `setup()`. The `tft.init()` block stays unchanged.

**Remove from the top of `setup()` (before `tft.init()`):**
- The `loadWifiCreds()` call can stay (it is platform-agnostic)
- Remove `loadTouchCal()` call (line ~2105) — replaced by `halLoadTouchCal()` after `halInit()`

**The BOOT-button block** (line ~2107) is already guarded `#ifndef TARGET_C3` in Task 5. Inside that block, also ensure the `Serial.printf` cal debug line (line ~2121, references `calXmin/calXmax/calYmin/calYmax` globals) is also inside the guard — it references the removed cal globals. Move it inside the `#ifndef TARGET_C3` guard if it isn't already.

**Replace everything after `tft.fillScreen(COL_BG);` and before WiFi connect with:**
```cpp
    // Backlight + touch/LED/speaker (CYD28) or BLE scan (C3)
    halInit();
    halLoadTouchCal();
```

Remove:
- `pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);`
- `touchSPI.begin(...)`, `ts.begin(...)`, `ts.setRotation(...)`, the startup touch drain loop
- `setupRGBLed(); setupSpeaker();`

- [ ] **Step 2: Refactor `selectAP()` input loops**

`selectAP()` has several `ts.touched()` / `ts.getPoint()` loops. Replace each with `halPollInput()` + `delay(10)`.

**AP list wait (waiting for digit):**
```cpp
// Was: while (selected < 0) { if (!ts.touched()) continue; ... }
// Replace with:
while (selected < 0) {
    InputEvent ev;
    if (!halPollInput(&ev)) { delay(10); continue; }
    if (ev.type == INPUT_CHAR && ev.ch >= '1' && ev.ch <= '9') {
        int idx = ev.ch - '1';
        if (idx < apCount) selected = idx;
    }
}
```

**"No networks found. Tap to retry." wait:**
```cpp
// Was: while (!ts.touched()) delay(50); while (ts.touched()) delay(5);
// Replace with:
{ InputEvent ev; while (!halPollInput(&ev)) delay(10); }
```

**Failed AP option selection (rows 1 and 2):**
```cpp
// Was: while (choice == 0) { if (!ts.touched()) continue; ... }
// Replace with:
while (choice == 0) {
    InputEvent ev;
    if (!halPollInput(&ev)) { delay(10); continue; }
    if (ev.type == INPUT_CHAR && ev.ch == '1') choice = 1;
    if (ev.type == INPUT_CHAR && ev.ch == '2') choice = 2;
}
```

- [ ] **Step 3: Refactor `enterPassword()` input loop**

Replace the entire `while (true)` touch loop with:
```cpp
    while (true) {
        InputEvent ev;
        if (!halPollInput(&ev)) { delay(10); continue; }
        if (ev.type == INPUT_ENTER) {
            strncpy(out, inputBuf, 63); out[63] = '\0';
            inputBuf[0] = '\0'; inputLen = 0;
            return;
        }
        if (ev.type == INPUT_BACKSPACE) {
            if (inputLen > 0) { inputBuf[--inputLen] = '\0'; drawInputBar(); }
            continue;
        }
        if (ev.type == INPUT_CHAR) {
            if (inputLen < 63) {
                inputBuf[inputLen++] = ev.ch;
                inputBuf[inputLen]   = '\0';
                drawInputBar();
            }
            continue;
        }
    }
```

- [ ] **Step 4: Refactor `selectModel()` input loop**

Replace the `while (true)` touch loop with a `halPollInput()` character loop. The function should accept INPUT_CHAR for '1'–'5', 'b', 'i', 'g', 'c' (same keys as before but driven by keyboard input on both targets):

```cpp
    while (true) {
        InputEvent ev;
        if (!halPollInput(&ev)) { delay(10); continue; }
        if (ev.type != INPUT_CHAR) continue;
        char ch = ev.ch;

        // Models 1–3 (Gemini)
        if (ch >= '1' && ch <= '3') {
            int i = ch - '1';
            halClickSound();
            slideOutSlug();
            strncpy(GEMINI_MODEL, modelIds[i], 47); GEMINI_MODEL[47] = '\0';
            geminiUseGlobal = modelGlobal[i];
            useGrok = false; useGroq = false;
            return;
        }
        if (ch == '4') { halClickSound(); slideOutSlug(); useGrok = true; useGroq = false; return; }
        if (ch == '5') { halClickSound(); slideOutSlug(); useGroq = true; useGrok = false; return; }
        if (ch == 'b' || ch == 'B') {
            largeFont = !largeFont;
            tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, COL_BG);
            // redraw header text and choices
            tft.setTextSize(1); tft.setTextColor(TFT_DARKGREY, COL_BG);
            tft.setCursor(0, 0); tft.print("SLUG AI chatbot. Large text.");
            tft.setCursor(0, 10); tft.print("Ready. Select AI model:");
            showModelChoices();
            continue;
        }
        if (ch == 'i' || ch == 'I') {
            invertDisplay = !invertDisplay;
            tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, COL_BG);
            tft.setTextSize(1); tft.setTextColor(TFT_DARKGREY, COL_BG);
            tft.setCursor(0, 0); tft.print("SLUG AI chatbot");
            tft.setCursor(0, 10); tft.print("Ready. Select AI model:");
            showModelChoices();
            continue;
        }
#ifndef TARGET_C3
        if (ch == 'c' || ch == 'C') {
            calibrateTouch();
            tft.fillScreen(COL_BG);
            tft.setTextSize(1); tft.setTextColor(TFT_DARKGREY, COL_BG);
            tft.setCursor(0, 0); tft.print("SLUG AI chatbot");
            tft.setCursor(0, 10); tft.print("Ready. Select AI model:");
            drawKeyboard(); drawInputBar(); showModelChoices();
            continue;
        }
#endif
    }
```

> **Note:** The old `selectModel()` referenced keyboard key positions (KB_Y, KEY_W etc.) which are CYD28 display concerns. The new version uses character input from halPollInput(), which works identically on CYD28 (tapping '1' on the on-screen keyboard) and C3 (pressing '1' on the BLE keyboard). Remove the old touch-based loop entirely.

- [ ] **Step 5: Refactor `loop()` — replace `handleTouch()` with `halPollInput()` dispatch**

Replace `void loop()`:
```cpp
void loop() {
    InputEvent ev;
    if (halPollInput(&ev)) {
        switch (ev.type) {

        case INPUT_CHAR:
            if (inputLen < INPUT_MAX_LEN) {
                moreMode = false;
                inputBuf[inputLen++] = ev.ch;
                inputBuf[inputLen]   = '\0';
                drawInputBar();
                halClickSound();
            }
            break;

        case INPUT_BACKSPACE:
            if (inputLen > 0) {
                inputBuf[--inputLen] = '\0';
                if (inputLen == 0 && historyCount > 0) moreMode = true;
                drawInputBar();
                halClickSound();
            }
            break;

        case INPUT_ENTER:
            // Word commands
            if (strcmp(inputBuf, "new") == 0) { goto do_new_conv; }
            if (strcmp(inputBuf, "more") == 0) {
                if (moreMode) sendPrompt();
                break;
            }
            // Guard empty sends
            if (inputLen == 0 && !moreMode) break;
            sendPrompt();
            break;

        case INPUT_NEW_CONV:
        do_new_conv:
            historyCount = 0; lineCount = 0; scrollOffset = 0;
            moreMode = false; inputBuf[0] = '\0'; inputLen = 0;
            history[historyCount].isUser = true;
            history[historyCount].isError = false;
            history[historyCount].displayOnly = true;
            strncpy(history[historyCount].text, "[New chat]", 2047);
            historyCount++;
            rebuildLines();
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            drawHistory(); drawInputBar();
#ifndef TARGET_C3
            kbVisible = true;
            drawKeyboard();
#endif
            break;

        case INPUT_MORE:
            if (moreMode) sendPrompt();
            break;

        case INPUT_SCROLL_UP: {
            int lineH   = largeFont ? LINE_H_LARGE : LINE_H_SMALL;
            int histH   = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
            int maxVis  = histH / lineH;
            scrollOffset = max(0, scrollOffset - 3);
            drawHistory();
            break;
        }
        case INPUT_SCROLL_DOWN: {
            int lineH   = largeFont ? LINE_H_LARGE : LINE_H_SMALL;
            int histH   = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
            int maxVis  = histH / lineH;
            int maxScroll = max(0, lineCount - maxVis);
            scrollOffset = min(scrollOffset + 3, maxScroll);
            drawHistory();
            break;
        }

        default: break;
        }
    }
    checkWiFiHealth();
}
```

> **Note on scroll steps:** The original used `abs(deltaY) / 10` steps proportional to swipe length. The BLE keyboard version uses a fixed 3 lines per arrow press. This is intentional — arrow key scrolling is per-press.

> **Note on `goto`:** Using goto to share the new-conv logic between INPUT_NEW_CONV and INPUT_ENTER word command. If preferred, extract to a `doNewConversation()` inline function instead.

- [ ] **Step 6: Guard `checkWiFiHealth()` Ping call**

In `checkWiFiHealth()`:
```cpp
#ifndef TARGET_C3
    bool ok = (WiFi.status() == WL_CONNECTED) && Ping.ping(IPAddress(8,8,8,8), 1);
#else
    bool ok = (WiFi.status() == WL_CONNECTED);
#endif
```

Also guard the `#include <ESP32Ping.h>` at the top (it's only in the cyd28 lib_deps now):
```cpp
#ifndef TARGET_C3
#include <ESP32Ping.h>
#endif
```

- [ ] **Step 7: Build test cyd28**

```bash
~/.platformio/penv/bin/pio run -e cyd28 2>&1 | tail -30
```
Expected: `[SUCCESS]`

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp
git commit -m "refactor: main.cpp uses halPollInput throughout, TARGET_C3 guards complete"
```

---

## Chunk 4: hal_c3.cpp + C3 Build Verification

### Task 7: Create hal_c3.cpp

**Files:**
- Create: `src/hal_c3.cpp`

- [ ] **Step 1: Create src/hal_c3.cpp**

```cpp
// hal_c3.cpp — ESP32-C3 Supermini: NimBLE BLE HID host keyboard input
// No LED, no speaker, no touch.
#ifdef TARGET_C3

#include "hal.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <TFT_eSPI.h>

// External TFT for status display during init
extern TFT_eSPI tft;

// --- BLE UUIDs ---
static const NimBLEUUID HID_SVC_UUID("1812");
static const NimBLEUUID HID_RPT_UUID("2A4D");

// --- Ring buffer ---
#define RB_SIZE 16
static InputEvent   rb[RB_SIZE];
static volatile int rb_head = 0;
static volatile int rb_tail = 0;
static SemaphoreHandle_t rb_mutex = nullptr;

static void rb_push(InputEvent ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    int next = (rb_head + 1) % RB_SIZE;
    if (next != rb_tail) {  // drop if full
        rb[rb_head] = ev;
        rb_head = next;
    }
    xSemaphoreGive(rb_mutex);
}

bool halPollInput(InputEvent* ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    bool has = (rb_tail != rb_head);
    if (has) {
        *ev = rb[rb_tail];
        rb_tail = (rb_tail + 1) % RB_SIZE;
    }
    xSemaphoreGive(rb_mutex);
    return has;
}

// --- NVS bonded address store ---
static NimBLEAddress loadBondedAddress(bool& found) {
    Preferences p;
    p.begin("ble_kb", true);
    found = p.getBool("bonded", false);
    if (!found) { p.end(); return NimBLEAddress(""); }
    String addrStr = p.getString("addr", "");
    uint8_t type   = p.getUChar("type", 0);
    p.end();
    return NimBLEAddress(addrStr.c_str(), type);
}

static void saveBondedAddress(const NimBLEAddress& addr) {
    Preferences p;
    p.begin("ble_kb", false);
    p.putBool("bonded", true);
    p.putString("addr", addr.toString().c_str());
    p.putUChar("type", addr.getType());
    p.end();
}

// --- HID scan code → ASCII ---
// USB HID scan codes 0x04–0x1D = a–z (add 0x5D for shifted A–Z)
static char hidToAscii(uint8_t code, bool shifted) {
    if (code >= 0x04 && code <= 0x1D) {
        char c = 'a' + (code - 0x04);
        return shifted ? (c - 32) : c;
    }
    if (code >= 0x1E && code <= 0x27) {  // 1–0
        static const char num[]    = "1234567890";
        static const char numSh[]  = "!@#$%^&*()";
        int i = code - 0x1E;
        return shifted ? numSh[i] : num[i];
    }
    switch (code) {
        case 0x2C: return ' ';
        case 0x2D: return shifted ? '_' : '-';
        case 0x2E: return shifted ? '+' : '=';
        case 0x2F: return shifted ? '{' : '[';
        case 0x30: return shifted ? '}' : ']';
        case 0x31: return shifted ? '|' : '\\';
        case 0x33: return shifted ? ':' : ';';
        case 0x34: return shifted ? '"' : '\'';
        case 0x35: return shifted ? '~' : '`';
        case 0x36: return shifted ? '<' : ',';
        case 0x37: return shifted ? '>' : '.';
        case 0x38: return shifted ? '?' : '/';
        default:   return 0;
    }
}

// --- HID notify callback ---
static void hidNotifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 2) return;
    uint8_t modifier = data[0];
    bool ctrl    = (modifier & 0x11) != 0;   // 0x01=LCtrl, 0x10=RCtrl
    bool shifted  = (modifier & 0x22) != 0;  // 0x02=LShift, 0x20=RShift

    // Check bytes 2–7 for keycodes (skip byte 1 = reserved)
    bool allZero = true;
    for (size_t i = 2; i < len && i < 8; i++) {
        if (data[i] != 0) { allZero = false; break; }
    }
    if (allZero) return;  // key-up report

    for (size_t i = 2; i < len && i < 8; i++) {
        uint8_t code = data[i];
        if (code == 0) continue;

        InputEvent ev = { INPUT_NONE, 0 };

        if (ctrl && code == 0x11) { ev.type = INPUT_NEW_CONV; }        // Ctrl+N
        else if (ctrl && code == 0x10) { ev.type = INPUT_MORE; }       // Ctrl+M
        else if (code == 0x52) { ev.type = INPUT_SCROLL_UP; }          // ↑
        else if (code == 0x51) { ev.type = INPUT_SCROLL_DOWN; }        // ↓
        else if (code == 0x28) { ev.type = INPUT_ENTER; }              // Enter
        else if (code == 0x2A) { ev.type = INPUT_BACKSPACE; }          // Backspace
        else {
            char c = hidToAscii(code, shifted);
            if (c) { ev.type = INPUT_CHAR; ev.ch = c; }
        }

        if (ev.type != INPUT_NONE) rb_push(ev);
    }
}

// --- BLE client + reconnect ---
static NimBLEClient*  bleClient   = nullptr;
static NimBLEAddress  bondedAddr  = NimBLEAddress("");
static bool           hasBonded   = false;
static volatile bool  connected   = false;

static bool doConnect(const NimBLEAddress& addr);

// Reconnect task — retries every 2 seconds on disconnect
static void reconnectTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!connected && hasBonded) {
            doConnect(bondedAddr);
        }
    }
}

class ClientCB : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int) override {
        connected = false;
    }
};

static bool subscribeHID(NimBLEClient* client) {
    NimBLERemoteService* svc = client->getService(HID_SVC_UUID);
    if (!svc) return false;
    // Subscribe to ALL HID Report characteristics (keyboard may have multiple)
    auto chars = svc->getCharacteristics(true);
    if (!chars) return false;  // null-check before dereference
    bool ok = false;
    for (auto& c : *chars) {
        if (c->getUUID() == HID_RPT_UUID) {
            c->subscribe(hidNotifyCB, nullptr, true);
            ok = true;
        }
    }
    return ok;
}

static bool doConnect(const NimBLEAddress& addr) {
    if (!bleClient) {
        bleClient = NimBLEDevice::createClient();
        bleClient->setClientCallbacks(new ClientCB(), false);
    }
    if (!bleClient->connect(addr)) return false;
    if (!subscribeHID(bleClient)) { bleClient->disconnect(); return false; }
    connected = true;
    return true;
}

// Scan callback — connects to first HID device found
class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->isAdvertisingService(HID_SVC_UUID)) return;
        NimBLEDevice::getScan()->stop();
        bondedAddr = dev->getAddress();
        hasBonded  = true;
        saveBondedAddress(bondedAddr);
        doConnect(bondedAddr);
    }
};

// --- halInit ---
void halInit() {
    rb_mutex = xSemaphoreCreateMutex();

    NimBLEDevice::init("");
    NimBLEDevice::setSecurityAuth(true, true, true);   // bonding, MITM, SC
    // Concrete security callback subclass — accepts "Just Works" pairing
    struct KB_SecCB : public NimBLESecurityCallbacks {
        uint32_t onPassKeyRequest()                              override { return 0; }
        void     onPassKeyNotify(uint32_t)                       override {}
        bool     onSecurityRequest()                             override { return true; }
        void     onAuthenticationComplete(NimBLEConnInfo& info)  override {}
        bool     onConfirmPIN(uint32_t)                          override { return true; }
    };
    NimBLEDevice::setSecurityCallbacks(new KB_SecCB());

    // Try bonded address first
    bondedAddr = loadBondedAddress(hasBonded);
    if (hasBonded) {
        if (!doConnect(bondedAddr)) hasBonded = false;  // stale bond, fall through to scan
    }

    if (!connected) {
        // Display waiting message
        tft.setTextSize(1);
        tft.setTextColor(0xFFFF, 0x0841);
        tft.setCursor(0, 0); tft.print("Waiting for BLE keyboard...");

        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setScanCallbacks(new ScanCB(), false);
        scan->setActiveScan(true);
        while (!connected) {
            scan->start(10, false);  // 10-second window, blocking
        }
        tft.fillRect(0, 0, 320, 12, 0x0841);  // clear waiting message
    }

    // Start reconnect background task
    xTaskCreate(reconnectTask, "ble_recon", 4096, nullptr, 1, nullptr);
}

// --- No-op stubs ---
void halClickSound() {}
void halSetLed(uint8_t, uint8_t, uint8_t) {}
void halLoadTouchCal() {}

#endif // TARGET_C3
```

> **Note on `NimBLESecurityCallbacks`:** The default `NimBLESecurityCallbacks` accepts all pairings with "Just Works" (no passkey). For a cheap BLE keyboard this is usually sufficient. If the keyboard requires a passkey display, override `onPassKeyEntry()` to return 0 and accept whatever is shown on the host side.

- [ ] **Step 2: Commit**

```bash
git add src/hal_c3.cpp
git commit -m "hal: add hal_c3.cpp BLE HID host for ESP32-C3 Supermini"
```

---

### Task 8: Verify both builds

**Files:**
- None (build verification only)

- [ ] **Step 1: Build cyd28**

```bash
~/.platformio/penv/bin/pio run -e cyd28 2>&1 | tail -20
```
Expected: `[SUCCESS]` with RAM/flash usage summary. No errors.

- [ ] **Step 2: Build c3**

```bash
~/.platformio/penv/bin/pio run -e c3 2>&1 | tail -20
```
Expected: `[SUCCESS]` with RAM/flash usage summary. No errors.

- [ ] **Step 3: Fix any remaining errors and commit**

Common issues to watch for:
- Undefined references: check that all removed functions have been replaced or guarded
- `constexpr bool kbVisible = false` assignment errors: check for any remaining `kbVisible = ...` not guarded `#ifndef TARGET_C3`
- `extern` declarations in hal_cyd28.cpp not matching main.cpp globals (name/type mismatch)

- [ ] **Step 4: Final commit**

```bash
git add -A
git commit -m "feat: dual-target build complete — CYD28 and ESP32-C3 Supermini"
```

---

## Reference: Key extern Dependencies

`hal_cyd28.cpp` declares these as `extern` from `main.cpp`:

| Symbol | Type | Used for |
|--------|------|----------|
| `shiftOn` | `bool` | Key character case |
| `altOn` | `bool` | Alt layer character |
| `kbVisible` | `bool` | KB show/hide state |
| `tft` | `TFT_eSPI` | KB drawing callbacks |

Forward declarations in `hal_cyd28.cpp` (file scope):
```cpp
void drawKeyboard();
void drawHistory();
void drawInputBar();
void drawKey(int x, int y, int w, int h, const char* label, uint16_t face, uint16_t text);
```

`hal_c3.cpp` declares:
```cpp
extern TFT_eSPI tft;
```

---

## Reference: Functions Removed from main.cpp

| Function | Destination |
|----------|-------------|
| `mapTouch()` | hal_cyd28.cpp (static) |
| `typeKBKey()` | hal_cyd28.cpp (static) |
| `handleTouch()` | hal_cyd28.cpp as `halPollInput()` |
| `setRGBLed()` | hal_cyd28.cpp as `halSetLed()` |
| `setupRGBLed()` | hal_cyd28.cpp inside `halInit()` |
| `setupSpeaker()` | hal_cyd28.cpp inside `halInit()` |
| `clickSound()` | hal_cyd28.cpp as `halClickSound()` |
| `loadTouchCal()` | hal_cyd28.cpp |
| `saveTouchCal()` | hal_cyd28.cpp |
| `calibrateTouch()` | hal_cyd28.cpp |
| `pollKBHide()` | hal_cyd28.cpp (called under `#ifndef TARGET_C3`) |
| `inRect()` | Stays in main.cpp (also used by calibrateTouch display code — harmless on both targets) |
