# AI Terminal Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a self-contained AI terminal on the CYD28 — touchscreen QWERTY keyboard, scrollable conversation history, Vertex AI Gemini via OAuth2 service account JWT.

**Architecture:** Single `src/main.cpp` file, blocking network calls. Screen splits into history area (top), input bar (middle), keyboard (bottom, show/hide). JWT signed on-device with mbedTLS; OAuth2 token cached and refreshed. Multi-turn conversation history sent with each API call (capped at 20 messages).

**Tech Stack:** PlatformIO / Arduino, TFT_eSPI, XPT2046_Touchscreen, WiFiClientSecure, ArduinoJson v7, mbedTLS (built-in to ESP32 Arduino framework)

---

## Before you start

The user will supply Vertex AI credentials. You need four values:
- `SA_EMAIL` — service account email (e.g. `mysa@project.iam.gserviceaccount.com`)
- `SA_PRIVATE_KEY` — PEM private key from service account JSON. The JSON stores `\n` as literal backslash-n; you must replace each `\n` with an actual newline in the C++ string literal (use a raw string or escape properly). Service account keys are typically PKCS#8 (`-----BEGIN PRIVATE KEY-----`); mbedTLS `mbedtls_pk_parse_key()` handles this format.
- `GCP_PROJECT` — GCP project ID
- `GCP_REGION` — e.g. `us-central1`

Ask the user for these before implementing Task 9 onwards.

---

## Task 1: Project scaffold

**Files:**
- Create: `platformio.ini`
- Create: `src/main.cpp`

**Step 1: Create `platformio.ini`**

```ini
[env:cyd28]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
upload_speed = 921600
lib_deps =
    bodmer/TFT_eSPI@2.5.43
    https://github.com/PaulStoffregen/XPT2046_Touchscreen.git
    bblanchon/ArduinoJson@^7.0.0
build_flags =
    -DUSER_SETUP_LOADED
    -DILI9341_2_DRIVER
    -DTFT_MOSI=13
    -DTFT_MISO=12
    -DTFT_SCLK=14
    -DTFT_CS=15
    -DTFT_DC=2
    -DTFT_RST=-1
    -DTOUCH_CS=-1
    -DTFT_INVERSION_ON
    -DLOAD_GLCD
    -DLOAD_FONT2
    -DLOAD_FONT4
    -DLOAD_GFXFF
    -DSMOOTH_FONT
    -DSPI_FREQUENCY=27000000
```

**Step 2: Create minimal `src/main.cpp` skeleton**

```cpp
// AI Terminal for CYD28 (ESP32-2432S028R)
// 270226 Initial scaffold

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"
#include <time.h>

void setup() {
    Serial.begin(115200);
}

void loop() {}
```

**Step 3: Verify it compiles**

```bash
cd /home/alun/esp32/cyd28/ai-touchscreen-kb
pio run
```
Expected: `SUCCESS` (no errors)

**Step 4: Commit**

```bash
git init
git add platformio.ini src/main.cpp
git commit -m "270226 Initial scaffold"
```

---

## Task 2: Display init, constants, backlight

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add constants and display init**

Replace the skeleton with the following (keep the includes from Task 1):

