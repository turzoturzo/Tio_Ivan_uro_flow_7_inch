#pragma once

// ─── Firmware version ───────────────────────────────────────────────────────
#define FW_VERSION "1.0.0"
#define DEVICE_NAME "Mongo-Pearl-1"

// ─── WiFi credentials (for NTP time sync on boot) ───────────────────────────
// WARNING: do not commit real credentials to a public repository
#define WIFI_SSID ""
#define WIFI_PASS ""
#define WIFI_TIMEOUT_S 12

// Cloud upload endpoint (e.g., a Webhook or API)
#define CLOUD_UPLOAD_URL                                                       \
  "https://script.google.com/macros/s/"                                        \
  "AKfycbwt8VMsRdyy_8KSigtyUo8gjyx9N1xk6sx3Ueue-DKrIPoGJcNuaZi-"               \
  "9OjitMhbPiJkvw/exec"

// ─── Acaia Pearl S BLE ───────────────────────────────────────────────────────
#define ACAIA_SEARCH_NAME "ACAIA" // substring match on advertised name
// ISSC transparent serial service (used by Acaia Pearl S)
#define ACAIA_SERVICE_UUID "49535343-FE7D-4AE5-8FA9-9FAFD205E455"
#define ACAIA_CHAR_NOTIFY "49535343-1e4d-4bd9-ba61-23c647249616"
#define ACAIA_CHAR_WRITE "49535343-8841-43f4-a8d4-ecbe34729bb3"

// Acaia protocol message types
#define ACAIA_MSG_SYSTEM 0
#define ACAIA_MSG_HEARTBEAT 2
#define ACAIA_MSG_TARE 4

#define HEARTBEAT_INTERVAL_MS 9000UL // scale drops connection if no heartbeat

// ─── Session parameters ──────────────────────────────────────────────────────
#define SESSION_MIN_DURATION_MS 3000UL // discard sessions shorter than this
#define SESSION_TIMEOUT_MS 45000UL // end active session after this idle period
#define CONNECT_TO_WEIGHT_TIMEOUT_MS                                           \
  90000UL // after BLE connect, wait this long for first weight
#define LOG_FLUSH_INTERVAL_MS 2000UL // buffered write flush period
#define SESSION_START_THRESHOLD_G                                              \
  50.0f // g — auto-start measurement when weight exceeds this
#define WEIGHT_REMOVAL_THRESHOLD_G                                             \
  5.0f // g — weight reading below this = "scale empty" (legacy, kept for ref)
#define SESSION_END_ZERO_G                                                     \
  1.0f // g — weight at or below this triggers end countdown
#define WEIGHT_REMOVAL_TIMEOUT_MS                                              \
  5000UL // ms — sustained at zero after threshold → end session

// ─── Hardware pins ───────────────────────────────────────────────────────────
#define TOUCH_SDA_PIN 8  // GT911 SDA - Waveshare ESP32-S3-Touch-LCD-7
#define TOUCH_SCL_PIN 9  // GT911 SCL
#define TOUCH_INT_PIN 4  // GT911 INT pin (optional, matching wiki)
#define TOUCH_RST_PIN -1 // GT911 RST — handled by CH422G on non-B board
#define TFT_BL_PIN 2     // RGB LCD backlight

// RGB panel profile selector:
// 0 = Waveshare-style 7" profile (original project mapping)
// 1 = ESP32_8048S070 profile
// 2 = ST7262 800x480 profile from Arduino_GFX examples
#define RGB_PANEL_PROFILE 3

// Default RGB pin map (used by profile 1 if you keep these macros)
#define RGB_DE 41
#define RGB_VSYNC 40
#define RGB_HSYNC 39
#define RGB_PCLK 42
#define RGB_R0 14
#define RGB_R1 21
#define RGB_R2 47
#define RGB_R3 48
#define RGB_R4 45
#define RGB_G0 9
#define RGB_G1 46
#define RGB_G2 3
#define RGB_G3 8
#define RGB_G4 16
#define RGB_G5 1
#define RGB_B0 15
#define RGB_B1 7
#define RGB_B2 6
#define RGB_B3 5
#define RGB_B4 4

// ─── NVS keys ────────────────────────────────────────────────────────────────
#define NVS_NAMESPACE "pearls"
#define NVS_KEY_MAC "scale_mac"
#define NVS_KEY_SEQNUM "seq_num"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
