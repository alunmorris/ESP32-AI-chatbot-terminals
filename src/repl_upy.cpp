// repl_upy.cpp — MicroPython REPL for e-paper target
// 180426 Add built-in mini eval: arithmetic, print(), help, variables, common built-ins
// 180426 Rework refresh: full on entry, partial input-bar on keypress, full refresh on Enter
// 180426 Initial: line-editor UI + micropython-embed hooks (guarded by MICROPYTHON_EMBED)
//
// To enable real MicroPython, add -DMICROPYTHON_EMBED to [env:epaper] build_flags and
// vendor ports/embed/ from the MicroPython repo into lib/micropython-embed/. See header.

#ifdef TARGET_EPAPER

#include <Arduino.h>
#include <math.h>
#include "hal.h"
#include "display_epaper.h"

#ifdef MICROPYTHON_EMBED
#  include "micropython_embed.h"
#  define UPYHEAP_SIZE  (48 * 1024)
static uint8_t upyHeap[UPYHEAP_SIZE];
static bool    upyInited = false;
#endif

extern EpaperDisplay tft;

// --- Layout -----------------------------------------------------------
static constexpr int REPL_LINE_H   = FONT_LINE_H;
static constexpr int REPL_MAX_ROWS = EPD_HEIGHT / REPL_LINE_H;
static constexpr int REPL_MAX_COLS = EPD_WIDTH  / FONT_TXT_W;
static constexpr int REPL_BUF_ROWS = 64;
static constexpr int REPL_INPUT_Y  = (REPL_MAX_ROWS - 1) * REPL_LINE_H;

// --- Scrollback -------------------------------------------------------
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

// --- Output pipeline --------------------------------------------------
static char outLineBuf[REPL_MAX_COLS + 1];
static int  outLineLen = 0;

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

static void replPrintf(const char* fmt, ...) {
    char tmp[REPL_MAX_COLS + 1];
    va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    replPuts(tmp);
}

#ifdef MICROPYTHON_EMBED
int mp_hal_stdout_tx_strn(const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) replPutchar(str[i]);
    return len;
}
#endif

// ======================================================================
// Mini built-in evaluator (used when MICROPYTHON_EMBED is not defined)
// Supports: integer/float arithmetic, variables, print(), help, type(),
//           int(), float(), str(), abs(), len(), bool(), common literals.
// ======================================================================
#ifndef MICROPYTHON_EMBED

#define EVAL_MAX_VARS  16
#define EVAL_STR_LEN   64

struct EvalVar { char name[24]; double val; bool isStr; char str[EVAL_STR_LEN]; };
static EvalVar evalVars[EVAL_MAX_VARS];
static int     evalVarCount = 0;

static EvalVar* evalFindVar(const char* name) {
    for (int i = 0; i < evalVarCount; i++)
        if (strcmp(evalVars[i].name, name) == 0) return &evalVars[i];
    return nullptr;
}

static EvalVar* evalSetVar(const char* name, double val) {
    EvalVar* v = evalFindVar(name);
    if (!v) {
        if (evalVarCount >= EVAL_MAX_VARS) return nullptr;
        v = &evalVars[evalVarCount++];
        strlcpy(v->name, name, sizeof(v->name));
    }
    v->val = val; v->isStr = false; v->str[0] = '\0';
    return v;
}

static EvalVar* evalSetStr(const char* name, const char* s) {
    EvalVar* v = evalFindVar(name);
    if (!v) {
        if (evalVarCount >= EVAL_MAX_VARS) return nullptr;
        v = &evalVars[evalVarCount++];
        strlcpy(v->name, name, sizeof(v->name));
    }
    v->isStr = true; strlcpy(v->str, s, sizeof(v->str)); v->val = 0;
    return v;
}

// --- Tokeniser --------------------------------------------------------
enum TokType { T_NUM, T_STR, T_ID, T_OP, T_LPAREN, T_RPAREN, T_COMMA, T_EQ, T_END, T_ERR };
struct Token { TokType type; double num; char str[EVAL_STR_LEN]; };

