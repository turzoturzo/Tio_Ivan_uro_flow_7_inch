#include "display.h"
#include "config.h"
#include "main.h" // AppState enum
#include <Arduino.h>
#include <Arduino_DataBus.h>
#include <display/Arduino_RGB_Display.h>
#include <initGT911.h>

// ── Colour palette
// ────────────────────────────────────────────────────────────
#define COL_BG 0x0000     // BLACK
#define COL_TITLE 0x07FF  // cyan
#define COL_LABEL 0xC618  // light grey
#define COL_GREEN 0x07E0  // GREEN
#define COL_YELLOW 0xFFE0 // YELLOW
#define COL_RED 0xF800    // RED
#define COL_WHITE 0xFFFF  // WHITE
#define COL_DIMGREY 0x4208
#define COL_SESSION_BG 0x0600 // dark green (R=0,G=48,B=0 in RGB565)
#define COL_BTN_ORANGE 0x18C3 // warm amber (BLE export button)
#define COL_BTN_BLUE 0x24BE   // medium blue (WiFi setup button)

// ── Normal-screen layout (800×480 portrait)
// ───────────────────────────────────
#define TITLE_Y 8
#define ROW1_Y 105 // 70 * 1.5
#define ROW2_Y 177 // 118 * 1.5
#define ROW3_Y 249 // 166 * 1.5
#define ROW4_Y 330 // 220 * 1.5
#define SEP_Y 83   // 55 * 1.5

// ── Session-screen layout
// ─────────────────────────────────────────────────────
#define SESS_HDR_H 58 // header area height
#define CHART_X 5
#define CHART_Y 62
#define CHART_W 230
#define CHART_H 214                         // y=62..275
#define CHART_Y_END (CHART_Y + CHART_H - 1) // 275
#define AXIS_Y 280

// ── Boot-screen button zones (used by main.cpp for touch routing)
// ───────────── BLE EXPORT button: y=105..180   // scaled from 80..150 WIFI
// SETUP button: y=190..260   // scaled from 158..218

Display::Display()
    : _gfx(nullptr), _touch(nullptr), _lastState(static_cast<AppState>(-1)),
      _lastCountdownSecs(-1) {
  memset(_lastBleStatus, 0, sizeof(_lastBleStatus));
  memset(_lastLogStatus, 0, sizeof(_lastLogStatus));
  memset(_lastElapsed, 0, sizeof(_lastElapsed));
  memset(_lastRows, 0, sizeof(_lastRows));
  memset(_lastWeight, 0, sizeof(_lastWeight));
}

void Display::begin() {
  Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
      RGB_DE, RGB_VSYNC, RGB_HSYNC, RGB_PCLK, RGB_R0, RGB_R1, RGB_R2, RGB_R3,
      RGB_R4, RGB_G0, RGB_G1, RGB_G2, RGB_G3, RGB_G4, RGB_G5, RGB_B0, RGB_B1,
      RGB_B2, RGB_B3, RGB_B4, 0 /* hsync_polarity */,
      40 /* hsync_front_porch */, 10 /* hsync_pulse_width */,
      10 /* hsync_back_porch */, 0 /* vsync_polarity */,
      40 /* vsync_front_porch */, 10 /* vsync_pulse_width */,
      10 /* vsync_back_porch */, 1 /* pclk_active_neg */,
      16000000 /* prefer_speed */);

  _gfx = new Arduino_RGB_Display(800, 480, rgbpanel);
  _gfx->begin();
  _gfx->fillScreen(COL_BG);

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  _touch = new initGT911(&Wire, GT911_I2C_ADDR_BA);
  if (!_touch->begin(TOUCH_INT_PIN, TOUCH_RST_PIN)) {
    Serial.println("GT911 init failed!");
  }

  _drawTitle();
}

bool Display::getTouch(int &x, int &y) {
  if (!_touch)
    return false;
  if (_touch->touched(GT911_MODE_POLLING)) {
    GTPoint p = _touch->getPoint(0);
    x = p.x;
    y = p.y;
    return true;
  }
  return false;
}

// ── Boot screens ─────────────────────────────────────────────────────────────

void Display::showBoot(const char *msg) {
  _gfx->fillScreen(COL_BG);
  _drawTitle();
  _gfx->setTextColor(COL_LABEL, COL_BG);

  _gfx->setTextSize(2); // larger for 7" screen
  _gfx->setCursor(20, 150);
  _gfx->print(msg);
}

