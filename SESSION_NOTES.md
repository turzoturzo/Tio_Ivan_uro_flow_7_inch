# Session Handoff Notes

## Current State (v1.5-dev — March 11, 2026)

**Branch:** `main`
**Status:** Firmware builds/flashes successfully. Cloud upload pipeline fully working end-to-end: ESP32 → Cloudflare Worker → Google Apps Script → Google Sheets. All 92 historical CSV files uploaded successfully (~620ms each). UI tearing partially improved but still present during ACTIVE screen.

### What Was Done Today (March 11)

#### Cloud Upload — FIXED ✅
- **Root cause:** ESP32 mbedTLS cannot receive HTTP responses from Google's servers (`script.google.com`). TLS handshake succeeds, data sends, but Google's 302 redirect response never arrives.
- **Solution:** Cloudflare Worker relay at `mongoflo-relay.black-heart-3a5a.workers.dev`
  - ESP32 POSTs CSV to Worker (~620ms for typical files)
  - Worker responds immediately with `{"status":"ok"}`
  - Worker forwards to Google Apps Script in background via `ctx.waitUntil()`
  - Had to manually follow Google's 302 redirect (HTTP spec converts POST→GET on redirect, losing body)
  - Worker uses `redirect: "manual"` then re-POSTs to the Location URL
- **Files:** `cloud-relay/src/index.js`, `cloud-relay/wrangler.toml`
- **ESP32 changes:** `src/config.h` (CLOUD_UPLOAD_URL → Worker), `src/session.cpp` (timeouts reduced to 15s, explicit host/port/path for HTTPClient)

#### Upload Queue — FIXED ✅
- **Problem 1:** Old code limited to ONE upload per boot ("keep BLE connection fast") — removed that limit since Worker responds in <1s
- **Problem 2:** Boot upload called `gBle.pauseForWifi()` before BLE was initialized — caused NimBLE crash and WiFi failure
- **Problem 3:** Sync queue (`/sync_queue.txt`) was empty — files existed on FFat but were never queued
- **Fix:** Inlined boot upload logic (no more `pauseForWifi` before BLE init), added filesystem scan to rebuild queue from CSV files on disk when queue is empty, drain all pending files in single boot
- **Result:** 64 files uploaded in one boot cycle, remaining 28 will drain on next boot (64-item queue limit)

#### Google Apps Script — Redeployed
- Previous deployment URL was returning 404
- User redeployed via Apps Script editor with Execute as: Me, Access: Anyone
- New deployment ID in `wrangler.toml`

### OPEN: UI Display Tearing 🔴
- **Symptom:** Text ghosting, duplicate elements, offset labels during ACTIVE screen (weight updates + chart drawing). Partially improved on boot/idle screens.
- **Root cause:** PSRAM bandwidth contention between LVGL draw buffer writes and RGB panel DMA reads. The `full_refresh = 1` in `display.cpp` line 242 was disabled (set to 0) which helped static screens, but ACTIVE screen still tears during rapid updates.
- **What's been tried:** Disabled `full_refresh`. Improved but not eliminated.
- **Next steps to try:**
  1. Move LVGL draw buffers from PSRAM to internal SRAM (reduces PSRAM bus contention)
  2. Reduce draw buffer size to fit in internal SRAM (e.g., 800×10 instead of 800×48)
  3. Use LVGL direct mode instead of double-buffered
  4. Throttle ACTIVE screen update rate (currently 200ms / 5Hz — try 500ms / 2Hz)
  5. Reduce chart point count or simplify ACTIVE screen layout
- **Key file:** `src/display.cpp` — draw buffer allocation ~line 220, flush callback, bounce buffer config

### Previous Known Issues (Still Open)
1. **Qmax spike bug** — Flow rate computation produces unrealistic peaks. Needs physiological cap or 1-second windowed calculation.
2. **BLE disconnection crash** — `LoadProhibited` on unexpected disconnect.
3. **BLE Export Mode crash** — NimBLE re-init assert failure.

### What Was Done Today (March 9)

#### Firmware Changes
- **Boot screen UX overhaul** — Replaced misleading 10-second countdown with indeterminate scanning animation (LVGL `lv_anim_t` sweeping bar). Shows descriptive status: "Scanning for scale...", "Connecting...", "Scale Connected!" with green text on success.
- **Lowered SESSION_START_THRESHOLD_G from 50g to 5g** — The 50g threshold caused session tare to capture 50g+ as baseline, losing initial volume. At 5g the tare matches actual container weight (~5g), so NET weight accurately reflects voided volume.
- **Weight label "CURRENT" → "NET"** on ACTIVE screen to clarify tare-adjusted display.

