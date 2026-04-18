// repl_upy.cpp — MicroPython REPL for e-paper target
// 180426 Initial: line-editor UI + micropython-embed hooks (guarded by MICROPYTHON_EMBED)
//
// To enable real MicroPython:
//   1. Copy ports/embed/ from the MicroPython repo into lib/micropython-embed/
//   2. Run micropython/py/makeversionhdr.py and makemoduledefs.py to generate headers
//   3. Run makeqstrdefs.py / makecompressedqstr.py to produce qstr data
//   4. Create lib/micropython-embed/mpconfigport.h (see template below)
//   5. Add -DMICROPYTHON_EMBED to build_flags in platformio.ini [env:epaper]
//   6. Add lib/micropython-embed/embed/*.c to build_src_filter or extra_scripts
//
// Without MICROPYTHON_EMBED the REPL UI still works (type/backspace/scroll);
// pressing Enter echoes ">>>" without executing code.

#ifdef TARGET_EPAPER

#include <Arduino.h>
#include "hal.h"
#include "display_epaper.h"

#ifdef MICROPYTHON_EMBED
#  include "micropython_embed.h"   // from lib/micropython-embed/embed/
#  define UPYHEAP_SIZE  (48 * 1024)
static uint8_t upyHeap[UPYHEAP_SIZE];
static bool    upyInited = false;
#endif

extern EpaperDisplay tft;

// --- Layout constants --------------------------------------------------
static constexpr int REPL_LINE_H   = FONT_LINE_H;
static constexpr int REPL_MAX_ROWS = EPD_HEIGHT / REPL_LINE_H;   // how many rows fit
static constexpr int REPL_MAX_COLS = EPD_WIDTH  / FONT_TXT_W;    // approx chars per row
static constexpr int REPL_BUF_ROWS = 64;                         // scrollback ring buffer

// --- Scrollback buffer -------------------------------------------------
static char  replBuf[REPL_BUF_ROWS][REPL_MAX_COLS + 1];
static int   replHead  = 0;   // ring: index of oldest row
static int   replCount = 0;   // rows populated (0..REPL_BUF_ROWS)

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

// --- Render visible rows to e-paper -----------------------------------
static void replRedraw(const char* inputLine, int cursor) {
    tft.beginPartialFrame(0, 0, EPD_WIDTH, EPD_HEIGHT);
    tft.fillScreen(EPD_C_WHITE);
    tft.u8g2.setFont(EPD_FONT);
    tft.u8g2.setForegroundColor(EPD_C_BLACK);
    tft.u8g2.setBackgroundColor(EPD_C_WHITE);

    // How many rows fit above the input line (reserve 1 row for input)
    int visRows = REPL_MAX_ROWS - 1;
    int startRow = (replCount > visRows) ? replCount - visRows : 0;

    for (int r = 0; r < visRows && (startRow + r) < replCount; r++) {
        int bufIdx = (replHead + startRow + r) % REPL_BUF_ROWS;
        int y = r * REPL_LINE_H;
        tft.u8g2.setCursor(0, y + EPD_FONT_ASCENT);
        tft.u8g2.print(replBuf[bufIdx]);
    }

    // Input row at bottom
    int inputY = (REPL_MAX_ROWS - 1) * REPL_LINE_H;
    tft.epd.fillRect(0, inputY, EPD_WIDTH, REPL_LINE_H, EPD_C_BLACK);
    tft.u8g2.setForegroundColor(EPD_C_WHITE);
    tft.u8g2.setBackgroundColor(EPD_C_BLACK);
    tft.u8g2.setCursor(0, inputY + EPD_FONT_ASCENT);
    tft.u8g2.print(">>> ");
    tft.u8g2.print(inputLine);

    tft.endFrame();
}

// --- Output callback used by MicroPython (and plain echo path) ---------
// Accumulates chars; flushes completed lines to replBuf.
static char   outLineBuf[REPL_MAX_COLS + 1];
static int    outLineLen = 0;

static void replFlushLine() {
    outLineBuf[outLineLen] = '\0';
    replPushRow(outLineBuf);
    outLineLen = 0;
}

static void replPutchar(char c) {
    if (c == '\n' || outLineLen >= REPL_MAX_COLS) {
        replFlushLine();
    } else {
        outLineBuf[outLineLen++] = c;
    }
}

static void replPuts(const char* s) {
    while (*s) replPutchar(*s++);
    if (outLineLen > 0) replFlushLine();
}

#ifdef MICROPYTHON_EMBED
// MicroPython stdout callback — called by the VM for every output char.
int mp_hal_stdout_tx_strn(const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) replPutchar(str[i]);
    return len;
}
#endif

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
    replPuts("See repl_upy.cpp to enable real MicroPython.");
#endif
    replPuts("Type Ctrl+D to exit.");

    char   inputLine[REPL_MAX_COLS + 1] = {};
    int    inputLen = 0;
    int    cursorPos = 0;   // insertion point within inputLine

    replRedraw(inputLine, cursorPos);

    while (true) {
        InputEvent ev;
        if (!halPollInput(&ev)) { delay(10); continue; }

        if (ev.type == INPUT_CTRL_D) break;

        bool redraw = false;

        if (ev.type == INPUT_CHAR && ev.ch >= 0x20 && inputLen < REPL_MAX_COLS) {
            memmove(inputLine + cursorPos + 1, inputLine + cursorPos,
                    inputLen - cursorPos + 1);
            inputLine[cursorPos] = ev.ch;
            inputLen++; cursorPos++;
            redraw = true;
        } else if (ev.type == INPUT_BACKSPACE && cursorPos > 0) {
            memmove(inputLine + cursorPos - 1, inputLine + cursorPos,
                    inputLen - cursorPos + 1);
            inputLen--; cursorPos--;
            redraw = true;
        } else if (ev.type == INPUT_DELETE && cursorPos < inputLen) {
            memmove(inputLine + cursorPos, inputLine + cursorPos + 1,
                    inputLen - cursorPos);
            inputLen--;
            redraw = true;
        } else if (ev.type == INPUT_CURSOR_LEFT && cursorPos > 0) {
            cursorPos--; redraw = true;
        } else if (ev.type == INPUT_CURSOR_RIGHT && cursorPos < inputLen) {
            cursorPos++; redraw = true;
        } else if (ev.type == INPUT_ENTER) {
            // Push the input line to scrollback and execute
            char promptLine[REPL_MAX_COLS + 5];
            snprintf(promptLine, sizeof(promptLine), ">>> %s", inputLine);
            replPushRow(promptLine);

#ifdef MICROPYTHON_EMBED
            mp_embed_exec_str(inputLine);
            if (outLineLen > 0) replFlushLine();
#endif
            inputLine[0] = '\0';
            inputLen  = 0;
            cursorPos = 0;
            redraw = true;
        }

        if (redraw) replRedraw(inputLine, cursorPos);
    }

    // Clear screen before model menu redraws (partial is fine — menu will redraw all content)
    tft.beginPartialFrame(0, 0, EPD_WIDTH, EPD_HEIGHT);
    tft.fillScreen(EPD_C_WHITE);
    tft.endFrame();
}

#endif // TARGET_EPAPER
