#pragma once
// font.h — central font selection for all display code
// Free_Fonts.h: TFT_eSPI GFX Free Font short-name defines (FF1..FF48, FSS9, FSSB9, TT1, etc.)
#ifndef TARGET_EPAPER
#  include "Free_Fonts.h"
#endif
// 240326 Added: #define FONT_BUILTIN_16PX for TFT_eSPI built-in Font 2 (16px, ASCII-only, no flash overhead)
// 240326 Added: #define FONT_18PX to switch between 12px (default) and 18px smooth font system-wide
//
// Uncomment exactly one (or none for 12px default):
//   FONT_BUILTIN_16PX — TFT_eSPI built-in Font 2, ~16px, ASCII only, no extra flash
//   FONT_18PX         — DejaVuSansBold 18px smooth, Unicode, ~54 KB extra flash
//   (neither)         — DejaVuSansBold 12px smooth, Unicode (default)
//#define FONT_BUILTIN_16PX
//#define FONT_18PX

#ifdef TARGET_P3
// P3 284×76 landscape: use TFT_eSPI built-in Font 1 (8px, ASCII). Gives ~8 chat lines.
#  define FONT_LINE_H  8
#  define FONT_TXT_H   8
#  define FONT_TXT_W   6
#  define FONT_LOAD(obj)   (obj).setTextFont(1)
#  define FONT_UNLOAD(obj) ((void)0)
#elif defined(FONT_BUILTIN_16PX)
#  define FONT_LINE_H 16
#  define FONT_TXT_H  16
#  define FONT_TXT_W   8
#  define FONT_LOAD(obj)   (obj).setTextFont(2)
#  define FONT_UNLOAD(obj) ((void)0)
#elif defined(FONT_18PX)
#  include "fonts/DejaVuSansBold18px.h"
#  define FONT_DATA   DejaVuSansBold18pxData
#  define FONT_LINE_H 22   // yAdvance from VLW header (0x16)
#  define FONT_TXT_H  22
#  define FONT_TXT_W  12
#  define FONT_LOAD(obj)   (obj).loadFont(FONT_DATA)
#  define FONT_UNLOAD(obj) (obj).unloadFont()
#else
#  include "fonts/DejaVuSansBold12px.h"
#  define FONT_DATA   DejaVuSansBold12pxData
#  define FONT_LINE_H 15   // yAdvance from VLW header (0x0F)
#  define FONT_TXT_H  15
#  define FONT_TXT_W   8
#  define FONT_LOAD(obj)   (obj).loadFont(FONT_DATA)
#  define FONT_UNLOAD(obj) (obj).unloadFont()
#endif
