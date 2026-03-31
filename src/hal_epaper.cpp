// hal_epaper.cpp — ESP32-C3 Supermini: NimBLE BLE HID host keyboard input
// No LED, no speaker, no touch.
// 300326 Initial: BLE keyboard + GxEPD2 frame-based boot screen
#ifdef TARGET_EPAPER

#include "hal.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "display_epaper.h"
#include "esp_coexist.h"  // coexistence preference API

// External e-paper display for status during init
extern EpaperDisplay tft;

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
    if (!found) { p.end(); return NimBLEAddress(); }
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
        else if (code == 0x52) { ev.type = INPUT_SCROLL_DOWN; }        // ↑ = scroll toward newer
        else if (code == 0x51) { ev.type = INPUT_SCROLL_UP; }          // ↓ = scroll toward older
        else if (code == 0x50) { ev.type = INPUT_CURSOR_LEFT; }        // ← = move cursor left
        else if (code == 0x4F) { ev.type = INPUT_CURSOR_RIGHT; }       // → = move cursor right
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
static NimBLEClient*  bleClient    = nullptr;
static NimBLEAddress  bondedAddr;
static bool           hasBonded    = false;
static volatile bool  connected    = false;
static volatile bool  wantConnect  = false;  // set by scan CB, consumed by main loop

static bool doConnect(const NimBLEAddress& addr, uint8_t connectTimeoutSec = 15);
static void setupScan();   // forward decl — registers scan callbacks after each NimBLE init

static TaskHandle_t reconnectTaskHandle = nullptr;

// Reconnect task — scan for keyboard (address increments each session, direct connect unreliable).
// The user's next keypress wakes the keyboard and makes it advertise.
static void reconnectTask(void*) {
    for (;;) {
        if (connected)  { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!hasBonded) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        // Scan for keyboard — address may have changed since last session
        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        wantConnect = false;
        scan->start(10, false);
        for (int i = 0; i < 100 && !connected && !wantConnect; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        scan->stop();
        if (wantConnect && !connected) {
            wantConnect = false;
            vTaskDelay(pdMS_TO_TICKS(200));  // let scan fully shut down before connecting
            doConnect(bondedAddr, 15);
        }
        vTaskDelay(pdMS_TO_TICKS(500));  // brief pause before next scan window
    }
}

class ClientCB : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int) override {
        connected = false;
    }
    // NimBLE 2.x: security callbacks are part of NimBLEClientCallbacks
    void onPassKeyEntry(NimBLEConnInfo&) override {}   // accept "Just Works"
    void onAuthenticationComplete(NimBLEConnInfo&) override {}
    void onConfirmPasskey(NimBLEConnInfo& info, uint32_t) override {
        NimBLEDevice::injectConfirmPasskey(info, true);
    }
};

static bool subscribeHID(NimBLEClient* client) {
    NimBLERemoteService* svc = client->getService(HID_SVC_UUID);
    if (!svc) return false;
    // Subscribe to ALL HID Report characteristics (keyboard may have multiple)
    // NimBLE 2.x: getCharacteristics() returns const std::vector<...>& (not a pointer)
    const auto& chars = svc->getCharacteristics(true);
    bool ok = false;
    for (auto* c : chars) {
        if (c->getUUID() == HID_RPT_UUID) {
            c->subscribe(true, hidNotifyCB, true);  // NimBLE 2.x arg order: (notify, cb, response)
            ok = true;
        }
    }
    return ok;
}

