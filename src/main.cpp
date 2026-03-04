// SLUG AI chatbot for CYD28 (ESP32-2432S028R)
// 040326 Unicode fonts: VLW smooth fonts, Latin Extended + special chars, remove transliteration
// 030326 Touch calibration: key C at boot, 2-point crosshair, saved to NVS
// 030326 Transliterate accented chars (Latin-1, Latin Extended-A) to ASCII in addMessage
// 030326 Add Groq API (api.groq.com), model qwen/qwen3-32b, key 5 at boot
// 030326 WiFi AP menu bugfixes: disconnect before scan, redraw KB after selectAP
// 030326 WiFi AP menu: NVS credential store, AP scan/picker, enterPassword, selectAP
// 020326 KB layout: BS height 40px, Hide aligned to dot key, clear KB remnants on Send
// 280226 Add Grok API (xAI), key 4 at boot; route callGemini/callGrok via useGrok flag
// 280226 Bold font: custom DejaVuSansBold 12px (yAdv=15), LINE_H_LARGE=15 SPLASH_H=45
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
#include "fonts/DejaVuSansBold12px.h"  // VLW smooth font 12px (Unicode)
#include "fonts/DejaVuSansBold8px.h"   // VLW smooth font 8px (Unicode)
#include "images/splash.h"             // SLUG splash 320x117 RGB565
#include "images/slugsmall.h"          // SLUGsmall 144x96 RGB565 with transparency
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP32Ping.h>
#include <Preferences.h>

// --- Credentials (kept out of version control) ---
#include "secrets.h"  // copy secrets.h.example → secrets.h and fill in your keys
char        GEMINI_MODEL[48]  = "gemini-3.1-pro-preview"; // overwritten at boot
bool        geminiUseGlobal   = false;  // true → /locations/global/ in path
bool        useGrok           = false;  // true → route to Grok (xAI) instead of Gemini
bool        useGroq           = false;  // true → route to Groq instead of Gemini
bool        largeFont         = false;  // true → DejaVuSansBold12px in history area
bool        invertDisplay     = false;  // true → light grey bg, black text in history area
#define COL_INVERT_BG   0xC618          // light grey (~RGB 192,192,192)

// --- WiFi credential store (NVS, up to 9 slots, slot 0 = most-recently-used) ---
#define WIFI_PREFS_MAX  9
#define WIFI_PREFS_NS   "wifi"

static char wifiSsid[WIFI_PREFS_MAX][33];  // SSID max 32 chars + null
static char wifiPass[WIFI_PREFS_MAX][64];  // WPA2 password max 63 chars + null
static int  wifiCredsCount = 0;

// --- Touch calibration (NVS, namespace "touch") ---
// ts.setRotation(3) handles the 180° axis inversion for ROTATE_180, so cal defaults are the same.
static int calXmin = 200, calXmax = 3900;
static int calYmin = 200, calYmax = 3900;

// Calibration is tagged with the ts rotation used so mismatched calibrations are ignored.
// ROTATE_180 uses ts rotation 3; normal uses ts rotation 1.
#ifdef ROTATE_180
  #define CAL_ORIENT 3
#else
  #define CAL_ORIENT 0
#endif

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
    p.putInt("xmin", calXmin);
    p.putInt("xmax", calXmax);
    p.putInt("ymin", calYmin);
    p.putInt("ymax", calYmax);
    p.putInt("orient", CAL_ORIENT);
    p.putBool("valid", true);
    p.end();
}

void saveWifiCreds() {
    Preferences p;
    p.begin(WIFI_PREFS_NS, false);
    p.putInt("n", wifiCredsCount);
    for (int i = 0; i < wifiCredsCount; i++) {
        char sk[4], pk[4];
        snprintf(sk, sizeof(sk), "s%d", i);
        snprintf(pk, sizeof(pk), "p%d", i);
        p.putString(sk, wifiSsid[i]);
        p.putString(pk, wifiPass[i]);
    }
    p.end();
}

void loadWifiCreds() {
    Preferences p;
    p.begin(WIFI_PREFS_NS, true);
    wifiCredsCount = p.getInt("n", 0);
    if (wifiCredsCount > WIFI_PREFS_MAX) wifiCredsCount = WIFI_PREFS_MAX;
    for (int i = 0; i < wifiCredsCount; i++) {
        char sk[4], pk[4];
        snprintf(sk, sizeof(sk), "s%d", i);
        snprintf(pk, sizeof(pk), "p%d", i);
        strncpy(wifiSsid[i], p.getString(sk, "").c_str(), 32); wifiSsid[i][32] = '\0';
        strncpy(wifiPass[i], p.getString(pk, "").c_str(), 63); wifiPass[i][63] = '\0';
    }
    p.end();
}

// Insert ssid+pass at slot 0 (most-recently-used). Shift others down. Cap at WIFI_PREFS_MAX.
void insertWifiCred(const char* ssid, const char* pass) {
    // Remove existing entry for this SSID if present
    for (int i = 0; i < wifiCredsCount; i++) {
        if (strcmp(wifiSsid[i], ssid) == 0) {
            for (int j = i; j < wifiCredsCount - 1; j++) {
                strncpy(wifiSsid[j], wifiSsid[j+1], 32); wifiSsid[j][32] = '\0';
                strncpy(wifiPass[j], wifiPass[j+1], 63); wifiPass[j][63] = '\0';
            }
            wifiCredsCount--;
            break;
        }
    }
    int newCount = wifiCredsCount + 1;
    if (newCount > WIFI_PREFS_MAX) newCount = WIFI_PREFS_MAX;
    for (int i = newCount - 1; i > 0; i--) {
        strncpy(wifiSsid[i], wifiSsid[i-1], 32); wifiSsid[i][32] = '\0';
        strncpy(wifiPass[i], wifiPass[i-1], 63); wifiPass[i][63] = '\0';
    }
    strncpy(wifiSsid[0], ssid, 32); wifiSsid[0][32] = '\0';
    strncpy(wifiPass[0], pass, 63); wifiPass[0][63] = '\0';
    wifiCredsCount = newCount;
    saveWifiCreds();
}

// Returns true and fills passOut (64 bytes) if password stored for ssid.
bool findWifiPass(const char* ssid, char* passOut) {
    for (int i = 0; i < wifiCredsCount; i++) {
        if (strcmp(wifiSsid[i], ssid) == 0) {
            strncpy(passOut, wifiPass[i], 63); passOut[63] = '\0';
            return wifiPass[i][0] != '\0';  // false if stored but blank
        }
    }
    return false;
}

// Blank the stored password for ssid (force re-entry next time).
void clearWifiPass(const char* ssid) {
    for (int i = 0; i < wifiCredsCount; i++) {
        if (strcmp(wifiSsid[i], ssid) == 0) {
            wifiPass[i][0] = '\0';
            saveWifiCreds();
            return;
        }
    }
}

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
//#define ROTATE_180          // uncomment to rotate display 180° (USB on left)

