# Unicode Font Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace ASCII-only GFX fonts with TFT_eSPI smooth fonts (VLW format) covering Latin Extended and key special characters, removing all transliteration.

**Architecture:** Generate VLW-format font data via Python/Pillow, embed as PROGMEM C arrays, replace `setFreeFont`/`setTextFont(1)` pairs with `fontOn()`/`fontOff()` helpers, rewrite `addMessage()` sanitisation as a UTF-8 passthrough filter. Both font sizes (8px small mode, 12px large mode) get Unicode support.

**Tech Stack:** Python 3 + Pillow (font generation), TFT_eSPI smooth font API (`loadFont`/`unloadFont`), C++/Arduino

---

## VLW Binary Format Reference

This is what `Smooth_font.cpp` (`loadFont`) expects:

```
Header — 6 × big-endian uint32 = 24 bytes:
  [0] gCount       number of glyphs
  [1] yAdvance     font size in points (recalculated from glyph metrics at load time)
  [2] ascent       overall font ascent (pixels above baseline for tall chars like 'd')
  [3] descent      overall font descent (pixels below baseline for chars like 'p')
  [4] 0            placeholder
  [5] 0            placeholder

Glyph table — gCount × 7 × big-endian int32 = gCount × 28 bytes:
  Each entry (in codepoint order):
  [0] codePoint    Unicode codepoint (uint32)
  [1] height       glyph bitmap height in pixels (uint32, read as uint8)
  [2] width        glyph bitmap width in pixels (uint32, read as uint8)
  [3] xAdvance     cursor advance in pixels (uint32, read as uint8)
  [4] gdY          pixels above baseline to top of bitmap (int32, read as int16)
  [5] gdX          x offset from cursor to left of bitmap (int32, read as int8)
  [6] 0            ignored

Bitmap data — immediately after glyph table:
  For each glyph (same order): width × height bytes, 8-bit alpha (0=transparent, 255=solid)
  Space and other zero-size glyphs contribute 0 bytes.
```

## Codepoints Included

```python
CODEPOINTS = sorted(set([
    *range(0x0020, 0x007F),   # Printable ASCII
    *range(0x00A0, 0x0180),   # Latin-1 Supplement + Latin Extended-A
    0x03A9, 0x03C0,           # Ω π
    0x2011, 0x2013, 0x2014,   # non-breaking hyphen, en dash, em dash
    0x2018, 0x2019,           # curly single quotes
    0x201C, 0x201D,           # curly double quotes
    0x2022,                   # bullet
    0x2026,                   # ellipsis
    *range(0x2070, 0x2080),   # superscripts ⁰ⁱ⁴⁻⁹⁺⁻⁼⁽⁾ⁿ
    0x20AC,                   # €
]))
```

---

## Task 1: Create `tools/make_font.py`

**Files:**
- Create: `tools/make_font.py`

**Step 1: Write the script**

