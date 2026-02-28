# Bold Font (Large Font Mode) Design

**Goal:** Replace Font 2 (16px fixed-width) with FreeSansBold9pt7b (bold proportional GFX font) in large-font mode.

**Scope:** Large-font mode only (`largeFont=true`). Small-font mode (GLCD) unchanged.

**Approach:** Use TFT_eSPI's built-in GFX FreeFont support (`LOAD_GFXFF` already set). No library change.

---

## Font

- `FreeSansBold9pt7b` — bold, proportional, ~12px ascent, yAdvance=16
- Header: `<Fonts/FreeSansBold9pt7b.h>` (part of TFT_eSPI)

## Layout changes

| Constant | Old | New |
|---|---|---|
| `LINE_H_LARGE` | 18 | 16 |
| `SPLASH_H` | 54 | 48 |

- KB-shown history area (116px): 7 lines (was 6)
- KB-hidden history area (210px): 13 lines (was 11)

## Code changes

All `tft.setTextFont(2)` → `tft.setFreeFont(&FreeSansBold9pt7b)`.

All `tft.setCursor(x, y); tft.print(str)` in large-font sections → `tft.drawString(str, x, y)`.
(`drawString` positions y at top of character bounding box for GFX fonts; `print` uses baseline.)

`tft.setTextFont(1)` calls already in place — these unset the GFX font correctly.

Word-wrap in `rebuildLines()` already calls `tft.textWidth()` so proportional widths are handled automatically.

## Files

- Modify: `src/main.cpp`
  - Add `#include <Fonts/FreeSansBold9pt7b.h>`
  - Update `LINE_H_LARGE` and `SPLASH_H` constants
  - Swap font calls in: `rebuildLines()`, `drawHistory()`, `connectWiFi()`, `selectModel()`