// --- Layout (KB shown) ---
#define HIST_Y_TOP      0
#define HIST_H_KB_SHOW  116
#define IBAR_Y_KB_SHOW  116
#define IBAR_H_KB_SHOW   20
#define KB_Y            136
#define KB_H            104

// --- Layout (KB hidden) ---
#define HIST_H_KB_HIDE  220
#define IBAR_Y_KB_HIDE  220
#define IBAR_H_KB_HIDE   20

// --- Colours ---
#define COL_BG          0x0841   // #080808 — nearest RGB565 to #0D0D0D
#define COL_USER        TFT_CYAN
#define COL_AI          0xF760   // #F8EC00 — nearest RGB565 to #FFEE00
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

#define KEY_W       32
#define KEY_H       20
#define KEY_GAP      1
#define KB_ROW1_X    0   // all rows flush left — 10 × 32 = 320px
#define KB_ROW2_X    0
#define KB_ROW3_X    0

// Row 4 special key widths and positions
// [Shift 40]+[Alt 40]+[Space 166]+[Hide 42] + tall BS (26×45) at bottom-right
// Hide RHS=288 aligns with '.' key RHS; BS starts at 294 (gap of 6px after Hide)
#define BS_W        26   // ~20% thinner than KEY_W=32
#define BS_X       294   // bottom right (SCREEN_W - BS_W)
#define BS_H        40   // tall delete key; drawn from (SCREEN_H - BS_H) upward
#define SHIFT_W     40
#define SHIFT_X      0
#define ALT_W       40
#define ALT_X       40   // SHIFT_X + SHIFT_W
#define SPACE_W    166   // HIDE_X(246) - SPACE_X(80)
#define SPACE_X     80   // ALT_X + ALT_W
#define HIDE_W      42
#define HIDE_X     246   // SPACE_X + SPACE_W; RHS=288 = '.' key RHS

// --- Key appearance ---
#define KEY_RADIUS        3     // rounded corner radius for keys

// --- Layout metrics ---
#define LINE_H_LARGE     15     // DejaVuSansBold12px line height (yAdvance=15)
#define LINE_H_SMALL     10     // GLCD line height (8px + 2px gap)
#define SPLASH_H         45     // height of boot splash area to clear after connect (3 × LINE_H_LARGE)

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
#define GROQ_HOST         "api.groq.com"
#define GROQ_MODEL        "openai/gpt-oss-120b"

// Load/unload the 12px bold smooth font. Always call fontOff() after fontOn().
void fontOn()  { tft.loadFont(DejaVuSansBold12pxData); }
void fontOff() { tft.unloadFont(); }

void drawKey(int x, int y, int w, int h, const char* label, uint16_t face, uint16_t text) {
    tft.fillRoundRect(x, y, w - 1, h, KEY_RADIUS, face);
    tft.setTextColor(text, face);
    fontOn();
    int tx = x + (w - (int)tft.textWidth(label)) / 2;
    int ty = y + (h - 15) / 2 + 1;   // 15 = yAdvance; +1 lowers text slightly
    tft.drawString(label, tx, ty);
    fontOff();           // restore GLCD for everything else
}

