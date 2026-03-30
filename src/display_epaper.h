// display_epaper.h — GxEPD2 display wrapper with TFT_eSPI-compatible API
// Used only when TARGET_EPAPER is defined.
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
#define EPD_MISO  5   // dummy — prevents SPI library crash

// --- Font metrics for u8g2_font_helvB08_tf ---
#define EPD_FONT            u8g2_font_helvB08_tf
#define EPD_FONT_ASCENT     8    // pixels above baseline for cap letters
#define FONT_LINE_H        12    // line spacing (glyph height ~10 + 2 px leading)
#define FONT_TXT_H         10    // approximate glyph height (ascent+descent)
#define FONT_TXT_W          7    // approximate average char width
#define FONT_LOAD(obj)   ((void)0)   // no-op: font pre-selected in EpaperDisplay::init()
#define FONT_UNLOAD(obj) ((void)0)   // no-op

// TL_DATUM / TR_DATUM values matching TFT_eSPI
#ifndef TL_DATUM
#  define TL_DATUM 0
#  define TR_DATUM 2
#endif

class EpaperDisplay {
public:
    GxEPD2_BW<GxEPD2_213_GDEY0213B74, GxEPD2_213_GDEY0213B74::HEIGHT> epd;
    U8G2_FOR_ADAFRUIT_GFX u8g2;

private:
    uint16_t _fg   = GxEPD_BLACK;
    uint16_t _bg   = GxEPD_WHITE;
    uint8_t  _datum = TL_DATUM;

    // Map any RGB565 colour to e-paper B&W.
    // Light colours (luminance > threshold) → white; everything else → black.
    static uint16_t mapCol(uint16_t rgb565) {
        uint16_t r = (rgb565 >> 11) & 0x1F;
        uint16_t g = (rgb565 >> 5)  & 0x3F;
        uint16_t b =  rgb565        & 0x1F;
        // Rough luminance (max possible: 31*8+63*4+31*8=748)
        uint16_t luma = r * 8 + g * 4 + b * 8;
        return (luma > 500) ? GxEPD_WHITE : GxEPD_BLACK;
    }

public:
    EpaperDisplay()
        : epd(GxEPD2_213_GDEY0213B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)) {}

    // --- Lifecycle ---

    void init(uint32_t baud = 0, bool initial = true) {
        SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
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
        u8g2.setCursor((u8g2_uint_t)x, (u8g2_uint_t)(y + EPD_FONT_ASCENT));
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
        u8g2.setCursor((u8g2_uint_t)x, (u8g2_uint_t)(y + EPD_FONT_ASCENT));
        u8g2.print(str);
        _datum = TL_DATUM;  // reset after use, matching TFT_eSPI behaviour
    }

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