```python
#!/usr/bin/env python3
"""Generate TFT_eSPI VLW smooth font C headers from DejaVuSans-Bold.ttf.

Usage:
  python tools/make_font.py <size_px> <varname> <output_header>

Example:
  python tools/make_font.py 12 DejaVuSansBold12pxData src/fonts/DejaVuSansBold12px.h
  python tools/make_font.py 8  DejaVuSansBold8pxData  src/fonts/DejaVuSansBold8px.h
"""

import struct
import sys
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

CODEPOINTS = sorted(set([
    *range(0x0020, 0x007F),
    *range(0x00A0, 0x0180),
    0x03A9, 0x03C0,
    0x2011, 0x2013, 0x2014,
    0x2018, 0x2019,
    0x201C, 0x201D,
    0x2022, 0x2026,
    *range(0x2070, 0x2080),
    0x20AC,
]))


def make_vlw(size: int) -> bytes:
    font = ImageFont.truetype(FONT_PATH, size)
    ascent_px, descent_px = font.getmetrics()
    canvas_h = ascent_px + descent_px + 8
    canvas_w = size * 6

    glyphs = []
    for cp in CODEPOINTS:
        char = chr(cp)

        # Draw at (4, ascent_px + 4) so baseline is at y = ascent_px + 4.
        # Default Pillow anchor places the ascender line at the given y, so
        # baseline = y + ascent_px. Adjust accordingly.
        img = Image.new('L', (canvas_w, canvas_h), 0)
        draw = ImageDraw.Draw(img)
        # anchor='ls': left side of baseline — requires Pillow >= 8.0.0
        try:
            draw.text((4, ascent_px + 4), char, font=font, fill=255, anchor='ls')
            baseline_y = ascent_px + 4
        except TypeError:
            # Older Pillow: no anchor param; default places ascender line at y
            draw.text((4, 4), char, font=font, fill=255)
            baseline_y = ascent_px + 4  # ascender at y=4, baseline = 4 + ascent_px

        bbox = img.getbbox()
        xadv = round(font.getlength(char))

        if bbox is None:
            # No visible pixels (e.g. space)
            glyphs.append({
                'cp': cp, 'w': 0, 'h': 0,
                'xAdv': xadv if xadv > 0 else (ascent_px + descent_px) // 4,
                'gdX': 0, 'gdY': 0,
                'pixels': b'',
            })
            continue

        x0, y0, x1, y1 = bbox
        w = x1 - x0
        h = y1 - y0
        gdX = x0 - 4         # x offset from cursor (usually 0)
        gdY = baseline_y - y0  # pixels above baseline to top of bitmap

        pixels = bytes(img.crop(bbox).getdata())
        glyphs.append({
            'cp': cp, 'w': w, 'h': h,
            'xAdv': xadv, 'gdX': gdX, 'gdY': gdY,
            'pixels': pixels,
        })

    gcount = len(glyphs)

    # Header: 6 × big-endian uint32
    header = struct.pack('>IIIIII',
        gcount,
        ascent_px + descent_px,  # yAdvance (recalculated at load time anyway)
        ascent_px,
        descent_px,
        0, 0)

    # Glyph table: gcount × 7 × big-endian int32
    glyph_table = b''
    for g in glyphs:
        glyph_table += struct.pack('>IIIIiiI',
            g['cp'],
            g['h'],
            g['w'],
            g['xAdv'],
            g['gdY'],   # signed: positive = above baseline
            g['gdX'],   # signed: usually 0 or small positive
            0)

    bitmap_data = b''.join(g['pixels'] for g in glyphs)
    return header + glyph_table + bitmap_data


def vlw_to_header(vlw: bytes, varname: str, size: int) -> str:
    total = len(vlw)
    lines = [
        f'// Generated by tools/make_font.py from DejaVuSans-Bold.ttf at {size}px',
        f'// {total} bytes VLW smooth font data',
        '',
        f'const uint8_t {varname}[] PROGMEM = {{',
    ]
    row = []
    for i, b in enumerate(vlw):
        row.append(f'0x{b:02X}')
        if len(row) == 16:
            lines.append('  ' + ', '.join(row) + ',')
            row = []
    if row:
        lines.append('  ' + ', '.join(row))
    lines.append('};')
    lines.append(f'const uint32_t {varname}Size = {total};')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    size = int(sys.argv[1])
    varname = sys.argv[2]
    outfile = sys.argv[3]

    vlw = make_vlw(size)
    header = vlw_to_header(vlw, varname, size)
    with open(outfile, 'w') as f:
        f.write(header)
    print(f'Written {len(vlw)} bytes ({len(vlw)//1024}KB) → {outfile}')
```

**Step 2: Commit**

```bash
cd /home/alun/esp32/cyd28/SLUG-copy-unicode-font
git add tools/make_font.py
git commit -m "Add tools/make_font.py: VLW Unicode font generator"
```

---

## Task 2: Generate Font Headers

**Files:**
- Modify: `src/fonts/DejaVuSansBold12px.h` (replace GFX with VLW)
- Modify: `src/fonts/DejaVuSansBold8px.h`  (replace GFX with VLW)

**Step 1: Run the generator**

```bash
cd /home/alun/esp32/cyd28/SLUG-copy-unicode-font
python tools/make_font.py 12 DejaVuSansBold12pxData src/fonts/DejaVuSansBold12px.h
python tools/make_font.py 8  DejaVuSansBold8pxData  src/fonts/DejaVuSansBold8px.h
```

Expected output (approximate):
```
Written ~65000 bytes (63KB) → src/fonts/DejaVuSansBold12px.h
Written ~38000 bytes (37KB) → src/fonts/DejaVuSansBold8px.h
```

If the output is much larger (>150KB per file) something is wrong — check that `getbbox()` isn't returning the full canvas instead of the glyph.

**Step 2: Sanity check the headers**

```bash
head -5 src/fonts/DejaVuSansBold12px.h
grep "DejaVuSansBold12pxData\[\]" src/fonts/DejaVuSansBold12px.h
grep "DejaVuSansBold12pxDataSize" src/fonts/DejaVuSansBold12px.h
```

Expected: a comment line, the `PROGMEM` array declaration, and the size constant.

**Step 3: Commit**

```bash
git add src/fonts/DejaVuSansBold12px.h src/fonts/DejaVuSansBold8px.h
git commit -m "Regenerate fonts as VLW smooth font arrays with Unicode support"
```

