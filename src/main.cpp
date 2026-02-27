// AI Terminal for CYD28 (ESP32-2432S028R)
// 270226 Initial scaffold
// 270226 Display init, constants, backlight

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
}

void loop() {}
