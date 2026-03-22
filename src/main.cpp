// Cheap AI Chat Keyboard — ESP32-C3 + CYD28
// 090326 Key click: 4 kHz squarewave 10 ms on GPIO 26 speaker (LEDC ch 3)
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
#include "fonts/DejaVuSansBold8px.h"   // VLW smooth font 10px (Unicode)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#ifndef TARGET_C3
#include <ESP32Ping.h>
#endif
#include <Preferences.h>
#include "hal.h"

// --- Credentials (kept out of version control) ---
#include "secrets.h"  // copy secrets.h.example → secrets.h and fill in your keys
char        GEMINI_MODEL[48]  = "gemini-3.1-pro-preview"; // overwritten at boot
bool        geminiUseGlobal   = false;  // true → /locations/global/ in path
bool        useGrok           = false;  // true → route to Grok (xAI) instead of Gemini
bool        useGroq           = false;  // true → route to Groq instead of Gemini
// largeFont removed — always use DejaVuSansBold12px everywhere
bool        invertDisplay     = true;   // true = Light Theme (light bg, black text)
#define COL_INVERT_BG   0xC618          // light grey (~RGB 192,192,192)

// --- WiFi credential store (NVS, up to 9 slots, slot 0 = most-recently-used) ---
#define WIFI_PREFS_MAX  9
#define WIFI_PREFS_NS   "wifi"

static char wifiSsid[WIFI_PREFS_MAX][33];  // SSID max 32 chars + null
static char wifiPass[WIFI_PREFS_MAX][64];  // WPA2 password max 63 chars + null
static int  wifiCredsCount = 0;

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
#ifdef TARGET_C3
#undef  HIST_H_KB_HIDE
#define HIST_H_KB_HIDE  SCREEN_H   // C3: no separate input bar — full screen is chat + input line
#endif

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

// --- UI text metrics: must match uiFontOn() ---
#ifdef TARGET_C3
#define TXT_W   8   // DejaVuSansBold12px average char width (proportional)
#define TXT_H  14   // DejaVuSansBold12px line height (matches LINE_H_LARGE)
#else
#define TXT_W  6    // GLCD size-1: 6px char width
#define TXT_H  8    // GLCD size-1: 8px char height
#endif

// --- Objects ---
TFT_eSPI tft = TFT_eSPI();

// --- Keyboard layout ---
#ifdef TARGET_C3
constexpr bool kbVisible = false;
#else
bool kbVisible = true;
#endif
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
#define KEY_INSET         1     // inset drawn box by this many px each side (~10% smaller)

// --- Layout metrics ---
#define LINE_H_LARGE     14     // DejaVuSansBold12px line height (yAdvance=13) + 1px gap
#define LINE_H_SMALL     12     // DejaVuSansBold10px line height (yAdvance=13) - 1px
#define SPLASH_H         45     // height of boot splash area to clear after connect (3 × LINE_H_LARGE)

// --- Input bar ---
#define INPUT_BUF_SIZE  128     // input text buffer including null terminator
#define INPUT_MAX_LEN   127     // max typeable characters (INPUT_BUF_SIZE - 1)
#define BTN_SEND_W       46     // width of Send/More button
#define BTN_SEND_X_TEXT  39     // distance of Send/More text from right edge
#define BTN_SHOWKB_W     58     // width of Show KB button
#define BTN_SHOWKB_X    144     // distance of Show KB button left edge from right (BTN_NEW_X + BTN_SHOWKB_W + 2)
#define BTN_NEW_W        36     // width of New button (KB hidden only)
#define BTN_NEW_X        84     // distance of New button left edge from right (BTN_SEND_W + BTN_NEW_W + 2)
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

// --- API ---
#define HTTPS_PORT        443
#define GEMINI_HOST       "generativelanguage.googleapis.com"
#define GROK_HOST         "eu-west-1.api.x.ai"
#define GROK_MODEL        "grok-4-1-fast-reasoning"
#define GROQ_HOST         "api.groq.com"
#define GROQ_MODEL        "openai/gpt-oss-120b"

// fontOn() loads 12px bold smooth font. fontOff() unloads and restores GLCD.
void fontOn()  { tft.loadFont(DejaVuSansBold12pxData); }
void fontOff() { tft.unloadFont(); tft.setTextFont(1); }  // always restore GLCD explicitly

// UI text helpers: C3 uses smooth font everywhere. CYD28 uses GLCD size 1 (6×8px).
#ifdef TARGET_C3
inline void uiFontOn()  { fontOn(); }
inline void uiFontOff() { fontOff(); }
#else
inline void uiFontOn()  { tft.setTextSize(1); }
inline void uiFontOff() {}
#endif

void drawKey(int x, int y, int w, int h, const char* label, uint16_t face, uint16_t text) {
    tft.fillRect(x, y, w, h, COL_BG);  // clear cell; COL_BG shows as border around inset box
    tft.fillRoundRect(x + KEY_INSET, y + KEY_INSET, w - 1 - 2*KEY_INSET, h - 2*KEY_INSET, KEY_RADIUS, face);
    tft.setTextColor(text, face);
    fontOn();
    int tx = x + (w - (int)tft.textWidth(label)) / 2;
    int ty = y + (h - 15) / 2 + 1;   // 15 = yAdvance; +1 lowers text slightly
    tft.drawString(label, tx, ty);
    fontOff();           // restore GLCD for everything else
}

#ifndef TARGET_C3
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
#endif

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
    uiFontOn();
    // Header row
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.setCursor(2, (AP_ROW_H - TXT_H) / 2);
    tft.print("Select WiFi network:");
    // Entry rows
    for (int i = 0; i < apCount; i++) {
        int y = AP_ROW_H * (i + 1);
        tft.fillRect(0, y, SCREEN_W, AP_ROW_H - 1, COL_KEY_FACE);
        tft.setTextColor(COL_KEY_LABEL, COL_KEY_FACE);
        int ty = y + (AP_ROW_H - TXT_H) / 2;
        // Number
        char num[3]; snprintf(num, sizeof(num), "%d", i + 1);
        tft.setCursor(2, ty); tft.print(num);
        // SSID (truncated to fit before signal column)
        int sigX = SCREEN_W - 9 * TXT_W;
        int ssidMaxChars = (sigX - 2*TXT_W - 8) / TXT_W;
        if (ssidMaxChars > 22) ssidMaxChars = 22;
        char ssidDisp[23] = {0};
        strncpy(ssidDisp, apSsids[i], ssidMaxChars);
        tft.setCursor(2 + TXT_W, ty); tft.print(ssidDisp);
        // Signal bars + dBm (right side)
        char sig[12];
        snprintf(sig, sizeof(sig), "%s %4d", rssiToBars(apRssi[i]), apRssi[i]);
        tft.setCursor(sigX, ty); tft.print(sig);
    }
    uiFontOff();
}

