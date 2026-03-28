// hal_s2.cpp — ESP32-S2 Mini: USB HID host keyboard + WS2812 status LED
// GPIO 15: WS2812 RGB LED (on-board) — driven by halSetLed(); main.cpp uses for WiFi status
// GPIO 19/20: D-/D+ USB OTG — USB-A socket here for wired keyboard (5 V VBUS required)
// No BLE, no touch, no speaker.
// 280326 Initial implementation
#ifdef TARGET_S2

#include "hal.h"
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <TFT_eSPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "font.h"
#include "usb/usb_host.h"

extern TFT_eSPI tft;

// ── NeoPixel ──────────────────────────────────────────────────────────────────
#define LED_PIN 15
static Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);

void halSetLed(uint8_t r, uint8_t g, uint8_t b) {
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();
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
static volatile uint8_t s_newAddr = 0;
static volatile bool    s_newDev  = false;
static volatile bool    s_devGone = false;

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
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    Serial.println("[USB] host lib installed");
    uint32_t flags;
    for (;;) {
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// USB client task — registers as a client, waits for device events, opens/closes keyboard.
static void usbClientTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(50));  // ensure daemon has installed the host lib

    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous             = false;
    clientCfg.max_num_event_msg          = 5;
    clientCfg.async.client_event_callback = clientEventCb;
    clientCfg.async.callback_arg          = nullptr;
    ESP_ERROR_CHECK(usb_host_client_register(&clientCfg, &s_client));

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

// ── halInit ───────────────────────────────────────────────────────────────────
void halInit() {
    rb_mutex = xSemaphoreCreateMutex();
    led.begin();
    led.setBrightness(50);
    halSetLed(0, 0, 64);  // dim blue during boot

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

    bootRow(0, "USB keyboard...");

    // Daemon at priority 5, client at priority 4 — daemon must install lib first
    xTaskCreate(usbHostDaemonTask, "usb_host",   4096, nullptr, 5, nullptr);
    xTaskCreate(usbClientTask,     "usb_client", 4096, nullptr, 4, nullptr);

    // Wait up to 5 s for a keyboard to connect
    for (int i = 0; i < 50 && !s_devOpen; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_devOpen) bootRow(1, "Connect keyboard");

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