```cpp
// --- Credentials (fill in before Task 9) ---
const char* WIFI_SSID       = "YOUR_SSID";
const char* WIFI_PASSWORD   = "YOUR_PASSWORD";
const char* SA_EMAIL        = "YOUR_SA_EMAIL";
const char* SA_PRIVATE_KEY  = "-----BEGIN PRIVATE KEY-----\n"
                               "YOUR_KEY_HERE\n"
                               "-----END PRIVATE KEY-----\n";
const char* GCP_PROJECT     = "YOUR_PROJECT_ID";
const char* GCP_REGION      = "us-central1";
const char* GEMINI_MODEL    = "gemini-2.0-flash";

// --- Hardware pins ---
#define TFT_BL          21
#define TOUCH_SCLK      25
#define TOUCH_MISO      39
#define TOUCH_MOSI      32
#define TOUCH_CS_PIN    33
#define TOUCH_IRQ       36

// --- Screen dimensions ---
#define SCREEN_W        320
#define SCREEN_H        240

// --- Layout (KB shown) ---
#define HIST_Y_TOP      0
#define HIST_H_KB_SHOW  120
#define IBAR_Y_KB_SHOW  120
#define IBAR_H_KB_SHOW   20
#define KB_Y            140
#define KB_H            100

// --- Layout (KB hidden) ---
#define HIST_H_KB_HIDE  210
#define IBAR_Y_KB_HIDE  210
#define IBAR_H_KB_HIDE   30

// --- Colours ---
#define COL_BG          TFT_BLACK
#define COL_USER        TFT_CYAN
#define COL_AI          TFT_YELLOW
#define COL_ERROR       TFT_RED
#define COL_KEY_FACE    0x4208   // dark grey
#define COL_KEY_LABEL   TFT_WHITE
#define COL_IBAR_BG     0x2104   // very dark grey
#define COL_IBAR_TEXT   TFT_WHITE
#define COL_BTN_BG      0x2945   // mid grey
#define COL_BTN_TEXT    TFT_WHITE

// --- Objects ---
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ);

void setup() {
    Serial.begin(115200);

    // Backlight
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // Display
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(COL_BG);

    // Touch
    touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS_PIN);
    ts.begin(touchSPI);
    ts.setRotation(1);

    // Drain spurious startup touch
    unsigned long t0 = millis();
    while (ts.touched() && millis() - t0 < 500) delay(10);
}

void loop() {}
```

**Step 2: Compile**

```bash
pio run
```
Expected: `SUCCESS`

**Step 3: Flash and verify backlight + black screen**

```bash
pio run -t upload
```
Expected: screen turns on, shows black.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Display init, constants, backlight"
```

---

## Task 3: Keyboard rendering

**Files:**
- Modify: `src/main.cpp`

The keyboard has 4 rows. Keys are 28×24px with 2px horizontal gap. Special keys on the right of rows 1 and 2. Row 4 has a wide space bar and CLR.

**Step 1: Add keyboard data and draw function**

Add after the globals (before `setup()`):

```cpp
// --- Keyboard layout ---
bool kbVisible = true;

const char* KB_ROW1 = "QWERTYUIOP";
const char* KB_ROW2 = "ASDFGHJKL";
const char* KB_ROW3 = "ZXCVBNM";

#define KEY_W       28
#define KEY_H       24
#define KEY_GAP      2
#define KB_ROW1_X    2
#define KB_ROW2_X    7
#define KB_ROW3_X   16

// Special key widths
#define HIDE_W      42   // [Hide KB] / [Show KB]
#define BS_W        36   // [⌫]
#define SPACE_W    160
#define CLR_W       60

void drawKey(int x, int y, int w, int h, const char* label, uint16_t face, uint16_t text) {
    tft.fillRoundRect(x, y, w, h, 3, face);
    tft.setTextColor(text, face);
    tft.setTextSize(1);
    int tx = x + (w - strlen(label) * 6) / 2;
    int ty = y + (h - 8) / 2;
    tft.setCursor(tx, ty);
    tft.print(label);
}