static bool doConnect(const NimBLEAddress& addr, uint8_t connectTimeoutSec) {
    // Always use a fresh client — reusing a client after a failed connect is unreliable
    if (bleClient) {
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
    }
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCB(), false);
    bleClient->setConnectTimeout(connectTimeoutSec);
    Serial.printf("[BLE] connecting to %s...\n", addr.toString().c_str());
    if (!bleClient->connect(addr)) {
        if (connectTimeoutSec <= 3) {
            // Boot-time fast try (3s) — keyboard likely asleep, fall through to scan
            Serial.println("[BLE] boot connect failed, falling through to scan");
            NimBLEDevice::deleteClient(bleClient);
            bleClient = nullptr;
            return false;
        }
        // Reconnect task: keyboard may still be in directed-advertising mode toward
        // our old address. Wait for it to time out and switch to undirected (~1.3s),
        // then retry WITHOUT deleting bonds — the bond is almost certainly still valid.
        // deleteAllBonds() is intentionally omitted here: wiping bonds on every failed
        // connect was the primary cause of having to re-pair on every boot.
        Serial.printf("[BLE] connect() failed (our addr=%s type=%d), waiting for undirected...\n",
            NimBLEDevice::getAddress().toString().c_str(),
            NimBLEDevice::getAddress().getType());
        NimBLEDevice::deleteClient(bleClient);
        vTaskDelay(pdMS_TO_TICKS(1500));
        bleClient = NimBLEDevice::createClient();
        bleClient->setClientCallbacks(new ClientCB(), false);
        bleClient->setConnectTimeout(connectTimeoutSec);
        Serial.printf("[BLE] retry connecting to %s...\n", addr.toString().c_str());
        if (!bleClient->connect(addr)) {
            Serial.println("[BLE] connect() failed (after wait)");
            return false;
        }
    }
    Serial.println("[BLE] connected, subscribing HID...");
    if (!subscribeHID(bleClient)) {
        Serial.println("[BLE] subscribeHID failed");
        bleClient->disconnect();
        return false;
    }
    // Request a longer connection interval (80 × 1.25ms = 100ms) to yield more radio time to WiFi.
    // Keyboard may or may not honour this; HID latency for typing is still acceptable at 100ms.
    bleClient->setConnectionParams(80, 80, 0, 400);  // min, max, latency, supervision_timeout
    connected = true;
    Serial.println("[BLE] HID subscribed OK");
    return true;
}

// Scan callback — static instance, reused after each NimBLE reinit
class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        bool isHID  = dev->isAdvertisingService(HID_SVC_UUID);
        bool isKB   = dev->getName().find("Keyboard") != std::string::npos ||
                      dev->getName().find("keyboard") != std::string::npos;
        Serial.printf("[BLE scan] found: %s name='%s' HID=%d KB=%d advType=%d\n",
            dev->getAddress().toString().c_str(),
            dev->getName().c_str(), isHID, isKB, dev->getAdvType());
        if (!isHID && !isKB) return;  // accept HID service OR keyboard name (directed adv has no UUID)
        NimBLEDevice::getScan()->stop();
        bondedAddr = dev->getAddress();
        if (!hasBonded) {
            // First-time pairing only: persist the address.
            // On reconnects the address may be a rotating private address; NimBLE
            // resolves it via IRK from its own bond store — no NVS write needed.
            hasBonded = true;
            saveBondedAddress(bondedAddr);
            Serial.println("[BLE scan] new keyboard — address saved");
        }
        Serial.println("[BLE scan] HID found — flagging for connect");
        wantConnect = true;  // connect from main loop, not from callback
    }
};
static ScanCB gScanCB;  // single instance reused across NimBLE reinits

// --- Address arithmetic ---
// Keyboard increments its last address octet each session (resolvable private address behaviour).
// Try bonded+1 on boot if direct connect fails, to avoid a full scan round-trip.
static NimBLEAddress addrPlusOne(const NimBLEAddress& addr) {
    std::string s = addr.toString();  // "xx:xx:xx:xx:xx:yy"
    if (s.size() < 2) return addr;
    // Parse last octet
    std::string lastHex = s.substr(s.size() - 2);
    unsigned long last = strtoul(lastHex.c_str(), nullptr, 16);
    last = (last + 1) & 0xFF;
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", (unsigned)last);
    std::string next = s.substr(0, s.size() - 2) + buf;
    return NimBLEAddress(next.c_str(), addr.getType());
}

// Register scan callbacks on the current scan singleton (must be called after each NimBLE init)
static void setupScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&gScanCB, false);
    scan->setActiveScan(true);
    scan->setInterval(320);  // 200 ms interval (units of 0.625 ms)
    scan->setWindow(96);     //  60 ms window  → 30% duty cycle, leaves 70% for WiFi
}

