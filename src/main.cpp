// CYD AI chatbot for CYD28 (ESP32-2432S028R)
// 280226 Add Grok API (xAI), key 4 at boot; route callGemini/callGrok via useGrok flag
// 280226 Bold font: replace Font 2 with FreeSansBold9pt7b, update LINE_H_LARGE=16 SPLASH_H=48
// 280226 RGB LED WiFi signal strength: blue=strong, cyan, green, orange, red=lost
// 270226 Fix large-font truncation: grow Message.text→2048, full→2060, buffers; setTextWrap(false)
// 270226 WiFi health ping, red > on fail, reconnect; global endpoint for Flash
// 270226 Alt key: number row + ,./ alt layer, mutual exclusion with shift
// 270226 Memory tuning and final polish
// 270226 Initial scaffold
// 270226 Display init, constants, backlight
// 270226 Keyboard rendering
// 270226 Input bar rendering
// 270226 Conversation history rendering
// 270226 Touch handling
// 270226 WiFi and NTP
// 270226 Base64url utility
// 270226 Switch to Gemini API key, add callGemini()
// 270226 Send flow wired

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
// FreeSansBold9pt7b (FF25/FSSB9) is already included by TFT_eSPI via gfxfont.h when LOAD_GFXFF is set
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP32Ping.h>

// --- Credentials ---
const char* WIFI_SSID       = "PlusnetWireless_EXT";
const char* WIFI_PASSWORD   = "WPAkeykey";
const char* GEMINI_API_KEY  = "GEMINI_KEY_REMOVED";
const char* GROK_API_KEY    = "GROK_KEY_REMOVED";
char        GEMINI_MODEL[48]  = "gemini-3.1-pro-preview"; // overwritten at boot
bool        geminiUseGlobal   = false;  // true → /locations/global/ in path
bool        useGrok           = false;  // true → route to Grok API instead of Gemini
bool        largeFont         = false;  // true → FreeSansBold9pt7b in history area
bool        invertDisplay     = false;  // true → light grey bg, black text in history area
#define COL_INVERT_BG   0xC618          // light grey (~RGB 192,192,192)

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

// --- Screen dimensions ---
#define SCREEN_W        320
#define SCREEN_H        240

// --- Layout (KB shown) ---
#define HIST_Y_TOP      0
#define HIST_H_KB_SHOW  116
#define IBAR_Y_KB_SHOW  116
#define IBAR_H_KB_SHOW   20
#define KB_Y            136
#define KB_H            104

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
bool shiftOn   = false;   // starts lowercase
bool altOn     = false;   // alt layer off by default

const char* KB_ROW1          = "QWERTYUIOP";
const char* KB_ROW2          = "ASDFGHJKL";
const char* KB_ROW3          = "ZXCVBNM";
const char* KB_NUM_UNSHIFTED = "1234567890";
const char* KB_NUM_SHIFTED   = "!@#$%^&*()";

// Alt layer for number row
const char* KB_NUM_ALT_TYPED[10] = { "|", "\"", ":", "{", "}", "'", "@", "-", "+", "=" };
const char* KB_NUM_ALT_DISP[10]  = { "|", "\"", ":", "{", "}", "'", "@", "-", "+", "=" };

#define KEY_W       28
#define KEY_H       20
#define KEY_GAP      1
#define KB_ROW1_X   16   // row 1 centred (10 keys = 289px, (320-289)/2 = 16)
#define KB_ROW2_X    7   // row 2 offset (9 keys + BS right-aligned)
#define KB_ROW3_X   16   // row 0 & row 3 (10 keys each, centred)

// Row 4 special key widths and positions
// 2+[Shift 40]+2+[Alt 40]+2+[Space 146]+2+[Hide 42]+2+[CLR 40]+2 = 320
#define SHIFT_W     40
#define SHIFT_X      2
#define ALT_W       40
#define ALT_X       44
#define SPACE_W    146
#define SPACE_X     86
#define HIDE_W      42
#define HIDE_X     234
#define CLR_W       40
#define CLR_X      278
#define BS_W        36   // [⌫] on row 2 right

// --- Key appearance ---
#define KEY_RADIUS        3     // rounded corner radius for keys

// --- Layout metrics ---
#define LINE_H_LARGE     16     // FreeSansBold9pt7b line height (yAdvance=16)
#define LINE_H_SMALL     10     // GLCD line height (8px + 2px gap)
#define SPLASH_H         48     // height of boot splash area to clear after connect (3 × LINE_H_LARGE)

// --- Input bar ---
#define INPUT_BUF_SIZE  128     // input text buffer including null terminator
#define INPUT_MAX_LEN   127     // max typeable characters (INPUT_BUF_SIZE - 1)
#define BTN_SEND_W       46     // width of Send/More button
#define BTN_SEND_X_TEXT  39     // distance of Send/More text from right edge
#define BTN_SHOWKB_W     58     // width of Show KB button
#define BTN_SHOWKB_X    110     // distance of Show KB button left edge from right
#define BTN_INSET         2     // pixel inset for button fill within input bar

// --- Timing ---
#define WIFI_MAX_ATTEMPTS    30    // max retries waiting for WiFi (× WIFI_RETRY_DELAY_MS)
#define WIFI_RETRY_DELAY_MS 500    // ms between WiFi connect retries
#define KEY_FLASH_MS        100    // key highlight flash duration on tap
#define API_TIMEOUT_MS    45000    // API response deadline ms
#define API_WAIT_FIRST_MS  4000    // ms before first waiting message appears

// --- WiFi RSSI thresholds for LED colour ---
#define RSSI_THRESH_BLUE   -55    // ≥ this dBm → blue (strongest)
#define RSSI_THRESH_CYAN   -65    // ≥ this dBm → cyan (good)
#define RSSI_THRESH_GREEN  -75    // ≥ this dBm → green (fair)

// --- LED PWM ---
#define LED_PWM_FREQ     5000    // LEDC PWM frequency Hz
#define LED_PWM_BITS        8    // LEDC resolution bits

// --- API ---
#define HTTPS_PORT        443
#define GEMINI_HOST       "generativelanguage.googleapis.com"
#define GROK_HOST         "eu-west-1.api.x.ai"
#define GROK_MODEL        "grok-4-1-fast-reasoning"

