# Coding Standards & Agent Guidelines

This document outlines the architectural patterns and constraints for working on the UroFlow 7-Inch firmware.

## 1. Memory Management (ESP32-S3)
- **Stack Safety**: Avoid creating large objects (like `HTTPClient` or `WiFiClientSecure`) locally on the stack. This can trigger a "Stack canary watchpoint" crash. Use `static` variables or heap allocation.
- **PSRAM Usage**: Use `MALLOC_CAP_SPIRAM` for large buffers (display buffers, chart data, etc.). The device has 8MB of PSRAM.
- **Heap Allocation**: Always check if pointers are `nullptr` after `malloc` or `new`.

## 2. UI Framework (LVGL)
- **Non-Blocking**: Never use `delay()` in code that runs alongside LVGL. Use `millis()` timers if needed.
- **Tick Source**: The firmware uses `LV_TICK_CUSTOM` via `millis()`.
- **Display Refresh**: Ensure `lv_task_handler()` is called frequently in the main `loop()`.

## 3. Bluetooth (NimBLE)
- **Thread Safety**: BLE callbacks (like `onWeight`) run on a separate task. Do not perform complex logic or blocking I/O inside them. Use flags or queues to signal the main loop.
- **Keep-Alive**: The Acaia scale requires a heartbeat packet every 9 seconds to maintain connection.

## 4. Storage (FFat)
- **Wait for Close**: Always ensure `File.close()` is called before renaming or deleting files.
- **Fragmentation**: Avoid frequent small writes. Use the `Session` buffer to batch CSV rows.

## 5. Development Workflow
- **PlatformIO**: Always build and test using the PlatformIO environment.
- **Serial Monitor**: Keep the monitor baud rate at 115200.
