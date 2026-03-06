# Project Roadmap & Priorities

This file tracks the current goals for the UroFlow 7-Inch project.

## 🔴 High Priority (Immediate)
- **Visual UI Redesign**: Transition from code-generated LVGL widgets to specialized SquareLine Studio exports for a premium look.
- **Legacy Cleanup**: Remove remaining `_gfx->draw...` calls from `display.cpp` that are superseded by LVGL.
- **WiFi Hardening**: Improve reconnection logic when `uploadToGoogleSheet` fails due to network flux.

## 🟡 Medium Priority
- **NVM Settings**: Persist display settings (brightness, theme) in NVS.
- **CSV Management**: Add an interface to view/delete old CSV logs from internal storage via the screen.
- **Heartbeat Monitoring**: Add a visual status indicator for the BLE connection health.

## 🟢 Low Priority
- **Dark/Light Mode**: Dynamic theme switching.
- **OTA Updates**: Enable Over-The-Air firmware updates for remote fixes.
