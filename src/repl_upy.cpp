// repl_upy.cpp — MicroPython REPL for e-paper target
// 180426 Rework refresh: full on entry, partial input-bar on keypress, full-screen partial on Enter
// 180426 Initial: line-editor UI + micropython-embed hooks (guarded by MICROPYTHON_EMBED)
//
// To enable real MicroPython:
//   1. Copy ports/embed/ from the MicroPython repo into lib/micropython-embed/
//   2. Run micropython/py/makeversionhdr.py and makemoduledefs.py to generate headers
//   3. Run makeqstrdefs.py / makecompressedqstr.py to produce qstr data
//   4. Create lib/micropython-embed/mpconfigport.h (see template below)
//   5. Add -DMICROPYTHON_EMBED to build_flags in platformio.ini [env:epaper]
//   6. Add lib/micropython-embed/embed/*.c to build_src_filter or extra_scripts

#ifdef TARGET_EPAPER

#include <Arduino.h>
#include "hal.h"
#include "display_epaper.h"

#ifdef MICROPYTHON_EMBED
#  include "micropython_embed.h"
#  define UPYHEAP_SIZE  (48 * 1024)
static uint8_t upyHeap[UPYHEAP_SIZE];
static bool    upyInited = false;
#endif

extern EpaperDisplay tft;

// --- Layout constants --------------------------------------------------
static constexpr int REPL_LINE_H   = FONT_LINE_H;
static constexpr int REPL_MAX_ROWS = EPD_HEIGHT / REPL_LINE_H;
static constexpr int REPL_MAX_COLS = EPD_WIDTH  / FONT_TXT_W;
static constexpr int REPL_BUF_ROWS = 64;
static constexpr int REPL_INPUT_Y  = (REPL_MAX_ROWS - 1) * REPL_LINE_H;

// --- Scrollback buffer -------------------------------------------------
static char  replBuf[REPL_BUF_ROWS][REPL_MAX_COLS + 1];
static int   replHead  = 0;
static int   replCount = 0;

static void replClear() {
    replHead  = 0;
    replCount = 0;
    memset(replBuf, 0, sizeof(replBuf));
}

static void replPushRow(const char* txt) {
    int idx = (replHead + replCount) % REPL_BUF_ROWS;
    strlcpy(replBuf[idx], txt, sizeof(replBuf[0]));
    if (replCount < REPL_BUF_ROWS) replCount++;
    else                            replHead = (replHead + 1) % REPL_BUF_ROWS;
}

// --- Output helpers ---------------------------------------------------
static char   outLineBuf[REPL_MAX_COLS + 1];
static int    outLineLen = 0;

static void replFlushLine() {
    outLineBuf[outLineLen] = '\0';
    replPushRow(outLineBuf);
    outLineLen = 0;
}

static void replPutchar(char c) {
    if (c == '\n' || outLineLen >= REPL_MAX_COLS) replFlushLine();
    else outLineBuf[outLineLen++] = c;
}

static void replPuts(const char* s) {
    while (*s) replPutchar(*s++);
    if (outLineLen > 0) replFlushLine();
}

#ifdef MICROPYTHON_EMBED
int mp_hal_stdout_tx_strn(const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) replPutchar(str[i]);
    return len;
}
#endif

// --- Draw helpers ------------------------------------------------------

static void drawScrollback() {
    tft.u8g2.setFont(EPD_FONT);
    tft.u8g2.setForegroundColor(EPD_C_BLACK);
    tft.u8g2.setBackgroundColor(EPD_C_WHITE);
    int visRows  = REPL_MAX_ROWS - 1;
    int startRow = (replCount > visRows) ? replCount - visRows : 0;
    for (int r = 0; r < visRows; r++) {
        int y = r * REPL_LINE_H;
        if ((startRow + r) < replCount) {
            int bufIdx = (replHead + startRow + r) % REPL_BUF_ROWS;
            tft.u8g2.setCursor(0, y + EPD_FONT_ASCENT);
            tft.u8g2.print(replBuf[bufIdx]);
        }
    }
}

static void drawInputBar(const char* inputLine) {
    tft.epd.fillRect(0, REPL_INPUT_Y, EPD_WIDTH, REPL_LINE_H, EPD_C_BLACK);
    tft.u8g2.setFont(EPD_FONT);
    tft.u8g2.setForegroundColor(EPD_C_WHITE);
    tft.u8g2.setBackgroundColor(EPD_C_BLACK);
    tft.u8g2.setCursor(0, REPL_INPUT_Y + EPD_FONT_ASCENT);
    tft.u8g2.print(">>> ");
    tft.u8g2.print(inputLine);
}