void Display::showBootCountdown(int sec, bool timeSynced, int csvCount,
                                const char *wifiSsid) {
  static bool drawn = false;

  if (!drawn) {
    _gfx->fillScreen(COL_BG);

    // Title
    _gfx->setTextColor(COL_TITLE);
    _gfx->setCursor(350, 40);
    _gfx->setTextSize(4);
    _gfx->print("UroFlow");

    // Subtitle
    _gfx->setTextColor(COL_LABEL);
    _gfx->setCursor(330, 100);
    _gfx->setTextSize(2);
    _gfx->print("Acaia Scale Logger");

    // BLE EXPORT button (y=105..180)
    _gfx->fillRoundRect(20, 105, 760, 70, 12, COL_BTN_ORANGE);
    _gfx->drawRoundRect(20, 105, 760, 70, 12, COL_WHITE);
    _gfx->setTextColor(COL_WHITE);
    _gfx->setCursor(340, 120);
    _gfx->setTextSize(3);
    _gfx->print("TAP FOR");
    _gfx->setCursor(300, 150);
    _gfx->setTextSize(2);
    _gfx->print("BLE EXPORT ALL");

    // WIFI SETUP button (y=190..260)
    _gfx->fillRoundRect(20, 190, 760, 70, 12, COL_BTN_BLUE);
    _gfx->drawRoundRect(20, 190, 760, 70, 12, COL_WHITE);
    _gfx->setTextColor(COL_WHITE);
    _gfx->setCursor(340, 215);
    _gfx->setTextSize(3);
    _gfx->print("WIFI SETUP");

    drawn = true;
  }

  // Time sync indicator (top-right, small dot)
  _gfx->fillCircle(780, 20, 8, timeSynced ? COL_GREEN : COL_YELLOW);

  // File count line
  _gfx->fillRect(10, 300, 780, 24, COL_BG);
  _gfx->setTextSize(2);
  if (csvCount < 0) {
    _gfx->setTextColor(COL_DIMGREY);
    _gfx->setCursor(200, 300);
    _gfx->print("Checking storage...");
  } else if (csvCount == 0) {
    _gfx->setTextColor(COL_DIMGREY);
    _gfx->setCursor(200, 300);
    _gfx->print("No measurements stored");
  } else {
    char countBuf[36];
    snprintf(countBuf, sizeof(countBuf),
             csvCount == 1 ? "1 measurement stored" : "%d measurements stored",
             csvCount);
    _gfx->setTextColor(COL_WHITE);
    _gfx->setCursor(200, 300);
    _gfx->print(countBuf);
  }

  // Countdown text
  _gfx->fillRect(10, 340, 780, 24, COL_BG);
  char buf[28];
  snprintf(buf, sizeof(buf), "Starting in %ds...", sec);
  _gfx->setTextColor(COL_DIMGREY);
  _gfx->setCursor(300, 340);
  _gfx->print(buf);

  // WiFi SSID line
  _gfx->fillRect(10, 380, 780, 20, COL_BG);
  _gfx->setCursor(300, 380);
  if (wifiSsid != nullptr && strlen(wifiSsid) > 0) {
    char wifiBuf[64];
    snprintf(wifiBuf, sizeof(wifiBuf), "WiFi: %s", wifiSsid);
    _gfx->setTextColor(COL_GREEN);
    _gfx->print(wifiBuf);
  } else {
    _gfx->setTextColor(COL_YELLOW);
    _gfx->print("No WiFi configured");
  }
}

// ── Export / MSC screen
// ───────────────────────────────────────────────────────

void Display::showMscMode() {
  _gfx->fillScreen(COL_WHITE);
  _gfx->setTextColor(COL_BG);

  _gfx->setCursor(300, 80);
  _gfx->setTextSize(4);
  _gfx->print("UroFlow Drive");

  _gfx->setTextColor(COL_GREEN);
  _gfx->setCursor(340, 120);
  _gfx->print("Connected");

  _gfx->setTextColor(COL_BG);
  _gfx->setCursor(200, 170);
  _gfx->setTextSize(2);
  _gfx->print("Copy CSV files to your computer");

  _gfx->setTextColor(COL_DIMGREY);
  _gfx->setCursor(320, 210);
  _gfx->print("Tap screen to exit");
}