// --- WiFi health ---
bool          wifiHealthy     = true;
unsigned long lastWiFiCheckMs = 0;

// --- Input buffer ---
char inputBuf[INPUT_BUF_SIZE] = {0};
int  inputLen      = 0;
bool moreMode      = false;  // true after AI reply → button shows "More"

void drawInputBar() {
#ifdef TARGET_C3
    // On C3 the input prompt is the last line of the chat area (no separate bar).
    // drawHistory() renders it as part of the history display; this function
    // redraws just that last slot when only the input text changes.
    {
        int lineH  = LINE_H_LARGE;
        int inputY = (HIST_H_KB_HIDE / lineH - 1) * lineH;
        uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
        TFT_eSprite spr(&tft);
        spr.setColorDepth(16);
        if (spr.createSprite(SCREEN_W, lineH)) {
            spr.loadFont(DejaVuSansBold12pxData);
            spr.fillSprite(bg);
            spr.setTextColor(wifiHealthy ? TFT_DARKGREY : TFT_RED, bg);
            spr.drawString("> ", 2, 0);
            int promptW = spr.textWidth("> ");
            spr.setTextColor(invertDisplay ? TFT_BLACK : TFT_WHITE, bg);
            int availW = SCREEN_W - promptW - 4;
            int start = 0;
            if (inputLen > 0) {
                start = inputLen;
                while (start > 0) {
                    char tmp[INPUT_BUF_SIZE];
                    strncpy(tmp, inputBuf + start - 1, inputLen - start + 1);
                    tmp[inputLen - start + 1] = '\0';
                    if (spr.textWidth(tmp) > availW) break;
                    start--;
                }
            }
            char dispBuf[INPUT_BUF_SIZE] = {0};
            strncpy(dispBuf, inputBuf + start, inputLen - start);
            spr.drawString(dispBuf, 2 + promptW, 0);
            spr.unloadFont();
            spr.pushSprite(0, inputY);
            spr.deleteSprite();
        } else {
            // Direct fallback
            tft.fillRect(0, inputY, SCREEN_W, lineH, bg);
            fontOn();
            tft.setTextColor(wifiHealthy ? TFT_DARKGREY : TFT_RED, bg);
            tft.drawString("> ", 2, inputY);
            int promptW = tft.textWidth("> ");
            tft.setTextColor(invertDisplay ? TFT_BLACK : TFT_WHITE, bg);
            int maxChars = (SCREEN_W - (int)promptW - 4) / TXT_W;
            char dispBuf[INPUT_BUF_SIZE] = {0};
            int start = (inputLen > maxChars) ? inputLen - maxChars : 0;
            strncpy(dispBuf, inputBuf + start, inputLen - start);
            tft.drawString(dispBuf, 2 + promptW, inputY);
            fontOff();
        }
        return;
    }
#endif
    // CYD28: traditional input bar at bottom of screen
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;

    tft.fillRect(0, barY, SCREEN_W, barH, COL_IBAR_BG);

    // Ensure smooth font is cleared — rebuildLines may leave it loaded on tft
    tft.unloadFont();
    tft.setTextFont(1);

    // Prompt marker — red when WiFi health check fails
    tft.setTextColor(wifiHealthy ? COL_IBAR_TEXT : TFT_RED, COL_IBAR_BG);
    uiFontOn();
    tft.setCursor(2, barY + (barH - TXT_H) / 2);
    tft.print("> ");
    tft.setTextColor(COL_IBAR_TEXT, COL_IBAR_BG);

    int maxChars = (SCREEN_W - BTN_SEND_W - 14) / TXT_W;
    char display[54] = {0};
    int start = (inputLen > maxChars) ? inputLen - maxChars : 0;
    strncpy(display, inputBuf + start, maxChars);
    tft.print(display);

    // Button
    tft.fillRect(SCREEN_W - BTN_SEND_W, barY + BTN_INSET, BTN_SEND_W - BTN_INSET*2, barH - BTN_INSET*2, COL_BTN_BG);
    tft.setTextColor(wifiHealthy ? COL_BTN_TEXT : TFT_RED, COL_BTN_BG);
    const char* btnLbl = moreMode ? "More" : "Send";
    int bw = tft.textWidth(btnLbl);
    tft.drawString(btnLbl, SCREEN_W - BTN_SEND_W + (BTN_SEND_W - bw) / 2, barY + (barH - TXT_H) / 2);
    uiFontOff();

    if (!kbVisible) {
        tft.fillRect(SCREEN_W - BTN_SHOWKB_X, barY + BTN_INSET, BTN_SHOWKB_W, barH - BTN_INSET*2, COL_BTN_BG);
        tft.setTextColor(COL_BTN_TEXT, COL_BTN_BG);
        tft.setCursor(SCREEN_W - BTN_SHOWKB_X + 3, barY + (barH - 8) / 2);
        tft.print("Show KB");

        tft.fillRect(SCREEN_W - BTN_NEW_X, barY + BTN_INSET, BTN_NEW_W, barH - BTN_INSET*2, COL_BTN_BG);
        tft.setTextColor(COL_BTN_TEXT, COL_BTN_BG);
        tft.setCursor(SCREEN_W - BTN_NEW_X + 4, barY + (barH - 8) / 2);
        tft.print("New");
    }
}

// --- Conversation history ---
struct Message {
    bool   isUser;
    bool   isError;
    bool   displayOnly;  // true = show in chat but never sent to AI
    char   text[2048];
};

#ifdef TARGET_C3
static const int  MAX_MESSAGES = 10;   // reduced from 20 to free heap for TLS+BLE coexistence
#else
static const int  MAX_MESSAGES = 20;
#endif
Message           history[MAX_MESSAGES];
int               historyCount = 0;