void drawKeyboard() {
    if (!kbVisible) return;
    tft.fillRect(0, KB_Y, SCREEN_W, KB_H, COL_BG);

    // Row 1: QWERTYUIOP + [Hide]
    int x = KB_ROW1_X;
    for (int i = 0; i < 10; i++) {
        char label[2] = { KB_ROW1[i], 0 };
        drawKey(x, KB_Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W + KEY_GAP;
    }
    drawKey(SCREEN_W - HIDE_W - 1, KB_Y, HIDE_W, KEY_H, "Hide", COL_BTN_BG, COL_BTN_TEXT);

    // Row 2: ASDFGHJKL + [⌫]
    x = KB_ROW2_X;
    int row2Y = KB_Y + KEY_H + KEY_GAP;
    for (int i = 0; i < 9; i++) {
        char label[2] = { KB_ROW2[i], 0 };
        drawKey(x, row2Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W + KEY_GAP;
    }
    drawKey(SCREEN_W - BS_W - 1, row2Y, BS_W, KEY_H, "<-", COL_BTN_BG, COL_BTN_TEXT);

    // Row 3: ZXCVBNM
    x = KB_ROW3_X;
    int row3Y = KB_Y + 2 * (KEY_H + KEY_GAP);
    for (int i = 0; i < 7; i++) {
        char label[2] = { KB_ROW3[i], 0 };
        drawKey(x, row3Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W + KEY_GAP;
    }

    // Row 4: SPACE + CLR
    int row4Y = KB_Y + 3 * (KEY_H + KEY_GAP);
    drawKey(2,                    row4Y, SPACE_W, KEY_H + 2, "SPACE", COL_BTN_BG, COL_BTN_TEXT);
    drawKey(SCREEN_W - CLR_W - 2, row4Y, CLR_W,  KEY_H + 2, "CLR",   COL_BTN_BG, COL_BTN_TEXT);
}
```

**Step 2: Call `drawKeyboard()` from `setup()`**

Add at end of `setup()`:
```cpp
    drawKeyboard();
```

**Step 3: Compile and flash**

```bash
pio run -t upload
```
Expected: keyboard drawn on lower portion of screen.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Keyboard rendering"
```

---

## Task 4: Input bar rendering

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add input buffer and `drawInputBar()`**

Add after keyboard globals:

```cpp
// --- Input buffer ---
char inputBuf[128] = {0};
int  inputLen      = 0;

void drawInputBar() {
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;

    tft.fillRect(0, barY, SCREEN_W, barH, COL_IBAR_BG);

    // Prompt marker
    tft.setTextColor(COL_IBAR_TEXT, COL_IBAR_BG);
    tft.setTextSize(1);
    tft.setCursor(2, barY + (barH - 8) / 2);
    tft.print("> ");

    // Input text (truncate if too long to fit)
    int maxChars = (SCREEN_W - 60) / 6;  // leave room for Send button
    char display[54] = {0};
    int start = (inputLen > maxChars) ? inputLen - maxChars : 0;
    strncpy(display, inputBuf + start, maxChars);
    tft.print(display);

    // Send button
    tft.fillRect(SCREEN_W - 46, barY + 2, 44, barH - 4, COL_BTN_BG);
    tft.setTextColor(COL_BTN_TEXT, COL_BTN_BG);
    tft.setCursor(SCREEN_W - 39, barY + (barH - 8) / 2);
    tft.print("Send");

    // Show KB button (only when KB hidden)
    if (!kbVisible) {
        tft.fillRect(SCREEN_W - 110, barY + 2, 58, barH - 4, COL_BTN_BG);
        tft.setTextColor(COL_BTN_TEXT, COL_BTN_BG);
        tft.setCursor(SCREEN_W - 107, barY + (barH - 8) / 2);
        tft.print("Show KB");
    }
}
```

**Step 2: Call `drawInputBar()` from `setup()` after `drawKeyboard()`**

```cpp
    drawInputBar();
```

**Step 3: Compile and flash**

```bash
pio run -t upload
```
Expected: input bar visible above keyboard with `>` prompt and `Send` button.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Input bar rendering"
```

---

## Task 5: Conversation history rendering

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add history data structures and renderer**

Add after input buffer globals:

```cpp
// --- Conversation history ---
struct Message {
    bool   isUser;
    bool   isError;
    char   text[512];
};

static const int  MAX_MESSAGES = 20;
Message           history[MAX_MESSAGES];
int               historyCount = 0;

// Rendered line cache
static const int  MAX_LINES  = 200;
char              lines[MAX_LINES][54];   // 53 chars + null per line
uint16_t          lineColor[MAX_LINES];
int               lineCount    = 0;
int               scrollOffset = 0;       // lines scrolled up from bottom

void rebuildLines() {
    lineCount = 0;
    for (int m = 0; m < historyCount && lineCount < MAX_LINES - 2; m++) {
        uint16_t col = history[m].isError ? COL_ERROR :
                       history[m].isUser  ? COL_USER  : COL_AI;
        const char* prefix = history[m].isUser ? "You: " : "AI:  ";
        char full[520];
        snprintf(full, sizeof(full), "%s%s", prefix, history[m].text);

        // Word-wrap at 53 chars
        int len = strlen(full);
        int pos = 0;
        while (pos < len && lineCount < MAX_LINES - 1) {
            int end = pos + 53;
            if (end >= len) {
                strncpy(lines[lineCount], full + pos, 53);
                lines[lineCount][len - pos] = '\0';
            } else {
                // Back up to last space
                int cut = end;
                while (cut > pos && full[cut] != ' ') cut--;
                if (cut == pos) cut = end;  // no space found — hard break
                strncpy(lines[lineCount], full + pos, cut - pos);
                lines[lineCount][cut - pos] = '\0';
                end = cut + (full[cut] == ' ' ? 1 : 0);
            }
            lineColor[lineCount] = col;
            lineCount++;
            pos = (pos + 53 < len) ? (full[pos + 53] == ' ' ? pos + 54 : end) : len;
            // Simpler: advance by chars written
            pos += strlen(lines[lineCount - 1]);
            if (pos < len && full[pos] == ' ') pos++;
        }
    }
}

void drawHistory() {
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    tft.fillRect(0, 0, SCREEN_W, histH, COL_BG);

    int lineH    = 10;   // 8px font + 2px gap
    int maxVis   = histH / lineH;

    // scrollOffset=0 means show bottom of history
    int total    = lineCount;
    int firstIdx = total - maxVis - scrollOffset;
    if (firstIdx < 0) firstIdx = 0;

    for (int i = 0; i < maxVis && (firstIdx + i) < total; i++) {
        int idx = firstIdx + i;
        tft.setTextColor(lineColor[idx], COL_BG);
        tft.setTextSize(1);
        tft.setCursor(0, i * lineH);
        tft.print(lines[idx]);
    }
}

void addMessage(bool isUser, bool isError, const char* text) {
    if (historyCount >= MAX_MESSAGES) {
        // Drop oldest two (user+AI pair)
        memmove(history, history + 2, (MAX_MESSAGES - 2) * sizeof(Message));
        historyCount -= 2;
    }
    history[historyCount].isUser  = isUser;
    history[historyCount].isError = isError;
    strncpy(history[historyCount].text, text, 511);
    history[historyCount].text[511] = '\0';
    historyCount++;
    scrollOffset = 0;   // auto-scroll to bottom
    rebuildLines();
    drawHistory();
}
```

**Step 2: Add a test message in `setup()`**

```cpp
    addMessage(true,  false, "Hello world, this is a test message that is quite long to test word wrapping");
    addMessage(false, false, "This is a test AI response. It should appear in yellow and also wrap if it is long enough.");
```

**Step 3: Compile and flash**

```bash
pio run -t upload
```
Expected: two wrapped messages in history area — cyan user, yellow AI.

**Step 4: Remove test messages from `setup()` after verifying**

Delete the two `addMessage(...)` lines added in Step 2.

**Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Conversation history rendering and word wrap"
```

---

## Task 6: Touch handling — keyboard input

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add touch coordinate mapper and key hit-test**

Add before `loop()`:

```cpp
// --- Touch coordinate mapping ---
// Raw XPT2046 range ~200–3900 → screen pixels
void mapTouch(TS_Point& tp, int& sx, int& sy) {
    sx = map(tp.x, 200, 3900, 0, SCREEN_W - 1);
    sy = map(tp.y, 200, 3900, 0, SCREEN_H - 1);
}

// --- Debounce ---
unsigned long lastTouchMs = 0;
#define TOUCH_DEBOUNCE_MS 250

bool touchReady() {
    if (!ts.touched()) return false;
    if (millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) {
        while (ts.touched()) delay(10);
        return false;
    }
    return true;
}

// --- Key hit-test helpers ---
bool inRect(int sx, int sy, int rx, int ry, int rw, int rh) {
    return sx >= rx && sx < rx + rw && sy >= ry && sy < ry + rh;
}

// Returns key char if a letter key was hit, '\0' otherwise
char hitTestKB(int sx, int sy) {
    // Row 1
    for (int i = 0; i < 10; i++) {
        int kx = KB_ROW1_X + i * (KEY_W + KEY_GAP);
        if (inRect(sx, sy, kx, KB_Y, KEY_W, KEY_H)) return KB_ROW1[i];
    }
    // Row 2
    int row2Y = KB_Y + KEY_H + KEY_GAP;
    for (int i = 0; i < 9; i++) {
        int kx = KB_ROW2_X + i * (KEY_W + KEY_GAP);
        if (inRect(sx, sy, kx, row2Y, KEY_W, KEY_H)) return KB_ROW2[i];
    }
    // Row 3
    int row3Y = KB_Y + 2 * (KEY_H + KEY_GAP);
    for (int i = 0; i < 7; i++) {
        int kx = KB_ROW3_X + i * (KEY_W + KEY_GAP);
        if (inRect(sx, sy, kx, row3Y, KEY_W, KEY_H)) return KB_ROW3[i];
    }
    return '\0';
}

void handleTouch();  // forward declaration
```

**Step 2: Implement `handleTouch()`**

```cpp
void handleTouch() {
    if (!touchReady()) return;

    TS_Point tp = ts.getPoint();
    while (ts.touched()) delay(10);   // wait for release
    lastTouchMs = millis();

    int sx, sy;
    mapTouch(tp, sx, sy);

    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;

    // --- Send button ---
    if (inRect(sx, sy, SCREEN_W - 46, barY + 2, 44, barH - 4)) {
        if (inputLen > 0) {
            // placeholder — wired up in Task 10
            addMessage(true, false, inputBuf);
            inputBuf[0] = '\0';
            inputLen    = 0;
            drawInputBar();
        }
        return;
    }

    // --- Show KB button (only when hidden) ---
    if (!kbVisible && inRect(sx, sy, SCREEN_W - 110, barY + 2, 58, barH - 4)) {
        kbVisible = true;
        tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
        drawHistory();
        drawInputBar();
        drawKeyboard();
        return;
    }

    // --- Keyboard area ---
    if (kbVisible && sy >= KB_Y) {
        int row2Y = KB_Y + KEY_H + KEY_GAP;
        int row3Y = KB_Y + 2 * (KEY_H + KEY_GAP);
        int row4Y = KB_Y + 3 * (KEY_H + KEY_GAP);

        // Hide KB button (row 1 right)
        if (inRect(sx, sy, SCREEN_W - HIDE_W - 1, KB_Y, HIDE_W, KEY_H)) {
            kbVisible = false;
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            drawHistory();
            drawInputBar();
            return;
        }

        // Backspace (row 2 right)
        if (inRect(sx, sy, SCREEN_W - BS_W - 1, row2Y, BS_W, KEY_H)) {
            if (inputLen > 0) { inputBuf[--inputLen] = '\0'; drawInputBar(); }
            return;
        }

        // Space (row 4 left)
        if (inRect(sx, sy, 2, row4Y, SPACE_W, KEY_H + 2)) {
            if (inputLen < 127) { inputBuf[inputLen++] = ' '; inputBuf[inputLen] = '\0'; drawInputBar(); }
            return;
        }

        // CLR (row 4 right)
        if (inRect(sx, sy, SCREEN_W - CLR_W - 2, row4Y, CLR_W, KEY_H + 2)) {
            inputBuf[0] = '\0'; inputLen = 0; drawInputBar();
            return;
        }

        // Letter key
        char c = hitTestKB(sx, sy);
        if (c && inputLen < 127) {
            inputBuf[inputLen++] = c;
            inputBuf[inputLen]   = '\0';
            drawInputBar();
        }
        return;
    }

    // --- History scroll (left 15px strip) ---
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    if (sx < 15 && sy < histH) {
        int mid = histH / 2;
        if (sy < mid) {
            // Scroll up (show older)
            int maxVis   = histH / 10;
            int maxScroll = lineCount - maxVis;
            if (maxScroll > 0 && scrollOffset < maxScroll) { scrollOffset++; drawHistory(); }
        } else {
            // Scroll down (show newer)
            if (scrollOffset > 0) { scrollOffset--; drawHistory(); }
        }
    }
}
```

**Step 3: Call `handleTouch()` from `loop()`**

```cpp
void loop() {
    handleTouch();
}
```

**Step 4: Compile and flash**

```bash
pio run -t upload
```
Expected: tapping letters appends to input bar; backspace removes; CLR clears; Hide/Show KB toggles keyboard.

**Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Touch handling and keyboard input"
```

---

## Task 7: WiFi connection and NTP

**Files:**
- Modify: `src/main.cpp`

Fill in `WIFI_SSID` and `WIFI_PASSWORD` constants now.

**Step 1: Add WiFi connect function**

Add before `setup()`:

```cpp
void connectWiFi() {
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.setTextSize(1);
    tft.setCursor(2, 2);
    tft.print("Connecting WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
    }
    tft.fillRect(0, 0, SCREEN_W, 12, COL_BG);
    if (WiFi.status() != WL_CONNECTED) {
        addMessage(false, true, "WiFi connect failed");
    }
}

void syncTime() {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    // Wait up to 5s for time
    struct tm ti;
    int attempts = 0;
    while (!getLocalTime(&ti) && attempts < 10) { delay(500); attempts++; }
}
```

**Step 2: Call from `setup()` before `drawHistory()` etc.**

```cpp
    connectWiFi();
    syncTime();
```

**Step 3: Compile and flash**

```bash
pio run -t upload
```
Expected: brief "Connecting WiFi..." on screen, then clears. Check Serial monitor (115200 baud) — no error.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "270226 WiFi connection and NTP sync"
```

---

## Task 8: Base64url utility

**Files:**
- Modify: `src/main.cpp`

Standard base64 + URL-safe alphabet (replace `+`→`-`, `/`→`_`, strip `=` padding). Needed for JWT.

**Step 1: Add base64url encode function**

Add before WiFi functions:

```cpp
// --- Base64url encoding ---
String base64url(const uint8_t* data, size_t len) {
    size_t b64Len = ((len + 2) / 3) * 4 + 1;
    uint8_t* b64 = (uint8_t*)malloc(b64Len);
    size_t olen = 0;
    mbedtls_base64_encode(b64, b64Len, &olen, data, len);
    b64[olen] = '\0';
    String s = (char*)b64;
    free(b64);
    // Make URL-safe
    s.replace('+', '-');
    s.replace('/', '_');
    while (s.endsWith("=")) s.remove(s.length() - 1);
    return s;
}

String base64urlStr(const char* str) {
    return base64url((const uint8_t*)str, strlen(str));
}
```

**Step 2: Compile**

```bash
pio run
```
Expected: `SUCCESS`

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Base64url utility"
```

---

## Task 9: JWT construction and signing

**Files:**
- Modify: `src/main.cpp`

**Before this task:** Fill in `SA_EMAIL` and `SA_PRIVATE_KEY` constants. The private key must use actual newlines (`\n` escape sequences in the string literal, not literal backslash-n). Copy it exactly from the service account JSON, then replace the literal `\n` characters with `\n` in C++. Example:

```cpp
const char* SA_PRIVATE_KEY =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC...\n"
    ...
    "-----END PRIVATE KEY-----\n";
```

**Step 1: Add JWT builder**

```cpp
// --- JWT / OAuth2 ---
String buildJWT() {
    // Header
    const char* header = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    String h = base64urlStr(header);

    // Payload
    time_t now = time(nullptr);
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"iss\":\"%s\","
        "\"scope\":\"https://www.googleapis.com/auth/cloud-platform\","
        "\"aud\":\"https://oauth2.googleapis.com/token\","
        "\"iat\":%ld,"
        "\"exp\":%ld}",
        SA_EMAIL, (long)now, (long)(now + 3600));
    String p = base64urlStr(payload);

    String sigInput = h + "." + p;

    // Sign with RS256
    mbedtls_pk_context     pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, nullptr, 0);

    int ret = mbedtls_pk_parse_key(&pk,
        (const unsigned char*)SA_PRIVATE_KEY,
        strlen(SA_PRIVATE_KEY) + 1,
        nullptr, 0,
        mbedtls_ctr_drbg_random, &ctr_drbg);

    if (ret != 0) {
        mbedtls_pk_free(&pk);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        Serial.printf("JWT: pk_parse_key failed: -0x%04X\n", (unsigned)(-ret));
        return "";
    }

    // Hash sigInput
    uint8_t hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t*)sigInput.c_str(), sigInput.length(), hash);

    // Sign
    uint8_t sig[512];
    size_t  sigLen = 0;
    ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                          sig, sizeof(sig), &sigLen,
                          mbedtls_ctr_drbg_random, &ctr_drbg);

    mbedtls_pk_free(&pk);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);

    if (ret != 0) {
        Serial.printf("JWT: pk_sign failed: -0x%04X\n", (unsigned)(-ret));
        return "";
    }

    return sigInput + "." + base64url(sig, sigLen);
}
```

**Step 2: Compile**

```bash
pio run
```
Expected: `SUCCESS`

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "270226 JWT construction and RS256 signing"
```