static const char* tok_src;
static Token tok_cur;

static void skipSpace() { while (*tok_src == ' ' || *tok_src == '\t') tok_src++; }

static Token nextToken() {
    skipSpace();
    Token t{};
    if (!*tok_src) { t.type = T_END; return t; }

    // String literal
    if (*tok_src == '"' || *tok_src == '\'') {
        char q = *tok_src++;
        int i = 0;
        while (*tok_src && *tok_src != q && i < EVAL_STR_LEN - 1)
            t.str[i++] = *tok_src++;
        t.str[i] = '\0';
        if (*tok_src == q) tok_src++;
        t.type = T_STR; return t;
    }

    // Number
    if (isdigit((uint8_t)*tok_src) || (*tok_src == '.' && isdigit((uint8_t)tok_src[1]))) {
        char* end;
        t.num = strtod(tok_src, &end);
        tok_src = end;
        t.type = T_NUM; return t;
    }

    // Identifier / keyword
    if (isalpha((uint8_t)*tok_src) || *tok_src == '_') {
        int i = 0;
        while ((isalnum((uint8_t)*tok_src) || *tok_src == '_') && i < (int)sizeof(t.str)-1)
            t.str[i++] = *tok_src++;
        t.str[i] = '\0';
        t.type = T_ID; return t;
    }

    // Two-char operators
    char c = *tok_src++;
    if (c == '*' && *tok_src == '*') { tok_src++; t.type = T_OP; t.str[0]='*'; t.str[1]='*'; t.str[2]=0; return t; }
    if (c == '/' && *tok_src == '/') { tok_src++; t.type = T_OP; t.str[0]='/'; t.str[1]='/'; t.str[2]=0; return t; }
    if (c == '=' && *tok_src == '=') { tok_src++; t.type = T_OP; t.str[0]='='; t.str[1]='='; t.str[2]=0; return t; }
    if (c == '!' && *tok_src == '=') { tok_src++; t.type = T_OP; t.str[0]='!'; t.str[1]='='; t.str[2]=0; return t; }
    if (c == '<' && *tok_src == '=') { tok_src++; t.type = T_OP; t.str[0]='<'; t.str[1]='='; t.str[2]=0; return t; }
    if (c == '>' && *tok_src == '=') { tok_src++; t.type = T_OP; t.str[0]='>'; t.str[1]='='; t.str[2]=0; return t; }

    if (c == '(') { t.type = T_LPAREN; return t; }
    if (c == ')') { t.type = T_RPAREN; return t; }
    if (c == ',') { t.type = T_COMMA; return t; }
    if (c == '=') { t.type = T_EQ; return t; }
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
        c == '<' || c == '>') {
        t.type = T_OP; t.str[0] = c; t.str[1] = '\0'; return t;
    }
    t.type = T_ERR; return t;
}

// --- Recursive-descent expression parser ------------------------------
// Forward declarations
struct EvalResult { bool ok; bool isStr; double num; char str[EVAL_STR_LEN]; };
static EvalResult parseExpr();

static EvalResult makeNum(double v)  { EvalResult r{}; r.ok=true; r.isStr=false; r.num=v; return r; }
static EvalResult makeStr(const char* s) { EvalResult r{}; r.ok=true; r.isStr=true; strlcpy(r.str,s,sizeof(r.str)); return r; }
static EvalResult makeErr(const char* m) { EvalResult r{}; r.ok=false; strlcpy(r.str,m,sizeof(r.str)); return r; }

static bool numIsInt(double v) { return v == floor(v) && fabs(v) < 1e15; }

static void fmtNum(double v, char* buf, int len) {
    if (numIsInt(v)) snprintf(buf, len, "%.0f", v);
    else             snprintf(buf, len, "%g",   v);
}

