// display_epaper.h — GxEPD2 display wrapper with TFT_eSPI-compatible API
// Used only when TARGET_EPAPER is defined.
// 170426 Switch 240x416 to GxEPD2_370_GDEY037T03 (WeAct 3.7"); remove colour inversion (wrong driver caused it)
// 170426 Add EPD_SIZE_240x416 for WeAct Studio 3.7" GDEY037T03 240×416 B&W panel
// 120426 200x200: use u8g2_font_t0_13b_tf, u8g2_font_t0_13_tf fonts; 1px top margin on all menus
// 010426 Change user font to u8g2_font_luBIS08_tf (italic serif)
// 300326 Initial
#pragma once
#ifdef TARGET_EPAPER

#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SPI.h>

// --- Pin definitions (ESP32-C3 Supermini) ---
#define EPD_SCK   4
#define EPD_MOSI  6
#define EPD_CS    7
#define EPD_DC    1
#define EPD_RST   2
#define EPD_BUSY  3
#define EPD_PWR_EN_L 5  // active-low display power enable (P-FET/load switch gate; LOW=on, HIGH=off)

// --- Display size selection (uncomment one) ---
//#define EPD_SIZE_250x122   // WeAct 2.13" GDEY0213B74 250×122 landscape
//#define EPD_SIZE_200x200   // WeAct 1.54" GDEY0154D67 200×200
#define EPD_SIZE_240x416   // WeAct 3.7" GDEY037T03 240×416 portrait

#ifdef EPD_SIZE_250x122
#  define EPD_WIDTH  250
#  define EPD_HEIGHT 122
#elif defined(EPD_SIZE_240x416)
// Panel scans natively in landscape; setRotation(1) gives logical 416×240.
#  define EPD_WIDTH  416
#  define EPD_HEIGHT 240
#else
#  define EPD_WIDTH  200
#  define EPD_HEIGHT 200
#endif

// --- Font selection ---
#ifdef EPD_SIZE_250x122
#  define EPD_FONT          u8g2_font_helvB08_tf
#  define EPD_FONT_USER     u8g2_font_luBIS08_tf
#  define EPD_FONT_ASCENT   8
#  define FONT_LINE_H      12
#elif defined(EPD_SIZE_240x416)
#  define EPD_FONT          u8g2_font_t0_13b_tf
#  define EPD_FONT_USER     u8g2_font_t0_13_tf
#  define EPD_FONT_HEADER   u8g2_font_helvB18_tf  // larger title font for menu header
#  define EPD_FONT_HDR_ASCENT  18                 // baseline offset for helvB18 (ascent ~17px)
#  define EPD_FONT_HDR_LINE_H  24                 // full line height for helvB18
#  define EPD_FONT_ASCENT   8
#  define FONT_LINE_H      13
#else                           // 200x200
#  define EPD_FONT          u8g2_font_t0_13b_tf
#  define EPD_FONT_USER     u8g2_font_t0_13_tf
#  define EPD_FONT_ASCENT  8
#  define FONT_LINE_H      13
#endif

// --- Display colour polarity ---
// EPD_C_WHITE / EPD_C_BLACK allow per-panel polarity correction if needed.
// GDEY037T03 has correct polarity (0xFF = white) — no swap needed.
#define EPD_C_WHITE  GxEPD_WHITE
#define EPD_C_BLACK  GxEPD_BLACK

// --- Refresh rate limit ---
// Full refresh cycles through all pixels (~1700 ms, higher voltage) and ages the panel.
// Enforce a minimum interval between full refreshes; content-triggered refreshes bypass this.
#define EPD_MIN_FULL_REFRESH_MS 60000UL
#define FONT_TXT_H         10    // approximate glyph height (ascent+descent)
#define FONT_TXT_W          7    // approximate average char width
#define FONT_LOAD(obj)   ((void)0)   // no-op: font pre-selected in EpaperDisplay::init()
#define FONT_UNLOAD(obj) ((void)0)   // no-op