---

## Task 10: OAuth2 token fetch and caching

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add token cache and fetch function**

Add after `buildJWT()`:

```cpp
// --- OAuth2 token cache ---
char   oauthToken[2048] = {0};
time_t tokenExpiry      = 0;

bool fetchOAuthToken() {
    String jwt = buildJWT();
    if (jwt.length() == 0) { return false; }

    WiFiClientSecure client;
    client.setInsecure();   // skip cert verification — acceptable for personal device

    if (!client.connect("oauth2.googleapis.com", 443)) {
        Serial.println("OAuth2: connect failed");
        return false;
    }

    String body = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + jwt;
    client.printf(
        "POST /token HTTP/1.1\r\n"
        "Host: oauth2.googleapis.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        body.length());
    client.print(body);

    // Skip response headers
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    String respBody = client.readString();
    client.stop();

    JsonDocument doc;
    if (deserializeJson(doc, respBody) != DeserializationError::Ok) {
        Serial.println("OAuth2: JSON parse failed");
        return false;
    }

    const char* token = doc["access_token"];
    int expiresIn     = doc["expires_in"] | 3600;
    if (!token) {
        Serial.println("OAuth2: no access_token in response");
        Serial.println(respBody);
        return false;
    }

    strncpy(oauthToken, token, sizeof(oauthToken) - 1);
    tokenExpiry = time(nullptr) + expiresIn - 300;  // refresh 5 min early
    Serial.println("OAuth2: token obtained");
    return true;
}

bool ensureToken() {
    if (oauthToken[0] != '\0' && time(nullptr) < tokenExpiry) return true;
    return fetchOAuthToken();
}
```

