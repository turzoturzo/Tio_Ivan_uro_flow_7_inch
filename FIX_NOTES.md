# Fix: TG1WDT_SYS_RST Watchdog Reset During Display Init

## Context

The ESP32-S3 device enters an infinite reboot loop during RGB display initialization. The serial log shows:
```
[DISPLAY] Starting RGB GFX...
rst:0x8 (TG1WDT_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)
```

**Root cause**: The 800x480 RGB LCD framebuffer is allocated in OPI PSRAM. Without bounce buffers, the LCD peripheral DMA competes with the CPU for PSRAM bandwidth, causing the Interrupt Watchdog Timer (Timer Group 1) to fire during `_gfx->begin()`. This is a well-documented ESP32-S3 issue with RGB displays + PSRAM.

**Fix**: Enable bounce buffers in the `Arduino_ESP32RGBPanel` constructor and upgrade the Arduino_GFX library (and espressif32 platform) to versions that support the `bounce_buffer_size_px` parameter.

## Changes Made

### 1. Switch to pioarduino platform (`platformio.ini`)

Changed `platform = espressif32@^6.0.0` to pioarduino zip URL.

`espressif32@^6.x` resolved to arduino-esp32 **2.0.17** (IDF 4.4), NOT 3.x as assumed.
pioarduino is the community fork that actually ships arduino-esp32 3.3.7 (IDF 5.x),
which provides `esp32-hal-periman.h` required by GFX Library 1.6.x.

### 2. Upgrade Arduino_GFX library (`platformio.ini`)

Changed `moononournation/GFX Library for Arduino @ 1.4.9` to `^1.5.6`.
Resolved to **1.6.5**. v1.4.9 lacks the `bounce_buffer_size_px` constructor parameter.

### 3. Upgrade NimBLE-Arduino to 2.x (`platformio.ini`)

Changed `h2zero/NimBLE-Arduino @ ^1.4.2` to `^2.0.0` (resolved to 2.3.8).

NimBLE 1.4.x is only compatible with IDF 4.4. On IDF 5.x it crashes during
`NimBLEDevice::init()` with a TLSF heap allocator assertion:
```
assert failed: block_locate_free tlsf_control_functions.h:618
```
NimBLE 2.x supports IDF 5.x. Required API changes throughout `ble_acaia.cpp/h`,
`ble_timesync.cpp`, and `main.cpp` (callback signatures, scan API, address constructor).

### 4. Correct RGB panel timings (`src/display.cpp`)

The original timing values were wrong for this panel. Corrected using confirmed
working values from the ESPHome community project for this exact board:

| Parameter | Wrong value | Correct value |
|---|---|---|
| pclk | 8 MHz | 16 MHz |
| hsync_pulse_width | 48 | 4 |
| hsync_front/back porch | 40 | 8 |
| vsync_pulse_width | 3 | 4 |
| vsync_front/back porch | 13/32 | 16 |

### 5. Add bounce buffer + 64-byte cache flag (`src/display.cpp`, `platformio.ini`)

Bounce buffer `800 * 10` pixels (16 KB in internal SRAM). The LCD DMA reads from
these while the CPU copies the next chunk from PSRAM, preventing PSRAM bandwidth
deadlock and scanline corruption artifacts (flickering pixels, horizontal lines).

Added `-DCONFIG_ESP32S3_DATA_CACHE_LINE_64B=y` to build flags for stable
PSRAM-backed RGB operation.

## Current Status (as of this branch)

- **Display**: WORKING. Boots, renders UI, color probe (R/G/B) works, no crashes.
- **BLE scanning**: WORKING. Device scans for Acaia scale after boot.
- **WiFi/NTP**: WORKING. Time syncs on boot.
- **Touch input**: NOT WORKING. GT911 driver initialises without error but taps
  are not registering. Likely a GT911 I2C address, INT pin, or RST pin issue.
  `TOUCH_RST_PIN = -1` relies on CH422G for reset — needs investigation.
- **Layout**: UI elements are cramped in top-left (hardcoded 240px-wide coordinates
  from the original smaller-display version). Needs scaling for 800×480.

## Known Next Steps

1. Debug GT911 touch — verify I2C address (`GT911_I2C_ADDR_BA`), check if CH422G
   is correctly toggling TP_RST during `Display::begin()`, check INT pin wiring.
2. Scale UI layout from 240×320 to 800×480 (all hardcoded coordinates in display.cpp).

## Build / Flash

```bash
cd /path/to/project
~/.platformio/penv/bin/pio run -e esp32-s3-touch-lcd-7 -t upload
~/.platformio/penv/bin/pio device monitor -b 115200
```
