#include "ui.h"
#include "chart_types.h"
#include "config.h"
#include <Arduino.h>

static lv_obj_t *main_screen = nullptr;
static lv_obj_t *header_label = nullptr;
static lv_obj_t *weight_label = nullptr;
static lv_obj_t *time_label = nullptr;
static lv_obj_t *chart = nullptr;
static lv_chart_series_t *ser = nullptr;
static lv_obj_t *status_label = nullptr;
static lv_obj_t *sync_panel = nullptr;

static UIState current_ui_state = UIState::BOOT;

void ui_init() {
  main_screen = lv_scr_act();
  lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_BLACK), 0);
  lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, 0);

  // Initial state: Boot
  ui_set_state(UIState::BOOT);
}

void ui_set_state(UIState state) {
  if (current_ui_state == state)
    return;
  current_ui_state = state;

  // Clean up current screen
  lv_obj_clean(main_screen);

  switch (state) {
  case UIState::BOOT: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_BLACK), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "UroFlow Initializing...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
  } break;

  case UIState::READY: {
    // Full Screen Green
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_GREEN), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "SCALE READY\nPLACE WEIGHT TO START");
    lv_label_set_recolor(status_label, true);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
  } break;

  case UIState::ACTIVE: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_BLACK), 0);

    // Header
    header_label = lv_label_create(main_screen);
    lv_label_set_text(header_label, "● RECORDING");
    lv_obj_set_style_text_color(header_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_align(header_label, LV_ALIGN_TOP_RIGHT, -20, 20);

    // Metrics
    lv_obj_t *elaps_lbl = lv_label_create(main_screen);
    lv_label_set_text(elaps_lbl, "ELAPSED");
    lv_obj_set_style_text_color(elaps_lbl, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_align(elaps_lbl, LV_ALIGN_TOP_LEFT, 20, 20);

    time_label = lv_label_create(main_screen);
    lv_label_set_text(time_label, "0s");
    lv_obj_set_style_text_color(time_label, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 20, 45);

    lv_obj_t *curr_lbl = lv_label_create(main_screen);
    lv_label_set_text(curr_lbl, "CURRENT");
    lv_obj_set_style_text_color(curr_lbl, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_align(curr_lbl, LV_ALIGN_TOP_LEFT, 160, 20);

    weight_label = lv_label_create(main_screen);
    lv_label_set_text(weight_label, "0.0g");
    lv_obj_set_style_text_color(weight_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(weight_label, &lv_font_montserrat_48, 0);
    lv_obj_align(weight_label, LV_ALIGN_TOP_LEFT, 160, 45);

    // Chart
    chart = lv_chart_create(main_screen);
    lv_obj_set_size(chart, 760, 320);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_point_count(chart, CHART_BUF_SIZE);

    // Dark grid lines
    lv_obj_set_style_line_color(chart, lv_color_hex(UI_COLOR_GRAY),
                                LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);

    ser = lv_chart_add_series(chart, lv_color_hex(UI_COLOR_GREEN),
                              LV_CHART_AXIS_PRIMARY_Y);
  } break;

  case UIState::SUCCESS:
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_GREEN), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "MEASUREMENT COLLECTED\nSUCCESSFULLY");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -40);
    break;

  case UIState::SYNCING:
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_GREEN), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "SYNCING TO CLOUD...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    break;

  case UIState::ERROR:
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0xFF0000),
                              0); // Red for error
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "SYSTEM ERROR\nPLEASE RESTART");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    break;
  }
}

void ui_update_weight(float weight_g, uint32_t elapsed_s) {
  if (current_ui_state != UIState::ACTIVE)
    return;

  if (weight_label) {
    lv_label_set_text_fmt(weight_label, "%.1fg", weight_g);
  }
  if (time_label) {
    lv_label_set_text_fmt(time_label, "%luts", (unsigned long)elapsed_s);
  }
  if (chart && ser) {
    lv_chart_set_next_value(chart, ser, (lv_coord_t)weight_g);
  }
}

void ui_set_boot_status(const char *status, int progress_pct) {
  if (current_ui_state != UIState::BOOT)
    return;
  if (status_label) {
    lv_label_set_text_fmt(status_label, "UroFlow v1.0.0\n\n%s\n[%d%%]", status,
                          progress_pct);
  }
}

void ui_set_sync_status(const char *message, bool is_error) {
  if (current_ui_state != UIState::SYNCING &&
      current_ui_state != UIState::SUCCESS)
    return;

  if (status_label) {
    lv_label_set_text(status_label, message);
    if (is_error) {
      lv_obj_set_style_bg_color(main_screen, lv_color_hex(0xFF0000), 0);
      lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_WHITE),
                                  0);
    } else {
      lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_GREEN), 0);
      lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK),
                                  0);
    }
  }
}
