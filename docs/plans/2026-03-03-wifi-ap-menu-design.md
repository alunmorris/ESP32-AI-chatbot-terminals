# WiFi AP Menu — Design

**Goal:** Show a WiFi AP picker when boot connection fails; store up to 9 credentials in NVS so future boots reconnect automatically.

---

## Storage

`Preferences` namespace `"wifi"`:

| Key | Type | Meaning |
|-----|------|---------|
| `"n"` | int | Number of stored credentials (0–9) |
| `"s0"`–`"s8"` | String | SSID for slot N |
| `"p0"`–`"p8"` | String | Password for slot N |

Slot 0 = most recently used. On first boot with `n==0`, seed from existing hardcoded `WIFI_SSID`/`WIFI_PASSWORD` so existing users don't need to re-enter credentials.

When a new successful connection is made: insert at slot 0, shift slots 1–8 down (discard slot 9 if full), increment `n` (cap at 9), persist.

---

## Boot Flow

```
setup()
  connectWiFi()               // load slot 0 from Preferences, try connect (15s)
    success → selectModel()   // normal startup
    fail    → selectAP()
                scan WiFi (WiFi.scanNetworks), sort by RSSI, keep top 9
                draw AP list full-screen (numbered 1–9)
                wait for touch on row 1–9
                  stored password → try connect directly
                  no password     → show keyboard, user types password, Send to submit
                connect success
                  → save to slot 0 (shift others), selectModel()
                connect fail (bad stored password)
                  → show "Failed. (1) Re-enter  (2) New scan"
                  tap 1 → clear stored password, show keyboard again
                  tap 2 → back to scan
```

`connectWiFi()` updated to accept optional SSID+password args (used by `selectAP()`); when called with no args it loads slot 0 from Preferences.

---

## AP List Screen

Full-screen takeover (like `selectModel()`). No keyboard shown.

Layout (320×240, rows 24px high):
```
[scanning... or list]
1  PlusnetWireless_EXT      ████░  -62
2  BT-Hub-5A                ███░░  -71
3  Sky12345                 ██░░░  -80
...
```

- Number: leftmost 16px
- SSID: truncated to ~22 chars (132px @ 6px/char)
- Signal bars: 4 chars right-side, derived from RSSI bands
- dBm value: rightmost ~30px
- Touch hit-test: full row height (24px), full width

---

## Password Entry

Reuse existing keyboard layout (history + input bar + keyboard). History area shows prompt: `"Password for: <SSID>"`. User types with existing KB. Send submits.

---

## Failed Password Handling

After a failed connect attempt with a stored password, show two-option prompt (full-screen, same style as AP list):

```
Connect failed for: <SSID>
1  Re-enter password
2  New scan
```

- Tap 1: clear stored password for that slot, show keyboard for fresh entry
- Tap 2: run a new scan and redisplay AP list
