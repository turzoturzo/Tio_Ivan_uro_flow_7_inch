# UroFlow Logger (7-Inch Edition)

This project is a firmware implementation for an ESP32-S3 powered device with a 7-inch RGB display (Waveshare ESP32-S3-Touch-LCD-7). It interfaces with an **Acaia Pearl S** digital scale via Bluetooth Low Energy (BLE) to log weight data for uroflowmetry sessions.

## Key Features
- **LVGL Integration**: Modern, scalable UI framework for smooth graphics and complex widgets.
- **BLE Scale Interface**: Reliable connection and weight streaming from Acaia Pearl S.
- **Cloud Sync**: Automatic session upload to Google Sheets via Google Apps Script.
- **Local Storage**: Buffered logging to FFat on internal flash for data safety.
- **On-Screen Setup**: Integrated WiFi and BLE provisioning via a built-in keyboard UI.

## Hardware Support
- **Core**: ESP32-S3 (Dual-core, Embedded PSRAM).
- **Display**: 800x480 RGB 16-bit panel via `Arduino_GFX`.
- **Touch**: GT911 Capactive Touch controller (connected via CH422G IO Expander).

## Setup & Configuration
Sensitive credentials and endpoints are managed in `src/config.h`. 

1. Copy `src/config.h.example` (or refer to the `.env.example` file) to setup your local environment.
2. Build and upload using PlatformIO.

## Development Status
Current work is focused on a transition from primitive `Arduino_GFX` drawing to a fully-realized LVGL interface. See `TASKS.md` for the current roadmap.
