// hal_cyd28.cpp — CYD28 hardware: XPT2046 touch, LEDC LED+speaker, touch calibration
// 310326 WiFi power save: PS_NONE during API calls, MAX_MODEM idle
#include "hal.h"
#include <Arduino.h>
#include <esp_wifi.h>  // esp_wifi_set_ps()
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

static void loadTouchCal() {
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

static void saveTouchCal() {
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
    if (n < 1) n = 1;
    int rx1 = sumX / n, ry1 = sumY / n;

    tft.fillScreen(0x0841);
    drawCrosshair(T2X, T2Y);
    tft.setCursor(60, 110); tft.print("Tap the crosshair");
    delay(300);
    while (!ts.touched()) delay(5);
    sumX = 0; sumY = 0; n = 0;
    while (ts.touched()) { TS_Point p = ts.getPoint(); sumX += p.x; sumY += p.y; n++; delay(5); }
    if (n < 1) n = 1;
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
void halBeforeApiCall() { esp_wifi_set_ps(WIFI_PS_NONE); }
void halAfterApiCall()  { esp_wifi_set_ps(WIFI_PS_MAX_MODEM); }

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