**Step 2: Compile**

```bash
pio run
```
Expected: `SUCCESS`

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "270226 OAuth2 token fetch and caching"
```

---

## Task 11: Vertex AI API call

**Files:**
- Modify: `src/main.cpp`

Fill in `GCP_PROJECT` and `GCP_REGION` constants now.

**Step 1: Add `callGemini()` function**

```cpp
// --- Vertex AI call ---
String callGemini(const char* prompt) {
    // Add current prompt to history temporarily so it's included in request
    // (caller adds it permanently after successful response)

    // Build host
    char host[80];
    snprintf(host, sizeof(host), "%s-aiplatform.googleapis.com", GCP_REGION);

    // Build endpoint path
    char path[256];
    snprintf(path, sizeof(path),
        "/v1/projects/%s/locations/%s/publishers/google/models/%s:generateContent",
        GCP_PROJECT, GCP_REGION, GEMINI_MODEL);

    // Build JSON body
    JsonDocument reqDoc;
    JsonObject sysInstr = reqDoc["system_instruction"].to<JsonObject>();
    JsonArray  sysParts = sysInstr["parts"].to<JsonArray>();
    sysParts.add(JsonObject());
    sysParts[0]["text"] = "Respond in 150 words or fewer.";

    JsonArray contents = reqDoc["contents"].to<JsonArray>();

    // Add history
    for (int i = 0; i < historyCount; i++) {
        JsonObject msg = contents.add<JsonObject>();
        msg["role"]    = history[i].isUser ? "user" : "model";
        JsonArray parts = msg["parts"].to<JsonArray>();
        parts.add(JsonObject());
        parts[0]["text"] = history[i].text;
    }

    // Add current prompt
    JsonObject curMsg = contents.add<JsonObject>();
    curMsg["role"]    = "user";
    JsonArray curParts = curMsg["parts"].to<JsonArray>();
    curParts.add(JsonObject());
    curParts[0]["text"] = prompt;

    String body;
    serializeJson(reqDoc, body);

    // Send request
    WiFiClientSecure client;
    client.setInsecure();

    if (!client.connect(host, 443)) {
        return "ERR: connect failed";
    }

    client.printf(
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        path, host, oauthToken, body.length());
    client.print(body);

    // Read status line
    String statusLine = client.readStringUntil('\n');
    bool ok = statusLine.indexOf("200") > 0;

    // Skip headers
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    String respBody = client.readString();
    client.stop();

    if (!ok) {
        Serial.println("Gemini error response:");
        Serial.println(respBody);
        return "ERR: API returned non-200";
    }

    JsonDocument respDoc;
    if (deserializeJson(respDoc, respBody) != DeserializationError::Ok) {
        return "ERR: JSON parse failed";
    }

    const char* text = respDoc["candidates"][0]["content"]["parts"][0]["text"];
    if (!text) return "ERR: no text in response";

    return String(text);
}
```

**Step 2: Compile**

```bash
pio run
```
Expected: `SUCCESS`

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Vertex AI Gemini API call"
```