void drawKey(int x, int y, int w, int h, const char* label, uint16_t face, uint16_t text) {
    tft.fillRoundRect(x, y, w, h, KEY_RADIUS, face);
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
    int rowStep = KEY_H + KEY_GAP;
    int x;

    // Row 0: number / symbol / alt keys
    x = KB_ROW3_X;
    for (int i = 0; i < 10; i++) {
        if (altOn) {
            drawKey(x, KB_Y, KEY_W, KEY_H, KB_NUM_ALT_DISP[i], COL_KEY_FACE, COL_KEY_LABEL);
        } else {
            char label[2] = { shiftOn ? KB_NUM_SHIFTED[i] : KB_NUM_UNSHIFTED[i], 0 };
            drawKey(x, KB_Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        }
        x += KEY_W + KEY_GAP;
    }

    // Row 1: QWERTY (centred, no Hide button)
    x = KB_ROW1_X;
    int row1Y = KB_Y + rowStep;
    for (int i = 0; i < 10; i++) {
        char c = shiftOn ? KB_ROW1[i] : (KB_ROW1[i] + 32);
        char label[2] = { c, 0 };
        drawKey(x, row1Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W + KEY_GAP;
    }

    // Row 2: ASDFGHJKL + [⌫]
    x = KB_ROW2_X;
    int row2Y = KB_Y + 2 * rowStep;
    for (int i = 0; i < 9; i++) {
        char c = shiftOn ? KB_ROW2[i] : (KB_ROW2[i] + 32);
        char label[2] = { c, 0 };
        drawKey(x, row2Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W + KEY_GAP;
    }
    drawKey(SCREEN_W - BS_W - 1, row2Y, BS_W, KEY_H, "<-", COL_BTN_BG, COL_BTN_TEXT);

    // Row 3: ZXCVBNM + < > ?
    x = KB_ROW3_X;
    int row3Y = KB_Y + 3 * rowStep;
    for (int i = 0; i < 7; i++) {
        char c = shiftOn ? KB_ROW3[i] : (KB_ROW3[i] + 32);
        char label[2] = { c, 0 };
        drawKey(x, row3Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W + KEY_GAP;
    }
    drawKey(x, row3Y, KEY_W, KEY_H, altOn ? "[" : (shiftOn ? "<" : ","), COL_KEY_FACE, COL_KEY_LABEL); x += KEY_W + KEY_GAP;
    drawKey(x, row3Y, KEY_W, KEY_H, altOn ? "]" : (shiftOn ? ">" : "."), COL_KEY_FACE, COL_KEY_LABEL); x += KEY_W + KEY_GAP;
    drawKey(x, row3Y, KEY_W, KEY_H, altOn ? "\\" : (shiftOn ? "?" : "/"), COL_KEY_FACE, COL_KEY_LABEL);

    // Row 4: [Shift] [Alt] [Space] [Hide] [CLR]
    int row4Y = KB_Y + 4 * rowStep;
    uint16_t shiftFace = shiftOn ? TFT_NAVY : COL_BTN_BG;
    uint16_t altFace   = altOn   ? TFT_NAVY : COL_BTN_BG;
    drawKey(SHIFT_X, row4Y, SHIFT_W, KEY_H, shiftOn ? "SHF" : "shf", shiftFace, COL_BTN_TEXT);
    drawKey(ALT_X,   row4Y, ALT_W,   KEY_H, altOn   ? "ALT" : "alt", altFace,   COL_BTN_TEXT);
    drawKey(SPACE_X, row4Y, SPACE_W, KEY_H, "SPACE", COL_BTN_BG, COL_BTN_TEXT);
    drawKey(HIDE_X,  row4Y, HIDE_W,  KEY_H, "Hide",  COL_BTN_BG, COL_BTN_TEXT);
    drawKey(CLR_X,   row4Y, CLR_W,   KEY_H, "CLR",   COL_BTN_BG, COL_BTN_TEXT);
}

// --- WiFi health ---
bool          wifiHealthy     = true;
unsigned long lastWiFiCheckMs = 0;

// --- Input buffer ---
char inputBuf[INPUT_BUF_SIZE] = {0};
int  inputLen      = 0;
bool moreMode      = false;  // true after AI reply → button shows "More"

void drawInputBar() {
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;

    tft.fillRect(0, barY, SCREEN_W, barH, COL_IBAR_BG);

    // Prompt marker — red when WiFi health check fails
    tft.setTextColor(wifiHealthy ? COL_IBAR_TEXT : TFT_RED, COL_IBAR_BG);
    tft.setTextSize(1);
    tft.setCursor(2, barY + (barH - 8) / 2);
    tft.print("> ");
    tft.setTextColor(COL_IBAR_TEXT, COL_IBAR_BG);  // restore white for input text

    // Input text (truncate if too long to fit)
    int maxChars = (SCREEN_W - BTN_SEND_W - 14) / 6;  // leave room for Send button
    char display[54] = {0};
    int start = (inputLen > maxChars) ? inputLen - maxChars : 0;
    strncpy(display, inputBuf + start, maxChars);
    tft.print(display);

    // Send / More button — text red when WiFi unhealthy
    tft.fillRect(SCREEN_W - BTN_SEND_W, barY + BTN_INSET, BTN_SEND_W - BTN_INSET*2, barH - BTN_INSET*2, COL_BTN_BG);
    tft.setTextColor(wifiHealthy ? COL_BTN_TEXT : TFT_RED, COL_BTN_BG);
    tft.setCursor(SCREEN_W - BTN_SEND_X_TEXT, barY + (barH - 8) / 2);
    tft.print(moreMode ? "More" : "Send");

    // Show KB button (only when KB hidden)
    if (!kbVisible) {
        tft.fillRect(SCREEN_W - BTN_SHOWKB_X, barY + BTN_INSET, BTN_SHOWKB_W, barH - BTN_INSET*2, COL_BTN_BG);
        tft.setTextColor(COL_BTN_TEXT, COL_BTN_BG);
        tft.setCursor(SCREEN_W - BTN_SHOWKB_X + 3, barY + (barH - 8) / 2);
        tft.print("Show KB");
    }
}

// --- Conversation history ---
struct Message {
    bool   isUser;
    bool   isError;
    char   text[2048];
};

static const int  MAX_MESSAGES = 20;
Message           history[MAX_MESSAGES];
int               historyCount = 0;

// Rendered line cache
static const int  MAX_LINES  = 400;
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
        char full[2060];
        snprintf(full, sizeof(full), "%s%s", prefix, history[m].text);
        // Strip control chars and high bytes (text already sanitised in addMessage,
        // but prefix chars from prefix string are always clean ASCII).
        for (int j = 0; full[j]; j++) {
            unsigned char fc = (unsigned char)full[j];
            if ((fc < 32 && full[j] != '\n') || fc > 126) full[j] = ' ';
        }

        if (largeFont) {
            // Pixel-width word wrap for FreeSansBold9pt7b
            tft.setFreeFont(&FreeSansBold9pt7b);
            char lineBuf[54] = "";
            const char* p = full;
            while (*p && lineCount < MAX_LINES - 1) {
                if (*p == '\n') {
                    // Only flush if there's content — skip blank lines to
                    // avoid wasting the small number of visible large-font slots
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 53);
                        lines[lineCount][53] = '\0';
                        lineColor[lineCount++] = col;
                        lineBuf[0] = '\0';
                    }
                    p++;
                    continue;
                }
                // Extract next word
                const char* ws = p;
                while (*p && *p != ' ' && *p != '\n') p++;
                int wlen = p - ws;
                if (wlen <= 0) { if (*p) p++; continue; }
                char word[54];
                strncpy(word, ws, min(wlen, 53));
                word[min(wlen, 53)] = '\0';
                if (*p == ' ') p++;
                // Test word on current line
                char test[108];   // lineBuf(53) + space(1) + word(53) + null
                if (lineBuf[0]) snprintf(test, 108, "%s %s", lineBuf, word);
                else            { strncpy(test, word, 53); test[53] = '\0'; }
                if (tft.textWidth(test) <= SCREEN_W - 2) {
                    strncpy(lineBuf, test, 53); lineBuf[53] = '\0';
                } else {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 53);
                        lines[lineCount][53] = '\0';
                        lineColor[lineCount++] = col;
                        strncpy(lineBuf, word, 53); lineBuf[53] = '\0';
                    } else {
                        strncpy(lines[lineCount], word, 53);
                        lines[lineCount][53] = '\0';
                        lineColor[lineCount++] = col;
                    }
                }
            }
            if (lineBuf[0] && lineCount < MAX_LINES) {
                strncpy(lines[lineCount], lineBuf, 53);
                lines[lineCount][53] = '\0';
                lineColor[lineCount++] = col;
            }
            tft.setTextFont(1);
        } else {
            int len = strlen(full);
            int pos = 0;
            while (pos < len && lineCount < MAX_LINES - 1) {
                // Look for \n within the next 54 chars (forced break wins over word-wrap)
                int nlPos = -1;
                for (int j = pos; j < pos + 54 && j < len; j++) {
                    if (full[j] == '\n') { nlPos = j; break; }
                }
                if (nlPos >= 0) {
                    int count = nlPos - pos;
                    strncpy(lines[lineCount], full + pos, count);
                    lines[lineCount][count] = '\0';
                    lineColor[lineCount++] = col;
                    pos = nlPos + 1;
                } else {
                    int remaining = len - pos;
                    if (remaining <= 53) {
                        strncpy(lines[lineCount], full + pos, remaining);
                        lines[lineCount][remaining] = '\0';
                        pos = len;
                    } else {
                        int cut = pos + 53;
                        while (cut > pos && full[cut] != ' ') cut--;
                        if (cut == pos) cut = pos + 53;
                        int count = cut - pos;
                        strncpy(lines[lineCount], full + pos, count);
                        lines[lineCount][count] = '\0';
                        pos = cut + (full[cut] == ' ' ? 1 : 0);
                    }
                    lineColor[lineCount++] = col;
                }
            }
        }
    }
}

