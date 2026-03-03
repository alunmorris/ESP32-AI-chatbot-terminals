# WiFi AP Menu Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** When boot WiFi connection fails, show a scan-based AP picker with persistent credential storage so subsequent boots reconnect automatically.

**Architecture:** Three layers added to `src/main.cpp`: (1) Preferences NVS credential store (slot 0 = most-recently-used, up to 9 SSIDs+passwords, seed from hardcoded creds on first boot); (2) `connectWiFi()` refactored to accept ssid/pass args and return bool; (3) `selectAP()` scan→list→password-entry→connect loop, called from `setup()` only when boot connect fails.

**Tech Stack:** ESP32, TFT_eSPI 2.5.43, XPT2046_Touchscreen, Arduino Preferences (NVS), PlatformIO. No test framework — verify by compile + upload + visual check.

---

### Task 1: Credential storage + connectWiFi() refactor

**Files:**
- Modify: `src/main.cpp`

The goal of this task is to add the NVS storage layer and change `connectWiFi()` to accept credentials as arguments. No UI change yet — just plumbing.

**Step 1: Add Preferences include**

Find the includes block (around line 29). Add after `#include <ESP32Ping.h>`:

```cpp
#include <Preferences.h>
```

**Step 2: Add credential globals and storage functions**

Find the `// --- Credentials ---` section (around line 31). After the existing credential lines, add:

```cpp
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
    // Seed with hardcoded creds on first boot so user doesn't have to re-enter
    if (wifiCredsCount == 0) {
        strncpy(wifiSsid[0], WIFI_SSID, 32);
        strncpy(wifiPass[0], WIFI_PASSWORD, 63);
        wifiCredsCount = 1;
        saveWifiCreds();
    }
}

// Insert ssid+pass at slot 0 (most-recently-used). Shift others down. Cap at WIFI_PREFS_MAX.
void insertWifiCred(const char* ssid, const char* pass) {
    // Remove existing entry for this SSID if present
    for (int i = 0; i < wifiCredsCount; i++) {
        if (strcmp(wifiSsid[i], ssid) == 0) {
            for (int j = i; j < wifiCredsCount - 1; j++) {
                strncpy(wifiSsid[j], wifiSsid[j+1], 32);
                strncpy(wifiPass[j], wifiPass[j+1], 63);
            }
            wifiCredsCount--;
            break;
        }
    }
    int newCount = wifiCredsCount + 1;
    if (newCount > WIFI_PREFS_MAX) newCount = WIFI_PREFS_MAX;
    for (int i = newCount - 1; i > 0; i--) {
        strncpy(wifiSsid[i], wifiSsid[i-1], 32);
        strncpy(wifiPass[i], wifiPass[i-1], 63);
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
```

**Step 3: Refactor connectWiFi()**

Find `void connectWiFi(bool showSplash = false)` (around line 757). Replace the entire function:

```cpp
// --- WiFi ---
// Returns true if connected. Pass showSplash=true for boot (draws title + "Connecting:" text).
bool connectWiFi(const char* ssid, const char* pass, bool showSplash = false) {
    if (showSplash) {
        tft.setFreeFont(&DejaVuSansBold12px);
        tft.setTextColor(TFT_YELLOW, COL_BG);
        tft.drawString("CYD AI chatbot v: 0.1.", 0, 0);
        tft.drawString("It's cheap for a reason.", 0, LINE_H_LARGE);
        tft.setTextColor(TFT_BLUE, COL_BG);
        char wifiMsg[80];
        snprintf(wifiMsg, sizeof(wifiMsg), "Connecting: %.55s...", ssid);
        tft.drawString(wifiMsg, 0, 2 * LINE_H_LARGE);
        tft.setTextFont(1);
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < WIFI_MAX_ATTEMPTS) {
        delay(WIFI_RETRY_DELAY_MS);
        attempts++;
    }
    if (showSplash) tft.fillRect(0, 0, SCREEN_W, SPLASH_H, COL_BG);
    updateLedWifi();
    return WiFi.status() == WL_CONNECTED;
}
```

**Step 4: Update all connectWiFi() call sites**

There are three places that call `connectWiFi()`:

1. `setup()` (around line 1372) — change:
   ```cpp
   connectWiFi(true);
   ```
   to:
   ```cpp
   bool wifiOk = connectWiFi(wifiSsid[0], wifiPass[0], true);
   if (!wifiOk) {
       addMessage(false, true, "WiFi connect failed");
   }
   ```
   Also add `loadWifiCreds();` as the first line of `setup()`, before `Serial.begin`:
   ```cpp
   void setup() {
       loadWifiCreds();   // load NVS credentials before anything else
       Serial.begin(115200);
       ...
   ```

2. `callGemini()` (around line 924) — change:
   ```cpp
   connectWiFi();
   ```
   to:
   ```cpp
   connectWiFi(wifiSsid[0], wifiPass[0]);
   ```

3. `callGrok()` (around line 1035) — change:
   ```cpp
   connectWiFi();
   ```
   to:
   ```cpp
   connectWiFi(wifiSsid[0], wifiPass[0]);
   ```

**Step 5: Update checkWiFiHealth() background reconnect**

