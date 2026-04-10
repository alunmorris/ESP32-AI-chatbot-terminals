// hal_c3.cpp — ESP32-C3 Supermini: NimBLE BLE HID host keyboard input
// No LED, no speaker, no touch.
// 090426 BLE: scan duty cycle 30% (320/96); reconnectTask timed 10s scan (NimBLE buffer flush)
// 010426 BLE: fast retry loop (3x/200ms) replaces 1.5s sleep; scan duration 0 (indefinite); remove canNotify() guard
// 010426 halInit: proceed to UI after Phase 1 if hasBonded; reconnectTask handles background reconnect
// 310326 WiFi power save: PS_NONE during API calls, MAX_MODEM idle; BLE coex preference
// 240326 Boot screen: sprite-based rendering (bootRow lambda) to fix C3 SPI glitches (white rect, garbled text)
// 240326 Font selection: use FONT_LOAD/FONT_UNLOAD macros from font.h; support FONT_BUILTIN_16PX
#ifdef TARGET_C3

#include "hal.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <TFT_eSPI.h>
#include "esp_coexist.h"  // coexistence preference API
#include <esp_wifi.h>     // esp_wifi_set_ps()

// External TFT for status display during init
extern TFT_eSPI tft;
#include "font.h"

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

bool halPeekInput(InputEvent* ev) { (void)ev; return false; }
void halSleepIdle() {}
void halDeepSleep() {}
bool halIsDeepSleepWake() { return false; }
void halStartBleReconnect() {}

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
        scan->start(10, false);  // 10s timed; NimBLE flushes buffers on completion
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
    // Some keyboards hide services until fully encrypted — bust the cache and retry.
    if (!svc) {
        Serial.println("[BLE] HID svc missing, forcing cache refresh...");
        vTaskDelay(pdMS_TO_TICKS(600));
        client->getServices(true);   // true = bypass cache, re-fetch from peripheral
        svc = client->getService(HID_SVC_UUID);
    }
    if (!svc) { Serial.println("[BLE] ERR: HID service not found"); return false; }

    // Force characteristic refresh too in case they were hidden pre-encryption.
    const auto& chars = svc->getCharacteristics(true);
    bool subscribedAny = false;
    for (auto* c : chars) {
        if (c->getUUID() == HID_RPT_UUID) {
            // Retry subscribe — rejected with "Insufficient Authentication" if SMP not done.
            bool subOk = false;
            for (int t = 0; t < 5; t++) {
                if (c->subscribe(true, hidNotifyCB, true)) {
                    Serial.println("[BLE] subscribed to HID report");
                    subOk = true; subscribedAny = true; break;
                }
                Serial.printf("[BLE] subscribe() rejected (auth race?), retry %d/5\n", t + 1);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            if (!subOk) Serial.println("[BLE] ERR: gave up subscribing to HID report");
        }
    }
    return subscribedAny;
}

static bool doConnect(const NimBLEAddress& addr, uint8_t connectTimeoutSec) {
    if (bleClient) {
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
    }
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCB(), false);
    bleClient->setConnectTimeout(connectTimeoutSec);
    Serial.printf("[BLE] connecting to %s...\n", addr.toString().c_str());

    // FAST RETRY LOOP: The KB is only awake for a brief window.
    // Do not use long delays here or it will go back to sleep!
    bool connectedOk = false;
    for (int i = 0; i < 3; i++) {
        if (bleClient->connect(addr)) {
            connectedOk = true;
            break;
        }
        Serial.printf("[BLE] connect attempt %d failed, retrying quickly...\n", i + 1);
        vTaskDelay(pdMS_TO_TICKS(200)); // Only 200ms wait, catch it before it sleeps
    }

    if (!connectedOk) {
        Serial.println("[BLE] all connect attempts failed. KB likely asleep.");
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
        return false;
    }

    Serial.println("[BLE] connected, securing link...");
    bleClient->secureConnection();
    // Give SMP time to do the cryptographic key exchange
    vTaskDelay(pdMS_TO_TICKS(1000));

    Serial.println("[BLE] link secure initiated, subscribing HID...");
    if (!subscribeHID(bleClient)) {
        Serial.println("[BLE] subscribeHID failed");
        bleClient->disconnect();
        return false;
    }

    // latency=30: keyboard may sleep for up to 30 events (~3s) between acks.
    // timeout=1000 (10s) > (1+30)*100ms*2 = 6.2s minimum required by spec.
    bleClient->setConnectionParams(80, 80, 30, 1000);
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
    scan->setWindow(96);     //  60 ms window  → 30% duty cycle; leaves radio time for WiFi
}