// Rendered line cache
#ifdef TARGET_C3
static const int  MAX_LINES  = 100;       // reduced from 250 to free heap for TLS+BLE coexistence
#else
static const int  MAX_LINES  = 250;
#endif
char              lines[MAX_LINES][128];  // 127 bytes + null per line (UTF-8 safe)
uint16_t          lineColor[MAX_LINES];
bool              lineIsUser[MAX_LINES];  // true = right-align (user message)
int               lineCount    = 0;
int               scrollOffset = 0;       // lines scrolled up from bottom

void rebuildLines() {
    lineCount = 0;
    for (int m = 0; m < historyCount && lineCount < MAX_LINES - 2; m++) {
        uint16_t col = history[m].isError ? COL_ERROR :
                       history[m].isUser  ? COL_USER  : COL_AI;
        bool isUser = history[m].isUser;
        const char* full = history[m].text;

        {
            // Pixel-width word wrap using smooth font (DejaVuSansBold12px)
            fontOn();
            char lineBuf[128] = "";
            const char* p = full;
            while (*p && lineCount < MAX_LINES - 1) {
                if (*p == '\n') {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount] = col; lineIsUser[lineCount++] = isUser;
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
                if (tft.textWidth(test) <= SCREEN_W - 4) {
                    strncpy(lineBuf, test, 127); lineBuf[127] = '\0';
                } else {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount] = col; lineIsUser[lineCount++] = isUser;
                        strncpy(lineBuf, word, 127); lineBuf[127] = '\0';
                    } else {
                        strncpy(lines[lineCount], word, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount] = col; lineIsUser[lineCount++] = isUser;
                    }
                }
            }
            if (lineBuf[0] && lineCount < MAX_LINES) {
                strncpy(lines[lineCount], lineBuf, 127);
                lines[lineCount][127] = '\0';
                lineColor[lineCount] = col; lineIsUser[lineCount++] = isUser;
            }
            fontOff();
        }
    }
}

