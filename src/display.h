#include "chart_types.h"
#include <Arduino_GFX.h>
#include <databus/Arduino_DataBus.h>
#include <databus/Arduino_ESP32RGBPanel.h>
#include <display/Arduino_RGB_Display.h>
// AppState defined in main.h — forward-declare to avoid circular include
enum class AppState;
class initGT911;

class Display {
public:
  Display();
  void begin();

  // Boot screens
  void showBoot(const char *msg);
  void showBootCountdown(int sec, bool timeSynced, int csvCount,
                         const char *wifiSsid = nullptr);

  // Export / MSC screen
  void showMscMode();

  // BLE export: post-transfer delete prompt
  void showBleExportDeletePrompt(int filesCount);
  void showBleExportDeleted(int deletedCount);

  // WiFi setup mode screen
  void showWifiSetup(bool timeSynced, const char *storedSsid = nullptr);

  // Post-session success screen (shown before deep sleep)
  void showSessionComplete(uint32_t rowCount, uint32_t durationMs);

  // Main status screen (BLE/session info) + session active chart screen
  void update(AppState state, float weight_g, bool logging, uint32_t elapsed_ms,
              uint32_t row_count, bool time_synced, const ChartSample *chart,
              int chartCount, int chartHead, int weightRemovalSecs = 0);

  bool getTouch(int &x, int &y);

private:
  Arduino_GFX *_gfx;
  initGT911 *_touch;
  AppState _lastState;
  int _lastCountdownSecs; // -1 = overlay not active

  // Cached values for dirty-flag redraws on the normal status screen
  char _lastBleStatus[24];
  char _lastLogStatus[8];
  char _lastElapsed[12];
  char _lastRows[12];
  char _lastWeight[16];

  void _drawLabel(int x, int y, const char *label, const char *value,
                  uint16_t valColor, char *prevValue);
  void _drawTitle();
  uint16_t _bleColor(AppState state);

  // Session active: green screen with live chart
  void _drawSessionScreen(bool firstFrame, float weight_g, uint32_t elapsed_ms,
                          const ChartSample *chart, int chartCount,
                          int chartHead);

  // Countdown overlay drawn on top of the session chart when weight is removed
  void _drawRemovalCountdownOverlay(int secsRemaining);
};