// --- halInit ---
void halInit() {
    rb_mutex = xSemaphoreCreateMutex();
    Serial.println("[C3 BLE] Starting NimBLE init...");
    NimBLEDevice::init("");
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);  // use factory MAC, stable across reinits
    Serial.println("[C3 BLE] NimBLE init done");
    NimBLEDevice::setSecurityAuth(true, false, false);  // bonding, Just Works
    // NimBLE 2.x: security callbacks live in NimBLEClientCallbacks (see ClientCB above)

    // Show boot status and attempt reconnect to known keyboard.
    // All text uses sprite rendering to avoid C3 per-glyph SPI setWindow glitches
    // (which cause white rectangles and garbled pixels with direct drawString).
    const uint16_t BOOT_BG = 0xC618;  // light grey — matches COL_INVERT_BG in main.cpp
    const uint16_t BOOT_FG = 0x0000;  // black text on light background
    const int lineH = FONT_LINE_H;  // rows are lineH pixels tall, same unit as chat display
    tft.fillScreen(BOOT_BG);  // fill entire screen with light background before drawing rows

    // Helper: push one boot-screen row as a sprite.
    // Row 0 = top of screen, row 1 = lineH, row 2 = 2*lineH, etc.
    auto bootRow = [&](int row, const char* text, uint16_t col = 0x0000) {
        TFT_eSprite spr(&tft);
        spr.setColorDepth(16);
        if (spr.createSprite(tft.width(), lineH)) {
            FONT_LOAD(spr);
            spr.fillSprite(BOOT_BG);
            spr.setTextColor(col, BOOT_BG);
            spr.drawString(text, 6, 0);
            FONT_UNLOAD(spr);
            spr.pushSprite(0, row * lineH);
            spr.deleteSprite();
        }
    };

    bondedAddr = loadBondedAddress(hasBonded);
    Serial.printf("[C3 BLE] hasBonded=%d\n", hasBonded);

    // Help text rows 0-3
    bootRow(0, "CRACK: Cheap Remote AI Chat Keyboard");
    bootRow(1, "");
    bootRow(2, "Chat commands:");
    bootRow(3, "more (or ctrl-M) / new (or ctrl-N) / menu");

    // Row 5 always: "Keyboard connection..."
    bootRow(5, "Keyboard connection...");

    if (hasBonded) {
        // Phase 1: scan for known keyboard — user taps a key to wake it and trigger advertising
        bootRow(6, "Tap any key to wake keyboard");

        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        // 3 × 10s = 30s window; ScanCB sets wantConnect when it finds the keyboard
        for (int w = 0; w < 3 && !connected; w++) {
            wantConnect = false;
            Serial.printf("[C3 BLE] reconnect scan window %d\n", w);
            scan->start(0, false);
            for (int i = 0; i < 100 && !connected && !wantConnect; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            scan->stop();
            if (wantConnect && !connected) {
                wantConnect = false;
                NimBLEAddress found = bondedAddr;
                if (doConnect(found, 15)) {
                    if (found != bondedAddr) { bondedAddr = found; saveBondedAddress(bondedAddr); }
                }
            }
        }
        Serial.printf("[C3 BLE] reconnect scan done: connected=%d\n", connected);
    }

    if (!connected) {
        if (hasBonded) {
            // Known keyboard didn't respond — proceed to UI, reconnect in background.
            bootRow(6, "Keyboard not found.");
            vTaskDelay(pdMS_TO_TICKS(1500));
        } else {
            // Phase 2: no bonded keyboard yet — pairing mode, block until paired.
            bootRow(6, "Set keyboard to pairing...");

            setupScan();
            NimBLEScan* scan = NimBLEDevice::getScan();
            String dots;
            while (!connected) {
                wantConnect = false;
                Serial.println("[BLE scan] starting 10s window...");
                scan->start(0, false);
                for (int i = 0; i < 100 && !connected && !wantConnect; i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                scan->stop();
                Serial.printf("[BLE scan] window done, connected=%d\n", connected);
                if (wantConnect && !connected) {
                    wantConnect = false;
                    doConnect(bondedAddr);
                }
                if (!connected) {
                    dots += '.';
                    bootRow(6, dots.c_str());
                }
            }
        }
    }

    // Always clear the boot area before returning (covers all connection paths).
    tft.fillRect(0, 0, tft.width(), 7 * lineH, BOOT_BG);

    // Start reconnect background task
    xTaskCreate(reconnectTask, "ble_recon", 4096, nullptr, 1, &reconnectTaskHandle);
}

// --- BLE coexistence around API calls ---
// Keep BLE connected — just give WiFi radio priority during TLS.
// BLE link survives (may be laggy) and resumes immediately after.
void halBeforeApiCall() {
    setCpuFrequencyMhz(160);  // TLS crypto is CPU-bound; 160 MHz halves handshake time
    if (reconnectTaskHandle) vTaskSuspend(reconnectTaskHandle);
    NimBLEDevice::getScan()->stop();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    esp_wifi_set_ps(WIFI_PS_NONE);   // full radio power during TLS — no dropped connections
    Serial.printf("[BLE] before API: heap=%u\n", ESP.getFreeHeap());
}

void halAfterApiCall() {
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);  // back to low-power idle
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    Serial.printf("[BLE] after API: heap=%u\n", ESP.getFreeHeap());
    if (reconnectTaskHandle) vTaskResume(reconnectTaskHandle);
    setCpuFrequencyMhz(80);   // back to low-power idle
}

// --- No-op stubs ---
void halClickSound() {}
void halSetLed(uint8_t, uint8_t, uint8_t) {}
void halLoadTouchCal() {}
void calibrateTouch() {}
void pollKBHide() {}

#endif // TARGET_C3