// TL_DATUM / TR_DATUM values matching TFT_eSPI
#ifndef TL_DATUM
#  define TL_DATUM 0
#  define TR_DATUM 2
#endif

// RGB565 colour constants — mirrors TFT_eSPI so main.cpp compiles without it
#ifndef TFT_BLACK
#  define TFT_BLACK       0x0000
#  define TFT_DARKGREY    0x7BEF
#  define TFT_BLUE        0x001F
#  define TFT_GREEN       0x07E0
#  define TFT_CYAN        0x07FF
#  define TFT_RED         0xF800
#  define TFT_YELLOW      0xFFE0
#  define TFT_WHITE       0xFFFF
#  define TFT_ORANGE      0xFDA0
#  define TFT_GREENYELLOW 0xB7E0
#  define TFT_DARKGREEN   0x03E0
#endif

class EpaperDisplay {
public:
#ifdef EPD_SIZE_250x122
    GxEPD2_BW<GxEPD2_213_GDEY0213B74, GxEPD2_213_GDEY0213B74::HEIGHT> epd;
#elif defined(EPD_SIZE_240x416)
    GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT> epd;
#else
    GxEPD2_BW<GxEPD2_154_GDEY0154D67, GxEPD2_154_GDEY0154D67::HEIGHT> epd;
#endif
    U8G2_FOR_ADAFRUIT_GFX u8g2;

private:
    uint16_t _fg   = EPD_C_BLACK;
    uint16_t _bg   = EPD_C_WHITE;
    uint8_t  _datum = TL_DATUM;

    // Map any RGB565 colour to e-paper B&W.
    // Light colours (luminance > threshold) → white; everything else → black.
    static uint16_t mapCol(uint16_t rgb565) {
        uint16_t r = (rgb565 >> 11) & 0x1F;
        uint16_t g = (rgb565 >> 5)  & 0x3F;
        uint16_t b =  rgb565        & 0x1F;
        // Rough luminance (max possible: 31*8+63*4+31*8=748)
        uint16_t luma = r * 8 + g * 4 + b * 8;
        return (luma > 500) ? EPD_C_WHITE : EPD_C_BLACK;
    }

public:
    EpaperDisplay()
#ifdef EPD_SIZE_250x122
        : epd(GxEPD2_213_GDEY0213B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)) {}
#elif defined(EPD_SIZE_240x416)
        : epd(GxEPD2_370_GDEY037T03(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)) {}
#else
        : epd(GxEPD2_154_GDEY0154D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)) {}