void drawHistory() {
    int histH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
    uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
    int lineH  = LINE_H_LARGE;
    int maxVis = histH / lineH;

    // On C3 the last history slot is reserved for the inline input prompt.
    int histSlots = maxVis;
#ifdef TARGET_C3
    histSlots = maxVis - 1;
#endif
    int firstIdx = lineCount - histSlots - scrollOffset;
    if (firstIdx < 0) firstIdx = 0;

#ifdef TARGET_C3
    // Render each line to a RAM sprite, then push to display in one SPI block.
    // This avoids repeated setWindow/pushBlock transitions that cause partial
    // screen updates on ESP32-C3 (SPI register sync issue without DMA).
    // Falls back to direct TFT drawing if heap is too fragmented.
    TFT_eSprite spr(&tft);
    spr.setColorDepth(16);
    if (spr.createSprite(SCREEN_W, lineH)) {
        spr.loadFont(DejaVuSansBold12pxData);
        spr.setTextWrap(false);

        for (int i = 0; i < histSlots; i++) {
            spr.fillSprite(bg);
            if ((firstIdx + i) < lineCount) {
                int idx = firstIdx + i;
                uint16_t col = lineColor[idx];
                if (invertDisplay) col = TFT_BLACK;
                spr.setTextColor(col, bg);
                if (lineIsUser[idx]) {
                    spr.setTextDatum(TR_DATUM);
                    spr.drawString(lines[idx], SCREEN_W, 0);
                    spr.setTextDatum(TL_DATUM);
                } else {
                    spr.drawString(lines[idx], 2, 0);
                }
            }
            spr.pushSprite(0, i * lineH);
        }

        // Input line — always the last visible slot
        {
            spr.fillSprite(bg);
            spr.setTextColor(wifiHealthy ? TFT_DARKGREY : TFT_RED, bg);
            spr.drawString("> ", 2, 0);
            int promptW = spr.textWidth("> ");
            spr.setTextColor(invertDisplay ? TFT_BLACK : TFT_WHITE, bg);
            int availW = SCREEN_W - promptW - 4;
            int start = 0;
            if (inputLen > 0) {
                start = inputLen;
                while (start > 0) {
                    char tmp[INPUT_BUF_SIZE];
                    strncpy(tmp, inputBuf + start - 1, inputLen - start + 1);
                    tmp[inputLen - start + 1] = '\0';
                    if (spr.textWidth(tmp) > availW) break;
                    start--;
                }
            }
            char dispBuf[INPUT_BUF_SIZE] = {0};
            strncpy(dispBuf, inputBuf + start, inputLen - start);
            spr.drawString(dispBuf, 2 + promptW, 0);
            spr.pushSprite(0, histSlots * lineH);
        }

        spr.unloadFont();
        spr.deleteSprite();

        int usedH = maxVis * lineH;
        if (usedH < histH)
            tft.fillRect(0, usedH, SCREEN_W, histH - usedH, bg);
        return;
    }
    // Sprite alloc failed — fall through to direct TFT drawing
#endif

    tft.fillRect(0, 0, SCREEN_W, histH, bg);

    fontOn();
    tft.setTextWrap(false);
    for (int i = 0; i < maxVis && (firstIdx + i) < lineCount; i++) {
        int idx = firstIdx + i;
        uint16_t col = lineColor[idx];
        if (invertDisplay) col = TFT_BLACK;
        tft.setTextColor(col, bg);
        if (lineIsUser[idx]) {
            tft.setTextDatum(TR_DATUM);
            tft.drawString(lines[idx], SCREEN_W, i * lineH);
            tft.setTextDatum(TL_DATUM);
        } else {
            tft.drawString(lines[idx], 2, i * lineH);
        }
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
    history[historyCount].isUser       = isUser;
    history[historyCount].isError      = isError;
    history[historyCount].displayOnly  = false;
    strncpy(history[historyCount].text, text, 2047);
    history[historyCount].text[2047] = '\0';
    // Sanitise: pass supported UTF-8 codepoints through unchanged.
    // Strip C0 controls; normalise space-like and hyphen-like chars not in font;
    // drop zero-width/invisible chars; replace remaining unsupported sequences with '?'.
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
                    if (cp == 0x00AD) {
                        /* U+00AD soft hyphen: drop silently */
                    } else if (supportedCodepoint(cp)) { *d++ = (char)c; *d++ = (char)b2; }
                    else                         *d++ = '?';
                    s += 2;
                } else { *d++ = '?'; s++; }
            } else if ((c & 0xF0) == 0xE0) {
                // 3-byte sequence
                unsigned char b2 = (unsigned char)s[1];
                unsigned char b3 = (unsigned char)s[2];
                if ((b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                    uint32_t cp = ((c & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                    // Typographic spaces not in font → ASCII space
                    if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F) {
                        *d++ = ' ';
                    // Hyphen-like chars not in font → ASCII hyphen-minus
                    } else if (cp == 0x2010 || cp == 0x2012 || cp == 0x2015 || cp == 0x2212) {
                        *d++ = '-';
                    // Zero-width / invisible: drop silently
                    } else if (cp == 0x200B || cp == 0x200C || cp == 0x200D ||
                               cp == 0x2060 || cp == 0xFEFF) {
                        /* drop */
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

// Show keyboard and let user type a password. Returns typed string in out (64 bytes).
// Uses the existing keyboard (inputBuf/inputLen/shiftOn/altOn globals).
// Tap the Send button area (input bar) to submit.
void enterPassword(const char* ssidPrompt, char* out) {
    inputBuf[0] = '\0';
    inputLen    = 0;
    moreMode    = false;
    shiftOn     = false;
    altOn       = false;
#ifndef TARGET_C3
    kbVisible   = true;
#endif

    tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    fontOff();
    uiFontOn();
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.drawString("Password for:", 2, 0);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawString(ssidPrompt, 2, TXT_H);
    uiFontOff();
    drawInputBar();
#ifndef TARGET_C3
    drawKeyboard();
#endif

    while (true) {
        InputEvent ev;
        if (!halPollInput(&ev)) { delay(10); continue; }

        switch (ev.type) {
            case INPUT_ENTER:
                strncpy(out, inputBuf, 63); out[63] = '\0';
                inputBuf[0] = '\0'; inputLen = 0;   // clear sensitive data
                return;
            case INPUT_CHAR:
                if (inputLen < 63) {
                    inputBuf[inputLen++] = ev.ch;
                    inputBuf[inputLen]   = '\0';
                    drawInputBar();
                }
                break;
            case INPUT_BACKSPACE:
                if (inputLen > 0) { inputBuf[--inputLen] = '\0'; drawInputBar(); }
                break;
            default:
                break;
        }
    }
}

void sendPrompt();  // forward declaration — defined after callGemini()

// Set LED colour based on WiFi RSSI (called after every health check)
// Blue ≥-55  Cyan ≥-65  Green ≥-75  Orange=weak-but-connected  Red=lost
void updateLedWifi() {
    if (WiFi.status() != WL_CONNECTED) {
        halSetLed(255, 0, 0);   // Red: WiFi lost
        return;
    }
    int rssi = WiFi.RSSI();
    if      (rssi >= RSSI_THRESH_BLUE)  halSetLed(  0,   0, 255);  // Blue:   strongest
    else if (rssi >= RSSI_THRESH_CYAN)  halSetLed(  0, 255, 255);  // Cyan:   good
    else if (rssi >= RSSI_THRESH_GREEN) halSetLed(  0, 255,   0);  // Green:  fair
    else                  halSetLed(255, 128,   0);  // Orange: weak
}

// --- WiFi ---
// Returns true if connected. Pass showSplash=true for boot (draws title + "Connecting:" text).
bool connectWiFi(const char* ssid, const char* pass, bool showSplash = false) {
    if (showSplash) {
        fontOn();
        tft.setTextColor(TFT_NAVY, TFT_WHITE);
        char wifiMsg[80];
        snprintf(wifiMsg, sizeof(wifiMsg), "Connecting: %.55s...", ssid);
        tft.drawString(wifiMsg, 2, 0);
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
#ifdef TARGET_C3
    WiFi.setSleep(true);  // modem sleep: yields radio to BLE between beacons
#endif
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
        fontOff(); uiFontOn(); tft.setTextColor(TFT_YELLOW, COL_BG);
        tft.drawString("Scanning WiFi...", 2, 0);

        int n = WiFi.scanNetworks();

        if (n <= 0) {
            tft.fillScreen(COL_BG);
            tft.drawString("No networks found. Tap to retry.", 2, 0);
            { InputEvent ev; while (!halPollInput(&ev)) delay(10); }
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
            InputEvent ev;
            if (!halPollInput(&ev)) { delay(10); continue; }
            if (ev.type == INPUT_CHAR && ev.ch >= '1' && ev.ch <= '9') {
                int idx = ev.ch - '1';
                if (idx < apCount) selected = idx;
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
            uiFontOn(); tft.setTextColor(TFT_BLUE, COL_BG);
            char msg[80]; snprintf(msg, sizeof(msg), "Connecting: %.55s...", selSsid);
            tft.drawString(msg, 2, 0);
            uiFontOff();

            bool ok = connectWiFi(selSsid, pass);

            if (ok) {
                insertWifiCred(selSsid, pass);
                return;  // connected — setup() continues to selectModel()
            }

            // --- Failed: offer re-enter or new scan ---
            tft.fillScreen(COL_BG);
            uiFontOn();
            tft.setTextColor(TFT_RED, COL_BG);
            char failMsg[64]; snprintf(failMsg, sizeof(failMsg), "Failed: %.40s", selSsid);
            tft.drawString(failMsg, 2, (AP_ROW_H - TXT_H) / 2);

            // Draw two option rows (same style as AP list)
            static const char* opts[] = { "1  Re-enter password", "2  New scan" };
            for (int i = 0; i < 2; i++) {
                int y = AP_ROW_H * (i + 1);
                tft.fillRect(0, y, SCREEN_W, AP_ROW_H - 1, COL_KEY_FACE);
                tft.setTextColor(COL_KEY_LABEL, COL_KEY_FACE);
                tft.drawString(opts[i], 2, y + (AP_ROW_H - TXT_H) / 2);
            }
            uiFontOff();

            // Wait for key 1 or 2
            int choice = 0;
            while (choice == 0) {
                InputEvent ev;
                if (!halPollInput(&ev)) { delay(10); continue; }
                if (ev.type == INPUT_CHAR && ev.ch == '1') choice = 1;
                if (ev.type == INPUT_CHAR && ev.ch == '2') choice = 2;
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
    "I'll try a Babbage Engine instead...",
    "Have you got brain fog?...",
    "I haven't got all day!...",
    "Get a move on!...",
    "Let's see some action here...",
    "Oh, please...",
    "This is nuts!...",
    "AI=Annoyingly Indolent...",
    "Awkward silence...",
    "Did all your bytes fall out?...",
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
    "I'm on Gigashit internet...",
	"Take the shell off a snail and it's SLUGGISH",
    "Speed up, you lazy git...",
    "It'd be quicker to walk to the data...",
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
#ifdef TARGET_C3
    {
        int lineH  = LINE_H_LARGE;
        int inputY = (HIST_H_KB_HIDE / lineH - 1) * lineH;
        uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
        tft.fillRect(0, inputY, SCREEN_W, lineH, bg);
        fontOn();
        tft.setTextColor(TFT_DARKGREY, bg);
        tft.drawString(msg, 2, inputY);
        fontOff();
        return;
    }
#endif
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;
    tft.fillRect(0, barY, SCREEN_W - BTN_SEND_W, barH, COL_IBAR_BG);
    tft.setTextColor(TFT_DARKGREY, COL_IBAR_BG);
    fontOn();
    tft.drawString(msg, 2, barY + (barH - TXT_H) / 2);
    fontOff();
}

// Extract the body from a raw HTTP response, decoding chunked transfer encoding if present.
// Works directly on resp with no intermediate copies to keep heap usage low.
static String extractBody(const String& resp) {
    int hdrEnd = resp.indexOf("\r\n\r\n");
    if (hdrEnd < 0) return resp;
    // Scan only the header section for chunked marker
    bool chunked = false;
    for (int i = 0; i < hdrEnd - 25; i++) {
        if (resp[i]=='T' || resp[i]=='t') {
            if (resp.substring(i, i + 26).equalsIgnoreCase("Transfer-Encoding: chunked")) {
                chunked = true; break;
            }
        }
    }
    if (!chunked) return resp.substring(hdrEnd + 4);
    // Decode chunked body directly from resp — no raw copy, no per-chunk substring
    String out;
    out.reserve(resp.length() - hdrEnd);
    int pos = hdrEnd + 4;
    while (pos < (int)resp.length()) {
        int nl = resp.indexOf('\n', pos);
        if (nl < 0) break;
        String szHex = resp.substring(pos, nl);
        szHex.trim();
        if (szHex.length() == 0) { pos = nl + 1; continue; }
        unsigned long sz = strtoul(szHex.c_str(), nullptr, 16);
        if (sz == 0) break;                              // terminal chunk
        pos = nl + 1;
        if (pos + (int)sz > (int)resp.length()) sz = resp.length() - pos;
        out.concat(resp.c_str() + pos, (unsigned int)sz); // no temp String
        pos += sz + 2;                                   // skip trailing \r\n
    }
    Serial.printf("[extractBody] raw=%d decoded=%d\n", resp.length(), out.length());
    return out;
}

// --- Gemini API call ---
String callGemini(const char* prompt) {
    // Build JSON body
    JsonDocument reqDoc;
    JsonObject sysInstr = reqDoc["system_instruction"].to<JsonObject>();
    JsonArray  sysParts = sysInstr["parts"].to<JsonArray>();
    JsonObject sysPart  = sysParts.add<JsonObject>();
    sysPart["text"]     = "Respond in 80 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols. Use paragraphs to separate distinct ideas. Never include URLs or hyperlinks. When quoting current or time-sensitive information, use your search tool to check live sources first.";

    JsonArray contents = reqDoc["contents"].to<JsonArray>();

    // Add conversation history (exclude the final entry — that IS the current prompt,
    // already appended to history[] by addMessage() in sendPrompt before this call)
    for (int i = 0; i < historyCount - 1; i++) {
        if (history[i].displayOnly) continue;
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

    showWaiting("Thinking...");  // re-assert just before blocking TLS handshake
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
    // Read response using read(buf,n) — avoids client.available() which can prematurely
    // process the server's TLS close_notify and discard buffered bytes.
    String fullResp = "";
    // No reserve — let String grow dynamically to minimize peak heap usage
    uint8_t rbuf[256];
    unsigned long deadline   = millis() + API_TIMEOUT_MS;
    unsigned long nextMsgMs  = millis() + API_WAIT_FIRST_MS;
    unsigned long lastData   = millis();
    bool receivedAny = false;
    while (millis() < deadline) {
        int n = client.read(rbuf, sizeof(rbuf));
        if (n > 0) {
            lastData = millis();
            receivedAny = true;
            for (int i = 0; i < n; i++) fullResp += (char)rbuf[i];
            continue;  // read more without delay
        }
        // n <= 0: "no data right now" — ESP32 NetworkClientSecure returns -1 from read()
        // whenever available() is 0, even mid-stream between TLS records. Never break on it.
        // When available() processes TLS close_notify it calls stop() internally;
        // connected() then returns false so we break immediately.
        if (receivedAny && !client.connected()) break;  // server closed connection
        if (receivedAny && millis() - lastData > 1000) break;  // 1s idle = response complete
        if (!receivedAny && millis() - lastData > 15000) break;  // 15s no first byte = give up
        if (WiFi.status() != WL_CONNECTED) break;
        if (millis() >= nextMsgMs) {
            showWaiting(WAIT_MSGS[waitMsgIdx]);
            waitMsgIdx = (waitMsgIdx + 1) % NUM_WAIT_MSGS;
            nextMsgMs += 4000;
        }
#ifndef TARGET_C3
        pollKBHide();
#endif
        delay(5);
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

    // Decode chunked encoding if present, then locate JSON body
    String jsonBody = extractBody(fullResp);
    int jsonStart = jsonBody.indexOf('{');
    if (jsonStart < 0) {
        char buf[40]; snprintf(buf, sizeof(buf), "ERR: HTTP %d, no JSON", httpStatus);
        return String(buf);
    }
    // Parse JSON — use filter to skip grounding metadata (can be 8KB+), only extract what we need
    JsonDocument filter;
    filter["candidates"][0]["content"]["parts"][0]["text"] = true;
    filter["error"]["message"] = true;
    JsonDocument respDoc;
    DeserializationError derr = deserializeJson(respDoc, jsonBody.c_str() + jsonStart,
                                                DeserializationOption::Filter(filter));
    if (derr == DeserializationError::Ok || derr == DeserializationError::IncompleteInput) {
        // IncompleteInput is normal: Gemini appends large metadata after the text field;
        // SSL close_notify arrives before all of it. Text is captured before truncation.
        const char* errMsg = respDoc["error"]["message"];
        if (errMsg) {
            char buf[80]; snprintf(buf, sizeof(buf), "ERR: %.72s", errMsg);
            return String(buf);
        }

        const char* text = respDoc["candidates"][0]["content"]["parts"][0]["text"];
        if (text) return String(text);
    }

    // JSON parse failed or text field not captured (truncated mid-string) —
    // manually extract "text": "..." from the raw response
    Serial.printf("[Gemini] JSON err=%s, trying manual extract\n", derr.c_str());
    const char* raw = jsonBody.c_str() + jsonStart;
    const char* marker = strstr(raw, "\"text\": \"");
    if (!marker) marker = strstr(raw, "\"text\":\"");
    if (marker) {
        const char* start = strchr(marker + 6, '"') + 1;  // skip past opening quote
        // Scan for unescaped closing quote or end of data
        String result;
        const char* p = start;
        while (*p) {
            if (*p == '\\' && *(p+1)) { result += *p++; result += *p++; continue; }
            if (*p == '"') break;
            result += *p++;
        }
        // Unescape JSON string escapes
        result.replace("\\n", "\n");
        result.replace("\\\"", "\"");
        result.replace("\\\\", "\\");
        if (result.length() > 0) {
            Serial.printf("[Gemini] manual extract OK (%d chars)\n", result.length());
            return result;
        }
    }

    Serial.println("Bad JSON (HTTP " + String(httpStatus) + "): " + jsonBody.substring(jsonStart, jsonStart + 300) + " err=" + derr.c_str());
    char buf[40]; snprintf(buf, sizeof(buf), "ERR: HTTP %d, bad JSON", httpStatus);
    return String(buf);
}

// --- Grok API call (xAI /v1/responses + web_search tool) ---
String callGrok(const char* prompt) {
    // /v1/responses uses "input" array and top-level "instructions" for system prompt
    JsonDocument reqDoc;
    reqDoc["model"]        = GROK_MODEL;
    reqDoc["stream"]       = false;
    reqDoc["instructions"] = "Respond in 80 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols, no numbered or unnumbered lists. Use paragraphs to separate distinct ideas. Never include URLs, hyperlinks, citations, footnotes, source references, or attribution of any kind. Do not mention where information came from. When quoting current or time-sensitive information, use your web search tool to check live sources first.";

    JsonArray input = reqDoc["input"].to<JsonArray>();

    // Conversation history (exclude last entry — that is the current prompt)
    for (int i = 0; i < historyCount - 1; i++) {
        if (history[i].displayOnly) continue;
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

    showWaiting("Thinking...");  // re-assert just before blocking TLS handshake
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
    // No reserve — let String grow dynamically to minimize peak heap usage
    uint8_t rbuf[256];
    unsigned long deadline  = millis() + API_TIMEOUT_MS;
    unsigned long nextMsgMs = millis() + API_WAIT_FIRST_MS;
    unsigned long lastData  = millis();
    bool receivedAny = false;
    while (millis() < deadline) {
        int n = client.read(rbuf, sizeof(rbuf));
        if (n > 0) {
            lastData = millis();
            receivedAny = true;
            for (int i = 0; i < n; i++) fullResp += (char)rbuf[i];
            continue;
        }
        if (receivedAny && !client.connected()) break;
        if (receivedAny && millis() - lastData > 1000) break;
        if (!receivedAny && millis() - lastData > 15000) break;
        if (WiFi.status() != WL_CONNECTED) break;
        if (millis() >= nextMsgMs) {
            showWaiting(WAIT_MSGS[waitMsgIdx]);
            waitMsgIdx = (waitMsgIdx + 1) % NUM_WAIT_MSGS;
            nextMsgMs += 6000;
        }
#ifndef TARGET_C3
        pollKBHide();
#endif
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
    sysMsgObj["content"] = "Respond in 80 words or fewer. Plain text only: no markdown, no ** or * emphasis, no tables, no bullet symbols, no numbered or unnumbered lists. Use paragraphs to separate distinct ideas. Never include URLs, hyperlinks, citations, footnotes, source references, or attribution of any kind.";

    // Conversation history (exclude last entry — current prompt)
    for (int i = 0; i < historyCount - 1; i++) {
        if (history[i].displayOnly) continue;
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

    showWaiting("Thinking...");  // re-assert just before blocking TLS handshake
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
    // No reserve — let String grow dynamically to minimize peak heap usage
    uint8_t rbuf[256];
    unsigned long deadline  = millis() + API_TIMEOUT_MS;
    unsigned long nextMsgMs = millis() + API_WAIT_FIRST_MS;
    unsigned long lastData  = millis();
    bool receivedAny = false;
    while (millis() < deadline) {
        int n = client.read(rbuf, sizeof(rbuf));
        if (n > 0) {
            lastData = millis();
            receivedAny = true;
            for (int i = 0; i < n; i++) fullResp += (char)rbuf[i];
            continue;
        }
        if (receivedAny && !client.connected()) break;
        if (receivedAny && millis() - lastData > 1000) break;
        if (!receivedAny && millis() - lastData > 15000) break;
        if (WiFi.status() != WL_CONNECTED) break;
        if (millis() >= nextMsgMs) {
            showWaiting(WAIT_MSGS[waitMsgIdx]);
            waitMsgIdx = (waitMsgIdx + 1) % NUM_WAIT_MSGS;
            nextMsgMs += 6000;
        }
#ifndef TARGET_C3
        pollKBHide();
#endif
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
#ifdef TARGET_C3
    {
        int lineH  = LINE_H_LARGE;
        int inputY = (HIST_H_KB_HIDE / lineH - 1) * lineH;
        uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
        tft.fillRect(0, inputY, SCREEN_W, lineH, bg);
        fontOn();
        tft.setTextColor(TFT_DARKGREY, bg);
        tft.drawString("Thinking...", 2, inputY);
        fontOff();
        return;
    }
#endif
    int barY = kbVisible ? IBAR_Y_KB_SHOW : IBAR_Y_KB_HIDE;
    int barH = kbVisible ? IBAR_H_KB_SHOW : IBAR_H_KB_HIDE;
    tft.fillRect(0, barY, SCREEN_W - BTN_SEND_W, barH, COL_IBAR_BG);
    tft.setTextColor(TFT_DARKGREY, COL_IBAR_BG);
    fontOn();
    tft.drawString("Thinking...", 2, barY + (barH - TXT_H) / 2);
    fontOff();
}

void sendPrompt() {
    char prompt[128];
    if (inputLen == 0) {
        if (historyCount == 0) return;  // no conversation yet — ignore
        strncpy(prompt, "Tell me more", 127);
    } else {
        strncpy(prompt, inputBuf, 127);
        inputBuf[0] = '\0';
        inputLen    = 0;
    }
    prompt[127] = '\0';
    moreMode = false;
#ifndef TARGET_C3
    kbVisible = false;   // hide KB; drawHistory() covers the KB area
#endif
    drawInputBar();      // clear full bar now — erases BS/Hide key remnants before API wait

    // Show user message and thinking indicator
    addMessage(true, false, prompt);
    showThinking();

    // Call API — deinit NimBLE on C3 to free ~30KB heap for TLS buffers.
    halBeforeApiCall();
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
        for (int i = 0; i < 50 && WiFi.status() != WL_CONNECTED; i++) delay(100);
    }
    String response = useGroq ? callGroq(prompt) : (useGrok ? callGrok(prompt) : callGemini(prompt));
    // Retry once on empty response (transient WiFi disruption after NimBLE deinit on C3)
    if (response == "ERR: empty response") {
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        for (int i = 0; i < 50 && WiFi.status() != WL_CONNECTED; i++) delay(100);
        response = useGroq ? callGroq(prompt) : (useGrok ? callGrok(prompt) : callGemini(prompt));
    }
    halAfterApiCall();

    if (response.startsWith("ERR:")) {
        addMessage(false, true, response.c_str());
    } else {
        addMessage(false, false, response.c_str());
    }

    moreMode = true;   // after every reply, offer "More"
    wifiHealthy = (WiFi.status() == WL_CONNECTED);  // refresh after API call; LED uses RSSI independently
    drawInputBar();
}

#ifndef TARGET_C3
void drawCrosshair(int x, int y) {
    tft.drawLine(x - 18, y, x + 18, y, TFT_WHITE);
    tft.drawLine(x, y - 18, x, y + 18, TFT_WHITE);
    tft.drawCircle(x, y, 6, TFT_WHITE);
}
#endif

void showModelChoices() {
    uiFontOn();
    tft.setTextColor(TFT_WHITE, COL_BG);
    int optY = 2 * TXT_H + 4;  // start below the two header lines
    tft.drawString("1 Gemini 2.5 Flash", 2, optY); optY += TXT_H;
    tft.drawString("2 Gemini 3 Flash",   2, optY); optY += TXT_H;
    tft.drawString("3 Gemini 3.1 Pro",   2, optY); optY += TXT_H;
    tft.drawString("4 Grok 4.1 Fast",    2, optY); optY += TXT_H;
    tft.drawString("5 Groq OSS-120b",    2, optY); optY += TXT_H;
    if (invertDisplay) {
        tft.drawString("d Dark Theme", 2, optY); optY += TXT_H;
    }
    uiFontOff();
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
        InputEvent ev;
        if (!halPollInput(&ev)) { delay(10); continue; }
        if (ev.type != INPUT_CHAR) continue;
        char ch = ev.ch;

        // Models 1–3 (Gemini)
        if (ch >= '1' && ch <= '3') {
            int i = ch - '1';
            halClickSound();
            strncpy(GEMINI_MODEL, modelIds[i], 47); GEMINI_MODEL[47] = '\0';
            geminiUseGlobal = modelGlobal[i];
            useGrok = false; useGroq = false;
            uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
            int clearH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
            tft.fillRect(0, 0, SCREEN_W, clearH, bg);
            uiFontOn();
            tft.setTextColor(TFT_GREEN, bg);
            tft.drawString("Cheap AI Chat Keyboard", 2, 0);
            tft.drawString(GEMINI_MODEL, 2, LINE_H_LARGE);
            tft.setTextColor(TFT_DARKGREY, bg);
            tft.drawString("Ready.", 2, 2 * LINE_H_LARGE);
            uiFontOff();
            return;
        }
        if (ch == '4') {
            halClickSound();
            useGrok = true; useGroq = false;
            uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
            int clearH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
            tft.fillRect(0, 0, SCREEN_W, clearH, bg);
            uiFontOn();
            tft.setTextColor(TFT_GREEN, bg);
            tft.drawString("Cheap AI Chat Keyboard", 2, 0);
            tft.drawString("Grok 4.1 Fast", 2, LINE_H_LARGE);
            tft.setTextColor(TFT_DARKGREY, bg);
            tft.drawString("Ready.", 2, 2 * LINE_H_LARGE);
            uiFontOff();
            return;
        }
        if (ch == '5') {
            halClickSound();
            useGroq = true; useGrok = false;
            uint16_t bg = invertDisplay ? COL_INVERT_BG : COL_BG;
            int clearH = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
            tft.fillRect(0, 0, SCREEN_W, clearH, bg);
            uiFontOn();
            tft.setTextColor(TFT_GREEN, bg);
            tft.drawString("Cheap AI Chat Keyboard", 2, 0);
            tft.drawString("Groq GPT-OSS-120b", 2, LINE_H_LARGE);
            tft.setTextColor(TFT_DARKGREY, bg);
            tft.drawString("Ready.", 2, 2 * LINE_H_LARGE);
            uiFontOff();
            return;
        }
        if (ch == 'd' || ch == 'D') {
            invertDisplay = !invertDisplay;
            tft.fillScreen(COL_BG);
            drawInputBar();
            uiFontOn(); tft.setTextColor(TFT_WHITE, COL_BG);
            tft.drawString("Cheap AI Chat Keyboard", 2, 0);
            tft.drawString("Select AI model:",       2, TXT_H + 4);
            uiFontOff();
            showModelChoices();
            continue;
        }
#ifndef TARGET_C3
        if (ch == 'c' || ch == 'C') {
            calibrateTouch();
            tft.fillScreen(COL_BG);
            uiFontOn(); tft.setTextColor(TFT_DARKGREY, COL_BG);
            tft.drawString("Cheap AI Chat Keyboard", 2, 0);
            tft.drawString("Select AI model:",       2, TXT_H);
            uiFontOff();
            drawKeyboard();
            drawInputBar(); showModelChoices();
            continue;
        }
#endif
    }
}

void setup() {
    loadWifiCreds();   // load NVS; shows AP picker on first boot if no credentials stored

#ifndef TARGET_C3
    // Hold BOOT button (GPIO0) on power-on to wipe touch calibration back to defaults
    pinMode(0, INPUT_PULLUP);
    if (digitalRead(0) == LOW) {
        Preferences p;
        p.begin("touch", false);
        p.remove("valid");
        p.remove("orient");
        p.end();
        Serial.begin(115200);
        Serial.println("[Cal] Reset to defaults via BOOT button");
        delay(500);
    }
#endif
    Serial.begin(115200);
#ifndef TARGET_C3
    Serial.println("[Cal] Loaded touch calibration from NVS");
#endif
    waitMsgIdx = random(NUM_WAIT_MSGS);
    // Display (init before backlight to avoid white flash)
    tft.init();
#ifdef TARGET_C3
    // ST7789 GRAM is 240col x 320row (portrait-native). Pre-fill all GRAM in
    // rotation 0 (CASET 0..239, RASET 0..319 both valid) so that GRAM rows
    // 240..319 contain COL_BG rather than power-on white. Without this, those
    // rows map to a visible white strip in some landscape rotations.
    tft.setRotation(0);
    tft.fillScreen(COL_BG);  // writes all 240x320 GRAM cells
    tft.setRotation(1);      // MX|MV landscape: correct physical orientation
    Serial.printf("[TFT] width=%d height=%d (expect %dx%d)\n", tft.width(), tft.height(), SCREEN_W, SCREEN_H);
#else
    #ifdef ROTATE_180
        tft.setRotation(3);
    #else
        tft.setRotation(1);
    #endif
#endif
    tft.fillScreen(COL_BG);

    // Hardware init (backlight, touch, LED, speaker) — delegated to HAL
    halInit();
    halLoadTouchCal(); // load saved touch calibration from NVS (or keep defaults)

    // Boot splash — plain background while connecting
    unsigned long splashStart = millis();
    tft.fillScreen(TFT_WHITE);
    bool wifiOk = connectWiFi(wifiSsid[0], wifiPass[0], true);
    // Splash visible for at least 3 seconds
    long splashRemain = 3000L - (long)(millis() - splashStart);
    if (splashRemain > 0) delay(splashRemain);
    tft.fillScreen(COL_BG);  // clear splash; back to normal dark background

    if (!wifiOk) {
        selectAP();  // scan → pick AP → enter password → connect; returns only on success
    }

    tft.fillScreen(COL_BG);  // clear AP/splash UI before drawing model menu

#ifndef TARGET_C3
    drawKeyboard();
#endif
    drawInputBar();

    uiFontOn();
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawString("Cheap AI Chat Keyboard", 2, 0);
    tft.drawString("Select AI model:",       2, TXT_H + 4);
    uiFontOff();

    selectModel();  // draws choices at y=30+ and waits for 1/2/3
}

void checkWiFiHealth() {
    updateLedWifi();  // instant — runs every loop, LED reacts immediately to dropout

    if (millis() - lastWiFiCheckMs < 3000) return;
    lastWiFiCheckMs = millis();

#ifndef TARGET_C3
    bool ok = (WiFi.status() == WL_CONNECTED) && Ping.ping(IPAddress(8,8,8,8), 1);
#else
    bool ok = (WiFi.status() == WL_CONNECTED);
#endif

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
    InputEvent ev;
    if (halPollInput(&ev)) {
        switch (ev.type) {
            case INPUT_SCROLL_UP: {
                int lineH     = LINE_H_LARGE;
                int histH     = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
                int maxVis    = histH / lineH;
#ifdef TARGET_C3
                int visSlots  = maxVis - 1;  // last slot is input line
#else
                int visSlots  = maxVis;
#endif
                scrollOffset  = max(0, scrollOffset - visSlots / 2);
                drawHistory();
                break;
            }
            case INPUT_SCROLL_DOWN: {
                int lineH     = LINE_H_LARGE;
                int histH     = kbVisible ? HIST_H_KB_SHOW : HIST_H_KB_HIDE;
                int maxVis    = histH / lineH;
#ifdef TARGET_C3
                int visSlots  = maxVis - 1;  // last slot is input line
#else
                int visSlots  = maxVis;
#endif
                int maxScroll = max(0, lineCount - visSlots);
                scrollOffset  = min(scrollOffset + visSlots / 2, maxScroll);
                drawHistory();
                break;
            }
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
#ifndef TARGET_C3
                kbVisible = true;
#endif
                tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
                drawHistory(); drawInputBar();
#ifndef TARGET_C3
                drawKeyboard();
#endif
                break;
            case INPUT_ENTER:
                // Word commands
                if (strcmp(inputBuf, "new") == 0) {
                    inputBuf[0] = '\0'; inputLen = 0;
                    goto do_new_conv;
                }
                if (strcmp(inputBuf, "more") == 0) {
                    inputBuf[0] = '\0'; inputLen = 0;
                    moreMode = true;
                    sendPrompt();
                    break;
                }
                if (inputLen == 0 && historyCount == 0) break;
                sendPrompt();
                break;
            case INPUT_MORE:
                if (moreMode) sendPrompt();
                break;
            case INPUT_BACKSPACE:
                if (inputLen > 0) {
                    inputBuf[--inputLen] = '\0';
                    if (inputLen == 0 && historyCount > 0) moreMode = true;
                    drawInputBar();
                }
                break;
            case INPUT_CHAR:
                if (ev.ch != 0 && inputLen < INPUT_MAX_LEN) {
                    moreMode = false;
                    inputBuf[inputLen++] = ev.ch;
                    inputBuf[inputLen]   = '\0';
                    drawInputBar();
                }
                break;
            default:
                break;
        }
    }
    checkWiFiHealth();
}