void drawKeyboard() {
    if (!kbVisible) return;
    tft.fillRect(0, KB_Y, SCREEN_W, KB_H, COL_BG);
    int rowStep = KEY_H + KEY_GAP;
    int x;

    // Row 0: number / symbol / alt keys
    x = 0;
    for (int i = 0; i < 10; i++) {
        if (altOn) {
            drawKey(x, KB_Y, KEY_W, KEY_H, KB_NUM_ALT_DISP[i], COL_KEY_FACE, COL_KEY_LABEL);
        } else {
            char label[2] = { shiftOn ? KB_NUM_SHIFTED[i] : KB_NUM_UNSHIFTED[i], 0 };
            drawKey(x, KB_Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        }
        x += KEY_W;
    }

    // Row 1: QWERTY (centred, no Hide button)
    x = 0;
    int row1Y = KB_Y + rowStep;
    for (int i = 0; i < 10; i++) {
        char c = shiftOn ? KB_ROW1[i] : (KB_ROW1[i] + 32);
        char label[2] = { c, 0 };
        drawKey(x, row1Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W;
    }

    // Row 2: ASDFGHJKL + /
    x = 0;
    int row2Y = KB_Y + 2 * rowStep;
    for (int i = 0; i < 9; i++) {
        char c = shiftOn ? KB_ROW2[i] : (KB_ROW2[i] + 32);
        char label[2] = { c, 0 };
        drawKey(x, row2Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W;
    }
    drawKey(x, row2Y, KEY_W, KEY_H, altOn ? "\\" : (shiftOn ? "?" : "/"), COL_KEY_FACE, COL_KEY_LABEL);

    // Row 3: ZXCVBNM + , .
    x = 0;
    int row3Y = KB_Y + 3 * rowStep;
    for (int i = 0; i < 7; i++) {
        char c = shiftOn ? KB_ROW3[i] : (KB_ROW3[i] + 32);
        char label[2] = { c, 0 };
        drawKey(x, row3Y, KEY_W, KEY_H, label, COL_KEY_FACE, COL_KEY_LABEL);
        x += KEY_W;
    }
    drawKey(x, row3Y, KEY_W, KEY_H, altOn ? "[" : (shiftOn ? "<" : ","), COL_KEY_FACE, COL_KEY_LABEL); x += KEY_W;
    drawKey(x, row3Y, KEY_W, KEY_H, altOn ? "]" : (shiftOn ? ">" : "."), COL_KEY_FACE, COL_KEY_LABEL);

    // Row 4: tall BS at bottom-left + [Shift] [Alt] [Space] [Hide]
    int row4Y = KB_Y + 4 * rowStep + 1;
    int bsY   = SCREEN_H - BS_H;
    uint16_t shiftFace = shiftOn ? TFT_NAVY : COL_BTN_BG;
    uint16_t altFace   = altOn   ? TFT_NAVY : COL_BTN_BG;
    drawKey(BS_X,    bsY,   BS_W,    BS_H,      "<-",    COL_BTN_BG, COL_BTN_TEXT);
    drawKey(SHIFT_X, row4Y, SHIFT_W, KEY_H - 1, shiftOn ? "SHF" : "shf", shiftFace, COL_BTN_TEXT);
    drawKey(ALT_X,   row4Y, ALT_W,   KEY_H - 1, altOn   ? "ALT" : "alt", altFace,   COL_BTN_TEXT);
    drawKey(SPACE_X, row4Y, SPACE_W, KEY_H - 1, "SPACE", COL_BTN_BG, COL_BTN_TEXT);
    drawKey(HIDE_X,  row4Y, HIDE_W,  KEY_H - 1, "Hide",  COL_BTN_BG, COL_BTN_TEXT);
}

// --- AP picker screen ---
#define AP_ROW_H  24   // height of each AP list row

// Convert RSSI to 4-char ASCII signal bar string.
static const char* rssiToBars(int rssi) {
    if (rssi >= -55) return "####";
    if (rssi >= -65) return "###.";
    if (rssi >= -75) return "##..";
    if (rssi >= -85) return "#...";
    return "....";
}

// Draw full-screen AP list. apCount entries from apSsids[]/apRssi[].
// Rows numbered 1–apCount starting at y=AP_ROW_H (row 0 = header).
void drawAPList(const char apSsids[][33], const int* apRssi, int apCount) {
    tft.fillScreen(COL_BG);
    fontOff();
    tft.setTextSize(1);
    // Header row
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.setCursor(2, (AP_ROW_H - 8) / 2);
    tft.print("Select WiFi network:");
    // Entry rows
    for (int i = 0; i < apCount; i++) {
        int y = AP_ROW_H * (i + 1);
        tft.fillRect(0, y, SCREEN_W, AP_ROW_H - 1, COL_KEY_FACE);
        tft.setTextColor(COL_KEY_LABEL, COL_KEY_FACE);
        int ty = y + (AP_ROW_H - 8) / 2;
        // Number
        char num[3]; snprintf(num, sizeof(num), "%d", i + 1);
        tft.setCursor(2, ty); tft.print(num);
        // SSID (truncated to 22 chars)
        char ssidDisp[23] = {0};
        strncpy(ssidDisp, apSsids[i], 22);
        tft.setCursor(16, ty); tft.print(ssidDisp);
        // Signal bars + dBm (right side, fixed position x=220)
        char sig[12];
        snprintf(sig, sizeof(sig), "%s %4d", rssiToBars(apRssi[i]), apRssi[i]);
        tft.setCursor(220, ty); tft.print(sig);
    }
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
static const int  MAX_LINES  = 250;       // 250×128=32KB DRAM; was 400 (overflow with 128-byte lines)
char              lines[MAX_LINES][128];  // 127 bytes + null per line (UTF-8 safe)
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
        // Text is pre-validated UTF-8 from addMessage(). Prefix chars are clean ASCII.

        if (largeFont) {
            // Pixel-width word wrap for DejaVuSansBold12px
            fontOn();
            char lineBuf[128] = "";
            const char* p = full;
            while (*p && lineCount < MAX_LINES - 1) {
                if (*p == '\n') {
                    // Only flush if there's content — skip blank lines to
                    // avoid wasting the small number of visible large-font slots
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
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
                char word[128];
                strncpy(word, ws, min(wlen, 127));
                word[min(wlen, 127)] = '\0';
                if (*p == ' ') p++;
                // Test word on current line
                char test[260];   // lineBuf(127) + space(1) + word(127) + null
                if (lineBuf[0]) snprintf(test, 260, "%s %s", lineBuf, word);
                else            { strncpy(test, word, 127); test[127] = '\0'; }
                if (tft.textWidth(test) <= SCREEN_W - 2) {
                    strncpy(lineBuf, test, 127); lineBuf[127] = '\0';
                } else {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                        strncpy(lineBuf, word, 127); lineBuf[127] = '\0';
                    } else {
                        strncpy(lines[lineCount], word, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                    }
                }
            }
            if (lineBuf[0] && lineCount < MAX_LINES) {
                strncpy(lines[lineCount], lineBuf, 127);
                lines[lineCount][127] = '\0';
                lineColor[lineCount++] = col;
            }
            fontOff();
        } else {
            // Pixel-width word wrap for DejaVuSansBold8px
            tft.loadFont(DejaVuSansBold8pxData);
            char lineBuf[128] = "";
            const char* p = full;
            while (*p && lineCount < MAX_LINES - 1) {
                if (*p == '\n') {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                        lineBuf[0] = '\0';
                    }
                    p++;
                    continue;
                }
                const char* ws = p;
                while (*p && *p != ' ' && *p != '\n') p++;
                int wlen = p - ws;
                if (wlen <= 0) { if (*p) p++; continue; }
                char word[128];
                strncpy(word, ws, min(wlen, 127));
                word[min(wlen, 127)] = '\0';
                if (*p == ' ') p++;
                char test[260];
                if (lineBuf[0]) snprintf(test, 260, "%s %s", lineBuf, word);
                else            { strncpy(test, word, 127); test[127] = '\0'; }
                if (tft.textWidth(test) <= SCREEN_W - 2) {
                    strncpy(lineBuf, test, 127); lineBuf[127] = '\0';
                } else {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                        strncpy(lineBuf, word, 127); lineBuf[127] = '\0';
                    } else {
                        strncpy(lines[lineCount], word, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                    }
                }
            }
            if (lineBuf[0] && lineCount < MAX_LINES) {
                strncpy(lines[lineCount], lineBuf, 127);
                lines[lineCount][127] = '\0';
                lineColor[lineCount++] = col;
            }
            tft.unloadFont();
        }
    }
}

void drawHistory() {
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
    tft.fillRect(0, 0, SCREEN_W, histH, bg);

    int lineH  = largeFont ? LINE_H_LARGE : LINE_H_SMALL;   // DejaVuSansBold12px: 15px  GLCD: 8px+2gap
    int maxVis = histH / lineH;

    // scrollOffset=0 means show bottom of history
    int firstIdx = lineCount - maxVis - scrollOffset;
    if (firstIdx < 0) firstIdx = 0;

    if (largeFont) fontOn(); else tft.loadFont(DejaVuSansBold8pxData);
    tft.setTextWrap(false);  // prevent overflow wrapping onto adjacent lines
    for (int i = 0; i < maxVis && (firstIdx + i) < lineCount; i++) {
        int idx = firstIdx + i;
        uint16_t col = lineColor[idx];
        if (invertDisplay) col = TFT_BLACK;
        tft.setTextColor(col, bg);
        tft.drawString(lines[idx], 0, i * lineH);
    }
    tft.setTextWrap(true);
    fontOff();
}

// Returns true if codepoint cp is covered by the VLW font.
static bool supportedCodepoint(uint32_t cp) {
    if (cp >= 0x0020 && cp <= 0x007E) return true;  // Printable ASCII
    if (cp >= 0x00A0 && cp <= 0x017F) return true;  // Latin-1 + Latin Extended-A
    if (cp == 0x03A9 || cp == 0x03C0) return true;  // Ω π
    if (cp == 0x2011 || cp == 0x2013 || cp == 0x2014) return true;  // dashes
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D) return true;  // quotes
    if (cp == 0x2022 || cp == 0x2026) return true;  // bullet, ellipsis
    if (cp >= 0x2070 && cp <= 0x207F) return true;  // superscripts
    if (cp == 0x20AC) return true;  // €
    return false;
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
    // Sanitise: pass supported UTF-8 codepoints through unchanged.
    // Strip C0 controls, drop U+200B (zero-width space), replace unsupported
    // multi-byte sequences with '?'.
    {
        const char* s = history[historyCount].text;
        char tmp[2060];
        char* d   = tmp;
        char* end = tmp + sizeof(tmp) - 4;
        while (*s && d < end) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) {
                // ASCII
                if (c == '\n' || (c >= 0x20 && c != 0x7F)) *d++ = (char)c;
                s++;
            } else if ((c & 0xE0) == 0xC0) {
                // 2-byte sequence
                unsigned char b2 = (unsigned char)s[1];
                if ((b2 & 0xC0) == 0x80) {
                    uint32_t cp = ((c & 0x1F) << 6) | (b2 & 0x3F);
                    if (supportedCodepoint(cp)) { *d++ = (char)c; *d++ = (char)b2; }
                    else                         *d++ = '?';
                    s += 2;
                } else { *d++ = '?'; s++; }
            } else if ((c & 0xF0) == 0xE0) {
                // 3-byte sequence
                unsigned char b2 = (unsigned char)s[1];
                unsigned char b3 = (unsigned char)s[2];
                if ((b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                    uint32_t cp = ((c & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                    if (cp == 0x200B) {
                        /* zero-width space: drop silently */
                    } else if (supportedCodepoint(cp)) {
                        *d++ = (char)c; *d++ = (char)b2; *d++ = (char)b3;
                    } else { *d++ = '?'; }
                    s += 3;
                } else { *d++ = '?'; s++; }
            } else if ((c & 0xF8) == 0xF0) {
                // 4-byte sequence: unsupported range
                *d++ = '?'; s += 4;
            } else {
                s++;  // invalid byte: skip
            }
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
    sx = map(tp.x, calXmin, calXmax, 0, SCREEN_W - 1);
    sy = map(tp.y, calYmin, calYmax, 0, SCREEN_H - 1);
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
            x = 0;
            for (int i = 0; i < 10; i++, x += KEY_W) {
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
            x = 0;
            for (int i = 0; i < 10; i++, x += KEY_W) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW1[i] : (KB_ROW1[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            break;
        }
        case 2: {  // ASDFGHJKL + /
            x = 0;
            for (int i = 0; i < 9; i++, x += KEY_W) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW2[i] : (KB_ROW2[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            if (typed.length() == 0 && inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                const char* s = altOn ? "\\" : (shiftOn ? "?" : "/");
                typed = String(s); strncpy(lbl, s, sizeof(lbl) - 1); kx = x;
            }
            break;
        }
        case 3: {  // ZXCVBNM + ,.  (shifted: <>  alt: [])
            x = 0;
            for (int i = 0; i < 7; i++, x += KEY_W) {
                if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                    char c = shiftOn ? KB_ROW3[i] : (KB_ROW3[i] + 32);
                    typed = String(c); lbl[0] = c; lbl[1] = '\0'; kx = x; break;
                }
            }
            if (typed.length() == 0) {
                const char unshifted[] = { ',', '.' };
                const char shifted[]   = { '<', '>' };
                const char alt_ext[]   = { '[', ']' };
                const char* extras = altOn ? alt_ext : (shiftOn ? shifted : unshifted);
                for (int i = 0; i < 2; i++, x += KEY_W) {
                    if (inRect(sx, sy, x, rowY, KEY_W, KEY_H)) {
                        typed = String(extras[i]); lbl[0] = extras[i]; lbl[1] = '\0'; kx = x; break;
                    }
                }
            }
            break;
        }
        // case 4: Shift/Alt/Space/Hide/BS — handled in handleTouch()
    }

    if (kx >= 0 && typed.length() > 0) {
        drawKey(kx, rowY, KEY_W, KEY_H, lbl, TFT_WHITE, COL_BG);
        delay(KEY_FLASH_MS);
        drawKey(kx, rowY, KEY_W, KEY_H, lbl, COL_KEY_FACE, COL_KEY_LABEL);
    }
    return typed;
}

// Show keyboard and let user type a password. Returns typed string in out (64 bytes).
// Uses the existing keyboard (inputBuf/inputLen/shiftOn/altOn globals).
// Tap the input bar (Send button area) to submit.
void enterPassword(const char* ssidPrompt, char* out) {
    inputBuf[0] = '\0';
    inputLen    = 0;
    moreMode    = false;
    shiftOn     = false;
    altOn       = false;
    kbVisible   = true;

    tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    fontOff();
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.setCursor(0, 0);  tft.print("Password for:");
    tft.setTextColor(TFT_WHITE,  COL_BG);
    tft.setCursor(0, 10); tft.print(ssidPrompt);
    drawInputBar();
    drawKeyboard();

    while (true) {
        if (!ts.touched()) continue;
        TS_Point pt = ts.getPoint();
        while (ts.touched()) delay(5);
        int sx, sy;
        mapTouch(pt, sx, sy);

        // Input bar tap → submit
        if (sy >= IBAR_Y_KB_SHOW && sy < IBAR_Y_KB_SHOW + IBAR_H_KB_SHOW) {
            strncpy(out, inputBuf, 63); out[63] = '\0';
            inputBuf[0] = '\0'; inputLen = 0;   // clear sensitive data
            return;
        }

        // Keyboard area
        if (sy >= KB_Y) {
            int bsY = SCREEN_H - BS_H;
            // BS key
            if (inRect(sx, sy, BS_X, bsY, BS_W, BS_H)) {
                drawKey(BS_X, bsY, BS_W, BS_H, "<-", TFT_WHITE, COL_BTN_TEXT);
                delay(KEY_FLASH_MS);
                drawKey(BS_X, bsY, BS_W, BS_H, "<-", COL_BTN_BG, COL_BTN_TEXT);
                if (inputLen > 0) { inputBuf[--inputLen] = '\0'; drawInputBar(); }
                continue;
            }
            // Row 4: Shift, Alt, Space (Hide ignored during password entry)
            int row4Y = KB_Y + 4 * (KEY_H + KEY_GAP) + 1;
            if (sy >= row4Y && sy < row4Y + KEY_H - 1) {
                if (inRect(sx, sy, SHIFT_X, row4Y, SHIFT_W, KEY_H - 1)) {
                    shiftOn = !shiftOn; altOn = false; drawKeyboard();
                } else if (inRect(sx, sy, ALT_X, row4Y, ALT_W, KEY_H - 1)) {
                    altOn = !altOn; shiftOn = false; drawKeyboard();
                } else if (inRect(sx, sy, SPACE_X, row4Y, SPACE_W, KEY_H - 1)) {
                    if (inputLen < 63) {
                        inputBuf[inputLen++] = ' '; inputBuf[inputLen] = '\0';
                        drawInputBar();
                    }
                }
                continue;
            }
            // Character keys
            String typed = typeKBKey(sx, sy);
            if (typed.length() > 0) {
                int addLen = typed.length();
                if (inputLen + addLen <= 63) {
                    memcpy(inputBuf + inputLen, typed.c_str(), addLen);
                    inputLen += addLen;
                    inputBuf[inputLen] = '\0';
                    drawInputBar();
                }
            }
        }
    }
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

    // --- Input bar ---
    if (sy >= barY && sy < barY + barH) {
        if (sx >= SCREEN_W - BTN_SEND_W) {
            // Send/More button
            sendPrompt();
        } else if (kbVisible) {
            // Tap text area with KB shown → backspace
            if (inputLen > 0) {
                inputBuf[--inputLen] = '\0';
                if (inputLen == 0 && historyCount > 0) moreMode = true;
                drawInputBar();
            }
        } else {
            // KB hidden → show it
            kbVisible = true;
            tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            drawHistory();
            drawInputBar();
            drawKeyboard();
        }
        return;
    }

    // --- Keyboard area ---
    if (kbVisible && sy >= KB_Y) {
        int rowStep = KEY_H + KEY_GAP;
        int row4Y   = KB_Y + 4 * rowStep + 1;

        // BS — tall key at bottom left, spans row 3/4 area
        int bsY = SCREEN_H - BS_H;
        if (inRect(sx, sy, BS_X, bsY, BS_W, BS_H)) {
            drawKey(BS_X, bsY, BS_W, BS_H, "<-", TFT_WHITE, COL_BTN_TEXT);
            delay(KEY_FLASH_MS);
            drawKey(BS_X, bsY, BS_W, BS_H, "<-", COL_BTN_BG, COL_BTN_TEXT);
            if (inputLen > 0) {
                inputBuf[--inputLen] = '\0';
                if (inputLen == 0 && historyCount > 0) moreMode = true;
                drawInputBar();
            }
            return;
        }

        // Row 4 special keys
        if (sy >= row4Y && sy < row4Y + KEY_H - 1) {
            if (inRect(sx, sy, SHIFT_X, row4Y, SHIFT_W, KEY_H - 1)) {
                shiftOn = !shiftOn;
                if (shiftOn) altOn = false;
                drawKeyboard();
            } else if (inRect(sx, sy, ALT_X, row4Y, ALT_W, KEY_H - 1)) {
                altOn = !altOn;
                if (altOn) shiftOn = false;
                drawKeyboard();
            } else if (inRect(sx, sy, SPACE_X, row4Y, SPACE_W, KEY_H - 1)) {
                drawKey(SPACE_X, row4Y, SPACE_W, KEY_H - 1, "SPACE", TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(SPACE_X, row4Y, SPACE_W, KEY_H - 1, "SPACE", COL_BTN_BG, COL_BTN_TEXT);
                if (inputLen < INPUT_MAX_LEN) { moreMode = false; inputBuf[inputLen++] = ' '; inputBuf[inputLen] = '\0'; drawInputBar(); }
            } else if (inRect(sx, sy, HIDE_X, row4Y, HIDE_W, KEY_H - 1)) {
                kbVisible = false;
                tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
                drawHistory();
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
// Returns true if connected. Pass showSplash=true for boot (draws title + "Connecting:" text).
bool connectWiFi(const char* ssid, const char* pass, bool showSplash = false) {
    if (showSplash) {
        fontOn();
        tft.setTextColor(TFT_NAVY, TFT_WHITE);
        char wifiMsg[80];
        snprintf(wifiMsg, sizeof(wifiMsg), "Connecting: %.55s...", ssid);
        tft.drawString(wifiMsg, 0, 0);
        fontOff();
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_ATTEMPTS) {
        delay(WIFI_RETRY_DELAY_MS);
        attempts++;
    }
    if (showSplash) tft.fillRect(0, 0, SCREEN_W, LINE_H_LARGE + 2, TFT_WHITE);
    updateLedWifi();
    return WiFi.status() == WL_CONNECTED;
}

// Scan WiFi, let user pick an AP, handle password entry, connect.
// Loops until connected. Call from setup() when connectWiFi() returns false.
void selectAP() {
    while (true) {  // outer: re-scan loop
        // --- Scan ---
        WiFi.disconnect(true);  // ensure clean idle state before scan (prev. begin() may leave driver busy)
        tft.fillScreen(COL_BG);
        fontOff(); tft.setTextColor(TFT_YELLOW, COL_BG);
        tft.setCursor(0, 0); tft.print("Scanning WiFi...");

        int n = WiFi.scanNetworks();

        if (n <= 0) {
            tft.fillScreen(COL_BG);
            tft.setCursor(0, 0); tft.print("No networks found. Tap to retry.");
            while (!ts.touched()) delay(50);
            while (ts.touched()) delay(5);
            continue;
        }

        // Sort indices by RSSI descending, keep top 9
        int indices[40];
        int total = (n < 40) ? n : 40;
        for (int i = 0; i < total; i++) indices[i] = i;
        for (int i = 0; i < total - 1; i++) {
            for (int j = i + 1; j < total; j++) {
                if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
                    int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
                }
            }
        }
        int apCount = (total < 9) ? total : 9;

        char apSsids[9][33];
        int  apRssi[9];
        for (int i = 0; i < apCount; i++) {
            strncpy(apSsids[i], WiFi.SSID(indices[i]).c_str(), 32);
            apSsids[i][32] = '\0';
            apRssi[i] = WiFi.RSSI(indices[i]);
        }
        WiFi.scanDelete();

        drawAPList(apSsids, apRssi, apCount);

        // --- Wait for AP selection ---
        int selected = -1;
        while (selected < 0) {
            if (!ts.touched()) continue;
            TS_Point pt = ts.getPoint();
            while (ts.touched()) delay(5);
            int sx, sy;
            mapTouch(pt, sx, sy);
            for (int i = 0; i < apCount; i++) {
                int rowY = AP_ROW_H * (i + 1);
                if (sy >= rowY && sy < rowY + AP_ROW_H) { selected = i; break; }
            }
        }

        char selSsid[33];
        strncpy(selSsid, apSsids[selected], 32); selSsid[32] = '\0';

        // --- Password + connect loop for this AP ---
        while (true) {
            char pass[64] = {0};
            bool hasStored = findWifiPass(selSsid, pass);

            if (!hasStored) {
                enterPassword(selSsid, pass);
            }

            // Show connecting
            tft.fillScreen(COL_BG);
            fontOff(); tft.setTextColor(TFT_BLUE, COL_BG);
            char msg[80]; snprintf(msg, sizeof(msg), "Connecting: %.55s...", selSsid);
            tft.setCursor(0, 0); tft.print(msg);

            bool ok = connectWiFi(selSsid, pass);

            if (ok) {
                insertWifiCred(selSsid, pass);
                return;  // connected — setup() continues to selectModel()
            }

            // --- Failed: offer re-enter or new scan ---
            tft.fillScreen(COL_BG);
            fontOff();
            tft.setTextColor(TFT_RED, COL_BG);
            char failMsg[64]; snprintf(failMsg, sizeof(failMsg), "Failed: %.40s", selSsid);
            tft.setCursor(2, (AP_ROW_H - 8) / 2); tft.print(failMsg);

            // Draw two option rows (same style as AP list)
            static const char* opts[] = { "1  Re-enter password", "2  New scan" };
            for (int i = 0; i < 2; i++) {
                int y = AP_ROW_H * (i + 1);
                tft.fillRect(0, y, SCREEN_W, AP_ROW_H - 1, COL_KEY_FACE);
                tft.setTextColor(COL_KEY_LABEL, COL_KEY_FACE);
                tft.setCursor(2, y + (AP_ROW_H - 8) / 2);
                tft.print(opts[i]);
            }

            // Wait for tap on row 1 or 2
            int choice = 0;
            while (choice == 0) {
                if (!ts.touched()) continue;
                TS_Point pt = ts.getPoint();
                while (ts.touched()) delay(5);
                int sx, sy; mapTouch(pt, sx, sy);
                if (sy >= AP_ROW_H && sy < AP_ROW_H * 2) choice = 1;  // re-enter
                if (sy >= AP_ROW_H * 2 && sy < AP_ROW_H * 3) choice = 2;  // new scan
            }

            if (choice == 2) break;  // break inner loop → outer re-scan loop
            // choice == 1: clear stored pass, loop inner (enterPassword runs next iteration)
            clearWifiPass(selSsid);
        }
    }
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
    "SLUGGISH. It's a slime of the times...",
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
    "This is a SLUGfest...",
    "If delays were cash, I'd be rich...",
    "Is this running on a TRS80?...",
    "Grrr...",
    "Hurry the fuck up, AI...",
    "You're a snail-ass bot...",
	"Take the shell off a snail and it's SLUGGISH",
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
        int row4Y = KB_Y + 4 * (KEY_H + KEY_GAP) + 1;
        if (inRect(sx, sy, HIDE_X, row4Y, HIDE_W, KEY_H - 1)) {
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
    sysPart["text"]     = "Respond in 150 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols. Use paragraphs to separate distinct ideas. Never include URLs or hyperlinks. When quoting current or time-sensitive information, use your search tool to check live sources first.";

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
        connectWiFi(wifiSsid[0], wifiPass[0]);
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

    // Extract HTTP status code
    int httpStatus = 200;
    int statusPos = fullResp.indexOf("HTTP/");
    if (statusPos >= 0) {
        int eol = fullResp.indexOf('\n', statusPos);
        sscanf(fullResp.substring(statusPos, eol).c_str(), "HTTP/%*s %d", &httpStatus);
    }

    // Locate JSON body — starts at first '{'
    int jsonStart = fullResp.indexOf('{');
    if (jsonStart < 0) {
        char buf[40]; snprintf(buf, sizeof(buf), "ERR: HTTP %d, no JSON", httpStatus);
        return String(buf);
    }
    String respBody = fullResp.substring(jsonStart);

    // Parse JSON — success if candidates present, error if error.message present
    JsonDocument respDoc;
    if (deserializeJson(respDoc, respBody) != DeserializationError::Ok) {
        Serial.println("Bad JSON (HTTP " + String(httpStatus) + "): " + respBody.substring(0, 300));
        char buf[40]; snprintf(buf, sizeof(buf), "ERR: HTTP %d, bad JSON", httpStatus);
        return String(buf);
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
    reqDoc["instructions"] = "Respond in 150 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols, no numbered or unnumbered lists. Use paragraphs to separate distinct ideas. Never include URLs, hyperlinks, citations, footnotes, source references, or attribution of any kind. Do not mention where information came from. When quoting current or time-sensitive information, use your web search tool to check live sources first.";

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
        connectWiFi(wifiSsid[0], wifiPass[0]);
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

String callGroq(const char* prompt) {
    // Groq: OpenAI-compatible /openai/v1/chat/completions
    JsonDocument reqDoc;
    reqDoc["model"]  = GROQ_MODEL;
    reqDoc["stream"] = false;

    JsonArray messages = reqDoc["messages"].to<JsonArray>();

    // System prompt
    JsonObject sysMsgObj = messages.add<JsonObject>();
    sysMsgObj["role"]    = "system";
    sysMsgObj["content"] = "Respond in 150 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols, no numbered or unnumbered lists. Use paragraphs to separate distinct ideas. Never include URLs, hyperlinks, citations, footnotes, source references, or attribution of any kind.";

    // Conversation history (exclude last entry — current prompt)
    for (int i = 0; i < historyCount - 1; i++) {
        JsonObject msg  = messages.add<JsonObject>();
        msg["role"]     = history[i].isUser ? "user" : "assistant";
        msg["content"]  = history[i].text;
    }

    // Current prompt
    JsonObject curMsg  = messages.add<JsonObject>();
    curMsg["role"]     = "user";
    curMsg["content"]  = prompt;

    String body;
    serializeJson(reqDoc, body);

    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi(wifiSsid[0], wifiPass[0]);
        if (WiFi.status() != WL_CONNECTED) return "ERR: WiFi not connected";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(API_TIMEOUT_MS / 1000);

    if (!client.connect(GROQ_HOST, HTTPS_PORT)) {
        return "ERR: connect failed (check WiFi/DNS)";
    }

    client.printf(
        "POST /openai/v1/chat/completions HTTP/1.0\r\n"
        "Host: " GROQ_HOST "\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n\r\n",
        GROQ_API_KEY, (int)body.length());
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
    Serial.printf("[Groq] response len=%d\n", fullResp.length());

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
    Serial.println("[Groq] JSON body: " + respBody.substring(0, 400));

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

    // OpenAI-compatible: choices[0].message.content
    const char* text = respDoc["choices"][0]["message"]["content"];
    if (text) return String(text);

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
    kbVisible = false;   // hide KB; drawHistory() covers the KB area
    drawInputBar();      // clear full bar now — erases BS/Hide key remnants before API wait

    // Show user message and thinking indicator
    addMessage(true, false, prompt);
    showThinking();

    // Call API
    String response = useGroq ? callGroq(prompt) : (useGrok ? callGrok(prompt) : callGemini(prompt));

    if (response.startsWith("ERR:")) {
        addMessage(false, true, response.c_str());
    } else {
        addMessage(false, false, response.c_str());
    }

    moreMode = true;   // after every reply, offer "More"
    wifiHealthy = (WiFi.status() == WL_CONNECTED);  // refresh after API call; LED uses RSSI independently
    drawInputBar();
}

void drawCrosshair(int x, int y) {
    tft.drawLine(x - 18, y, x + 18, y, TFT_WHITE);
    tft.drawLine(x, y - 18, x, y + 18, TFT_WHITE);
    tft.drawCircle(x, y, 6, TFT_WHITE);
}

void calibrateTouch() {
    const int T1X = 20,           T1Y = 20;
    const int T2X = SCREEN_W - 20, T2Y = SCREEN_H - 20;

    tft.fillScreen(COL_BG);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, COL_BG);

    // Flush any spurious touches
    while (ts.touched()) delay(5);
    delay(200);

    // --- Point 1: top-left ---
    drawCrosshair(T1X, T1Y);
    tft.setCursor(60, 110); tft.print("Tap the crosshair");

    while (!ts.touched()) delay(5);
    long sumX = 0, sumY = 0; int n = 0;
    while (ts.touched()) {
        TS_Point p = ts.getPoint();
        sumX += p.x; sumY += p.y; n++;
        delay(5);
    }
    int rx1 = sumX / n, ry1 = sumY / n;

    // --- Point 2: bottom-right ---
    tft.fillScreen(COL_BG);
    drawCrosshair(T2X, T2Y);
    tft.setCursor(60, 110); tft.print("Tap the crosshair");

    delay(300);
    while (!ts.touched()) delay(5);
    sumX = 0; sumY = 0; n = 0;
    while (ts.touched()) {
        TS_Point p = ts.getPoint();
        sumX += p.x; sumY += p.y; n++;
        delay(5);
    }
    int rx2 = sumX / n, ry2 = sumY / n;

    // Extrapolate raw values to screen edges (0 and SCREEN_W/H - 1)
    long dRx = rx2 - rx1, dTx = T2X - T1X;
    long dRy = ry2 - ry1, dTy = T2Y - T1Y;
    calXmin = (int)(rx1 - dRx * T1X / dTx);
    calXmax = (int)(rx2 + dRx * (SCREEN_W - 1 - T2X) / dTx);
    calYmin = (int)(ry1 - dRy * T1Y / dTy);
    calYmax = (int)(ry2 + dRy * (SCREEN_H - 1 - T2Y) / dTy);

    saveTouchCal();
    Serial.printf("[Cal] xmin=%d xmax=%d ymin=%d ymax=%d\n", calXmin, calXmax, calYmin, calYmax);

    tft.fillScreen(COL_BG);
    tft.setTextColor(TFT_GREEN, COL_BG);
    tft.setCursor(60, 110); tft.print("Calibration saved!");
    delay(1500);
}

// Slide the SLUG logo off to the right over 1 second
void slideOutSlug() {
    const int    imgX0 = SCREEN_W - SLUGSMALL_W;   // 176
    const int    imgY  = 20;
    const unsigned long dur = 1000UL;
    unsigned long t0 = millis();
    int prevX = imgX0;
    while (true) {
        unsigned long elapsed = millis() - t0;
        if (elapsed >= dur) break;
        int x = imgX0 + (int)((long)SLUGSMALL_W * elapsed / dur);
        if (x > prevX) {
            // Erase only the newly-exposed left strip (no transparent holes to worry about)
            tft.fillRect(prevX, imgY, x - prevX, SLUGSMALL_H, COL_BG);
            if (x < SCREEN_W)
                tft.pushImage(x, imgY, SLUGSMALL_W, SLUGSMALL_H, SLUG_SMALL);
            prevX = x;
        }
        delay(16);
    }
    tft.fillRect(imgX0, imgY, SLUGSMALL_W, SLUGSMALL_H, COL_BG);
}

void showModelChoices() {
    // Slug logo top-right (below the two help lines, clear of left-aligned model text)
    tft.pushImage(SCREEN_W - SLUGSMALL_W, 20, SLUGSMALL_W, SLUGSMALL_H, SLUG_SMALL);

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.setCursor(0, 30); tft.print("Hit 1 for Gemini 2.5 Flash");
    tft.setCursor(0, 40); tft.print("    2 for Gemini 3 Flash");
    tft.setCursor(0, 50); tft.print("    3 for Gemini 3.1 Pro");
    tft.setCursor(0, 60); tft.print("    4 for Grok 4.1 Fast Reasoning");
    tft.setCursor(0, 70); tft.print("    5 for Groq GPT-OSS-120b");
    int nextY = 80;
    if (!largeFont) {
        tft.setCursor(0, nextY); tft.print("    b for bigger font");
        nextY += 10;
    }
    if (!invertDisplay) {
        tft.setCursor(0, nextY); tft.print("    i  to invert colours");
        nextY += 10;
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
        int x = 0;
        for (int i = 0; i < 3; i++, x += KEY_W) {
            if (inRect(sx, sy, x, KB_Y, KEY_W, KEY_H)) {
                char lbl[2] = { char('1' + i), '\0' };
                drawKey(x, KB_Y, KEY_W, KEY_H, lbl, TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(x, KB_Y, KEY_W, KEY_H, lbl, COL_KEY_FACE, COL_KEY_LABEL);
                slideOutSlug();
                strncpy(GEMINI_MODEL, modelIds[i], 47);
                GEMINI_MODEL[47] = '\0';
                geminiUseGlobal  = modelGlobal[i];
                useGrok          = false;
                useGroq          = false;
                uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
                tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, bg);
                if (largeFont) {
                    fontOn();
                    tft.setTextColor(TFT_GREEN, bg);
                    tft.drawString("SLUG AI chatbot", 0, 0);
                    tft.drawString(GEMINI_MODEL, 0, LINE_H_LARGE);
                    tft.setTextColor(TFT_DARKGREY, bg);
                    tft.drawString("Ready.", 0, 2 * LINE_H_LARGE);
                    fontOff();
                } else {
                    tft.setTextSize(1);
                    tft.setTextColor(TFT_GREEN, bg);
                    tft.setCursor(0,  0); tft.print("SLUG AI Chatbot v0.1");
                    tft.setCursor(0, 10); tft.print("Model: "); tft.print(GEMINI_MODEL);
                    tft.setTextColor(TFT_DARKGREY, bg);
                    tft.setCursor(0, 20); tft.print("Ready.");
                }
                return;
            }
        }

        // Key 4 — Grok 4.1 Fast
        int x4 = 3 * KEY_W;
        if (inRect(sx, sy, x4, KB_Y, KEY_W, KEY_H)) {
            drawKey(x4, KB_Y, KEY_W, KEY_H, "4", TFT_WHITE, COL_BG);
            delay(KEY_FLASH_MS);
            drawKey(x4, KB_Y, KEY_W, KEY_H, "4", COL_KEY_FACE, COL_KEY_LABEL);
            slideOutSlug();
            useGrok = true;
            useGroq = false;
            uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
            tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, bg);
            if (largeFont) {
                fontOn();
                tft.setTextColor(TFT_GREEN, bg);
                tft.drawString("SLUG AI Chatbot", 0, 0);
                tft.drawString("Grok 4.1 Fast", 0, LINE_H_LARGE);
                tft.setTextColor(TFT_DARKGREY, bg);
                tft.drawString("Ready.", 0, 2 * LINE_H_LARGE);
                fontOff();
            } else {
                tft.setTextSize(1);
                tft.setTextColor(TFT_GREEN, bg);
                tft.setCursor(0,  0); tft.print("SLUG AI Chatbot. Swipe down/up to scroll.");
                tft.setCursor(0, 10); tft.print("Model: Grok 4.1 Fast");
                tft.setTextColor(TFT_DARKGREY, bg);
                tft.setCursor(0, 20); tft.print("Ready.");
            }
            return;
        }

        // Key 5 — Groq GPT-OSS-120b
        int x5 = 4 * KEY_W;
        if (inRect(sx, sy, x5, KB_Y, KEY_W, KEY_H)) {
            drawKey(x5, KB_Y, KEY_W, KEY_H, "5", TFT_WHITE, COL_BG);
            delay(KEY_FLASH_MS);
            drawKey(x5, KB_Y, KEY_W, KEY_H, "5", COL_KEY_FACE, COL_KEY_LABEL);
            slideOutSlug();
            useGroq = true;
            useGrok = false;
            uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
            tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, bg);
            if (largeFont) {
                fontOn();
                tft.setTextColor(TFT_GREEN, bg);
                tft.drawString("SLUG AI chatbot", 0, 0);
                tft.drawString("Groq GPT-OSS-120b", 0, LINE_H_LARGE);
                tft.setTextColor(TFT_DARKGREY, bg);
                tft.drawString("Ready.", 0, 2 * LINE_H_LARGE);
                fontOff();
            } else {
                tft.setTextSize(1);
                tft.setTextColor(TFT_GREEN, bg);
                tft.setCursor(0,  0); tft.print("SLUG AI chatbot. Swipe down/up to scroll.");
                tft.setCursor(0, 10); tft.print("Model: Groq GPT-OSS-120b");
                tft.setTextColor(TFT_DARKGREY, bg);
                tft.setCursor(0, 20); tft.print("Ready.");
            }
            return;
        }

        // Key B (ZXCVBNM row, index 4) — toggle large text, re-show choices
        if (!largeFont) {
            int rowStep = KEY_H + KEY_GAP;
            int row3Y   = KB_Y + 3 * rowStep;
            int xb      = 4 * KEY_W;
            if (inRect(sx, sy, xb, row3Y, KEY_W, KEY_H)) {
                drawKey(xb, row3Y, KEY_W, KEY_H, "B", TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(xb, row3Y, KEY_W, KEY_H, "b", COL_KEY_FACE, COL_KEY_LABEL);
                largeFont = true;
                tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, COL_BG);
                fontOn();
                tft.setTextColor(TFT_DARKGREY, COL_BG);
                tft.drawString("SLUG AI chatbot. Large text.", 0, 0);
                tft.drawString("Select AI model:", 0, LINE_H_LARGE);
                fontOff();
                showModelChoices();
            }
        }

        // Key I (QWERTY row, index 7) — toggle invert colours, re-show choices
        {
            int rowStep = KEY_H + KEY_GAP;
            int row1Y   = KB_Y + rowStep;
            int xi      = 7 * KEY_W;
            if (inRect(sx, sy, xi, row1Y, KEY_W, KEY_H)) {
                drawKey(xi, row1Y, KEY_W, KEY_H, "I", TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(xi, row1Y, KEY_W, KEY_H, "i", COL_KEY_FACE, COL_KEY_LABEL);
                invertDisplay = !invertDisplay;
                tft.fillRect(0, 0, SCREEN_W, HIST_H_KB_SHOW, COL_BG);
                if (largeFont) {
                    fontOn();
                    tft.setTextColor(TFT_DARKGREY, COL_BG);
                    tft.drawString("SLUG AI chatbot. Large text.", 0, 0);
                    tft.drawString("Select AI model:", 0, LINE_H_LARGE);
                    fontOff();
                } else {
                    tft.setTextSize(1);
                    tft.setTextColor(TFT_DARKGREY, COL_BG);
                    tft.setCursor(0,  0); tft.print("SLUG AI chatbot");
                    tft.setCursor(0, 10); tft.print("Ready. Select AI model:");
                }
                showModelChoices();
            }
        }

        // Key C (ZXCVBNM row, index 2) — touch calibration
        {
            int rowStep = KEY_H + KEY_GAP;
            int row3Y   = KB_Y + 3 * rowStep;
            int xc      = 2 * KEY_W;
            if (inRect(sx, sy, xc, row3Y, KEY_W, KEY_H)) {
                drawKey(xc, row3Y, KEY_W, KEY_H, "C", TFT_WHITE, COL_BG);
                delay(KEY_FLASH_MS);
                drawKey(xc, row3Y, KEY_W, KEY_H, "c", COL_KEY_FACE, COL_KEY_LABEL);
                calibrateTouch();
                tft.fillScreen(COL_BG);
                tft.setTextSize(1);
                tft.setTextColor(TFT_DARKGREY, COL_BG);
                tft.setCursor(0,  0); tft.print("SLUG AI chatbot");
                tft.setCursor(0, 10); tft.print("Ready. Select AI model:");
                drawKeyboard();
                drawInputBar();
                showModelChoices();
            }
        }
    }
}

void setup() {
    loadWifiCreds();   // load NVS; shows AP picker on first boot if no credentials stored
    loadTouchCal();    // load saved touch calibration from NVS (or keep defaults)

    // Hold BOOT button (GPIO0) on power-on to wipe touch calibration back to defaults
    pinMode(0, INPUT_PULLUP);
    if (digitalRead(0) == LOW) {
        Preferences p;
        p.begin("touch", false);
        p.remove("valid");
        p.remove("orient");
        p.end();
        calXmin = 200; calXmax = 3900; calYmin = 200; calYmax = 3900;
        Serial.begin(115200);
        Serial.println("[Cal] Reset to defaults via BOOT button");
        delay(500);
    }
    Serial.begin(115200);
    Serial.printf("[Cal] xmin=%d xmax=%d ymin=%d ymax=%d\n", calXmin, calXmax, calYmin, calYmax);
    waitMsgIdx = random(NUM_WAIT_MSGS);

    // Display (init before backlight to avoid white flash)
    tft.init();
#ifdef ROTATE_180
    tft.setRotation(3);
#else
    tft.setRotation(1);
#endif
    tft.fillScreen(COL_BG);

    // Backlight on after screen is ready
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // Touch
    touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS_PIN);
    ts.begin(touchSPI);
#ifdef ROTATE_180
    ts.setRotation(3);  // 180°: xraw = 4095-x, yraw = 4095-y; keeps same cal defaults (200/3900)
#else
    ts.setRotation(1);  // landscape USB-right: xraw = x, yraw = y
#endif

    // Drain spurious startup touch
    unsigned long t0 = millis();
    while (ts.touched() && millis() - t0 < 500) delay(10);

    setupRGBLed();

    // Boot splash at bottom; white area above it; "Connecting..." is the only text shown
    unsigned long splashStart = millis();
    tft.fillRect(0, 0, SCREEN_W, SCREEN_H - SLUG_H, TFT_WHITE);
    tft.pushImage(0, SCREEN_H - SLUG_H, SLUG_W, SLUG_H, SLUG_SPLASH);
    bool wifiOk = connectWiFi(wifiSsid[0], wifiPass[0], true);
    // Splash visible for at least 3 seconds
    long splashRemain = 3000L - (long)(millis() - splashStart);
    if (splashRemain > 0) delay(splashRemain);
    tft.fillScreen(COL_BG);  // clear splash; back to normal dark background

    if (!wifiOk) {
        selectAP();  // scan → pick AP → enter password → connect; returns only on success
    }

    drawKeyboard();
    drawInputBar();

    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, COL_BG);
    tft.setCursor(0,  0); tft.print("SLUG AI chatbot");
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
        WiFi.begin(wifiSsid[0], wifiPass[0]);
    }
}

void loop() {
    handleTouch();
    checkWiFiHealth();
}
