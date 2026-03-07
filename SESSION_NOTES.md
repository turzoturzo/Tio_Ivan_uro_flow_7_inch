# Session Handoff Notes

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
| `SESSION_START_THRESHOLD_G` | 50.0g | Auto-start measurement |
| `SESSION_END_ZERO_G` | 1.0g | Weight "removed" threshold |
| `WEIGHT_REMOVAL_TIMEOUT_MS` | 5000ms | Sustained zero → end session |
| `SESSION_TIMEOUT_MS` | 45000ms | No data → end session |
| `SESSION_MIN_DURATION_MS` | 3000ms | Discard sessions shorter than this |

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
