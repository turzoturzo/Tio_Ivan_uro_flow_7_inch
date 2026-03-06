#pragma once
#include <lvgl.h>

// Color Palette (Industrial Cyberpunk)
#define UI_COLOR_BLACK 0x0A0A0A
#define UI_COLOR_GREEN 0x00E660
#define UI_COLOR_WHITE 0xF2F2F2
#define UI_COLOR_GRAY 0x222222

// UI States
enum class UIState {
  BOOT,
  READY,   // Connected to scale, waiting for weight
  ACTIVE,  // Recording (shows chart)
  SUCCESS, // Measurement finished
  SYNCING, // Uploading to cloud
  ERROR    // General error state
};

void ui_init();
void ui_set_state(UIState state);
void ui_update_weight(float weight_g, uint32_t elapsed_s);
void ui_set_boot_status(const char *status, int progress_pct);
void ui_set_sync_status(const char *message, bool is_error);

// Callback registration for interactions
void ui_set_home_cb(void (*cb)());
void ui_set_start_cb(void (*cb)());