#### Dashboard Changes (docs/index.html)
- **Device name tags** — Derived from filename prefix via `DEVICE_MAP` (`pearls_` → "Mongo-Pearl-1"). Shown as green badges on session cards and in session detail header.
- **Search bar** — Full-text search across date, time, device name, voided volume, Qmax, and filename. Supports multi-word queries with live filtering. Auto-hides empty date group headers.
- **Session header** — Shows full date and device name in session detail view.
- **Dynamic header meta** — Lists unique connected devices.

### Serial Log Observations (March 9, live capture)

```
Boot sequence:
  WiFi connects → NTP syncs → 18 pending uploads → upload attempt → -11 timeout
  BLE scan finds PEARLS50C7DA immediately
  BLE connect fails 3-7 times before succeeding (radio contention after WiFi)
  Scale connected → weight notifications flowing

Session captures:
  pearls_20260309_170410.csv — 573.2mL, Qmax=347.56 mL/s, Qave=24.02, 23.9s
  pearls_20260309_170448.csv — 307.6mL, Qmax=182.95 mL/s, Qave=15.01, 20.5s
```

### Current Known Issues (Priority Order)

1. **CRITICAL: Qmax spike bug** — Flow rate computation produces unrealistic peaks (180-350 mL/s vs normal 15-40 mL/s). Root cause: instantaneous flow rate from single BLE sample deltas (~150ms apart) creates spikes when liquid hits scale suddenly. The 4-sample moving average is insufficient. **Fix needed:** Cap max physiological flow rate at ~50 mL/s, and/or use 1-second time windows instead of per-sample deltas for flow rate calculation. File: `session.cpp` lines 118-143.

2. **Cloud upload -11 timeout** — Every upload attempt fails with HTTP code -11 (read timeout). TLS handshake to Google Apps Script (`script.google.com`) times out on ESP32. 18 files queued and growing. The `WiFiClientSecure` with `setInsecure()` and 45s timeout isn't enough. Possible fixes: increase timeout further, use chunked transfer, try HTTP (non-TLS) proxy, or switch to a simpler endpoint (e.g., Google Forms POST).

3. **BLE connect takes many retries after WiFi** — Scale is found by scan immediately but `_connectTo()` fails 3-7 times before connecting. Likely WiFi/BLE radio contention — the ESP32-S3 shares the 2.4GHz radio. The upload attempt runs WiFi right before BLE connect. Adding a longer delay between WiFi teardown and BLE connect may help.

4. **LVGL graphics corruption** — Still reproducible around screen transitions (ACTIVE → SUCCESS). Symptoms: ghosting, duplicate layers.

5. **BLE disconnection crash** — Pre-existing `LoadProhibited` on unexpected disconnect.

### Recommended Next Steps

1. **Fix Qmax computation** — In `session.cpp::_processWeight()`:
   - Add `if (instantFlowRate > 50.0f) instantFlowRate = 50.0f;` as a physiological cap
   - Or compute flow rate over 1-second sliding windows instead of per-sample
   - Consider ignoring the first 2-3 samples after session start (tare settling)

2. **Debug cloud upload** — Add detailed TLS handshake logging:
   ```cpp
   client->setHandshakeTimeout(30); // separate from HTTP timeout
   ```
   Try connecting to a simpler HTTPS endpoint first (e.g., httpbin.org) to isolate if it's Google-specific. Consider using HTTP POST to a non-TLS endpoint as fallback.

3. **BLE/WiFi radio handoff** — After `WiFi.mode(WIFI_OFF)`, add `delay(500)` before BLE scan resumes to let the radio fully settle.

4. **Verify 5g threshold in field** — The new 5g start threshold means sessions start as soon as ~5g is on the scale. Verify this doesn't cause false starts from vibration or accidental touches. May need to add a debounce (e.g., 3 consecutive readings above 5g).

### What Was Done March 8 (Previous Session)

- Added flow rate computation (smoothed derivative of cumulative weight, 4-sample MA)
- Added clinical summary END row in CSV with voided_vol, Qmax, Qave, TQmax, duration
- Added `flow_rate_ml_s` column to CSV output
- Renamed device from "PearlsLogger" to "Mongo-Pearl-1"
- Fixed upload loop bug (break after first attempt instead of retrying infinitely)
- Built MongoFlo urologist web dashboard (docs/index.html)
- Enabled GitHub Pages from docs/ folder
- Fixed boot BLE/WiFi deadlock, config.h WiFi credentials, upload processing, session WiFi check

