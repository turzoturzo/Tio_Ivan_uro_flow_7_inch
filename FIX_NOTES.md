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

### 1. Upgrade espressif32 platform (`platformio.ini`)

Changed `platform = espressif32` to `platform = espressif32@^6.0.0`.

Arduino_GFX 1.5.x requires arduino-esp32 3.x (IDF 5.x), which ships `esp32-hal-periman.h`. The old unversioned platform resolved to espressif32 5.x (arduino-esp32 2.x / IDF 4.4) which lacks this header.

### 2. Upgrade Arduino_GFX library (`platformio.ini`)

Changed `moononournation/GFX Library for Arduino @ 1.4.9` to `moononournation/GFX Library for Arduino @ ^1.5.6`.

v1.4.9 does NOT have the `bounce_buffer_size_px` constructor parameter. It was added in 1.5.x.

### 3. Add bounce buffer to RGB panel init (`src/display.cpp`)

In the `RGB_PANEL_PROFILE == 3` block, added three trailing parameters to the `Arduino_ESP32RGBPanel` constructor:

```cpp
rgbpanel = new Arduino_ESP32RGBPanel(
    5 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 7 /* PCLK */, 1 /* R0 */,
    2 /* R1 */, 42 /* R2 */, 41 /* R3 */, 40 /* R4 */, 39 /* G0 */,
    0 /* G1 */, 45 /* G2 */, 48 /* G3 */, 47 /* G4 */, 21 /* G5 */,
    14 /* B0 */, 38 /* B1 */, 18 /* B2 */, 17 /* B3 */, 10 /* B4 */,
    0 /* hsync_p */, 40 /* h_fp */, 48 /* h_pw */, 40 /* h_bp */,
    0 /* vsync_p */, 13 /* v_fp */, 3 /* v_pw */, 32 /* v_bp */,
    1 /* pclk_active_neg */, 8000000 /* prefer_speed */,
    false /* useBigEndian */, 0 /* de_idle_high */, 0 /* pclk_idle_high */,
    800 * 10 /* bounce_buffer_size_px = 10 lines */);
```

Bounce buffers (8000 pixels = 16 KB) live in internal SRAM. The LCD DMA reads from these while the CPU asynchronously copies the next chunk from PSRAM, eliminating the bandwidth deadlock that triggered the IWDT.

### 4. Add 64-byte data cache line build flag (`platformio.ini`)

Added `-DCONFIG_ESP32S3_DATA_CACHE_LINE_64B=y` to `build_flags`.

Required for stable PSRAM-backed RGB operation — without this, the display can drift/shift horizontally after some time.

## Verification

1. Build: `pio run -e esp32-s3-touch-lcd-7`
2. Flash + monitor: `pio run -e esp32-s3-touch-lcd-7 -t upload && pio device monitor -b 115200`
3. Confirm serial output passes "Starting RGB GFX..." without `TG1WDT_SYS_RST`
4. Confirm display renders without color shifting or screen drift