// ── BLE export delete prompt
// ──────────────────────────────────────────────────

void Display::showBleExportDeletePrompt(int filesCount) {
  _gfx->fillScreen(COL_BG);
  _gfx->setTextColor(COL_WHITE);
  _gfx->setCursor(300, 40);
  _gfx->setTextSize(4);
  _gfx->print("Export Done!");

  char countBuf[36];
  snprintf(countBuf, sizeof(countBuf),
           filesCount == 1 ? "1 file exported" : "%d files exported",
           filesCount);
  _gfx->setTextColor(COL_LABEL);
  _gfx->setCursor(300, 80);
  _gfx->setTextSize(2);
  _gfx->print(countBuf);

  _gfx->setCursor(300, 110);
  _gfx->print("Delete all files?");

  // YES button (y=130..206)
  _gfx->fillRoundRect(100, 130, 600, 76, 12, COL_RED);
  _gfx->drawRoundRect(100, 130, 600, 76, 12, COL_WHITE);
  _gfx->setTextColor(COL_WHITE);
  _gfx->setCursor(300, 155);
  _gfx->setTextSize(3);
  _gfx->print("YES, DELETE");

  // NO button (y=216..272)
  _gfx->fillRoundRect(100, 216, 600, 56, 12, COL_DIMGREY);
  _gfx->drawRoundRect(100, 216, 600, 56, 12, COL_WHITE);
  _gfx->setTextColor(COL_WHITE);
  _gfx->setCursor(300, 235);
  _gfx->setTextSize(2);
  _gfx->print("NO, KEEP FILES");

  _gfx->setTextColor(COL_DIMGREY);
  _gfx->setCursor(330, 298);
  _gfx->print("(auto-exit in 15s)");
}

void Display::showBleExportDeleted(int deletedCount) {
  _gfx->fillScreen(COL_BG);

  _gfx->setTextColor(COL_GREEN, COL_BG);
  _gfx->setCursor(120, 100);
  _gfx->setTextSize(4);
  _gfx->print("Files Deleted");

  char buf[36];
  snprintf(buf, sizeof(buf),
           deletedCount == 1 ? "1 file removed" : "%d files removed",
           deletedCount);
  _gfx->setTextColor(COL_WHITE, COL_BG);
  _gfx->setCursor(120, 148);
  _gfx->setTextSize(2);
  _gfx->print(buf);

  _gfx->setTextColor(COL_DIMGREY, COL_BG);
  _gfx->setCursor(120, 220);
  _gfx->setTextSize(2);
  _gfx->print("Restarting...");
}

// ── WiFi setup mode screen
// ────────────────────────────────────────────────────

void Display::showWifiSetup(bool timeSynced, const char *storedSsid) {
  _gfx->fillScreen(COL_BG);

  _gfx->setTextColor(COL_TITLE, COL_BG);
  _gfx->setCursor(120, 30);
  _gfx->setTextSize(4);
  _gfx->print("WiFi Setup");

  // Current SSID or "No WiFi stored"
  if (storedSsid != nullptr && strlen(storedSsid) > 0) {
    char ssidBuf[48];
    snprintf(ssidBuf, sizeof(ssidBuf), "Stored: %s", storedSsid);
    _gfx->setTextColor(COL_GREEN, COL_BG);
    _gfx->setCursor(120, 68);
    _gfx->setTextSize(2);
    _gfx->print(ssidBuf);
  } else {
    _gfx->setTextColor(COL_YELLOW, COL_BG);
    _gfx->setCursor(120, 68);
    _gfx->setTextSize(2);
    _gfx->print("No WiFi stored");
  }

  // Time sync status
  _gfx->fillCircle(120 - 55, 94, 5, timeSynced ? COL_GREEN : COL_YELLOW);
  _gfx->setTextColor(timeSynced ? COL_GREEN : COL_YELLOW, COL_BG);

  _gfx->setCursor(120 - 46, 94);
  _gfx->setTextSize(2);
  _gfx->print(timeSynced ? "Time synced" : "Time not synced");

  // Instructions box
  _gfx->drawRoundRect(10, 114, 220, 130, 10, COL_DIMGREY);
  _gfx->setTextColor(COL_LABEL, COL_BG);
  _gfx->setCursor(120, 132);
  _gfx->setTextSize(2);
  _gfx->print("Connect via BLE:");
  _gfx->setTextColor(COL_WHITE, COL_BG);
  _gfx->setCursor(120, 156);
  _gfx->setTextSize(2);
  _gfx->print("Device: 'Logger'");
  _gfx->setTextColor(COL_LABEL, COL_BG);
  _gfx->setCursor(120, 178);
  _gfx->setTextSize(2);
  _gfx->print("Then send:");
  _gfx->setTextColor(COL_YELLOW, COL_BG);
  _gfx->setCursor(120, 202);
  _gfx->setTextSize(2);
  _gfx->print("WIFI:ssid|password");
  _gfx->setTextColor(COL_DIMGREY, COL_BG);
  _gfx->setCursor(120, 228);
  _gfx->setTextSize(1);
  _gfx->print("(via ble_export_receive.py)");

  _gfx->setTextColor(COL_DIMGREY, COL_BG);
  _gfx->setCursor(120, 292);
  _gfx->setTextSize(2);
  _gfx->print("Tap to exit");
}