Find `WiFi.begin(WIFI_SSID, WIFI_PASSWORD)` in `checkWiFiHealth()` (around line 1399). Change to:
```cpp
WiFi.begin(wifiSsid[0], wifiPass[0]);
```

**Step 6: Compile**

```bash
~/.platformio/penv/bin/pio run
```

Expected: SUCCESS. If `loadWifiCreds` or `insertWifiCred` not declared — check the globals block was added in the right place (before the function that calls them; all NVS functions added in Step 2 are fine since they're before connectWiFi).

**Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "030326 WiFi AP menu: add Preferences credential store, refactor connectWiFi()"
```

---

### Task 2: AP list display helper

**Files:**
- Modify: `src/main.cpp`

Add `drawAPList()` — a standalone drawing function that takes the scanned data and renders the full-screen AP picker. No touch logic here, just rendering.

**Step 1: Add drawAPList() after drawKeyboard()**

Find the end of `drawKeyboard()` (around line 240, ends with `}`). Insert after it:

```cpp
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
    tft.setTextFont(1);
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
```

**Step 2: Compile**

```bash
~/.platformio/penv/bin/pio run
```

Expected: SUCCESS.

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "030326 WiFi AP menu: add drawAPList() helper"
```

---

### Task 3: enterPassword() + selectAP() + wire into setup()

**Files:**
- Modify: `src/main.cpp`

This task adds the two remaining functions and wires everything into `setup()`.

**Step 1: Add enterPassword() after typeKBKey()**

Find the end of `typeKBKey()` (around line 603, ends with `}`). Insert after it:

```cpp
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
    tft.setTextFont(1);
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
                    if (inputLen < INPUT_MAX_LEN) {
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
                if (inputLen + addLen <= INPUT_MAX_LEN) {
                    memcpy(inputBuf + inputLen, typed.c_str(), addLen);
                    inputLen += addLen;
                    inputBuf[inputLen] = '\0';
                    drawInputBar();
                }
            }
        }
    }
}
```

**Step 2: Add selectAP() after connectWiFi()**

Find the end of `connectWiFi()` (around line 783, ends with `}`). Insert after it:

```cpp
// Scan WiFi, let user pick an AP, handle password entry, connect.
// Loops until connected. Call from setup() when connectWiFi() returns false.
void selectAP() {
    while (true) {  // outer: re-scan loop
        // --- Scan ---
        tft.fillScreen(COL_BG);
        tft.setTextFont(1); tft.setTextColor(TFT_YELLOW, COL_BG);
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
        int indices[20];
        int total = (n < 20) ? n : 20;
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
            tft.setTextFont(1); tft.setTextColor(TFT_BLUE, COL_BG);
            char msg[80]; snprintf(msg, sizeof(msg), "Connecting: %.55s...", selSsid);
            tft.setCursor(0, 0); tft.print(msg);

            bool ok = connectWiFi(selSsid, pass);

            if (ok) {
                insertWifiCred(selSsid, pass);
                return;  // connected — setup() continues to selectModel()
            }

            // --- Failed: offer re-enter or new scan ---
            tft.fillScreen(COL_BG);
            tft.setTextFont(1);
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
```

**Step 3: Wire selectAP() into setup()**

Find `setup()` (around line 1346). Make two changes:

a) Add `loadWifiCreds()` as the first statement:
```cpp
void setup() {
    loadWifiCreds();   // load NVS; seeds from WIFI_SSID/WIFI_PASSWORD on first boot
    Serial.begin(115200);
```

b) Replace the `connectWiFi(true)` call block with:
```cpp
    bool wifiOk = connectWiFi(wifiSsid[0], wifiPass[0], true);
    if (!wifiOk) {
        selectAP();  // scan → pick AP → enter password → connect; returns only on success
    }
```

Note: remove the old `connectWiFi(true)` line and the existing `addMessage(false, true, "WiFi connect failed")` that was inside connectWiFi() — we no longer need that message since selectAP() handles the failure case.

**Step 4: Compile**

```bash
~/.platformio/penv/bin/pio run
```

Expected: SUCCESS. Common issues:
- `selectAP` not declared before `setup()` — add a forward declaration `void selectAP();` near the top of the file, after the credential store globals (just like `void sendPrompt();` at line 605).
- `AP_ROW_H` not visible — it was defined in Task 2's block; confirm it's before `selectAP()`.
- `drawAPList` not declared before `selectAP()` — add forward declaration `void drawAPList(const char apSsids[][33], const int* apRssi, int apCount);` near the top alongside the selectAP forward declaration.

**Step 5: Upload and verify**

```bash
~/.platformio/penv/bin/pio run -t upload
```

To test the AP menu without flashing wrong credentials:
1. Temporarily change `WIFI_PASSWORD` at the top of main.cpp to a wrong value and reflash — boot should fail the first connect and drop into the scan screen.
2. Select your AP from the list, type the correct password, tap Send.
3. Device should connect, LED should go blue/cyan, then show the model picker.
4. On the next reboot (with the wrong WIFI_PASSWORD still set): boot should try slot 0 (the one you just saved) and connect without showing the AP menu. This confirms the NVS saved correctly.
5. Restore the correct WIFI_PASSWORD.

**Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "030326 WiFi AP menu: add enterPassword(), selectAP(), wire into setup()"
```
