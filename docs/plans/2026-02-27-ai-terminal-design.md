# AI Terminal — Design Document
_2026-02-27_

## Overview

A self-contained AI terminal for the CYD28 (ESP32-2432S028R). The user types
prompts via a touchscreen QWERTY keyboard, which can be shown or hidden. The
device connects to WiFi, authenticates with Google Vertex AI using a service
account, and displays a scrollable multi-turn conversation history.

---

## 1. Screen Layout

**Display:** 320×240 ILI9341, landscape, USB connector on right.

### KB shown
```
┌────────────────────────────────────────┐  y=0
│   Conversation history (scrollable)    │  120px
├──────────────────────────────────┬─────┤  y=120
│ > typed text here...             │Send │  20px  input bar
├──────────────────────────────────┴─────┤  y=140
│ Q  W  E  R  T  Y  U  I  O  P  [Hide] │  24px
│  A  S  D  F  G  H  J  K  L  [⌫]      │  24px
│   Z  X  C  V  B  N  M                 │  24px
│ [    SPACE    ]        [CLR]           │  28px
└────────────────────────────────────────┘  y=240
```

### KB hidden
```
┌────────────────────────────────────────┐  y=0
│   Conversation history (scrollable)    │  210px
├──────────────────────────────────┬─────┤  y=210
│ > typed text here...         [Show KB]│  30px
└────────────────────────────────────────┘  y=240
```

**Colours:**
- Background: black
- User messages: cyan
- AI messages: yellow
- Key faces: dark grey, white labels
- Error lines: red

**Scroll:** 10px-wide up/down tap zones on the left edge of the history area.

**Thinking indicator:** Flashing dot in the input bar while awaiting API response.

---

## 2. Keyboard & Input

**Key grid** (4 rows, 28×24px keys, 2px gap):

| Row | y   | Keys |
|-----|-----|------|
| 1   | 140 | Q W E R T Y U I O P  +  [Hide KB] |
| 2   | 166 | A S D F G H J K L  +  [⌫] |
| 3   | 190 | Z X C V B N M |
| 4   | 214 | [SPACE (wide)]  [CLR] |

- **Input buffer:** `char input[128]`
- **[⌫]:** removes last character
- **[CLR]:** clears entire buffer
- **[Send] / Send button in input bar:** submits prompt
- **Touch debounce:** 250ms lockout after each key press

---

## 3. Authentication (Vertex AI / OAuth2)

The ESP32 obtains a Google OAuth2 access token using a service account JWT,
then caches it in RAM.

### Startup
- `configTime()` syncs via NTP (`pool.ntp.org`) — required for valid JWT `iat`/`exp`

### Token acquisition
1. Build JWT: base64url(`header`) + `.` + base64url(`payload`)
2. Sign with RS256 using `mbedtls_pk_sign()` (mbedTLS built into ESP32 Arduino)
3. Append base64url signature as third segment
4. POST JWT to `https://oauth2.googleapis.com/token`
5. Parse `access_token` + `expires_in` from JSON response (ArduinoJson)
6. Cache token + expiry timestamp in RAM

Token is refreshed when within 5 minutes of expiry.

### Credential constants (top of main.cpp)
```cpp
const char* SA_EMAIL       = "my-sa@project.iam.gserviceaccount.com";
const char* SA_PRIVATE_KEY = "-----BEGIN RSA PRIVATE KEY-----\n...";
const char* GCP_PROJECT    = "my-project-id";
const char* GCP_REGION     = "us-central1";
```

**Libraries used:** `mbedtls/pk.h`, `mbedtls/md.h`, `mbedtls/entropy.h`,
`mbedtls/ctr_drbg.h` — all built into the ESP32 Arduino framework.

---

## 4. API Calls & Conversation Management

### Conversation history buffer
```cpp
struct Message { bool isUser; char text[512]; };
Message history[20];
int historyCount = 0;
```
Cap: 20 messages. When full, oldest user+AI pair is dropped before adding new one.

### Request format (Vertex AI generateContent)
```json
{
  "system_instruction": {
    "parts": [{"text": "Respond in 150 words or fewer."}]
  },
  "contents": [
    {"role": "user",  "parts": [{"text": "..."}]},
    {"role": "model", "parts": [{"text": "..."}]},
    {"role": "user",  "parts": [{"text": "current prompt"}]}
  ]
}
```

### Endpoint
```
POST https://{GCP_REGION}-aiplatform.googleapis.com/v1/projects/{GCP_PROJECT}
     /locations/{GCP_REGION}/publishers/google/models/gemini-2.0-flash:generateContent
Authorization: Bearer {access_token}
Content-Type: application/json
```

### Response parsing
Extract `candidates[0].content.parts[0].text` via ArduinoJson (8 KB document).

### Display scroll
- History word-wrapped into `lines[]` char array
- `scrollOffset` int tracks lines scrolled up
- Auto-scroll to bottom after each new message

### Error handling
If WiFi is down, token fetch fails, or HTTP status ≠ 200 — append a red error
line to history, remain on current screen.

---

## 5. Architecture

- **Single file:** `src/main.cpp`
- **Network calls:** blocking (`WiFiClientSecure`) — UI shows "Thinking..." during wait
- **WiFi credentials:** hardcoded constants in `main.cpp`

### Key libraries (platformio.ini)
```
bodmer/TFT_eSPI@2.5.43
https://github.com/PaulStoffregen/XPT2046_Touchscreen.git
bblanchon/ArduinoJson@^7.0.0
```
mbedTLS is built-in; no extra entry needed.