static EvalResult callBuiltin(const char* name) {
    // Peek at next token to handle no-arg calls
    skipSpace();
    bool hasArgs = (*tok_src != ')');
    EvalResult arg{};
    char argStr[EVAL_STR_LEN] = {};
    bool gotArg = false;

    if (hasArgs) {
        arg = parseExpr();
        if (!arg.ok) return arg;
        if (arg.isStr) strlcpy(argStr, arg.str, sizeof(argStr));
        else           fmtNum(arg.num, argStr, sizeof(argStr));
        gotArg = true;
    }
    // consume closing ')'
    skipSpace();
    if (*tok_src == ')') tok_src++;

    if (strcmp(name, "print") == 0) {
        replPuts(gotArg ? argStr : "");
        return makeNum(0);  // None
    }
    if (strcmp(name, "help") == 0) {
        replPuts("Mini REPL: arithmetic, variables, print()");
        replPuts("Operators: + - * / // % ** == != < >");
        replPuts("Built-ins: print abs int float str len bool");
        replPuts("True False None  |  Ctrl+D to exit");
        return makeNum(0);
    }
    if (strcmp(name, "exit") == 0 || strcmp(name, "quit") == 0) {
        replPuts("Press Ctrl+D to exit.");
        return makeNum(0);
    }
    if (strcmp(name, "abs") == 0) {
        if (!gotArg || arg.isStr) return makeErr("TypeError: abs() needs a number");
        return makeNum(fabs(arg.num));
    }
    if (strcmp(name, "int") == 0) {
        if (!gotArg) return makeErr("TypeError: int() needs an argument");
        if (arg.isStr) {
            char* e; double v = strtod(arg.str, &e);
            if (e == arg.str) return makeErr("ValueError: invalid literal for int()");
            return makeNum(trunc(v));
        }
        return makeNum(trunc(arg.num));
    }
    if (strcmp(name, "float") == 0) {
        if (!gotArg) return makeErr("TypeError: float() needs an argument");
        if (arg.isStr) {
            char* e; double v = strtod(arg.str, &e);
            if (e == arg.str) return makeErr("ValueError: could not convert to float");
            return makeNum(v);
        }
        return makeNum(arg.num);
    }
    if (strcmp(name, "str") == 0) {
        if (!gotArg) return makeStr("");
        return makeStr(argStr);
    }
    if (strcmp(name, "bool") == 0) {
        if (!gotArg) return makeNum(0);
        return makeNum((arg.isStr ? arg.str[0] != '\0' : arg.num != 0.0) ? 1.0 : 0.0);
    }
    if (strcmp(name, "len") == 0) {
        if (!gotArg || !arg.isStr) return makeErr("TypeError: len() needs a string");
        return makeNum(strlen(arg.str));
    }
    if (strcmp(name, "type") == 0) {
        if (!gotArg) return makeStr("<class 'NoneType'>");
        char tmp[32]; snprintf(tmp, sizeof(tmp), "<class '%s'>", arg.isStr ? "str" : (numIsInt(arg.num) ? "int" : "float"));
        return makeStr(tmp);
    }
    if (strcmp(name, "round") == 0) {
        if (!gotArg || arg.isStr) return makeErr("TypeError: round() needs a number");
        return makeNum(round(arg.num));
    }
    if (strcmp(name, "max") == 0 || strcmp(name, "min") == 0) {
        // only one-arg form: max(x) = x
        if (!gotArg || arg.isStr) return makeErr("TypeError: needs a number");
        return arg;
    }
    char err[EVAL_STR_LEN]; snprintf(err, sizeof(err), "NameError: name '%s' is not defined", name);
    return makeErr(err);
}