---

## Task 3: Update Buffer Sizes and Word Wrap in `main.cpp`

The `lines[MAX_LINES][54]` buffer is 53 bytes per line. UTF-8 Latin Extended chars are 2 bytes each, so a 53-visible-char line could be 106 bytes. Expand to 128 bytes and update all char-count references.

Also: `rebuildLines()` currently strips `fc > 126` at line ~488 — remove that block since text is now pre-validated UTF-8.

**Files:**
- Modify: `src/main.cpp`

**Step 1: Expand `lines` buffer and related constants**

Find and replace:
```cpp
char              lines[MAX_LINES][54];   // 53 chars + null per line
```
With:
```cpp
char              lines[MAX_LINES][128];  // 127 bytes + null per line (UTF-8 safe)
```

**Step 2: Remove the strip-non-ASCII loop in `rebuildLines()`**

Find (lines ~486-490):
```cpp
        // Strip control chars and high bytes (text already sanitised in addMessage,
        // but prefix chars from prefix string are always clean ASCII).
        for (int j = 0; full[j]; j++) {
            unsigned char fc = (unsigned char)full[j];
            if ((fc < 32 && full[j] != '\n') || fc > 126) full[j] = ' ';
        }
```
Replace with:
```cpp
        // Text is pre-validated UTF-8 from addMessage(). Prefix chars are clean ASCII.
```
(i.e. delete the loop entirely, keep the comment if desired)

**Step 3: Update large-font word-wrap buffer sizes**

In the `if (largeFont)` word-wrap block (lines ~493-543), find:
```cpp
            char lineBuf[54] = "";
```
Replace with:
```cpp
            char lineBuf[128] = "";
```

Find all `strncpy(lines[lineCount], lineBuf, 53)` and `strncpy(lines[lineCount], word, 53)`:
```cpp
                        strncpy(lines[lineCount], lineBuf, 53);
                        lines[lineCount][53] = '\0';
```
Replace with:
```cpp
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
```
Do the same for all other `strncpy` calls into `lines[lineCount]` in this block (there are ~4 of them). Also update:
```cpp
                strncpy(lineBuf, test, 53); lineBuf[53] = '\0';
```
To:
```cpp
                strncpy(lineBuf, test, 127); lineBuf[127] = '\0';
```
And the test buffer:
```cpp
                char test[108];   // lineBuf(53) + space(1) + word(53) + null
```
Replace with:
```cpp
                char test[260];   // lineBuf(127) + space(1) + word(127) + null
```
And the snprintf:
```cpp
                if (lineBuf[0]) snprintf(test, 108, "%s %s", lineBuf, word);
                else            { strncpy(test, word, 53); test[53] = '\0'; }
```
Replace with:
```cpp
                if (lineBuf[0]) snprintf(test, 260, "%s %s", lineBuf, word);
                else            { strncpy(test, word, 127); test[127] = '\0'; }
```
And the word buffer:
```cpp
                char word[54];
                strncpy(word, ws, min(wlen, 53));
                word[min(wlen, 53)] = '\0';
```
Replace with:
```cpp
                char word[128];
                strncpy(word, ws, min(wlen, 127));
                word[min(wlen, 127)] = '\0';
```

**Step 4: Update small-font word-wrap to use pixel-width (unify with large-font path)**

The `else` branch (lines ~544-576) uses character-count wrapping. Multi-byte UTF-8 means byte count ≠ visible char count. Replace the entire `else` block with a pixel-width version matching the large-font path (but using the 8px font to measure):

Find the `} else {` block that starts at ~line 544 through ~576, and replace with:

```cpp
        } else {
            // Pixel-width word wrap for DejaVuSansBold8px
            tft.loadFont(DejaVuSansBold8pxData);
            char lineBuf[128] = "";
            const char* p = full;
            while (*p && lineCount < MAX_LINES - 1) {
                if (*p == '\n') {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                        lineBuf[0] = '\0';
                    }
                    p++;
                    continue;
                }
                const char* ws = p;
                while (*p && *p != ' ' && *p != '\n') p++;
                int wlen = p - ws;
                if (wlen <= 0) { if (*p) p++; continue; }
                char word[128];
                strncpy(word, ws, min(wlen, 127));
                word[min(wlen, 127)] = '\0';
                if (*p == ' ') p++;
                char test[260];
                if (lineBuf[0]) snprintf(test, 260, "%s %s", lineBuf, word);
                else            { strncpy(test, word, 127); test[127] = '\0'; }
                if (tft.textWidth(test) <= SCREEN_W - 2) {
                    strncpy(lineBuf, test, 127); lineBuf[127] = '\0';
                } else {
                    if (lineBuf[0]) {
                        strncpy(lines[lineCount], lineBuf, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                        strncpy(lineBuf, word, 127); lineBuf[127] = '\0';
                    } else {
                        strncpy(lines[lineCount], word, 127);
                        lines[lineCount][127] = '\0';
                        lineColor[lineCount++] = col;
                    }
                }
            }
            if (lineBuf[0] && lineCount < MAX_LINES) {
                strncpy(lines[lineCount], lineBuf, 127);
                lines[lineCount][127] = '\0';
                lineColor[lineCount++] = col;
            }
            tft.unloadFont();
        }
```

**Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Expand line buffers for UTF-8, unify word-wrap to pixel-width for both font sizes"
```

---

## Task 4: Add `fontOn()`/`fontOff()` and Replace `setFreeFont` Call Sites

**Files:**
- Modify: `src/main.cpp`

**Step 1: Update includes at top of `main.cpp`**

Find:
```cpp
#include "fonts/DejaVuSansBold12px.h"  // custom 12px bold, yAdvance=15
#include "fonts/DejaVuSansBold8px.h"   // custom 8px bold, yAdvance=10
```
Replace with:
```cpp
#include "fonts/DejaVuSansBold12px.h"  // VLW smooth font 12px (Unicode)
#include "fonts/DejaVuSansBold8px.h"   // VLW smooth font 8px (Unicode)
```

**Step 2: Add `fontOn()`/`fontOff()` helpers**

Add these two functions just before the first function that uses `setFreeFont` (i.e. before `drawButton()`, around line 295). Add after the `#include` block and global declarations:

```cpp
// Load the bold smooth font (12px). Call fontOff() when done.
void fontOn()  { tft.loadFont(DejaVuSansBold12pxData); }
void fontOff() { tft.unloadFont(); }
```

**Step 3: Replace all `setFreeFont` / `setTextFont(1)` pairs with `fontOn()` / `fontOff()`**

There are ~9 call sites. For each one, replace:
```cpp
tft.setFreeFont(&DejaVuSansBold12px);
```
with:
```cpp
fontOn();
```

And replace:
```cpp
tft.setTextFont(1);
```
with:
```cpp
fontOff();
```

**Special case — `drawHistory()` at line ~593:**

Find:
```cpp
    if (largeFont) tft.setFreeFont(&DejaVuSansBold12px); else tft.setTextSize(1);
    tft.setTextWrap(false);  // prevent overflow wrapping onto adjacent lines
    for (int i = 0; i < maxVis && (firstIdx + i) < lineCount; i++) {
        int idx = firstIdx + i;
        uint16_t col = lineColor[idx];
        if (invertDisplay) col = TFT_BLACK;
        tft.setTextColor(col, bg);
        if (largeFont) tft.drawString(lines[idx], 0, i * lineH);
        else { tft.setCursor(0, i * lineH); tft.print(lines[idx]); }
    }
    tft.setTextWrap(true);
    tft.setTextFont(1);  // restore GLCD for everything else
```
Replace with:
```cpp
    if (largeFont) fontOn(); else tft.loadFont(DejaVuSansBold8pxData);
    tft.setTextWrap(false);  // prevent overflow wrapping onto adjacent lines
    for (int i = 0; i < maxVis && (firstIdx + i) < lineCount; i++) {
        int idx = firstIdx + i;
        uint16_t col = lineColor[idx];
        if (invertDisplay) col = TFT_BLACK;
        tft.setTextColor(col, bg);
        tft.drawString(lines[idx], 0, i * lineH);
    }
    tft.setTextWrap(true);
    fontOff();
```

**Special case — word-wrap font measurement in `rebuildLines()` at line ~494:**

Find:
```cpp
            tft.setFreeFont(&DejaVuSansBold12px);
```
(at the start of the `if (largeFont)` word-wrap block)

Replace with:
```cpp
            fontOn();
```

Find:
```cpp
            tft.setTextFont(1);
```
(at the end of the `if (largeFont)` word-wrap block, line ~543)