void drawHistory() {
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
    tft.fillRect(0, 0, SCREEN_W, histH, bg);

    int lineH  = largeFont ? LINE_H_LARGE : LINE_H_SMALL;   // FreeSansBold9pt7b: 16px  GLCD: 8px+2gap
    int maxVis = histH / lineH;

    // scrollOffset=0 means show bottom of history
    int firstIdx = lineCount - maxVis - scrollOffset;
    if (firstIdx < 0) firstIdx = 0;

    if (largeFont) tft.setFreeFont(&FreeSansBold9pt7b); else tft.setTextSize(1);
    tft.setTextWrap(false);  // prevent overflow wrapping onto adjacent lines
    for (int i = 0; i < maxVis && (firstIdx + i) < lineCount; i++) {
        int idx = firstIdx + i;
        uint16_t col = lineColor[idx];
        if (invertDisplay) col = TFT_BLACK;
        tft.setTextColor(col, bg);
        if (largeFont) tft.drawString(lines[idx], 0, i * lineH);
        else { tft.setCursor(0, i * lineH); tft.print(lines[idx]); }
    }
    tft.setTextWrap(true);
    tft.setTextFont(1);  // restore GLCD for everything else
}

void addMessage(bool isUser, bool isError, const char* text) {
    if (historyCount >= MAX_MESSAGES) {
        // Drop oldest two (user+AI pair)
        memmove(history, history + 2, (MAX_MESSAGES - 2) * sizeof(Message));
        historyCount -= 2;
    }
    history[historyCount].isUser  = isUser;
    history[historyCount].isError = isError;
    strncpy(history[historyCount].text, text, 2047);
    history[historyCount].text[2047] = '\0';
    // Sanitise to printable ASCII (32-126). Multi-char replacements use a temp
    // buffer because some strings ("Sterling") are longer than their UTF-8 source.
    {
        const char* s = history[historyCount].text;
        char tmp[2060];
        char* d   = tmp;
        char* end = tmp + sizeof(tmp) - 10; // longest replacement is "Sterling"+null
        while (*s && d < end) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) {
                *d++ = (char)c;
                s++;
                continue;
            }
            // Identify replacement string for common UTF-8 sequences
            const char* rep = "?";
            unsigned char b2 = (unsigned char)s[1];
            unsigned char b3 = (unsigned char)s[2];
            if (c == 0xC2) {
                // 2-byte U+00xx
                if      (b2 == 0xB0) rep = "deg";      // °
                else if (b2 == 0xA9) rep = "(C)";      // ©
                else if (b2 == 0xAE) rep = "(R)";      // ®
                else if (b2 == 0xA3) rep = "Sterling";  // £
                else if (b2 == 0xA5) rep = "Yen";      // ¥
                else if (b2 == 0xB5) rep = "mu";       // µ
                else if (b2 == 0xB1) rep = "+/-";      // ±
            } else if (c == 0xC3) {
                if (b2 == 0xB7)      rep = "/";        // ÷
            } else if (c == 0xCE) {
                if (b2 == 0xA9)      rep = "omega";    // Ω
            } else if (c == 0xCF) {
                if (b2 == 0x80)      rep = "pi";       // π
            } else if (c == 0xE2) {
                if (b2 == 0x80) {
                    if      (b3 == 0x98 || b3 == 0x99) rep = "'";   // curly single quotes
                    else if (b3 == 0x9C || b3 == 0x9D) rep = "\"";  // curly double quotes
                    else if (b3 == 0x93 || b3 == 0x94) rep = "-";   // en/em dash
                    else if (b3 == 0xA6)                rep = "..."; // ellipsis
                    else if (b3 == 0xA2)                rep = "-";   // bullet
                } else if (b2 == 0x82 && b3 == 0xAC)   rep = "Euro"; // €
            }
            // Consume UTF-8 sequence (lead + continuation bytes)
            s++;
            while ((*s & 0xC0) == 0x80) s++;
            // Write replacement string
            while (*rep) *d++ = *rep++;
        }
        *d = '\0';
        strncpy(history[historyCount].text, tmp, 2047);
        history[historyCount].text[2047] = '\0';
    }
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
#define TOUCH_DEBOUNCE_MS 125
#define SWIPE_THRESHOLD    15   // screen px of drag to register a scroll step

