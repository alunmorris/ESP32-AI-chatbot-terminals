// hal_c3.cpp — ESP32-C3 Supermini: NimBLE BLE HID host keyboard input
// No LED, no speaker, no touch.
#ifdef TARGET_C3

#include "hal.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <TFT_eSPI.h>

// External TFT for status display during init
extern TFT_eSPI tft;

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
    if (!found) { p.end(); return NimBLEAddress(""); }
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
        else if (code == 0x52) { ev.type = INPUT_SCROLL_UP; }          // ↑
        else if (code == 0x51) { ev.type = INPUT_SCROLL_DOWN; }        // ↓
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
static NimBLEClient*  bleClient   = nullptr;
static NimBLEAddress  bondedAddr  = NimBLEAddress("");
static bool           hasBonded   = false;
static volatile bool  connected   = false;

static bool doConnect(const NimBLEAddress& addr);

// Reconnect task — retries every 2 seconds on disconnect
static void reconnectTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!connected && hasBonded) {
            doConnect(bondedAddr);
        }
    }
}

class ClientCB : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int) override {
        connected = false;
    }
};

static bool subscribeHID(NimBLEClient* client) {
    NimBLERemoteService* svc = client->getService(HID_SVC_UUID);
    if (!svc) return false;
    // Subscribe to ALL HID Report characteristics (keyboard may have multiple)
    auto chars = svc->getCharacteristics(true);
    if (!chars) return false;  // null-check before dereference
    bool ok = false;
    for (auto& c : *chars) {
        if (c->getUUID() == HID_RPT_UUID) {
            c->subscribe(hidNotifyCB, nullptr, true);
            ok = true;
        }
    }
    return ok;
}

static bool doConnect(const NimBLEAddress& addr) {
    if (!bleClient) {
        bleClient = NimBLEDevice::createClient();
        bleClient->setClientCallbacks(new ClientCB(), false);
    }
    if (!bleClient->connect(addr)) return false;
    if (!subscribeHID(bleClient)) { bleClient->disconnect(); return false; }
    connected = true;
    return true;
}

// Scan callback — connects to first HID device found
class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->isAdvertisingService(HID_SVC_UUID)) return;
        NimBLEDevice::getScan()->stop();
        bondedAddr = dev->getAddress();
        hasBonded  = true;
        saveBondedAddress(bondedAddr);
        doConnect(bondedAddr);
    }
};

// --- halInit ---
void halInit() {
    rb_mutex = xSemaphoreCreateMutex();

    NimBLEDevice::init("");
    NimBLEDevice::setSecurityAuth(true, true, true);   // bonding, MITM, SC
    // Concrete security callback subclass — accepts "Just Works" pairing
    struct KB_SecCB : public NimBLESecurityCallbacks {
        uint32_t onPassKeyRequest()                              override { return 0; }
        void     onPassKeyNotify(uint32_t)                       override {}
        bool     onSecurityRequest()                             override { return true; }
        void     onAuthenticationComplete(NimBLEConnInfo& info)  override {}
        bool     onConfirmPIN(uint32_t)                          override { return true; }
    };
    NimBLEDevice::setSecurityCallbacks(new KB_SecCB());

    // Try bonded address first
    bondedAddr = loadBondedAddress(hasBonded);
    if (hasBonded) {
        if (!doConnect(bondedAddr)) hasBonded = false;  // stale bond, fall through to scan
    }

    if (!connected) {
        // Display waiting message
        tft.setTextSize(1);
        tft.setTextColor(0xFFFF, 0x0841);
        tft.setCursor(0, 0); tft.print("Waiting for BLE keyboard...");

        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setScanCallbacks(new ScanCB(), false);
        scan->setActiveScan(true);
        while (!connected) {
            scan->start(10, false);  // 10-second window, blocking
        }
        tft.fillRect(0, 0, 320, 12, 0x0841);  // clear waiting message
    }

    // Start reconnect background task
    xTaskCreate(reconnectTask, "ble_recon", 4096, nullptr, 1, nullptr);
}

// --- No-op stubs ---
void halClickSound() {}
void halSetLed(uint8_t, uint8_t, uint8_t) {}
void halLoadTouchCal() {}
void halWaitTap(int* sx, int* sy) { if (sx) *sx = -1; if (sy) *sy = -1; }
void calibrateTouch() {}
void pollKBHide() {}

#endif // TARGET_C3