static EvalResult parsePrimary() {
    tok_cur = nextToken();

    if (tok_cur.type == T_NUM) return makeNum(tok_cur.num);

    if (tok_cur.type == T_STR) return makeStr(tok_cur.str);

    if (tok_cur.type == T_LPAREN) {
        EvalResult r = parseExpr();
        skipSpace();
        if (*tok_src == ')') tok_src++;
        return r;
    }

    if (tok_cur.type == T_OP && tok_cur.str[0] == '-') {
        EvalResult r = parsePrimary();
        if (!r.ok || r.isStr) return makeErr("TypeError: unary minus on non-number");
        return makeNum(-r.num);
    }
    if (tok_cur.type == T_OP && tok_cur.str[0] == '+') {
        return parsePrimary();
    }

    if (tok_cur.type == T_ID) {
        // Keywords
        if (strcmp(tok_cur.str, "True")  == 0) return makeNum(1);
        if (strcmp(tok_cur.str, "False") == 0) return makeNum(0);
        if (strcmp(tok_cur.str, "None")  == 0) return makeNum(0);  // treat as 0
        if (strcmp(tok_cur.str, "help")  == 0) {
            skipSpace();
            if (*tok_src == '(') { tok_src++; return callBuiltin("help"); }
            // bare 'help'
            replPuts("Mini REPL: arithmetic, variables, print()");
            replPuts("Operators: + - * / // % ** == != < >");
            replPuts("Built-ins: print abs int float str len bool");
            replPuts("True False None  |  Ctrl+D to exit");
            return makeNum(0);
        }

        // Function call?
        skipSpace();
        if (*tok_src == '(') {
            tok_src++;  // consume '('
            return callBuiltin(tok_cur.str);
        }

        // Variable lookup
        EvalVar* v = evalFindVar(tok_cur.str);
        if (v) return v->isStr ? makeStr(v->str) : makeNum(v->val);

        char err[EVAL_STR_LEN]; snprintf(err, sizeof(err), "NameError: name '%s' is not defined", tok_cur.str);
        return makeErr(err);
    }

    return makeErr("SyntaxError: unexpected token");
}

static EvalResult parsePower() {
    EvalResult base = parsePrimary();
    if (!base.ok) return base;
    skipSpace();
    if (*tok_src == '*' && tok_src[1] == '*') {
        tok_src += 2;
        EvalResult exp = parsePower();  // right-associative
        if (!exp.ok) return exp;
        if (base.isStr || exp.isStr) return makeErr("TypeError: unsupported operand for **");
        return makeNum(pow(base.num, exp.num));
    }
    return base;
}

static EvalResult parseTerm() {
    EvalResult left = parsePower();
    if (!left.ok) return left;
    while (true) {
        skipSpace();
        if (!*tok_src) break;
        char op1 = tok_src[0], op2 = tok_src[1];
        if (op1 == '*' && op2 != '*') { tok_src++; }
        else if (op1 == '/' && op2 == '/') { tok_src += 2; op1 = 'I'; }  // //
        else if (op1 == '/') { tok_src++; }
        else if (op1 == '%') { tok_src++; }
        else break;
        EvalResult right = parsePower();
        if (!right.ok) return right;
        if (left.isStr || right.isStr) return makeErr("TypeError: unsupported operand");
        if (op1 == '*') left = makeNum(left.num * right.num);
        else if (op1 == '/') {
            if (right.num == 0) return makeErr("ZeroDivisionError: division by zero");
            left = makeNum(left.num / right.num);
        }
        else if (op1 == 'I') {
            if (right.num == 0) return makeErr("ZeroDivisionError: division by zero");
            left = makeNum(floor(left.num / right.num));
        }
        else if (op1 == '%') {
            if (right.num == 0) return makeErr("ZeroDivisionError: modulo by zero");
            left = makeNum(fmod(left.num, right.num));
        }
    }
    return left;
}

