// AI Terminal for CYD28 (ESP32-2432S028R)
// 270226 Initial scaffold
// 270226 Display init, constants, backlight
// 270226 Keyboard rendering
// 270226 Input bar rendering
// 270226 Conversation history rendering
// 270226 Touch handling

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

// --- Keyboard layout ---
bool kbVisible = true;

const char* KB_ROW1 = "QWERTYUIOP";
const char* KB_ROW2 = "ASDFGHJKL";
const char* KB_ROW3 = "ZXCVBNM";

#define KEY_W       28
#define KEY_H       24
#define KEY_GAP      1
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
    drawKey(2,                    row4Y, SPACE_W, KEY_H, "SPACE", COL_BTN_BG, COL_BTN_TEXT);
    drawKey(SCREEN_W - CLR_W - 2, row4Y, CLR_W,  KEY_H, "CLR",   COL_BTN_BG, COL_BTN_TEXT);
}

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
            int remaining = len - pos;
            if (remaining <= 53) {
                strncpy(lines[lineCount], full + pos, remaining);
                lines[lineCount][remaining] = '\0';
                pos = len;
            } else {
                // Back up to last space
                int cut = pos + 53;
                while (cut > pos && full[cut] != ' ') cut--;
                if (cut == pos) cut = pos + 53;  // no space found — hard break
                int count = cut - pos;
                strncpy(lines[lineCount], full + pos, count);
                lines[lineCount][count] = '\0';
                pos = cut + (full[cut] == ' ' ? 1 : 0);
            }
            lineColor[lineCount] = col;
            lineCount++;
        }
    }
}

void drawHistory() {
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    tft.fillRect(0, 0, SCREEN_W, histH, COL_BG);

    int lineH    = 10;   // 8px font + 2px gap
    int maxVis   = histH / lineH;

    // scrollOffset=0 means show bottom of history
    int firstIdx = lineCount - maxVis - scrollOffset;
    if (firstIdx < 0) firstIdx = 0;

    for (int i = 0; i < maxVis && (firstIdx + i) < lineCount; i++) {
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

void handleTouch() {
    if (!touchReady()) return;

    TS_Point tp = ts.getPoint();
    while (ts.touched()) delay(10);   // wait for release
    lastTouchMs = millis();

    int sx, sy;
    mapTouch(tp, sx, sy);
    if (sx < 0 || sy < 0 || sx >= SCREEN_W || sy >= SCREEN_H) return;

    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;

    // --- Send button ---
    if (inRect(sx, sy, SCREEN_W - 46, barY + 2, 44, barH - 4)) {
        if (inputLen > 0) {
            // placeholder — wired up in Task 12
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
        if (inRect(sx, sy, 2, row4Y, SPACE_W, KEY_H)) {
            if (inputLen < 127) { inputBuf[inputLen++] = ' '; inputBuf[inputLen] = '\0'; drawInputBar(); }
            return;
        }

        // CLR (row 4 right)
        if (inRect(sx, sy, SCREEN_W - CLR_W - 2, row4Y, CLR_W, KEY_H)) {
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
            int maxVis    = histH / 10;
            int maxScroll = lineCount - maxVis;
            if (maxScroll > 0 && scrollOffset < maxScroll) { scrollOffset++; drawHistory(); }
        } else {
            // Scroll down (show newer)
            if (scrollOffset > 0) { scrollOffset--; drawHistory(); }
        }
    }
}

void setup() {
    Serial.begin(115200);

    // Display (init before backlight to avoid white flash)
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(COL_BG);

    // Backlight on after screen is ready
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // Touch
    touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS_PIN);
    ts.begin(touchSPI);
    ts.setRotation(1);

    // Drain spurious startup touch
    unsigned long t0 = millis();
    while (ts.touched() && millis() - t0 < 500) delay(10);

    drawKeyboard();
    drawInputBar();
}

void loop() {
    handleTouch();
}
