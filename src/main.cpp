// AI Terminal for CYD28 (ESP32-2432S028R)
// 270226 Initial scaffold
// 270226 Display init, constants, backlight
// 270226 Keyboard rendering
// 270226 Input bar rendering

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

void loop() {}