static EvalResult parseAdd() {
    EvalResult left = parseTerm();
    if (!left.ok) return left;
    while (true) {
        skipSpace();
        if (*tok_src != '+' && *tok_src != '-') break;
        // Don't consume negative sign before a number that's part of the next token
        char op = *tok_src++;
        EvalResult right = parseTerm();
        if (!right.ok) return right;
        if (op == '+') {
            if (left.isStr && right.isStr) {
                char tmp[EVAL_STR_LEN];
                snprintf(tmp, sizeof(tmp), "%s%s", left.str, right.str);
                left = makeStr(tmp);
            } else if (!left.isStr && !right.isStr) {
                left = makeNum(left.num + right.num);
            } else {
                return makeErr("TypeError: can only concatenate str to str");
            }
        } else {
            if (left.isStr || right.isStr) return makeErr("TypeError: unsupported operand for -");
            left = makeNum(left.num - right.num);
        }
    }
    return left;
}

static EvalResult parseCompare() {
    EvalResult left = parseAdd();
    if (!left.ok) return left;
    skipSpace();
    const char* p = tok_src;
    // Two-char comparison operators
    char op1 = p[0], op2 = p[1];
    bool twoChar = (op2 == '=') && (op1 == '=' || op1 == '!' || op1 == '<' || op1 == '>');
    bool oneChar = !twoChar && (op1 == '<' || op1 == '>');
    if (!twoChar && !oneChar) return left;
    tok_src += (twoChar ? 2 : 1);
    EvalResult right = parseAdd();
    if (!right.ok) return right;
    if (left.isStr || right.isStr) return makeErr("TypeError: cannot compare");
    double l = left.num, r = right.num;
    bool res;
    if      (op1=='=' && op2=='=') res = (l == r);
    else if (op1=='!' && op2=='=') res = (l != r);
    else if (op1=='<' && op2=='=') res = (l <= r);
    else if (op1=='>' && op2=='=') res = (l >= r);
    else if (op1=='<') res = (l < r);
    else               res = (l > r);
    return makeNum(res ? 1.0 : 0.0);
}

static EvalResult parseExpr() { return parseCompare(); }

// --- Statement handler ------------------------------------------------
// Returns true if something was printed (caller should not auto-print result).
static bool evalStatement(const char* line) {
    // Strip leading whitespace
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return true;  // blank line, nothing to show

    // Check for assignment: identifier = expr (not ==)
    const char* eq = strchr(line, '=');
    if (eq && eq > line && *(eq+1) != '=' && *(eq-1) != '!' && *(eq-1) != '<' && *(eq-1) != '>') {
        // Extract variable name (must be all alnum/_)
        const char* p = line;
        char varname[24] = {};
        int ni = 0;
        while ((*p == '_' || isalnum((uint8_t)*p)) && ni < 23) varname[ni++] = *p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '=' && *(p+1) != '=') {
            // It's an assignment
            tok_src = p + 1;
            EvalResult r = parseExpr();
            if (!r.ok) { replPuts(r.str); return true; }
            if (r.isStr) evalSetStr(varname, r.str);
            else         evalSetVar(varname, r.num);
            return true;  // assignments print nothing
        }
    }

    // Expression
    tok_src = line;
    EvalResult r = parseExpr();

    // Skip trailing whitespace / check nothing left
    skipSpace();
    bool extraTokens = (*tok_src != '\0');

    if (!r.ok) { replPuts(r.str); return true; }
    if (extraTokens) { replPuts("SyntaxError: unexpected token"); return true; }

    // Print result (like a REPL — don't print None/0 from print() etc.)
    if (!r.isStr) {
        if (r.num == 0 && (strncmp(line, "print", 5) == 0 || strncmp(line, "help", 4) == 0 ||
                           strncmp(line, "exit", 4) == 0 || strncmp(line, "quit", 4) == 0)) {
            return true;  // suppress None from void builtins
        }
        char tmp[32];
        if (r.num == 1.0 && strncmp(line, "True", 4) == 0) { replPuts("True"); return true; }
        if (r.num == 0.0 && strncmp(line, "False", 5) == 0) { replPuts("False"); return true; }
        if (r.num == 1.0) { fmtNum(r.num, tmp, sizeof(tmp));
            if (strcmp(tmp,"1")==0 && strstr(line,"True")) { replPuts("True"); return true; } }
        fmtNum(r.num, tmp, sizeof(tmp));
        replPuts(tmp);
    } else {
        char tmp[EVAL_STR_LEN + 2];
        snprintf(tmp, sizeof(tmp), "'%s'", r.str);
        replPuts(tmp);
    }
    return true;
}

