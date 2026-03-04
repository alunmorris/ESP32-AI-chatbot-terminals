# Unicode Font Design

040326

## Goal

Replace ASCII-only GFX fonts with TFT_eSPI smooth fonts (VLW format) covering Latin Extended and key special characters, so AI responses display accented letters, bullets, dashes, quotes, and symbols without transliteration.

## Codepoints Included

| Range | Description |
|-------|-------------|
| U+0020–U+007E | Printable ASCII |
| U+00A0–U+017F | Latin-1 Supplement + Latin Extended-A |
| U+03A9, U+03C0 | Ω, π |
| U+2011, U+2013, U+2014 | Non-breaking hyphen, en dash, em dash |
| U+2018, U+2019, U+201C, U+201D | Curly single and double quotes |
| U+2022 | Bullet |
| U+2026 | Ellipsis |
| U+2070–U+207F | Superscripts |
| U+20AC | Euro sign |

## Components

### 1. `tools/make_font.py`

Python script using `fonttools` to generate TFT_eSPI VLW-format C header files from `DejaVuSans-Bold.ttf`. Accepts font size as argument. Outputs to `src/fonts/`. Run once to regenerate fonts if TTF or codepoint list changes.

### 2. Font headers

`src/fonts/DejaVuSansBold8px.h` and `src/fonts/DejaVuSansBold12px.h` replaced with VLW byte arrays in PROGMEM. Same filenames; format changes from GFX (`GFXfont` struct) to VLW (`const uint8_t[]`).

### 3. Font helpers in `main.cpp`

```cpp
void fontOn()  { tft.loadFont(largeFont ? DejaVuSansBold12pxData : DejaVuSansBold8pxData); }
void fontOff() { tft.unloadFont(); }
```

All ~10 `setFreeFont` / `setTextFont(1)` pairs replaced with `fontOn()` / `fontOff()`.

### 4. Sanitisation in `addMessage()`

Transliteration block replaced with UTF-8 passthrough filter:
- Strip C0 control chars (U+0000–U+001F, U+007F)
- Drop U+200B (zero-width space — no glyph)
- Pass all supported codepoints through unchanged
- Replace unsupported multi-byte sequences with `?`

## Font Format Notes

GFX format (`setFreeFont`) requires a contiguous codepoint range. Spanning ASCII through Euro (U+20AC) would require ~8,300 glyphs — too large. VLW smooth font format supports non-contiguous glyph sets and uses the same `drawString()` API. Loading changes from `setFreeFont(&Font)` to `tft.loadFont(array)` / `tft.unloadFont()`.