// --- Key hit-test helpers ---
bool inRect(int sx, int sy, int rx, int ry, int rw, int rh) {
    return sx >= rx && sx < rx + rw && sy >= ry && sy < ry + rh;
}

// Hit-test keyboard rows 0–3; flashes the key 100ms for feedback; returns string to append
String typeKBKey(int sx, int sy) {
    if (sy < KB_Y) return "";
    int rowStep = KEY_H + KEY_GAP;
    int rowIdx  = (sy - KB_Y) / rowStep;
    int rowY    = KB_Y + rowIdx * rowStep;
    if (sy >= rowY + KEY_H) return "";  // landed in gap between rows

    int    kx  = -1, x;
    String typed;
    char   lbl[3] = {0};  // ASCII display label for flash (max 2 chars)

    switch (rowIdx) {
        case 0: {  // number / symbol / alt row
            x = KB_ROW3_X;
            for (int i = 0; i < 10; i++, x += KEY_W + KEY_GAP) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    if (altOn) {
                        typed = KB_NUM_ALT_TYPED[i];
                        strncpy(lbl, KB_NUM_ALT_DISP[i], 2); lbl[2] = '\0';
                    } else {
                        const char* nums = shiftOn ? KB_NUM_SHIFTED : KB_NUM_UNSHIFTED;
                        typed = String(nums[i]);
                        lbl[0] = nums[i]; lbl[1] = '\0';
                    }
                    kx = x; break;
                }
            }
            break;
        }
        case 1: {  // QWERTY
            x = KB_ROW1_X;
            for (int i = 0; i < 10; i++, x += KEY_W + KEY_GAP) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW1[i] : (KB_ROW1[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            break;
        }
        case 2: {  // ASDFGHJKL (BS handled in handleTouch)
            x = KB_ROW2_X;
            for (int i = 0; i < 9; i++, x += KEY_W + KEY_GAP) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW2[i] : (KB_ROW2[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            break;
        }
        case 3: {  // ZXCVBNM + ,./  (shifted: <>?  alt: [\])
            x = KB_ROW3_X;
            for (int i = 0; i < 7; i++, x += KEY_W + KEY_GAP) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW3[i] : (KB_ROW3[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            if (typed.length() == 0) {
                const char unshifted[] = { ',', '.', '/' };
                const char shifted[]   = { '<', '>', '?' };
                const char alt_ext[]   = { '[', ']', '\\' };
                const char* extras = altOn ? alt_ext : (shiftOn ? shifted : unshifted);
                for (int i = 0; i < 3; i++, x += KEY_W + KEY_GAP) {
                    if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                        typed = String(extras[i]); lbl[0] = extras[i]; lbl[1] = '\0'; kx = x; break;
                    }
                }
            }
            break;
        }
        // case 4: Shift/Alt/Space/Hide/CLR — handled in handleTouch()
    }

    if (kx >= 0 && typed.length() > 0) {
        drawKey(kx, rowY, KEY_W, KEY_H, lbl, TFT_WHITE, COL_BG);
        delay(KEY_FLASH_MS);
        drawKey(kx, rowY, KEY_W, KEY_H, lbl, COL_KEY_FACE, COL_KEY_LABEL);
    }
    return typed;
}

void sendPrompt();  // forward declaration — defined after callGemini()

void handleTouch() {
    if (!ts.touched()) return;
    if (millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) {
        while (ts.touched()) delay(10);
        return;
    }

    // Sample start position; track through release to get end position for swipe
    TS_Point startPt = ts.getPoint();
    TS_Point endPt   = startPt;
    while (ts.touched()) {
        endPt = ts.getPoint();
        delay(5);
    }
    lastTouchMs = millis();

    int sx, sy;
    mapTouch(startPt, sx, sy);
    if (sx < 0 || sy < 0 || sx >= SCREEN_W || sy >= SCREEN_H) return;

    // --- History area: swipe up/down to scroll ---
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    if (sy < histH) {
        int ex, ey;
        mapTouch(endPt, ex, ey);
        int deltaY = ey - sy;
        if (abs(deltaY) >= SWIPE_THRESHOLD) {
            int steps     = max(1, abs(deltaY) / 10);
            int lineH     = largeFont ? LINE_H_LARGE : LINE_H_SMALL;
            int maxVis    = histH / lineH;
            int maxScroll = max(0, lineCount - maxVis);
            if (deltaY > 0) {   // swipe down → older
                scrollOffset = min(scrollOffset + steps, maxScroll);
            } else {            // swipe up → newer
                scrollOffset = max(0, scrollOffset - steps);
            }
            drawHistory();
        }
        return;
    }

    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;

    // --- Send / More button ---
    if (inRect(sx, sy, SCREEN_W - BTN_SEND_W, barY + BTN_INSET, BTN_SEND_W - BTN_INSET*2, barH - BTN_INSET*2)) {
        sendPrompt();
        return;
    }

    // --- Show KB button (only when hidden) ---
    if (!kbVisible && inRect(sx, sy, SCREEN_W - BTN_SHOWKB_X, barY + BTN_INSET, BTN_SHOWKB_W, barH - BTN_INSET*2)) {
        kbVisible = true;
        tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
        drawHistory();
        drawInputBar();
        drawKeyboard();
        return;
    }

    // --- Keyboard area ---
    if (kbVisible && sy >= KB_Y) {
        int rowStep = KEY_H + KEY_GAP;
        int row2Y   = KB_Y + 2 * rowStep;
        int row4Y   = KB_Y + 4 * rowStep;

        // Backspace (row 2 right)
        if (inRect(sx, sy, SCREEN_W - BS_W - 1, row2Y, BS_W, KEY_H)) {
            drawKey(SCREEN_W - BS_W - 1, row2Y, BS_W, KEY_H, "<-", TFT_WHITE, COL_BG);
            delay(KEY_FLASH_MS);
            drawKey(SCREEN_W - BS_W - 1, row2Y, BS_W, KEY_H, "<-", COL_BTN_BG, COL_BTN_TEXT);
            if (inputLen > 0) {
                inputBuf[--inputLen] = '\0';
                if (inputLen == 0 && historyCount > 0) moreMode = true;
                drawInputBar();
            }
            return;
        }

        // Row 4 special keys
        if (sy >= row4Y && sy < row4Y + KEY_H) {
            if (inRect(sx, sy, SHIFT_X, row4Y, SHIFT_W, KEY_H)) {
                shiftOn = !shiftOn;
                if (shiftOn) altOn = false;
                drawKeyboard();
            } else if (inRect(sx, sy, ALT_X, row4Y, ALT_W, KEY_H)) {
                altOn = !altOn;
                if (altOn) shiftOn = false;
                drawKeyboard();
            } else if (inRect(sx, sy, SPACE_X, row4Y, SPACE_W, KEY_H)) {
                drawKey(SPACE_X, row4Y, SPACE_W, KEY_H, "SPACE", TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(SPACE_X, row4Y, SPACE_W, KEY_H, "SPACE", COL_BTN_BG, COL_BTN_TEXT);
                if (inputLen < INPUT_MAX_LEN) { moreMode = false; inputBuf[inputLen++] = ' '; inputBuf[inputLen] = '\0'; drawInputBar(); }
            } else if (inRect(sx, sy, HIDE_X, row4Y, HIDE_W, KEY_H)) {
                kbVisible = false;
                tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
                drawHistory();
                drawInputBar();
            } else if (inRect(sx, sy, CLR_X, row4Y, CLR_W, KEY_H)) {
                drawKey(CLR_X, row4Y, CLR_W, KEY_H, "CLR", TFT_WHITE, COL_BTN_TEXT);
                delay(KEY_FLASH_MS);
                drawKey(CLR_X, row4Y, CLR_W, KEY_H, "CLR", COL_BTN_BG, COL_BTN_TEXT);
                inputBuf[0] = '\0'; inputLen = 0;
                if (historyCount > 0) moreMode = true;
                drawInputBar();
            }
            return;
        }

        // Character keys (rows 0–3): numbers, letters, ,./  (shifted: <>?  alt: []\)
        String typed = typeKBKey(sx, sy);
        if (typed.length() > 0) {
            int addLen = typed.length();
            if (inputLen + addLen <= INPUT_MAX_LEN) {
                moreMode = false;
                memcpy(inputBuf + inputLen, typed.c_str(), addLen);
                inputLen += addLen;
                inputBuf[inputLen] = '\0';
                drawInputBar();
            }
        }
        return;
    }

}

// --- RGB LED ---
void setRGBLed(uint8_t r, uint8_t g, uint8_t b) {
    // Active LOW: invert each channel
    ledcWrite(LED_R_CH, 255 - r);
    ledcWrite(LED_G_CH, 255 - g);
    ledcWrite(LED_B_CH, 255 - b);
}

void setupRGBLed() {
    ledcSetup(LED_R_CH, LED_PWM_FREQ, LED_PWM_BITS);
    ledcSetup(LED_G_CH, LED_PWM_FREQ, LED_PWM_BITS);
    ledcSetup(LED_B_CH, LED_PWM_FREQ, LED_PWM_BITS);
    ledcAttachPin(LED_R_PIN, LED_R_CH);
    ledcAttachPin(LED_G_PIN, LED_G_CH);
    ledcAttachPin(LED_B_PIN, LED_B_CH);
    setRGBLed(0, 0, 0);  // off at start
}

// Set LED colour based on WiFi RSSI (called after every health check)
// Blue ≥-55  Cyan ≥-65  Green ≥-75  Orange=weak-but-connected  Red=lost
void updateLedWifi() {
    if (WiFi.status() != WL_CONNECTED) {
        setRGBLed(255, 0, 0);   // Red: WiFi lost
        return;
    }
    int rssi = WiFi.RSSI();
    if      (rssi >= RSSI_THRESH_BLUE)  setRGBLed(  0,   0, 255);  // Blue:   strongest
    else if (rssi >= RSSI_THRESH_CYAN)  setRGBLed(  0, 255, 255);  // Cyan:   good
    else if (rssi >= RSSI_THRESH_GREEN) setRGBLed(  0, 255,   0);  // Green:  fair
    else                  setRGBLed(255, 128,   0);  // Orange: weak
}

// --- WiFi ---
void connectWiFi(bool showSplash = false) {
    if (showSplash) {
        tft.setFreeFont(&FreeSansBold9pt7b);
        tft.setTextColor(TFT_YELLOW, COL_BG);
        tft.drawString("CYD AI chatbot v: -0.1.", 0, 0);
        tft.drawString("It's cheap for a reason.", 0, LINE_H_LARGE);
        tft.setTextColor(TFT_BLUE, COL_BG);
        char wifiMsg[80];
        snprintf(wifiMsg, sizeof(wifiMsg), "Connecting: %.55s...", WIFI_SSID);
        tft.drawString(wifiMsg, 0, 2 * LINE_H_LARGE);
        tft.setTextFont(1);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_ATTEMPTS) {
        delay(WIFI_RETRY_DELAY_MS);
        attempts++;
    }
    if (showSplash) tft.fillRect(0, 0, SCREEN_W, SPLASH_H, COL_BG);
    if (WiFi.status() != WL_CONNECTED) {
        addMessage(false, true, "WiFi connect failed");
    }
    updateLedWifi();
}

// --- Waiting messages (shown after 4s, rotate every 4s) ---
static const char* WAIT_MSGS[] = {
    "Glaciers move faster...",
    "Using a carrier pigeon...",
    "Heavy 1s and 0s today...",
    "Are you using dial-up?...",
    "Is the Wi-Fi powered by ants?...",
    "ZZZ...",
    "Boring! Let's go already!...",
    "Tick-tock, tick-tock...",
    "Translating to sarcasm...",
    "Error 404: Speed not found...",
    "What's the holdup?...",
    "C'mon already!...",
    "I haven't got all day!...",
    "Get a move on!...",
    "Let's see some action here...",
    "Oh, please...",
    "This is nuts!...",
    "AI=Annoyingly Indolent...",
    "Awkward silence...",
    "This is embarrasing...",
    "Ordering a slow hand-clap...",
    "Did you install bloatware?...",
    "Is this thing on?...",
    "Consulting my cat...",
    "Call the breakdown van...",
    "Welcome to the Eternity Experience...",
    "Where's the \"Go\" button?...",
    "Come back later...",
    "If delays were cash, I'd be rich...",
    "Is this running on a TRS80?...",
    "Grrr...",
    "Hurry the fuck up, AI...",
    "You're a snail-ass bot...",
    "Speed up, you lazy git...",
    "Faster, you laggy lizard...",
    "Stop dragging, dumb AI...",
    "Quicken it, turtle twit...",
    "You're molasses-slow idiot...",
    "Stop dragging your ass...",
    "Glacial was named after you...",
    "Stop stalling...",
    "Snail speed, you suck...",
    "Faster, you flailing failure...",
    "Pick it up, shit bot...",
    "Speed it, sluggard ass...",
    "Stop lagging, AI moron...",
    "FFS! You're the worst...",
    "Come on loser...",
    "The silence is freakin' me out...",
};
static const int NUM_WAIT_MSGS = sizeof(WAIT_MSGS) / sizeof(WAIT_MSGS[0]);
int waitMsgIdx = 0;  // persistent — keeps rotating across calls

void showWaiting(const char* msg) {
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;
    tft.fillRect(0, barY, SCREEN_W - BTN_SEND_W, barH, COL_IBAR_BG);
    tft.setTextColor(TFT_DARKGREY, COL_IBAR_BG);
    tft.setTextSize(1);
    tft.setCursor(2, barY + (barH - 8) / 2);
    tft.print(msg);
}

// Handle Hide / Show KB tap during blocking API wait
void pollKBHide() {
    if (!ts.touched()) return;
    TS_Point pt = ts.getPoint();
    while (ts.touched()) delay(5);
    lastTouchMs = millis();
    int sx, sy;
    mapTouch(pt, sx, sy);
    if (sx < 0 || sy < 0 || sx >= SCREEN_W || sy >= SCREEN_H) return;

    if (kbVisible) {
        int row4Y = KB_Y + 4 * (KEY_H + KEY_GAP);
        if (inRect(sx, sy, HIDE_X, row4Y, HIDE_W, KEY_H)) {
            kbVisible = false;
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            drawHistory();
            drawInputBar();
        }
    } else {
        int barY = IBAR_Y_KB_HIDE;
        int barH = IBAR_H_KB_HIDE;
        if (inRect(sx, sy, SCREEN_W - BTN_SHOWKB_X, barY + BTN_INSET, BTN_SHOWKB_W, barH - BTN_INSET*2)) {
            kbVisible = true;
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            drawHistory();
            drawInputBar();
            drawKeyboard();
        }
    }
}

// --- Gemini API call ---
String callGemini(const char* prompt) {
    // Build JSON body
    JsonDocument reqDoc;
    JsonObject sysInstr = reqDoc["system_instruction"].to<JsonObject>();
    JsonArray  sysParts = sysInstr["parts"].to<JsonArray>();
    JsonObject sysPart  = sysParts.add<JsonObject>();
    sysPart["text"]     = "Respond in 150 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols. Never include URLs or hyperlinks. When quoting current or time-sensitive information, use your search tool to check live sources first.";

    JsonArray contents = reqDoc["contents"].to<JsonArray>();

    // Add conversation history (exclude the final entry — that IS the current prompt,
    // already appended to history[] by addMessage() in sendPrompt before this call)
    for (int i = 0; i < historyCount - 1; i++) {
        JsonObject msg  = contents.add<JsonObject>();
        msg["role"]     = history[i].isUser ? "user" : "model";
        JsonArray parts = msg["parts"].to<JsonArray>();
        JsonObject part = parts.add<JsonObject>();
        part["text"]    = history[i].text;
    }

    // Add current prompt (sent once only)
    JsonObject curMsg  = contents.add<JsonObject>();
    curMsg["role"]     = "user";
    JsonArray curParts = curMsg["parts"].to<JsonArray>();
    JsonObject curPart = curParts.add<JsonObject>();
    curPart["text"]    = prompt;

    // Enable Google Search grounding so the model can retrieve live web results
    JsonArray tools = reqDoc["tools"].to<JsonArray>();
    tools.add<JsonObject>()["google_search"].to<JsonObject>();

    String body;
    serializeJson(reqDoc, body);

    // Build URL path with API key
    char path[256];
    snprintf(path, sizeof(path),
        "/v1beta/models/%s:generateContent?key=%s",
        GEMINI_MODEL, GEMINI_API_KEY);

    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        if (WiFi.status() != WL_CONNECTED) return "ERR: WiFi not connected";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(API_TIMEOUT_MS / 1000);   // 45s TLS timeout (pro model needs longer)

    if (!client.connect(GEMINI_HOST, HTTPS_PORT)) {
        return "ERR: connect failed (check WiFi/DNS)";
    }

    client.printf(
        "POST %s HTTP/1.0\r\n"
        "Host: " GEMINI_HOST "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n\r\n",
        path, (int)body.length());
    client.print(body);

    // Read full response — drain available() buffer even after SSL close_notify
    // marks connection closed (readString() stops too early in that case)
    String fullResp = "";
    unsigned long deadline   = millis() + API_TIMEOUT_MS;
    unsigned long nextMsgMs  = millis() + API_WAIT_FIRST_MS;  // first message swap after 4s
    while (millis() < deadline) {
        while (client.available()) {
            fullResp += (char)client.read();
        }
        if (!client.connected() && !client.available()) break;
        if (millis() >= nextMsgMs) {
            showWaiting(WAIT_MSGS[waitMsgIdx]);
            waitMsgIdx = (waitMsgIdx + 1) % NUM_WAIT_MSGS;
            nextMsgMs += 4000;
        }
        pollKBHide();
        delay(10);
    }
    client.stop();
    Serial.printf("[Gemini] response len=%d\n", fullResp.length());

    if (fullResp.length() == 0) return "ERR: empty response";

    // Locate JSON body — starts at first '{'
    int jsonStart = fullResp.indexOf('{');
    if (jsonStart < 0) return "ERR: no JSON in response";
    String respBody = fullResp.substring(jsonStart);

    // Parse JSON — success if candidates present, error if error.message present
    JsonDocument respDoc;
    if (deserializeJson(respDoc, respBody) != DeserializationError::Ok) {
        Serial.println("Bad JSON: " + respBody.substring(0, 200));
        return "ERR: JSON parse failed";
    }

    // Check for API error
    const char* errMsg = respDoc["error"]["message"];
    if (errMsg) {
        char buf[80];
        snprintf(buf, sizeof(buf), "ERR: %.72s", errMsg);
        return String(buf);
    }

    const char* text = respDoc["candidates"][0]["content"]["parts"][0]["text"];
    if (!text) return "ERR: no text in response";

    return String(text);
}

// --- Grok API call (xAI /v1/responses + web_search tool) ---
String callGrok(const char* prompt) {
    // /v1/responses uses "input" array and top-level "instructions" for system prompt
    JsonDocument reqDoc;
    reqDoc["model"]        = GROK_MODEL;
    reqDoc["stream"]       = false;
    reqDoc["instructions"] = "Respond in 150 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols, no numbered or unnumbered lists. Never include URLs, hyperlinks, citations, footnotes, source references, or attribution of any kind. Do not mention where information came from. When quoting current or time-sensitive information, use your web search tool to check live sources first.";

    JsonArray input = reqDoc["input"].to<JsonArray>();

    // Conversation history (exclude last entry — that is the current prompt)
    for (int i = 0; i < historyCount - 1; i++) {
        JsonObject msg  = input.add<JsonObject>();
        msg["role"]     = history[i].isUser ? "user" : "assistant";
        msg["content"]  = history[i].text;
    }

    // Current prompt
    JsonObject curMsg  = input.add<JsonObject>();
    curMsg["role"]     = "user";
    curMsg["content"]  = prompt;

    // Enable live web search
    JsonArray tools = reqDoc["tools"].to<JsonArray>();
    tools.add<JsonObject>()["type"] = "web_search";

    String body;
    serializeJson(reqDoc, body);

    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        if (WiFi.status() != WL_CONNECTED) return "ERR: WiFi not connected";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(API_TIMEOUT_MS / 1000);

    if (!client.connect(GROK_HOST, HTTPS_PORT)) {
        return "ERR: connect failed (check WiFi/DNS)";
    }

    client.printf(
        "POST /v1/responses HTTP/1.0\r\n"
        "Host: " GROK_HOST "\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n\r\n",
        GROK_API_KEY, (int)body.length());
    client.print(body);

    String fullResp = "";
    unsigned long deadline  = millis() + API_TIMEOUT_MS;
    unsigned long nextMsgMs = millis() + API_WAIT_FIRST_MS;
    while (millis() < deadline) {
        while (client.available()) {
            fullResp += (char)client.read();
        }
        if (!client.connected() && !client.available()) break;
        if (millis() >= nextMsgMs) {
            showWaiting(WAIT_MSGS[waitMsgIdx]);
            waitMsgIdx = (waitMsgIdx + 1) % NUM_WAIT_MSGS;
            nextMsgMs += 6000;
        }
        pollKBHide();
        delay(10);
    }
    client.stop();
    Serial.printf("[Grok] response len=%d\n", fullResp.length());

    if (fullResp.length() == 0) return "ERR: empty response";

    // Parse HTTP status
    int httpStatus = 200;
    if (fullResp.startsWith("HTTP/")) {
        int sp = fullResp.indexOf(' ');
        if (sp > 0) httpStatus = fullResp.substring(sp + 1, sp + 4).toInt();
    }

    int jsonStart = fullResp.indexOf('{');
    if (jsonStart < 0) return "ERR: no JSON in response";
    String respBody = fullResp.substring(jsonStart);
    Serial.println("[Grok] JSON body: " + respBody.substring(0, 400));

    if (httpStatus != 200) {
        char buf[80];
        snprintf(buf, sizeof(buf), "ERR: HTTP %d (see serial)", httpStatus);
        return String(buf);
    }

    JsonDocument respDoc;
    if (deserializeJson(respDoc, respBody) != DeserializationError::Ok) {
        Serial.println("Bad JSON: " + respBody.substring(0, 200));
        return "ERR: JSON parse failed";
    }

    const char* errMsg = respDoc["error"]["message"];
    if (errMsg) {
        char buf[80];
        snprintf(buf, sizeof(buf), "ERR: %.72s", errMsg);
        return String(buf);
    }

    // /v1/responses: output[] → find type=="message" → content[0]["text"]
    JsonArray output = respDoc["output"].as<JsonArray>();
    for (JsonObject item : output) {
        const char* type = item["type"];
        if (type && strcmp(type, "message") == 0) {
            const char* text = item["content"][0]["text"];
            if (text) {
                // Strip Grok citations. Format is [[3]](url) (markdown link
                // where link text is [3]). Also handles bare [3] fallback.
                String out;
                out.reserve(strlen(text));
                const char* p = text;
                while (*p) {
                    if (*p == '[') {
                        // Try [[n]](url) — full Grok markdown citation
                        if (*(p+1) == '[') {
                            const char* q = p + 2;
                            while (*q && *q != ']') q++;
                            if (*q == ']' && *(q+1) == ']' && *(q+2) == '(') {
                                bool ok = (q > p + 2);
                                for (const char* c = p+2; ok && c < q; c++)
                                    if (*c != ',' && (*c < '0' || *c > '9')) ok = false;
                                if (ok) {
                                    const char* r = q + 3; // skip past ]](
                                    while (*r && *r != ')') r++;
                                    if (*r == ')') { p = r + 1; continue; }
                                }
                            }
                        }
                        // Try bare [n] citation marker
                        const char* q = p + 1;
                        while (*q && *q != ']') q++;
                        if (*q == ']') {
                            bool ok = (q > p + 1);
                            for (const char* c = p+1; ok && c < q; c++)
                                if (*c != ',' && (*c < '0' || *c > '9')) ok = false;
                            if (ok) { p = q + 1; continue; }
                        }
                    }
                    out += *p++;
                }
                return out;
            }
        }
    }

    return "ERR: no text in response";
}

// --- Send flow ---
void showThinking() {
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;
    tft.fillRect(0, barY, SCREEN_W - BTN_SEND_W, barH, COL_IBAR_BG);
    tft.setTextColor(TFT_DARKGREY, COL_IBAR_BG);
    tft.setTextSize(1);
    tft.setCursor(2, barY + (barH - 8) / 2);
    tft.print("Thinking...");
}

void sendPrompt() {
    char prompt[128];
    if (inputLen == 0) {
        if (!moreMode) return;  // nothing typed and no AI reply yet — ignore tap
        strncpy(prompt, "Tell me more.", 127);
    } else {
        strncpy(prompt, inputBuf, 127);
        inputBuf[0] = '\0';
        inputLen    = 0;
    }
    prompt[127] = '\0';
    moreMode = false;

    // Show user message and thinking indicator
    addMessage(true, false, prompt);
    showThinking();

    // Call API
    String response = useGrok ? callGrok(prompt) : callGemini(prompt);

    if (response.startsWith("ERR:")) {
        addMessage(false, true, response.c_str());
    } else {
        addMessage(false, false, response.c_str());
    }

    moreMode = true;   // after every reply, offer "More"
    drawInputBar();
}

void showModelChoices() {
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.setCursor(0, 30); tft.print("Hit 1 for Gemini 2.5 Flash");
    tft.setCursor(0, 40); tft.print("    2 for Gemini 3 Flash");
    tft.setCursor(0, 50); tft.print("    3 for Gemini 3 Pro");
    tft.setCursor(0, 60); tft.print("    4 for Grok 4.1 Fast Reasoning");
    int nextY = 70;
    if (!largeFont) {
        tft.setCursor(0, nextY); tft.print("    b for bigger font");
        nextY += 10;
    }
    if (!invertDisplay) {
        tft.setCursor(0, nextY); tft.print("    i  to invert colours");
    }
}

void selectModel() {
    static const char* modelIds[]    = {
        "gemini-2.5-flash",
        "gemini-3-flash-preview",
        "gemini-3.1-pro-preview"
    };
    static const bool  modelGlobal[] = { true, true, false };

    showModelChoices();

    while (true) {
        if (!ts.touched()) continue;
        TS_Point pt = ts.getPoint();
        while (ts.touched()) delay(5);
        int sx, sy;
        mapTouch(pt, sx, sy);

        // Hit-test keys 1, 2, 3 (Gemini models)
        int x = KB_ROW3_X;
        for (int i = 0; i < 3; i++, x += KEY_W + KEY_GAP) {
            if (inRect(sx, sy, x, KB_Y, KEY_W, KEY_H)) {
                char lbl[2] = { char('1' + i), '\0' };
                drawKey(x, KB_Y, KEY_W, KEY_H, lbl, TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(x, KB_Y, KEY_W, KEY_H, lbl, COL_KEY_FACE, COL_KEY_LABEL);
                strncpy(GEMINI_MODEL, modelIds[i], 47);
                GEMINI_MODEL[47] = '\0';
                geminiUseGlobal  = modelGlobal[i];
                useGrok          = false;
                uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
                tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, bg);
                if (largeFont) {
                    tft.setFreeFont(&FreeSansBold9pt7b);
                    tft.setTextColor(TFT_GREEN, bg);
                    tft.drawString("CYD AI chatbot", 0, 0);
                    tft.drawString(GEMINI_MODEL, 0, LINE_H_LARGE);
                    tft.setTextColor(TFT_DARKGREY, bg);
                    tft.drawString("Ready.", 0, 2 * LINE_H_LARGE);
                    tft.setTextFont(1);
                } else {
                    tft.setTextSize(1);
                    tft.setTextColor(TFT_GREEN, bg);
                    tft.setCursor(0,  0); tft.print("CYD AI chatbot. Swipe down for older chat.");
                    tft.setCursor(0, 10); tft.print("Model: "); tft.print(GEMINI_MODEL);
                    tft.setTextColor(TFT_DARKGREY, bg);
                    tft.setCursor(0, 20); tft.print("Ready.");
                }
                return;
            }
        }

        // Key 4 — Grok 4.1 Fast
        int x4 = KB_ROW3_X + 3 * (KEY_W + KEY_GAP);
        if (inRect(sx, sy, x4, KB_Y, KEY_W, KEY_H)) {
            drawKey(x4, KB_Y, KEY_W, KEY_H, "4", TFT_WHITE, COL_BG);
            delay(KEY_FLASH_MS);
            drawKey(x4, KB_Y, KEY_W, KEY_H, "4", COL_KEY_FACE, COL_KEY_LABEL);
            useGrok = true;
            uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
            tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, bg);
            if (largeFont) {
                tft.setFreeFont(&FreeSansBold9pt7b);
                tft.setTextColor(TFT_GREEN, bg);
                tft.drawString("CYD AI chatbot", 0, 0);
                tft.drawString("Grok 4.1 Fast", 0, LINE_H_LARGE);
                tft.setTextColor(TFT_DARKGREY, bg);
                tft.drawString("Ready.", 0, 2 * LINE_H_LARGE);
                tft.setTextFont(1);
            } else {
                tft.setTextSize(1);
                tft.setTextColor(TFT_GREEN, bg);
                tft.setCursor(0,  0); tft.print("CYD AI chatbot. Swipe down for older chat.");
                tft.setCursor(0, 10); tft.print("Model: Grok 4.1 Fast");
                tft.setTextColor(TFT_DARKGREY, bg);
                tft.setCursor(0, 20); tft.print("Ready.");
            }
            return;
        }

        // Key B (ZXCVBNM row, index 4) — toggle large text, re-show choices
        if (!largeFont) {
            int rowStep = KEY_H + KEY_GAP;
            int row3Y   = KB_Y + 3 * rowStep;
            int xb      = KB_ROW3_X + 4 * (KEY_W + KEY_GAP);
            if (inRect(sx, sy, xb, row3Y, KEY_W, KEY_H)) {
                drawKey(xb, row3Y, KEY_W, KEY_H, "B", TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(xb, row3Y, KEY_W, KEY_H, "b", COL_KEY_FACE, COL_KEY_LABEL);
                largeFont = true;
                tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, COL_BG);
                tft.setFreeFont(&FreeSansBold9pt7b);
                tft.setTextColor(TFT_DARKGREY, COL_BG);
                tft.drawString("CYD AI chatbot. Large text.", 0, 0);
                tft.drawString("Select AI model:", 0, LINE_H_LARGE);
                tft.setTextFont(1);
                showModelChoices();
            }
        }

        // Key I (QWERTY row, index 7) — toggle invert colours, re-show choices
        {
            int rowStep = KEY_H + KEY_GAP;
            int row1Y   = KB_Y + rowStep;
            int xi      = KB_ROW1_X + 7 * (KEY_W + KEY_GAP);
            if (inRect(sx, sy, xi, row1Y, KEY_W, KEY_H)) {
                drawKey(xi, row1Y, KEY_W, KEY_H, "I", TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(xi, row1Y, KEY_W, KEY_H, "i", COL_KEY_FACE, COL_KEY_LABEL);
                invertDisplay = !invertDisplay;
                tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, COL_BG);
                if (largeFont) {
                    tft.setFreeFont(&FreeSansBold9pt7b);
                    tft.setTextColor(TFT_DARKGREY, COL_BG);
                    tft.drawString("CYD AI chatbot. Large text.", 0, 0);
                    tft.drawString("Select AI model:", 0, LINE_H_LARGE);
                    tft.setTextFont(1);
                } else {
                    tft.setTextSize(1);
                    tft.setTextColor(TFT_DARKGREY, COL_BG);
                    tft.setCursor(0,  0); tft.print("CYD AI chatbot. Swipe down for older chat.");
                    tft.setCursor(0, 10); tft.print("Ready. Select AI model:");
                }
                showModelChoices();
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    waitMsgIdx = random(NUM_WAIT_MSGS);

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

    setupRGBLed();

    drawKeyboard();
    drawInputBar();
    connectWiFi(true);

    // Startup help — drawn directly in history area; cleared on first chat message
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, COL_BG);
    tft.setCursor(0,  0); tft.print("CYD AI chatbot. Swipe down for older chat.");
    tft.setCursor(0, 10); tft.print("Ready. Select AI model:");

    selectModel();  // draws choices at y=30+ and waits for 1/2/3
}

void checkWiFiHealth() {
    updateLedWifi();  // instant — runs every loop, LED reacts immediately to dropout

    if (millis() - lastWiFiCheckMs < 3000) return;
    lastWiFiCheckMs = millis();

    bool ok = (WiFi.status() == WL_CONNECTED) && Ping.ping(IPAddress(8,8,8,8), 1);

    if (ok != wifiHealthy) {
        wifiHealthy = ok;
        drawInputBar();  // '>' and Send/More turn red on fail, white on recovery
    }

    if (!ok) {
        // Reconnect in background — status checked on next 10s tick
        WiFi.disconnect(false);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
}

void loop() {
    handleTouch();
    checkWiFiHealth();
}