// Full-screen redraw (initial or after new scrollback line).
static void replRedrawFull(const char* inputLine, bool useFullRefresh) {
    if (useFullRefresh) tft.beginFrame();
    else                tft.beginPartialFrame(0, 0, EPD_WIDTH, EPD_HEIGHT);
    tft.epd.fillScreen(EPD_C_WHITE);
    drawScrollback();
    drawInputBar(inputLine);
    tft.endFrame();
}

// Fast update: just the input bar row (partial, ~50 ms).
static void replRedrawInput(const char* inputLine) {
    tft.beginPartialFrame(0, REPL_INPUT_Y, EPD_WIDTH, REPL_LINE_H);
    tft.epd.fillRect(0, REPL_INPUT_Y, EPD_WIDTH, REPL_LINE_H, EPD_C_BLACK);
    tft.u8g2.setFont(EPD_FONT);
    tft.u8g2.setForegroundColor(EPD_C_WHITE);
    tft.u8g2.setBackgroundColor(EPD_C_BLACK);
    tft.u8g2.setCursor(0, REPL_INPUT_Y + EPD_FONT_ASCENT);
    tft.u8g2.print(">>> ");
    tft.u8g2.print(inputLine);
    tft.endFrame();
}

// --- Main REPL entry point --------------------------------------------
void runMicroPythonRepl() {
#ifdef MICROPYTHON_EMBED
    if (!upyInited) {
        mp_embed_init(upyHeap, UPYHEAP_SIZE);
        upyInited = true;
    }
#endif

    replClear();
    outLineLen = 0;

#ifdef MICROPYTHON_EMBED
    replPuts("MicroPython " MICROPY_VERSION_STRING " on ESP32-C3");
#else
    replPuts("MicroPython (stub) — MICROPYTHON_EMBED not defined");
    replPuts("See repl_upy.cpp header to enable real MicroPython.");
#endif
    replPuts("Ctrl+D to exit.");

    char inputLine[REPL_MAX_COLS + 1] = {};
    int  inputLen  = 0;
    int  cursorPos = 0;

    // Full refresh on entry to clear model-menu ghosting cleanly.
    replRedrawFull(inputLine, /*useFullRefresh=*/true);

    while (true) {
        InputEvent ev;
        if (!halPollInput(&ev)) { delay(10); continue; }

        if (ev.type == INPUT_CTRL_D) break;

        if (ev.type == INPUT_CHAR && ev.ch >= 0x20 && inputLen < REPL_MAX_COLS) {
            memmove(inputLine + cursorPos + 1, inputLine + cursorPos,
                    inputLen - cursorPos + 1);
            inputLine[cursorPos] = ev.ch;
            inputLen++; cursorPos++;
            replRedrawInput(inputLine);

        } else if (ev.type == INPUT_BACKSPACE && cursorPos > 0) {
            memmove(inputLine + cursorPos - 1, inputLine + cursorPos,
                    inputLen - cursorPos + 1);
            inputLen--; cursorPos--;
            replRedrawInput(inputLine);

        } else if (ev.type == INPUT_DELETE && cursorPos < inputLen) {
            memmove(inputLine + cursorPos, inputLine + cursorPos + 1,
                    inputLen - cursorPos);
            inputLen--;
            replRedrawInput(inputLine);

        } else if (ev.type == INPUT_CURSOR_LEFT && cursorPos > 0) {
            cursorPos--;
            replRedrawInput(inputLine);

        } else if (ev.type == INPUT_CURSOR_RIGHT && cursorPos < inputLen) {
            cursorPos++;
            replRedrawInput(inputLine);

        } else if (ev.type == INPUT_ENTER) {
            char promptLine[REPL_MAX_COLS + 5];
            snprintf(promptLine, sizeof(promptLine), ">>> %s", inputLine);
            replPushRow(promptLine);

#ifdef MICROPYTHON_EMBED
            mp_embed_exec_str(inputLine);
            if (outLineLen > 0) replFlushLine();
#else
            if (inputLen > 0)
                replPuts("[MicroPython not built — see repl_upy.cpp]");
#endif
            inputLine[0] = '\0';
            inputLen  = 0;
            cursorPos = 0;
            // Full refresh to guarantee the new scrollback lines are clearly visible.
            replRedrawFull(inputLine, /*useFullRefresh=*/true);
        }
    }
}

#endif // TARGET_EPAPER
