// hal_s2.cpp — ESP32-S2 Mini: USB HID host keyboard + simple GPIO LED
// GPIO 15: single LED (active-high) — driven by halSetLed(); main.cpp uses for WiFi status
// GPIO 19/20: D-/D+ USB OTG via USB-C — keyboard powered externally (5 V VBUS required)
// No BLE, no touch, no speaker.
// 280326 Initial implementation
// 280326 S2_DUMMY_INPUT: bypass USB, feed fixed string into input ring buffer
// 290326 LED: replace NeoPixel with simple GPIO (active-high)
// 290326 SPI: use GPIO 34/35/36/37 (native FSPI pins on right header); MISO must be real pin
// 290326 USB host: require ARDUINO_USB_CDC_ON_BOOT=0 to free OTG peripheral from CDC
#ifdef TARGET_S2

#include "hal.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "font.h"
#ifndef S2_DUMMY_INPUT
#include "usb/usb_host.h"
#endif

extern TFT_eSPI tft;

// ── LED (simple GPIO, active-high) ────────────────────────────────────────────
#define LED_PIN 15

void halSetLed(uint8_t r, uint8_t g, uint8_t b) {
    // Single-colour LED: on if any channel non-zero
    digitalWrite(LED_PIN, (r || g || b) ? HIGH : LOW);
}

// ── Ring buffer ───────────────────────────────────────────────────────────────
#define RB_SIZE 16
static InputEvent        rb[RB_SIZE];
static volatile int      rb_head  = 0;
static volatile int      rb_tail  = 0;
static SemaphoreHandle_t rb_mutex = nullptr;

static void rb_push(InputEvent ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    int next = (rb_head + 1) % RB_SIZE;
    if (next != rb_tail) { rb[rb_head] = ev; rb_head = next; }
    xSemaphoreGive(rb_mutex);
}

bool halPollInput(InputEvent* ev) {
    xSemaphoreTake(rb_mutex, portMAX_DELAY);
    bool has = (rb_tail != rb_head);
    if (has) { *ev = rb[rb_tail]; rb_tail = (rb_tail + 1) % RB_SIZE; }
    xSemaphoreGive(rb_mutex);
    return has;
}

#ifndef S2_DUMMY_INPUT
// ── HID scan code → ASCII ─────────────────────────────────────────────────────
// Identical to hal_c3.cpp — pure table lookup, no BLE dependency.
static char hidToAscii(uint8_t code, bool shifted) {
    if (code >= 0x04 && code <= 0x1D) {
        char c = 'a' + (code - 0x04);
        return shifted ? (c - 32) : c;
    }
    if (code >= 0x1E && code <= 0x27) {
        static const char num[]   = "1234567890";
        static const char numSh[] = "!@#$%^&*()";
        return shifted ? numSh[code - 0x1E] : num[code - 0x1E];
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

// ── HID report parser ─────────────────────────────────────────────────────────
// Standard USB HID boot-protocol keyboard report: 8 bytes.
// byte 0: modifier mask  byte 1: reserved  bytes 2-7: up to 6 keycodes
static void parseHidReport(const uint8_t* data, size_t len) {
    if (len < 2) return;
    uint8_t modifier = data[0];
    bool ctrl    = (modifier & 0x11) != 0;   // 0x01=LCtrl, 0x10=RCtrl
    bool shifted = (modifier & 0x22) != 0;   // 0x02=LShift, 0x20=RShift

    bool allZero = true;
    for (size_t i = 2; i < len && i < 8; i++) if (data[i]) { allZero = false; break; }
    if (allZero) return;  // key-up report — ignore

    for (size_t i = 2; i < len && i < 8; i++) {
        uint8_t code = data[i];
        if (!code) continue;
        InputEvent ev = { INPUT_NONE, 0 };
        if      (ctrl && code == 0x11) ev.type = INPUT_NEW_CONV;    // Ctrl+N
        else if (ctrl && code == 0x10) ev.type = INPUT_MORE;        // Ctrl+M
        else if (code == 0x52)         ev.type = INPUT_SCROLL_DOWN; // ↑ = newer
        else if (code == 0x51)         ev.type = INPUT_SCROLL_UP;   // ↓ = older
        else if (code == 0x50)         ev.type = INPUT_CURSOR_LEFT;
        else if (code == 0x4F)         ev.type = INPUT_CURSOR_RIGHT;
        else if (code == 0x28)         ev.type = INPUT_ENTER;
        else if (code == 0x2A)         ev.type = INPUT_BACKSPACE;
        else {
            char c = hidToAscii(code, shifted);
            if (c) { ev.type = INPUT_CHAR; ev.ch = c; }
        }
        if (ev.type != INPUT_NONE) rb_push(ev);
    }
}

// ── USB Host ─────────────────────────────────────────────────────────────────
static usb_host_client_handle_t s_client   = nullptr;
static usb_device_handle_t      s_dev      = nullptr;
static usb_transfer_t*          s_xfer     = nullptr;
static uint8_t                  s_ifaceNum = 0;
static volatile bool            s_devOpen  = false;

// Flags set by clientEventCb, consumed by usbClientTask
static volatile uint8_t s_newAddr    = 0;
static volatile bool    s_newDev     = false;
static volatile bool    s_devGone    = false;
static volatile bool    s_usbInitErr = false;  // set if usb_host_install fails

static void transferCb(usb_transfer_t* xfer) {
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0)
        parseHidReport(xfer->data_buffer, xfer->actual_num_bytes);
    if (s_devOpen && xfer->status == USB_TRANSFER_STATUS_COMPLETED)
        usb_host_transfer_submit(xfer);  // resubmit for next interrupt report
}

static void clientEventCb(const usb_host_client_event_msg_t* msg, void*) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_newAddr = msg->new_dev.address;
        s_newDev  = true;
    } else {
        s_devGone = true;
    }
}

