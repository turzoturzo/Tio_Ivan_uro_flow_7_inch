# Session Handoff Notes (March 6, 2026)

## Achievements
- **LVGL Core Integrated**: Successfully ported the display and touch drivers (`GT911` + `Arduino_GFX`) to LVGL 8.3.11.
- **Crash Fixes**: 
    - Resolved a stack overflow crash during WiFi uploads by moving large network objects (`HTTPClient`, `WiFiClientSecure`) to static memory. 
    - Fixed an `lv_tick_inc` compilation error by correctly configuring `LV_TICK_CUSTOM`.
- **Logic Improvements**:
    - **UI Overlap Fixed**: Made the post-session "Cloud Sync" screen fully opaque (LV_OPA_COVER) to completely hide the underlying chart.
    - **Display Noise Fixed**: Identified and removed a GPIO conflict; the code was trying to use GPIO 2 as a backlight pin, but it's actually an RGB data line (R1). This was causing the "lines and noise" on the screen.
    - **Restart Button Optimized**: The "Start New Measurement" button is nowCentered and fully responsive.
    - Replaced the post-measurement "Deep Sleep" with a non-blocking LVGL UI.
    - Added a "Start New Measurement" button that resets the session state without dropping the BLE connection to the Acaia scale.
    - Extended the Google Apps Script upload timeout to 30 seconds to handle cold starts.
- **Thread Safety & BLE Hardening**: 
    - Moved all blocking File I/O and String formatting from the BLE task context to the main loop (`Session::tick`).
    - Implemented `std::atomic` flags in `Session` for safe data transfer between tasks.
    - Improved BLE connection reliability by stopping scanning and adding a 100ms settle delay before connecting.
    - Added a 10Hz real-time charting update to the LVGL main screen.

## Verification
- Device successfully Boots, Syncs Time, and Connects to Scale.
- Real-time chart updates smoothly at 10Hz.
- Thread safety confirmed: no more GUI/FS crashes during weight notifications.

## Next Steps for the Next Agent/Developer
- **UI Redesign**: Replicate the high-contrast Lovable.dev design (Rich Black `#0A0A0A` + Vibrant Green `#00E660`).
- **BLE Connection Loss**: Handle the "LoadProhibited" crash that occurs if the scale disconnects or turns off unexpectedly.
- **SquareLine Studio**: Full transition to exported UI assets.