// ── Post-session success screen
// ───────────────────────────────────────────────

void Display::showSessionComplete(uint32_t rowCount, uint32_t durationMs) {
  _gfx->fillScreen(COL_SESSION_BG);

  // Big green "Measurement / Logged!" headline
  _gfx->setTextColor(COL_GREEN, COL_SESSION_BG);
  _gfx->setCursor(120, 75);
  _gfx->setTextSize(4);
  _gfx->print("Measurement");
  _gfx->setCursor(120, 115);
  _gfx->setTextSize(4);
  _gfx->print("Logged!");

  // Session stats
  _gfx->setTextColor(COL_WHITE, COL_SESSION_BG);
  uint32_t s = durationMs / 1000;
  char buf[40];
  snprintf(buf, sizeof(buf), "%02lu:%02lu recorded", s / 60, s % 60);
  _gfx->setCursor(120, 170);
  _gfx->setTextSize(2);
  _gfx->print(buf);
  snprintf(buf, sizeof(buf), "%lu samples saved", (unsigned long)rowCount);
  _gfx->setCursor(120, 196);
  _gfx->setTextSize(2);
  _gfx->print(buf);

  // Sleep notice
  _gfx->setTextColor(COL_DIMGREY, COL_SESSION_BG);
  _gfx->setCursor(120, 248);
  _gfx->setTextSize(2);
  _gfx->print("Going into low power mode...");
}

// ── Main update
// ───────────────────────────────────────────────────────────────