// Scan the active configuration descriptor for the first HID interface
// (class 0x03) that has an interrupt-IN endpoint.
// Accepts any HID subclass/protocol — most keyboards support boot protocol
// regardless of what they advertise.
static bool findHidEndpoint(usb_device_handle_t dev,
                            uint8_t* epAddr, uint16_t* maxPkt, uint8_t* ifNum) {
    const usb_config_desc_t* cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return false;

    bool    inHid  = false;
    int     offset = 0;
    const uint8_t* p = (const uint8_t*)cfg;

    while (offset + 2 <= cfg->wTotalLength) {
        uint8_t dLen  = p[offset];
        uint8_t dType = p[offset + 1];
        if (!dLen || offset + dLen > cfg->wTotalLength) break;

        if (dType == 0x04) {  // USB_B_DESCRIPTOR_TYPE_INTERFACE
            const usb_intf_desc_t* intf = (const usb_intf_desc_t*)(p + offset);
            inHid = (intf->bInterfaceClass == 0x03);  // HID class
            if (inHid) *ifNum = intf->bInterfaceNumber;
        } else if (dType == 0x05 && inHid) {  // USB_B_DESCRIPTOR_TYPE_ENDPOINT
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(p + offset);
            bool isIn  = (ep->bEndpointAddress & 0x80) != 0;
            bool isInt = (ep->bmAttributes & 0x03) == 0x03;
            if (isIn && isInt) {
                *epAddr = ep->bEndpointAddress;
                *maxPkt = ep->wMaxPacketSize;
                return true;
            }
        }
        offset += dLen;
    }
    return false;
}

static void openDevice(uint8_t addr) {
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
        Serial.println("[USB] device_open failed");
        return;
    }
    uint8_t  epAddr = 0;
    uint16_t maxPkt = 8;
    if (!findHidEndpoint(s_dev, &epAddr, &maxPkt, &s_ifaceNum)) {
        Serial.println("[USB] no HID interrupt-IN endpoint — not a keyboard?");
        usb_host_device_close(s_client, s_dev);
        s_dev = nullptr;
        return;
    }
    if (usb_host_interface_claim(s_client, s_dev, s_ifaceNum, 0) != ESP_OK) {
        Serial.println("[USB] interface_claim failed");
        usb_host_device_close(s_client, s_dev);
        s_dev = nullptr;
        return;
    }
    if (usb_host_transfer_alloc(maxPkt, 0, &s_xfer) != ESP_OK) {
        Serial.println("[USB] transfer_alloc failed");
        usb_host_interface_release(s_client, s_dev, s_ifaceNum);
        usb_host_device_close(s_client, s_dev);
        s_dev = nullptr;
        return;
    }
    s_xfer->device_handle    = s_dev;
    s_xfer->bEndpointAddress = epAddr;
    s_xfer->callback         = transferCb;
    s_xfer->context          = nullptr;
    s_xfer->num_bytes        = maxPkt;
    s_xfer->timeout_ms       = 0;  // interrupt: fire when data arrives
    s_devOpen = true;
    usb_host_transfer_submit(s_xfer);
    Serial.printf("[USB] keyboard open: iface=%d ep=0x%02x maxPkt=%d\n",
                  s_ifaceNum, epAddr, maxPkt);
}