## Current State (v1.3 — March 7, 2026)

**Branch:** `main` at commit `efa34a4`
**Firmware:** Builds and flashes successfully. BOOT and READY screens verified on hardware.

### What Was Done This Session

#### v1.2 → v1.3 Measurement UX Overhaul
- **Auto-start threshold**: Raised from 5g to 50g (`SESSION_START_THRESHOLD_G`). Prevents false starts from placing hands near scale.
- **Smart auto-end**: Session only ends when weight exceeds 50g during measurement, then drops to ≤1g (`SESSION_END_ZERO_G`) for 5 seconds. Previously ended at <5g which was too sensitive.
- **Manual END button**: Red button bottom-left of ACTIVE screen. Calls `session.forceEnd()` via `ui_set_end_cb()` / `LV_OBJ_FLAG_USER_3`.
- **Chart fixes**: Y-axis capped at 500g. Series initialized with `LV_CHART_POINT_NONE` to prevent stale buffer rendering as line artifacts.
- **Time format**: Elapsed time shows M:SS after 60 seconds.
- **Removed Sample# label** from ACTIVE screen (not useful to end user).

#### BOOT Screen UX Fixes
- **Tap-to-scroll glitch fixed**: Cleared `LV_OBJ_FLAG_SCROLLABLE` on metric cards (containers created via `lv_obj_create()` are scrollable by default).
- **Card taps now work**: Cleared `LV_OBJ_FLAG_CLICKABLE` on `icon_bg` child objects so clicks pass through to the parent card.
- **Removed CHANGE WIFI button**: Entire WiFi card is now the tap target (simpler, more intuitive).
- **Removed "SYSTEM READY" label + green dot**: Unnecessary clutter.
- **Countdown UX improved**: When countdown reaches 0, the number/SECONDS/bar are hidden via `LV_OBJ_FLAG_HIDDEN`. Only "TURN ON SCALE" title remains. BLE scanner keeps running — transitions to READY when scale connects.

#### READY Screen
- Updated text: "Starts automatically at 50g" / "Tap anywhere to begin manually"
- Cleared scrollable on decorative circle elements

### Verified on Hardware
- WiFi card tap → WiFi modal opens
- Export card tap → BLE export mode triggers (but crashes — see known issues)
- No screen glitch on tap
- Countdown counts down, then shows "TURN ON SCALE"
- Scale connection transitions to READY screen

---

## Known Issues (Priority Order)

### 1. BLE Export Mode Crash (Critical)
**Symptom:** `assert failed: ble_svc_gap_init ble_svc_gap.c:370 (rc == 0)` — device reboots.
**Root cause:** `enterBleExportMode()` calls `NimBLEDevice::init()` (line 326 of main.cpp) when NimBLE is already initialized from the Acaia scanner (`gBle`). The GAP service cannot be re-initialized.
**Fix needed:** Call `NimBLEDevice::deinit(true)` before reinitializing for server mode, or restructure to share a single NimBLE stack. The `gBle` scanner must be fully stopped and deinitialized first.

### 2. Touch Event Replay After Crash
**Symptom:** After crash-reboot, "Export card clicked" fires 5 times in rapid succession.
**Root cause:** Touch controller (GT911) may buffer events across resets, or LVGL processes stale touch state during init.
**Fix needed:** Add a boot guard that ignores touch events for the first ~500ms, or clear GT911 touch buffer during init.

### 3. BLE Disconnection Crash
**Symptom:** `LoadProhibited` crash if scale disconnects unexpectedly.
**Status:** Pre-existing, not addressed this session.

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
- Callbacks registered via `ui_set_home_cb()`, `ui_set_start_cb()`, `ui_set_end_cb()`
- **Deferred execution pattern**: Card click handlers set `sDeferWifiSetup`/`sDeferBleExport` flags. Main `loop()` checks flags and calls blocking mode-entry functions outside LVGL callback context.

### Thread Safety
- BLE callbacks run on separate RTOS task
- Weight passed via `std::atomic<float> _pendingWeight` / `std::atomic<bool> _newWeightPending`
- Never call LVGL or blocking I/O from BLE callbacks

---

## Previous Session Notes (v1.0–v1.2)

- GPIO 2 is R1 (RGB data), not backlight — controlled via CH422G IO expander
- Large network objects (`HTTPClient`, `WiFiClientSecure`) must be static to avoid stack overflow
- `lv_tick_inc` configured via `LV_TICK_CUSTOM` in lv_conf.h
- Post-session screen uses `LV_OPA_COVER` to fully hide chart underneath
- Google Apps Script upload timeout set to 30s for cold starts
