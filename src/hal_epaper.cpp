// hal_epaper.cpp — ESP32-C3 Supermini: NimBLE BLE HID host keyboard input
// No LED, no speaker, no touch.
// 080426 halDeepSleep(): revert to light sleep + GPIO9; reconnectTask handles BLE on wake
// 070426 halDeepSleep(): vTaskDelay loop (BLE alive); wake: drawInputBar() not full drawHistory()
// 070426 halDeepSleep(): two-phase sleep — 100ms timer wakeup for 110s (BLE alive), then GPIO9 only
// 070426 setupScan(): continuous scan (window=interval=100ms) — 2% duty cycle missed KB adverts after wake
// 070426 halDeepSleep(): switched to light sleep — GPIO9 (BOOT) not RTC-capable on C3, can't deep sleep
// 060426 halDeepSleep(): GPIO 9 wakeup deep sleep; halIsDeepSleepWake(); halInit() skips boot screen on wake
// 050426 halSleepIdle: call esp_pm_configure(light_sleep=true) — was no-op, tickless idle never engaged
// 050426 pioarduino: __wrap_log_printf stub; patch_esptool.py --use-segments for epaper3v3 tickless idle
// 040426 epaper3v3: TARGET_EPAPER3V3 define; silence Serial; setCpuFrequencyMhz(80) at halInit start
// 030426 fixup CPU not sleeping. Add halSleepIdle() and call dummyScanCB (Gemini)
// 010426 Power: BLE scan duty cycle 30%, btStop/btStart during API calls for mutual exclusion (Gemini)
// 010426 BLE: fast retry loop (3x/200ms) replaces 1.5s sleep; scan duration 0 (indefinite); remove canNotify() guard
// 310326 WiFi power save: PS_NONE during API calls, MAX_MODEM idle; BLE coex preference
// 310326 BLE: secureConnection before subscribeHID; subscribe retry 5x; cache-bust hidden HID service
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
#include <esp_wifi.h>     // esp_wifi_set_ps()
#include <esp_pm.h>       // esp_pm_configure()
#include <esp_sleep.h>    // esp_light_sleep_start(), esp_sleep_enable_gpio_wakeup()
#include <esp_system.h>   // esp_reset_reason()
#include <driver/gpio.h>  // gpio_wakeup_enable()
#ifdef TARGET_EPAPER3V3
#include "soc/usb_serial_jtag_struct.h"  // USB_SERIAL_JTAG.conf0.usb_pad_enable
#endif

// epaper3v3: no USB, no Serial — silence all debug output at compile time
#ifdef TARGET_EPAPER3V3
#define EPD_LOG(...)  do {} while(0)
#else
#define EPD_LOG(...)  Serial.printf(__VA_ARGS__)
#endif

// Workaround: pioarduino 55.03.37 framework-arduinoespressif32-libs esp32c3
// pre-compiled libespressif__esp_diagnostics.a is missing __wrap_log_printf,
// but the linker uses --wrap=log_printf which requires it.
// No-op stub: safe during early IDF boot before USB/UART is initialised.
#include <stdarg.h>
extern "C" {
    void __wrap_log_printf(const char *format, ...) {
        (void)format;
    }
}

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

