# Session Handoff Notes

## Current State (v1.4-dev — March 11, 2026)

**Branch:** `main`
**Status:** Firmware builds and flashes. Issues 2-5 from v1.4 plan implemented and verified. Cloud upload (Issue 1) still failing — major progress on diagnosis but not yet resolved.

### What Was Done This Session

#### Issue 1: Cloud Upload (IN PROGRESS — NOT RESOLVED)
- **Root cause diagnosed**: `HTTPClient::setTimeout()` takes `uint16_t` (max 65535). Passing `90000` overflowed to `24464` (~25s) — this was the true timeout, not the intended 90s.
- **Fix applied**: Changed to `http->setTimeout(60000)` (60s, fits uint16_t). Also set `client->setTimeout(60000)` on WiFiClientSecure (takes ms on ESP32 v3.x).
- **Result**: Timeout now correctly at 60s, but Google Apps Script STILL doesn't respond. Data is fully sent (confirmed via raw WiFiClientSecure diagnostic build — 20KB sent in <1s), but no HTTP response comes back.
- **Raw WiFiClientSecure attempt crashed**: `assert failed: udp_new_ip_type udp.c:1278 (Required to lock TCPIP core functionality!)` — lwIP threading issue. Must use HTTPClient which handles TCPIP locking internally.
- **Added**: User-Agent (`ESP32-MongoFlo/1.4`), Accept (`*/*`) headers, `http->setReuse(false)`, bulk file read instead of char-by-char.
- **Curl from Mac works fine** with the same endpoint — returns 302 instantly. Something about the ESP32's TLS/HTTP request causes Google to never respond.

**Next steps for cloud upload:**
1. Compare exact bytes-on-wire between curl and ESP32 (tcpdump/Wireshark on local network)
2. Try `HTTPC_FORCE_FOLLOW_REDIRECTS` with the corrected 60s timeout (previously failed at 25s due to overflow — may work now with enough time for two TLS handshakes)
3. Check if `NetworkClient::setTimeout(60000)` actually propagates to socket SO_RCVTIMEO — the HTTPClient `connect()` calls `_client->setTimeout(_tcpTimeout)` which may override our pre-set value
4. Consider Google Apps Script v2 URL format or alternative endpoint

#### Issue 2: Scale Weight Off by ~5g (DONE)
- Added `_preTareWeight` to Session — tracks last sub-threshold weight as baseline
- `_startSession()` now tares from `_preTareWeight` instead of `_lastWeight` (which was ≥5g, causing double-tare)
- `forceStart()` sets `_preTareWeight = _lastWeight` before starting

#### Issue 3: Horizontal Static Lines on Display (DONE)
- Increased bounce buffer from `800 * 10` to `800 * 20` in `display.cpp`

#### Issue 4: Negative Weight Handling (DONE)
- Changed error sentinel in `ble_acaia.cpp` from `-1.0f` to `NAN` (with `#include <cmath>`)
- Guard changed from `weight < 0.0f` to `isnan(weight)` — real negative weights now pass through
- Session ends when `relativeWeight < -2.0f` (weight removed after tare)

#### Issue 5: Manual Mode (DONE)
- **session.h/cpp**: Added `_manualMode` bool + `setManualMode()`/`manualMode()`. IDLE weight processing skips auto-start when manual mode is on.
- **ui.h/cpp**: Added `ui_set_manual_mode_cb()`, `ui_set_manual_mode()`, `ui_get_manual_mode()`. Toggle card on BOOT screen at y=360 with `LV_OBJ_FLAG_USER_4`. READY screen shows "Manual Mode Active / Tap anywhere to begin" when enabled.
- **main.cpp**: `onUiManualMode()` callback wired in `setup()`.

#### Other Changes
- **Immediate upload after session end**: main.cpp session-end handler now attempts upload right away before user navigates.
- **Batch upload loop**: `processPendingUploads()` no longer breaks after first file — continues until WiFi failure.
- **WiFi lifecycle**: WiFi disconnect moved to caller (after batch uploads complete) instead of per-upload.