// --- halInit ---
void halInit() {
    rb_mutex = xSemaphoreCreateMutex();
    Serial.println("[EPD BLE] Starting NimBLE init...");
    NimBLEDevice::init("");
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);  // use factory MAC, stable across reinits
    Serial.println("[EPD BLE] NimBLE init done");
    NimBLEDevice::setSecurityAuth(true, false, false);  // bonding, Just Works
    // NimBLE 2.x: security callbacks live in NimBLEClientCallbacks (see ClientCB above)

    // Show boot status using e-paper frame-based rendering.
    // Each bootRow() does a partial refresh (500 ms) of one text row.
    const int lineH = FONT_LINE_H;

    // Initial full-screen white clear
    tft.beginFrame();
    tft.epd.fillScreen(GxEPD_WHITE);
    tft.endFrame();

    auto bootRow = [&](int row, const char* text) {
        int rowY = row * lineH;
        tft.beginPartialFrame(0, rowY, tft.epd.width(), lineH + 4);
        tft.epd.fillRect(0, rowY, tft.epd.width(), lineH + 4, GxEPD_WHITE);
        tft.u8g2.setForegroundColor(GxEPD_BLACK);
        tft.u8g2.setBackgroundColor(GxEPD_WHITE);
        tft.u8g2.setCursor(6, rowY + EPD_FONT_ASCENT);
        tft.u8g2.print(text);
        tft.endFrame();
    };

    bondedAddr = loadBondedAddress(hasBonded);
    Serial.printf("[EPD BLE] hasBonded=%d\n", hasBonded);

    // Row 0 always: "Keyboard connection..."
    bootRow(0, "Keyboard connection...");

    if (hasBonded) {
        // Phase 1: scan for known keyboard — user taps a key to wake it and trigger advertising
        bootRow(1, "Tap Enter twice");

        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        // 3 × 10s = 30s window; ScanCB sets wantConnect when it finds the keyboard
        for (int w = 0; w < 3 && !connected; w++) {
            wantConnect = false;
            Serial.printf("[EPD BLE] reconnect scan window %d\n", w);
            scan->start(10, false);
            for (int i = 0; i < 100 && !connected; i++) {
                if (wantConnect) {
                    wantConnect = false;
                    NimBLEAddress found = bondedAddr;
                    if (doConnect(found, 15)) {
                        if (found != bondedAddr) { bondedAddr = found; saveBondedAddress(bondedAddr); }
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        scan->stop();
        Serial.printf("[EPD BLE] reconnect scan done: connected=%d\n", connected);
    }

    if (!connected) {
        // Phase 2: pairing mode — any HID keyboard can connect.
        // Clear row 1 (was "Tap a key") and show pairing prompt.
        bootRow(1, "Set keyboard to pairing...");

        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        String dots;
        while (!connected) {
            wantConnect = false;
            Serial.println("[BLE scan] starting 10s window...");
            scan->start(10, false);
            for (int i = 0; i < 100 && !connected; i++) {
                if (wantConnect) {
                    wantConnect = false;
                    doConnect(bondedAddr);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            Serial.printf("[BLE scan] window done, connected=%d\n", connected);
            if (!connected) {
                dots += '.';
                bootRow(2, dots.c_str());
            }
        }
    }

    // Always show final connection status before returning (covers all connection paths).
    bootRow(0, "Keyboard connection...");
    bootRow(1, "Connected.");

    // Start reconnect background task
    xTaskCreate(reconnectTask, "ble_recon", 4096, nullptr, 1, &reconnectTaskHandle);
}

// --- BLE coexistence around API calls ---
// Keep BLE connected — just give WiFi radio priority during TLS.
// BLE link survives (may be laggy) and resumes immediately after.
void halBeforeApiCall() {
    if (reconnectTaskHandle) vTaskSuspend(reconnectTaskHandle);
    NimBLEDevice::getScan()->stop();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    Serial.printf("[BLE] before API: heap=%u\n", ESP.getFreeHeap());
}

void halAfterApiCall() {
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    Serial.printf("[BLE] after API: heap=%u\n", ESP.getFreeHeap());
    if (reconnectTaskHandle) vTaskResume(reconnectTaskHandle);
}

// --- No-op stubs ---
void halClickSound() {}
void halSetLed(uint8_t, uint8_t, uint8_t) {}
void halLoadTouchCal() {}
void calibrateTouch() {}
void pollKBHide() {}

#endif // TARGET_EPAPER