bool halPeekInput(InputEvent* ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    bool has = (rb_tail != rb_head);
    if (has) *ev = rb[rb_tail];   // read without advancing tail
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
static NimBLEClient* bleClient    = nullptr;
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
        if (connected)  { vTaskDelay(pdMS_TO_TICKS(100));  continue; }  // 100ms: react quickly after sleep wake
        if (!hasBonded) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        // Scan for keyboard — address may have changed since last session.
        // start() blocks; ScanCB calls stop() immediately on find, unblocking it.
        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        wantConnect = false;
        scan->start(0, false);
        for (int i = 0; i < 100 && !connected && !wantConnect; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        scan->stop();
        if (wantConnect && !connected) {
            wantConnect = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            doConnect(bondedAddr, 15);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
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
        EPD_LOG("%s\n", "[BLE] HID svc missing, forcing cache refresh...");
        vTaskDelay(pdMS_TO_TICKS(600));
        client->getServices(true);   // true = bypass cache, re-fetch from peripheral
        svc = client->getService(HID_SVC_UUID);
    }
    if (!svc) { EPD_LOG("%s\n", "[BLE] ERR: HID service not found"); return false; }

    // Force characteristic refresh too in case they were hidden pre-encryption.
    const auto& chars = svc->getCharacteristics(true);
    bool subscribedAny = false;
    for (auto* c : chars) {
        if (c->getUUID() == HID_RPT_UUID) {
            // Retry subscribe — rejected with "Insufficient Authentication" if SMP not done.
            bool subOk = false;
            for (int t = 0; t < 5; t++) {
                if (c->subscribe(true, hidNotifyCB, true)) {
                    EPD_LOG("%s\n", "[BLE] subscribed to HID report");
                    subOk = true; subscribedAny = true; break;
                }
                EPD_LOG("[BLE] subscribe() rejected (auth race?), retry %d/5\n", t + 1);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            if (!subOk) EPD_LOG("%s\n", "[BLE] ERR: gave up subscribing to HID report");
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
    EPD_LOG("[BLE] connecting to %s...\n", addr.toString().c_str());

    // FAST RETRY LOOP: The KB is only awake for a brief window.
    // Do not use long delays here or it will go back to sleep!
    bool connectedOk = false;
    for (int i = 0; i < 3; i++) {
        if (bleClient->connect(addr)) {
            connectedOk = true;
            break;
        }
        EPD_LOG("[BLE] connect attempt %d failed, retrying quickly...\n", i + 1);
        vTaskDelay(pdMS_TO_TICKS(200)); // Only 200ms wait, catch it before it sleeps
    }

    if (!connectedOk) {
        EPD_LOG("%s\n", "[BLE] all connect attempts failed. KB likely asleep.");
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
        return false;
    }

    EPD_LOG("%s\n", "[BLE] connected, securing link...");
    bleClient->secureConnection();
    // Give SMP time to do the cryptographic key exchange
    vTaskDelay(pdMS_TO_TICKS(1000));

    EPD_LOG("%s\n", "[BLE] link secure initiated, subscribing HID...");
    if (!subscribeHID(bleClient)) {
        EPD_LOG("%s\n", "[BLE] subscribeHID failed");
        bleClient->disconnect();
        return false;
    }

    // latency=30: keyboard may sleep for up to 30 events (~3s) between acks.
    // timeout=1000 (10s) > (1+30)*100ms*2 = 6.2s minimum required by spec.
    bleClient->setConnectionParams(80, 80, 30, 1000);
    connected = true;
    EPD_LOG("%s\n", "[BLE] HID subscribed OK");
    return true;
}

// Scan callback — static instance, reused after each NimBLE reinit
class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        bool isHID  = dev->isAdvertisingService(HID_SVC_UUID);
        bool isKB   = dev->getName().find("Keyboard") != std::string::npos ||
                      dev->getName().find("keyboard") != std::string::npos;
        EPD_LOG("[BLE scan] found: %s name='%s' HID=%d KB=%d advType=%d\n",
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
            EPD_LOG("%s\n", "[BLE scan] new keyboard — address saved");
        }
        EPD_LOG("%s\n", "[BLE scan] HID found — flagging for connect");
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

bool halIsDeepSleepWake() {
    return esp_reset_reason() == ESP_RST_DEEPSLEEP;
}

// Register scan callbacks on the current scan singleton (must be called after each NimBLE init)
//030426 Gemini mod for low power
static void setupScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&gScanCB, false);
    // Passive scan: no SCAN_REQ Tx, just listen. Keyboards advertise UUID in the adv packet.
    scan->setActiveScan(false);
    // Continuous scan (window == interval): catches first KB advertisement immediately after wake.
    // Previous 2% duty cycle (20ms/1s) took ~6s to find KB after it started advertising.
    scan->setInterval(160); // 100 ms (units of 0.625 ms)
    scan->setWindow(160);   // 100 ms — continuous
}

// --- halInit ---
void halInit() {
#ifdef TARGET_EPAPER3V3
    // Disable USB Serial/JTAG PHY — still powered by default even with ARDUINO_USB_MODE=0.
    // Saves ~7 mA on ESP32-C3.
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
#endif
    rb_mutex = xSemaphoreCreateMutex();
    EPD_LOG("%s\n", "[EPD BLE] Starting NimBLE init...");
    NimBLEDevice::init("");
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);  // use factory MAC, stable across reinits
    EPD_LOG("%s\n", "[EPD BLE] NimBLE init done");
    NimBLEDevice::setSecurityAuth(true, false, false);  // bonding, Just Works
    // NimBLE 2.x: security callbacks live in NimBLEClientCallbacks (see ClientCB above)

    // Show boot status using e-paper frame-based rendering.
    // Each bootRow() does a partial refresh (500 ms) of one text row.
    const int lineH = FONT_LINE_H;

    bondedAddr = loadBondedAddress(hasBonded);
    EPD_LOG("[EPD BLE] hasBonded=%d\n", hasBonded);

    // On deep sleep wake: skip full boot screen and blocking BLE scan.
    // The reconnect task runs in the background; keyboard reconnects on first keypress.
    if (halIsDeepSleepWake()) {
        // BLE reconnect task is started later via halStartBleReconnect(), called from setup()
        // after WiFi connects. Starting BLE scan here would race with WiFi association on the
        // ESP32-C3's shared radio and prevent WiFi from connecting.
        EPD_LOG("%s\n", "[EPD BLE] Deep sleep wake — skipping boot screen, deferring BLE scan");
        const int lineH = FONT_LINE_H;
        tft.beginPartialFrame(0, 0, tft.epd.width(), lineH);
        tft.epd.fillRect(0, 0, tft.epd.width(), lineH, GxEPD_BLACK);
        tft.u8g2.setFont(EPD_FONT);
        tft.u8g2.setForegroundColor(GxEPD_WHITE);
        tft.u8g2.setBackgroundColor(GxEPD_BLACK);
        tft.u8g2.setCursor(6, EPD_FONT_ASCENT + 2);
        tft.u8g2.print("CRACK: Cheap Remote AI Chat Keyboard");
        tft.endFrame();
        return;
    }

    // Render help text + "Keyboard connection..." + initial status in one full frame.
    // Drawing all static rows together avoids the ~500ms per-row partial refresh delay.
    auto drawBootLine = [&](int row, const char* text) {
        tft.u8g2.setForegroundColor(GxEPD_BLACK);
        tft.u8g2.setBackgroundColor(GxEPD_WHITE);
        tft.u8g2.setCursor(6, row * lineH + EPD_FONT_ASCENT);
        tft.u8g2.print(text);
    };
    tft.beginFrame();
    tft.epd.fillScreen(GxEPD_WHITE);
    tft.epd.fillRect(0, 0, tft.epd.width(), lineH, GxEPD_BLACK);
    tft.u8g2.setForegroundColor(GxEPD_WHITE);
    tft.u8g2.setBackgroundColor(GxEPD_BLACK);
    tft.u8g2.setCursor(6, EPD_FONT_ASCENT + 2);  // +2px so title sits clear of the top edge
    tft.u8g2.print("CRACK: Cheap Remote AI Chat Keyboard");
    drawBootLine(2, "Chat commands:");
    drawBootLine(3, "more (or ctrl-M) / new (or ctrl-N) / menu");

    drawBootLine(5, "Keyboard connection...");
    drawBootLine(6, hasBonded ? "Tap any key to wake keyboard" : "");
    tft.endFrame();

    // Partial-frame update for a single status row — height exactly lineH avoids overlap.
    auto bootRow = [&](int row, const char* text) {
        int rowY = row * lineH;
        tft.beginPartialFrame(0, rowY, tft.epd.width(), lineH);
        tft.epd.fillRect(0, rowY, tft.epd.width(), lineH, GxEPD_WHITE);
        tft.u8g2.setForegroundColor(GxEPD_BLACK);
        tft.u8g2.setBackgroundColor(GxEPD_WHITE);
        tft.u8g2.setCursor(6, rowY + EPD_FONT_ASCENT);
        tft.u8g2.print(text);
        tft.endFrame();
    };

    if (hasBonded) {
        // Phase 1: scan for known keyboard — user taps a key to wake it and trigger advertising

        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        // 3 × 10s windows; start() blocks until ScanCB calls stop() (KB found) or 10s expires.
        for (int w = 0; w < 3 && !connected; w++) {
            wantConnect = false;
            EPD_LOG("[EPD BLE] reconnect scan window %d\n", w);
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
        // Extra 3s grace before pairing mode — keyboard may still be waking
        if (!connected) {
            bootRow(5, "Keyboard connection... (3s)");
            wantConnect = false;
            scan->start(0, false);
            for (int i = 0; i < 30 && !connected && !wantConnect; i++) {
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
        EPD_LOG("[EPD BLE] reconnect scan done: connected=%d\n", connected);
    }

    if (!connected) {
        // Phase 2: pairing mode — clear screen first, then show pairing prompt.
        tft.beginFrame();
        tft.epd.fillScreen(GxEPD_WHITE);
        tft.epd.fillRect(0, 0, tft.epd.width(), lineH, GxEPD_BLACK);
        tft.u8g2.setForegroundColor(GxEPD_WHITE);
        tft.u8g2.setBackgroundColor(GxEPD_BLACK);
        tft.u8g2.setCursor(6, EPD_FONT_ASCENT + 2);
        tft.u8g2.print("CRACK: Cheap Remote AI Chat Keyboard");
        drawBootLine(2, "Set keyboard to pairing...");
        tft.endFrame();

        setupScan();
        NimBLEScan* scan = NimBLEDevice::getScan();
        String dots;
        while (!connected) {
            wantConnect = false;
            EPD_LOG("%s\n", "[BLE scan] starting 10s window...");
            scan->start(0, false);
            for (int i = 0; i < 100 && !connected && !wantConnect; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            scan->stop();
            if (wantConnect && !connected) {
                wantConnect = false;
                doConnect(bondedAddr);
            }
            EPD_LOG("[BLE scan] window done, connected=%d\n", connected);
            if (!connected) {
                dots += '.';
                bootRow(3, dots.c_str());
            }
        }
    }

    // Update status row only — row 4 ("Keyboard connection...") already correct from full frame.
    bootRow(5, "Connected.");

    // Start reconnect background task
    xTaskCreate(reconnectTask, "ble_recon", 4096, nullptr, 1, &reconnectTaskHandle);

    // Enable automatic light sleep — CPU sleeps whenever FreeRTOS is idle.
    // BLE wakes it on incoming HID notifications; halBeforeApiCall disables it during TLS.
    halSleepIdle();
}

// --- BLE coexistence around API calls ---
// Give WiFi radio priority during TLS; BLE link survives (may be briefly laggy).
void halBeforeApiCall() {
    if (reconnectTaskHandle) vTaskSuspend(reconnectTaskHandle);
    NimBLEDevice::getScan()->stop();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    esp_wifi_set_ps(WIFI_PS_NONE);
    EPD_LOG("[Power] before API: Coex WiFi. Heap=%u\n", ESP.getFreeHeap());
}

void halAfterApiCall() {
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    EPD_LOG("[Power] after API: Coex Balance. Heap=%u\n", ESP.getFreeHeap());
    if (reconnectTaskHandle) vTaskResume(reconnectTaskHandle);
}

// Start BLE reconnect task after WiFi is up (deep sleep wake path only).
void halStartBleReconnect() {
    if (!reconnectTaskHandle) {
        xTaskCreate(reconnectTask, "ble_recon", 4096, nullptr, 1, &reconnectTaskHandle);
    }
}

// --- No-op stubs ---
void halClickSound() {}
void halSetLed(uint8_t, uint8_t, uint8_t) {}
void halLoadTouchCal() {}
void calibrateTouch() {}
void pollKBHide() {}

void halSleepIdle() {
    // Light sleep via esp_pm_configure() requires CONFIG_PM_ENABLE=y (pioarduino only).
    // Standard espressif32 builds use halDeepSleep() on idle timeout instead.
}

void halDeepSleep() {
    // Light sleep: very low current (~1-2mA). BLE radio clock is gated so the keyboard
    // disconnects, but reconnectTask re-scans automatically on wake using continuous scan
    // (setInterval/setWindow=160) so the keyboard reconnects quickly.
    // Only GPIO9 (BOOT) wakes the device.
    gpio_set_direction(GPIO_NUM_9, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_9, GPIO_PULLUP_ONLY);
    gpio_wakeup_enable(GPIO_NUM_9, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    // Disconnect cleanly before sleep: clears NimBLE's internal connection state so it can
    // scan immediately on wake rather than spending seconds cleaning up a stale connection.
    // This also makes the keyboard start advertising immediately (it sees the disconnect).
    if (bleClient && bleClient->isConnected()) {
        bleClient->disconnect();
    }
    connected = false;
    esp_light_sleep_start();
    // FreeRTOS resumes here. NimBLE is already in clean disconnected state; reconnectTask scans.
}

#endif // TARGET_EPAPER