static void closeDevice() {
    s_devOpen = false;
    if (s_xfer && s_dev) {
        usb_host_endpoint_halt(s_dev, s_xfer->bEndpointAddress);
        usb_host_endpoint_flush(s_dev, s_xfer->bEndpointAddress);
    }
    if (s_xfer) { usb_host_transfer_free(s_xfer); s_xfer = nullptr; }
    if (s_dev)  {
        usb_host_interface_release(s_client, s_dev, s_ifaceNum);
        usb_host_device_close(s_client, s_dev);
        s_dev = nullptr;
    }
    Serial.println("[USB] keyboard closed");
}

// USB host library daemon — must run at higher priority than the client task
// so it processes events (including the initial "host lib installed" signal) first.
static void usbHostDaemonTask(void*) {
    usb_host_config_t cfg = {};
    cfg.skip_phy_setup = false;
    cfg.intr_flags     = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[USB] host_install failed: 0x%x\n", err);
        s_usbInitErr = true;
        vTaskDelete(nullptr);
        return;
    }
    Serial.println("[USB] host lib installed");
    uint32_t flags;
    for (;;) {
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// USB client task — registers as a client, waits for device events, opens/closes keyboard.
static void usbClientTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(100));  // ensure daemon has installed the host lib
    if (s_usbInitErr) { vTaskDelete(nullptr); return; }

    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous             = false;
    clientCfg.max_num_event_msg          = 5;
    clientCfg.async.client_event_callback = clientEventCb;
    clientCfg.async.callback_arg          = nullptr;
    esp_err_t err = usb_host_client_register(&clientCfg, &s_client);
    if (err != ESP_OK) {
        Serial.printf("[USB] client_register failed: 0x%x\n", err);
        s_usbInitErr = true;
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
        if (s_newDev) {
            s_newDev = false;
            Serial.printf("[USB] new device addr=%d\n", s_newAddr);
            openDevice(s_newAddr);
        }
        if (s_devGone) {
            s_devGone = false;
            closeDevice();
        }
    }
}

#else  // S2_DUMMY_INPUT ────────────────────────────────────────────────────────

// Feeds "ESP32-S2 dummy text\n" into the ring buffer once, 3 s after boot,
// then repeats every 10 s so the app stays exercised.
static void dummyInputTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (;;) {
        const char* msg = "ESP32-S2 dummy text";
        for (const char* p = msg; *p; p++) {
            InputEvent ev = { INPUT_CHAR, *p };
            rb_push(ev);
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        InputEvent enter = { INPUT_ENTER, 0 };
        rb_push(enter);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

#endif  // S2_DUMMY_INPUT

// ── halInit ───────────────────────────────────────────────────────────────────
void halInit() {
    rb_mutex = xSemaphoreCreateMutex();
    pinMode(LED_PIN, OUTPUT);
    halSetLed(0, 0, 1);  // on during boot

    const uint16_t BOOT_BG = 0xC618;  // matches COL_INVERT_BG in main.cpp
    const int lineH = FONT_LINE_H;
    tft.fillScreen(BOOT_BG);

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

#ifdef S2_DUMMY_INPUT
    bootRow(0, "Dummy input mode");
    xTaskCreate(dummyInputTask, "dummy_input", 2048, nullptr, 1, nullptr);
#else
    bootRow(0, "USB keyboard...");

    // Use priority 1 (same as Arduino setup task) so both tasks wait until
    // halInit() reaches its vTaskDelay loop before preempting.
    xTaskCreate(usbHostDaemonTask, "usb_host",   4096, nullptr, 1, nullptr);
    xTaskCreate(usbClientTask,     "usb_client", 4096, nullptr, 1, nullptr);

    // Wait up to 5 s for keyboard, or bail out if USB init failed
    for (int i = 0; i < 50 && !s_devOpen && !s_usbInitErr; i++) vTaskDelay(pdMS_TO_TICKS(100));

    if (s_usbInitErr) {
        // USB host init failed — show red error so we can see the device is running
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.setTextFont(1);
        tft.drawString("USB host init failed", 6, 10);
        tft.drawString("Check UART0 for error", 6, 30);
        halSetLed(64, 0, 0);  // dim red LED
        return;  // let setup() continue; keyboard won't work but device boots
    }

    if (!s_devOpen) bootRow(1, "Connect keyboard");
#endif

    // Clear boot rows before returning to main
    tft.fillRect(0, 0, tft.width(), 3 * lineH, BOOT_BG);
    halSetLed(0, 0, 0);  // off; main.cpp takes over via halSetLed for WiFi status
}

// ── No-op stubs ───────────────────────────────────────────────────────────────
void halClickSound()  {}
void halLoadTouchCal() {}
void calibrateTouch() {}
void pollKBHide()     {}
void halBeforeApiCall() {}
void halAfterApiCall()  {}

#endif // TARGET_S2