### Key Discovery: HTTPClient uint16_t Overflow

```
HTTPClient::setTimeout(uint16_t timeout);  // MAX 65535!
// Passing 90000 → 90000 % 65536 = 24464 → ~25s actual timeout
// This was the root cause of ALL previous -11 timeout failures
```

`NetworkClient::setTimeout()` takes milliseconds (not seconds). Default is 3000ms.

### Files Modified
- `src/session.cpp` — cloud upload (HTTPClient + timeout fix + User-Agent), weight tare fix, manual mode
- `src/session.h` — `_preTareWeight`, `_manualMode` members
- `src/main.cpp` — immediate upload, batch loop fix, manual mode callback
- `src/ui.cpp` — manual mode toggle card, READY screen text
- `src/ui.h` — manual mode API declarations
- `src/display.cpp` — bounce buffer 10→20
- `src/ble_acaia.cpp` — NAN sentinel for decode errors
- `src/config.h` — (read only, contains CLOUD_UPLOAD_URL)

### Current Known Issues

1. **Cloud upload not working** — Google doesn't respond to ESP32's HTTPS POST within 60s. Curl works from Mac. See "Next steps" above.
2. **BLE Export Mode Crash** — Pre-existing, NimBLE double-init issue.
3. **BLE Disconnection Crash** — Pre-existing LoadProhibited.

---

## Architecture Reference

### State Machines
- **UIState** (ui.h): `BOOT → READY → ACTIVE → SUCCESS/SYNCING/ERROR`
- **Session::State** (session.h): `IDLE → ACTIVE → ENDED → UPLOAD → WAITING`
- **AppState** (main.h): `BOOT → BLE_SCANNING → BLE_CONNECTING → CONNECTED_IDLE → SESSION_ACTIVE → SESSION_END`

### Key Constants (config.h)
| Constant | Value | Purpose |
|---|---|---|
| `SESSION_START_THRESHOLD_G` | 5.0g | Auto-start measurement (was 50g, lowered Mar 9) |
| `SESSION_END_ZERO_G` | 1.0g | Weight "removed" threshold |
| `WEIGHT_REMOVAL_TIMEOUT_MS` | 5000ms | Sustained zero → end session |
| `SESSION_TIMEOUT_MS` | 45000ms | No data → end session |
| `SESSION_MIN_DURATION_MS` | 3000ms | Discard sessions shorter than this |
| `HEARTBEAT_INTERVAL_MS` | 9000ms | BLE heartbeat to keep Acaia connection alive |
| `LOG_FLUSH_INTERVAL_MS` | 2000ms | Buffered CSV write flush period |

### UI Callback System (ui.cpp)
- `LV_OBJ_FLAG_USER_1` → `on_home_clicked()` (WiFi card on BOOT, home on other screens)
- `LV_OBJ_FLAG_USER_2` → `on_start_clicked()` (Export card on BOOT, start on READY)
- `LV_OBJ_FLAG_USER_3` → `on_end_clicked()` (END button on ACTIVE screen)
- `LV_OBJ_FLAG_USER_4` → Manual mode toggle (BOOT screen)
- Callbacks registered via `ui_set_home_cb()`, `ui_set_start_cb()`, `ui_set_end_cb()`, `ui_set_manual_mode_cb()`

### Thread Safety
- BLE callbacks run on separate RTOS task
- Weight passed via `std::atomic<float> _pendingWeight` / `std::atomic<bool> _newWeightPending`
- Never call LVGL or blocking I/O from BLE callbacks
- **Raw WiFiClientSecure::connect() crashes** without TCPIP core lock — must use HTTPClient wrapper

---

## Previous Session Notes (v1.0–v1.3)

- GPIO 2 is R1 (RGB data), not backlight — controlled via CH422G IO expander
- Large network objects (`HTTPClient`, `WiFiClientSecure`) must be heap-allocated (not static) to avoid stale TLS state
- `lv_tick_inc` configured via `LV_TICK_CUSTOM` in lv_conf.h
- Post-session screen uses `LV_OPA_COVER` to fully hide chart underneath