---

## Task 12: Wire the send flow

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add `showThinking()` and `sendPrompt()` functions**

```cpp
void showThinking() {
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;
    tft.fillRect(0, barY, SCREEN_W - 46, barH, COL_IBAR_BG);
    tft.setTextColor(TFT_DARKGREY, COL_IBAR_BG);
    tft.setTextSize(1);
    tft.setCursor(2, barY + (barH - 8) / 2);
    tft.print("Thinking...");
}

void sendPrompt() {
    if (inputLen == 0) return;

    char prompt[128];
    strncpy(prompt, inputBuf, 127);
    prompt[127] = '\0';

    // Clear input
    inputBuf[0] = '\0';
    inputLen    = 0;

    // Show user message and thinking indicator
    addMessage(true, false, prompt);
    showThinking();

    // Ensure valid token
    if (!ensureToken()) {
        addMessage(false, true, "Auth failed — check SA credentials");
        drawInputBar();
        return;
    }

    // Call API
    String response = callGemini(prompt);

    if (response.startsWith("ERR:")) {
        addMessage(false, true, response.c_str());
    } else {
        addMessage(false, false, response.c_str());
    }

    drawInputBar();
}
```

**Step 2: Replace the placeholder `addMessage` in `handleTouch()` Send block**

Find in `handleTouch()`:
```cpp
        if (inputLen > 0) {
            // placeholder — wired up in Task 10
            addMessage(true, false, inputBuf);
            inputBuf[0] = '\0';
            inputLen    = 0;
            drawInputBar();
        }
```
Replace with:
```cpp
        if (inputLen > 0) {
            sendPrompt();
        }
```