#endif

    // --- Lifecycle ---

    void init(uint32_t baud = 0, bool initial = true) {
        SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);  // MISO unused (write-only display)
        epd.init(baud, initial);
        u8g2.begin(epd);
        u8g2.setFont(EPD_FONT);
    }

    void setRotation(uint8_t r)   { epd.setRotation(r); }
    int16_t width()               { return epd.width(); }
    int16_t height()              { return epd.height(); }

    // --- Frame management ---
    // All drawing must happen between beginFrame() and endFrame() (GxEPD2 page loop).
    // With page_height = full display height, firstPage/nextPage runs exactly once,
    // so drawing outside this pair is silently dropped.

    void beginFrame() {
        epd.setFullWindow();
        epd.firstPage();
    }

    // Partial frame: only the specified region is refreshed (500 ms vs 1700 ms full).
    void beginPartialFrame(int16_t x, int16_t y, int16_t w, int16_t h) {
        epd.setPartialWindow(x, y, w, h);
        epd.firstPage();
    }

    // Call once after all drawing is done. Triggers the SPI transfer + panel refresh.
    void endFrame() { epd.nextPage(); }

    // --- TFT_eSPI no-op compatibility stubs ---
    void loadFont(const uint8_t*)   {}
    void unloadFont()               {}
    void setTextFont(uint8_t)       {}
    void setTextSize(uint8_t)       {}
    void setTextWrap(bool)          {}
    void invertDisplay(bool)        {}

    // --- Drawing ---

    void fillScreen(uint16_t c) {
        epd.fillScreen(mapCol(c));
    }

    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
        epd.fillRect(x, y, w, h, mapCol(c));
    }

    void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h,
                       int32_t r, uint16_t c) {
        epd.fillRoundRect(x, y, w, h, r, mapCol(c));
    }

    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) {
        epd.drawLine(x0, y0, x1, y1, mapCol(c));
    }

    void fillCircle(int32_t cx, int32_t cy, int32_t r, uint16_t c) {
        epd.fillCircle(cx, cy, r, mapCol(c));
    }

    // TFT_eSPI drawArc has no Adafruit_GFX equivalent — no-op on e-paper.
    void drawArc(int32_t, int32_t, int32_t, int32_t,
                 float, float, uint16_t, uint16_t) {}

    // --- Text ---

    void setTextColor(uint16_t fg, uint16_t bg) {
        _fg = mapCol(fg);
        _bg = mapCol(bg);
        u8g2.setForegroundColor(_fg);
        u8g2.setBackgroundColor(_bg);
    }

    void setTextDatum(uint8_t d) { _datum = d; }

    // setCursor: TFT_eSPI y = top of glyph; U8g2 y = baseline → add ascent.
    void setCursor(int32_t x, int32_t y) {
        u8g2.setCursor((int16_t)x, (int16_t)(y + EPD_FONT_ASCENT));
    }

    void print(const char* s)  { u8g2.print(s); }
    void print(int n)          { u8g2.print(n); }
    void println(const char* s){ u8g2.println(s); }

    // drawString: TFT_eSPI convention — x,y is top-left of text.
    // Honours TL_DATUM (default) and TR_DATUM (right-align from x).
    void drawString(const char* str, int32_t x, int32_t y) {
        u8g2.setForegroundColor(_fg);
        u8g2.setBackgroundColor(_bg);
        if (_datum == TR_DATUM)
            x -= (int32_t)u8g2.getUTF8Width(str);
        u8g2.setCursor((int16_t)x, (int16_t)(y + EPD_FONT_ASCENT));
        u8g2.print(str);
        _datum = TL_DATUM;  // reset after use, matching TFT_eSPI behaviour
    }

    int16_t getCursorX() { return u8g2.getCursorX(); }

    int32_t textWidth(const char* str) {
        return (int32_t)u8g2.getUTF8Width(str);
    }
};

// ---------------------------------------------------------------------------
// EpaperSprite — stub that matches the TFT_eSprite API used in main.cpp.
// createSprite() always returns false → all sprite blocks are skipped and
// execution falls through to the existing direct tft.* rendering paths.
// ---------------------------------------------------------------------------
class EpaperSprite {
    EpaperDisplay* _p;
public:
    explicit EpaperSprite(EpaperDisplay* p) : _p(p) {}
    void     setColorDepth(uint8_t)                              {}
    bool     createSprite(int32_t, int32_t)                      { return false; }
    void     deleteSprite()                                      {}
    void     setTextWrap(bool)                                   {}
    void     setTextDatum(uint8_t)                               {}
    void     fillSprite(uint16_t)                                {}
    void     fillRect(int32_t, int32_t, int32_t, int32_t, uint16_t) {}
    void     fillCircle(int32_t, int32_t, int32_t, uint16_t)        {}
    void     drawArc(int32_t, int32_t, int32_t, int32_t, float, float, uint16_t, uint16_t) {}
    void     setTextColor(uint16_t, uint16_t)                    {}
    void     drawString(const char*, int32_t, int32_t)           {}
    int32_t  textWidth(const char*)                              { return 0; }
    void     pushSprite(int32_t, int32_t)                        {}
    bool     loadFont(const uint8_t*)                            { return false; }
    void     unloadFont()                                        {}
};

#endif // TARGET_EPAPER