void Display::update(AppState state, float weight_g, bool logging,
                     uint32_t elapsed_ms, uint32_t row_count, bool time_synced,
                     const ChartSample *chart, int chartCount, int chartHead,
                     int weightRemovalSecs) {

  bool stateChanged = (state != _lastState);
  _lastState = state;

  // ── Session active: full green screen with live chart ─────────────────────
  if (state == AppState::SESSION_ACTIVE) {
    _drawSessionScreen(stateChanged, weight_g, elapsed_ms, chart, chartCount,
                       chartHead);
    if (weightRemovalSecs > 0) {
      _drawRemovalCountdownOverlay(weightRemovalSecs);
    }
    _lastCountdownSecs = weightRemovalSecs;
    return;
  }

  // ── Normal status screen ──────────────────────────────────────────────────
  // Re-initialise screen
  if (stateChanged) {
    _gfx->fillScreen(COL_BG);
    _drawTitle();
    memset(_lastBleStatus, 0, sizeof(_lastBleStatus));
    memset(_lastLogStatus, 0, sizeof(_lastLogStatus));
    memset(_lastElapsed, 0, sizeof(_lastElapsed));
    memset(_lastRows, 0, sizeof(_lastRows));
    memset(_lastWeight, 0, sizeof(_lastWeight));
  }

  // BLE status string
  char bleStr[24];
  switch (state) {
  case AppState::BLE_SCANNING:
    snprintf(bleStr, sizeof(bleStr), "SCANNING...");
    break;
  case AppState::BLE_CONNECTING:
    snprintf(bleStr, sizeof(bleStr), "CONNECTING...");
    break;
  case AppState::CONNECTED_IDLE:
  case AppState::SESSION_END:
    snprintf(bleStr, sizeof(bleStr), "CONNECTED");
    break;
  default:
    snprintf(bleStr, sizeof(bleStr), "---");
    break;
  }
  uint16_t bleCol = _bleColor(state);

  // LOG status
  char logStr[8];
  snprintf(logStr, sizeof(logStr), logging ? "YES" : "NO");
  uint16_t logCol = logging ? COL_GREEN : COL_DIMGREY;

  // Elapsed time mm:ss
  uint32_t secs = elapsed_ms / 1000;
  char elStr[12];
  snprintf(elStr, sizeof(elStr), "%02lu:%02lu", secs / 60, secs % 60);

  // Row count
  char rowStr[12];
  snprintf(rowStr, sizeof(rowStr), "%lu", (unsigned long)row_count);

  // Weight
  char wtStr[16];
  snprintf(wtStr, sizeof(wtStr), "%.1f g", weight_g);

  _gfx->setTextSize(1);

  _drawLabel(10, ROW1_Y, "BLE: ", bleStr, bleCol, _lastBleStatus);
  _drawLabel(10, ROW2_Y, "LOG: ", logStr, logCol, _lastLogStatus);

  if (strcmp(elStr, _lastElapsed) != 0) {
    _gfx->setTextColor(COL_BG, COL_BG);
    _gfx->setCursor(155, ROW2_Y);
    _gfx->print(_lastElapsed);
    _gfx->setCursor(155, ROW2_Y);
    _gfx->setTextColor(COL_WHITE, COL_BG);
    _gfx->print(elStr);
    strlcpy(_lastElapsed, elStr, sizeof(_lastElapsed));
  }

  _drawLabel(10, ROW3_Y, "ROWS: ", rowStr, COL_WHITE, _lastRows);
  _drawLabel(10, ROW4_Y, "WT:  ", wtStr, COL_WHITE, _lastWeight);

  // Time sync dot
  _gfx->fillCircle(228, 10, 5, time_synced ? COL_GREEN : COL_YELLOW);
}

// ── Session active green screen
// ───────────────────────────────────────────────

void Display::_drawSessionScreen(bool firstFrame, float weight_g,
                                 uint32_t elapsed_ms, const ChartSample *chart,
                                 int chartCount, int chartHead) {
  if (firstFrame) {
    _gfx->fillScreen(COL_SESSION_BG);
    // Static separator
    _gfx->drawFastHLine(0, SESS_HDR_H, 240, COL_DIMGREY);
  }

  // ── Weight value (Font 4 × size 2, left, white) ──────────────────────────
  _gfx->fillRect(0, 0, 195, SESS_HDR_H, COL_SESSION_BG);

  _gfx->setTextSize(2);
  _gfx->setTextColor(COL_WHITE, COL_SESSION_BG);

  char wtStr[20];
  snprintf(wtStr, sizeof(wtStr), "%.1f g", weight_g);
  _gfx->setCursor(4, 4);
  _gfx->print(wtStr);
  _gfx->setTextSize(1);

  // ── Elapsed time (Font 2, top-right, grey) ───────────────────────────────
  uint32_t secs = elapsed_ms / 1000;
  char elStr[12];
  snprintf(elStr, sizeof(elStr), "%02lu:%02lu", secs / 60, secs % 60);
  _gfx->fillRect(196, 0, 44, SESS_HDR_H, COL_SESSION_BG);

  _gfx->setTextColor(COL_LABEL, COL_SESSION_BG);

  _gfx->setCursor(238, 18);
  _gfx->setTextSize(2);
  _gfx->print(elStr);

  // ── Chart area (black background) ────────────────────────────────────────
  _gfx->fillRect(CHART_X, CHART_Y, CHART_W, CHART_H, COL_BG);

  if (chartCount >= 2) {
    // Find max values for axis scaling
    float maxW = 1.0f;
    uint32_t maxT = 1;
    for (int i = 0; i < chartCount; i++) {
      int idx = (chartHead + i) % CHART_BUF_SIZE;
      if (chart[idx].weight_g > maxW)
        maxW = chart[idx].weight_g;
      if (chart[idx].t_ms > maxT)
        maxT = chart[idx].t_ms;
    }
    maxW *= 1.15f; // 15% headroom above peak

    // Draw polyline
    int prevPx = -1, prevPy = -1;
    for (int i = 0; i < chartCount; i++) {
      int idx = (chartHead + i) % CHART_BUF_SIZE;
      // Map t_ms → x (CHART_X .. CHART_X+CHART_W-1)
      int px = CHART_X + (int)((float)chart[idx].t_ms / maxT * (CHART_W - 1));
      // Map weight → y (CHART_Y_END down to CHART_Y), inverted
      int py = CHART_Y_END - (int)(chart[idx].weight_g / maxW * (CHART_H - 1));
      px = constrain(px, CHART_X, CHART_X + CHART_W - 1);
      py = constrain(py, CHART_Y, CHART_Y_END);
      if (prevPx >= 0) {
        _gfx->drawLine(prevPx, prevPy, px, py, COL_WHITE);
      } else {
        _gfx->drawPixel(px, py, COL_WHITE);
      }
      prevPx = px;
      prevPy = py;
    }
  }

  // ── Axis labels ───────────────────────────────────────────────────────────
  _gfx->fillRect(0, AXIS_Y, 240, 320 - AXIS_Y, COL_SESSION_BG);

  _gfx->setTextColor(COL_LABEL, COL_SESSION_BG);

  _gfx->setCursor(CHART_X, AXIS_Y + 2);
  _gfx->setTextSize(1);
  _gfx->print("0s");

  if (chartCount > 0) {
    int lastIdx = (chartHead + chartCount - 1) % CHART_BUF_SIZE;
    uint32_t maxT = chart[lastIdx].t_ms;
    char maxTStr[12];
    snprintf(maxTStr, sizeof(maxTStr), "%lus", (unsigned long)(maxT / 1000));

    _gfx->setCursor(CHART_X + CHART_W - 1, AXIS_Y + 2);
    _gfx->setTextSize(1);
    _gfx->print(maxTStr);
  }
}