**Step 3: Compile and flash**

```bash
pio run -t upload
```
Expected: type a message, tap Send → user message appears in cyan, "Thinking..." shows, then AI response in yellow.

Check Serial monitor for any error output if it doesn't work.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "270226 Wire send flow — thinking indicator and full API round-trip"
```

---

## Task 13: Final polish and memory tuning

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add stack/heap diagnostic to `setup()` (temporary)**

```cpp
    Serial.printf("Free heap after init: %d bytes\n", ESP.getFreeHeap());
```

Flash and check Serial. The `callGemini()` JSON document and response string are the largest consumers. If heap is below 20 KB, reduce `MAX_MESSAGES` or `MAX_LINES`.

**Step 2: Tune if needed**

If free heap < 20 KB, reduce:
```cpp
static const int MAX_MESSAGES = 10;   // was 20
static const int MAX_LINES    = 100;  // was 200
```

**Step 3: Remove heap diagnostic line**

Delete the `Serial.printf("Free heap...")` line.

**Step 4: Final compile and flash**

```bash
pio run -t upload
```

**Step 5: Final commit**

```bash
git add src/main.cpp
git commit -m "270226 Polish and memory tuning"
```

---

## Notes

- `client.setInsecure()` skips TLS certificate verification — fine for a personal device on a trusted home network
- The private key string must have actual `\n` newlines, not escaped `\\n`
- If JWT signing fails (`-0x3E80` = `MBEDTLS_ERR_PK_KEY_INVALID_FORMAT`), the key format is wrong — check PKCS#8 vs PKCS#1
- Token refresh happens automatically via `ensureToken()` before each API call
- ArduinoJson v7 uses `JsonDocument` (no size arg); the request doc can be large — ensure enough heap