Replace with:
```cpp
            fontOff();
```

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Add fontOn/fontOff helpers, replace all setFreeFont calls with smooth font API"
```

---

## Task 5: Replace Transliteration with UTF-8 Passthrough in `addMessage()`

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add `supportedCodepoint()` helper before `addMessage()`**

Find the line `void addMessage(...)` and add this function immediately before it:

```cpp
// Returns true if codepoint cp is covered by the VLW font.
static bool supportedCodepoint(uint32_t cp) {
    if (cp >= 0x0020 && cp <= 0x007E) return true;  // Printable ASCII
    if (cp >= 0x00A0 && cp <= 0x017F) return true;  // Latin-1 + Latin Extended-A
    if (cp == 0x03A9 || cp == 0x03C0) return true;  // Ω π
    if (cp == 0x2011 || cp == 0x2013 || cp == 0x2014) return true;  // dashes
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D) return true;  // quotes
    if (cp == 0x2022 || cp == 0x2026) return true;  // bullet, ellipsis
    if (cp >= 0x2070 && cp <= 0x207F) return true;  // superscripts
    if (cp == 0x20AC) return true;  // €
    return false;
}
```

**Step 2: Replace the sanitisation block in `addMessage()`**

Find the entire sanitisation block (lines ~617-720+), from:
```cpp
    // Sanitise to printable ASCII (32-126). Multi-char replacements use a temp
    // buffer because some strings ("Sterling") are longer than their UTF-8 source.
    {
        const char* s = history[historyCount].text;
        char tmp[2060];
        ...
```
through to the closing `}` of that block (including all the transliteration logic), and replace with:

```cpp
    // Sanitise: pass supported UTF-8 codepoints through unchanged.
    // Strip C0 controls, drop U+200B (zero-width space), replace unsupported
    // multi-byte sequences with '?'.
    {
        const char* s = history[historyCount].text;
        char tmp[2060];
        char* d   = tmp;
        char* end = tmp + sizeof(tmp) - 4;
        while (*s && d < end) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) {
                // ASCII
                if (c == '\n' || (c >= 0x20 && c != 0x7F)) *d++ = (char)c;
                s++;
            } else if ((c & 0xE0) == 0xC0) {
                // 2-byte sequence
                unsigned char b2 = (unsigned char)s[1];
                if ((b2 & 0xC0) == 0x80) {
                    uint32_t cp = ((c & 0x1F) << 6) | (b2 & 0x3F);
                    if (supportedCodepoint(cp)) { *d++ = (char)c; *d++ = (char)b2; }
                    else                         *d++ = '?';
                    s += 2;
                } else { *d++ = '?'; s++; }
            } else if ((c & 0xF0) == 0xE0) {
                // 3-byte sequence
                unsigned char b2 = (unsigned char)s[1];
                unsigned char b3 = (unsigned char)s[2];
                if ((b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                    uint32_t cp = ((c & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                    if (cp == 0x200B) {
                        /* zero-width space: drop silently */
                    } else if (supportedCodepoint(cp)) {
                        *d++ = (char)c; *d++ = (char)b2; *d++ = (char)b3;
                    } else { *d++ = '?'; }
                    s += 3;
                } else { *d++ = '?'; s++; }
            } else if ((c & 0xF8) == 0xF0) {
                // 4-byte sequence: unsupported range
                *d++ = '?'; s += 4;
            } else {
                s++;  // invalid byte: skip
            }
        }
        *d = '\0';
        strncpy(history[historyCount].text, tmp, 2047);
        history[historyCount].text[2047] = '\0';
    }
```

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Replace transliteration with UTF-8 passthrough filter in addMessage()"
```

---

## Task 6: Build and Verify

**Step 1: Build**

```bash
cd /home/alun/esp32/cyd28/SLUG-copy-unicode-font
pio run
```

Expected: compiles cleanly, no errors. Warnings about unused variables from the old transliteration code are fine if any remnants remain — fix them.

If you see `error: 'DejaVuSansBold12px' was not declared`: you missed a `setFreeFont` call site — search for remaining ones:
```bash
grep -n "setFreeFont\|setTextFont" src/main.cpp
```
Replace any remaining `setFreeFont(&DejaVuSansBold12px)` with `fontOn()` and `setTextFont(1)` with `fontOff()`.

If you see linker errors about `DejaVuSansBold12pxData`: confirm the header now exports `const uint8_t DejaVuSansBold12pxData[]` (not `GFXfont DejaVuSansBold12px`).

**Step 2: Flash and manual test**

```bash
pio run --target upload && pio device monitor
```

Test sequence:
1. Send a message that gets a response with accented characters (ask "translate hello to French")
2. Verify `é`, `à`, `ç` etc. render correctly instead of `?`
3. Toggle large font mode (key L at boot or however it's activated) and verify both 8px and 12px modes show Unicode correctly
4. Check bullet points and em dashes if the AI response includes them

**Step 3: Final commit**

```bash
git add src/main.cpp
git commit -m "040326 Unicode font: VLW smooth fonts, Latin Extended + special chars, remove transliteration"
```