// ── Weight-removal countdown overlay ─────────────────────────────────────────

void Display::_drawRemovalCountdownOverlay(int secsRemaining) {
  // Overlay a warning band in the lower portion of the chart area (y=224..274)
  const int OV_X = CHART_X; // 5
  const int OV_Y = 224;
  const int OV_W = CHART_W; // 230
  const int OV_H = 50;

  _gfx->fillRect(OV_X, OV_Y, OV_W, OV_H, COL_SESSION_BG); // erase chart pixels
  _gfx->drawRoundRect(OV_X, OV_Y, OV_W, OV_H, 8, COL_YELLOW);

  char buf[28];
  snprintf(buf, sizeof(buf), "Concluding in %d...", secsRemaining);

  _gfx->setTextColor(COL_YELLOW, COL_SESSION_BG);
  _gfx->setCursor(120, OV_Y + OV_H / 2);
  _gfx->setTextSize(4);
  _gfx->print(buf);
}

// ── Private helpers
// ───────────────────────────────────────────────────────────

void Display::_drawTitle() {

  _gfx->setTextSize(1);
  _gfx->setTextColor(COL_TITLE, COL_BG);
  _gfx->setCursor(10, TITLE_Y);
  _gfx->print("ACAIA LOGGER");

  _gfx->setTextColor(COL_DIMGREY, COL_BG);
  _gfx->setCursor(10, TITLE_Y + 30);
  _gfx->print("v" FW_VERSION);
  _gfx->drawLine(0, SEP_Y, 240, SEP_Y, COL_DIMGREY);
}

void Display::_drawLabel(int x, int y, const char *label, const char *value,
                         uint16_t valColor, char *prevValue) {
  if (strcmp(value, prevValue) == 0)
    return;

  _gfx->setTextSize(1);

  int labelWidth = strlen(label) * 12;
  _gfx->setTextColor(COL_BG, COL_BG);
  _gfx->setCursor(x + labelWidth, y);
  _gfx->print(prevValue);

  _gfx->setTextColor(COL_LABEL, COL_BG);
  _gfx->setCursor(x, y);
  _gfx->print(label);

  _gfx->setTextColor(valColor, COL_BG);
  _gfx->print(value);

  strlcpy(prevValue, value, 24);
}

uint16_t Display::_bleColor(AppState state) {
  switch (state) {
  case AppState::CONNECTED_IDLE:
  case AppState::SESSION_ACTIVE:
  case AppState::SESSION_END:
    return COL_GREEN;
  case AppState::BLE_SCANNING:
  case AppState::BLE_CONNECTING:
    return COL_YELLOW;
  default:
    return COL_RED;
  }
}