#endif  // !MICROPYTHON_EMBED

// --- Display helpers --------------------------------------------------

static void drawScrollback() {
    tft.u8g2.setFont(EPD_FONT);
    tft.u8g2.setForegroundColor(EPD_C_BLACK);
    tft.u8g2.setBackgroundColor(EPD_C_WHITE);
    int visRows  = REPL_MAX_ROWS - 1;
    int startRow = (replCount > visRows) ? replCount - visRows : 0;
    for (int r = 0; r < visRows; r++) {
        if ((startRow + r) >= replCount) break;
        int bufIdx = (replHead + startRow + r) % REPL_BUF_ROWS;
        tft.u8g2.setCursor(0, r * REPL_LINE_H + EPD_FONT_ASCENT);
        tft.u8g2.print(replBuf[bufIdx]);
    }
}

static void drawReplInputBar(const char* inputLine) {
    tft.epd.fillRect(0, REPL_INPUT_Y, EPD_WIDTH, REPL_LINE_H, EPD_C_BLACK);
    tft.u8g2.setFont(EPD_FONT);
    tft.u8g2.setForegroundColor(EPD_C_WHITE);
    tft.u8g2.setBackgroundColor(EPD_C_BLACK);
    tft.u8g2.setCursor(0, REPL_INPUT_Y + EPD_FONT_ASCENT);
    tft.u8g2.print(">>> ");
    tft.u8g2.print(inputLine);
}

static void replRedrawFull(const char* inputLine, bool useFullRefresh) {
    if (useFullRefresh) tft.beginFrame();
    else                tft.beginPartialFrame(0, 0, EPD_WIDTH, EPD_HEIGHT);
    tft.epd.fillScreen(EPD_C_WHITE);
    drawScrollback();
    drawReplInputBar(inputLine);
    tft.endFrame();
}

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

// --- Main entry point -------------------------------------------------
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
    replPuts("Mini REPL v1  (Ctrl+D exits)");
    replPuts("Type help for built-ins.");
#endif

    char inputLine[REPL_MAX_COLS + 1] = {};
    int  inputLen  = 0;
    int  cursorPos = 0;

    replRedrawFull(inputLine, /*useFullRefresh=*/true);

    while (true) {
        InputEvent ev;
        if (!halPollInput(&ev)) { delay(10); continue; }

        if (ev.type == INPUT_CTRL_D) break;

        if (ev.type == INPUT_CHAR && ev.ch >= 0x20 && inputLen < REPL_MAX_COLS) {
            memmove(inputLine + cursorPos + 1, inputLine + cursorPos, inputLen - cursorPos + 1);
            inputLine[cursorPos] = ev.ch;
            inputLen++; cursorPos++;
            replRedrawInput(inputLine);

        } else if (ev.type == INPUT_BACKSPACE && cursorPos > 0) {
            memmove(inputLine + cursorPos - 1, inputLine + cursorPos, inputLen - cursorPos + 1);
            inputLen--; cursorPos--;
            replRedrawInput(inputLine);

        } else if (ev.type == INPUT_DELETE && cursorPos < inputLen) {
            memmove(inputLine + cursorPos, inputLine + cursorPos + 1, inputLen - cursorPos);
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
            evalStatement(inputLine);
            if (outLineLen > 0) replFlushLine();
#endif
            inputLine[0] = '\0'; inputLen = 0; cursorPos = 0;
            replRedrawFull(inputLine, /*useFullRefresh=*/true);
        }
    }
}

#endif // TARGET_EPAPER
