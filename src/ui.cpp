#include "ui.h"
#include "chart_types.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(space_grotesk_48);
LV_FONT_DECLARE(space_grotesk_24);
LV_FONT_DECLARE(jetbrains_mono_14);

static lv_obj_t *main_screen = nullptr;
static UIState current_ui_state = (UIState)-1;

// Shared refs
static lv_obj_t *status_label = nullptr;

// Boot refs
static lv_obj_t *boot_title_label = nullptr;
static lv_obj_t *boot_count_label = nullptr;
static lv_obj_t *boot_bar = nullptr;
static lv_obj_t *boot_seconds_label = nullptr;
static lv_obj_t *boot_wifi_value = nullptr;

// Active refs
static lv_obj_t *weight_label = nullptr;
static lv_obj_t *time_label = nullptr;
static lv_obj_t *points_label = nullptr;
static lv_obj_t *chart = nullptr;
static lv_chart_series_t *ser = nullptr;
static lv_obj_t *end_overlay = nullptr;
static lv_obj_t *end_count_label = nullptr;

// Success refs
static lv_obj_t *success_count_label = nullptr;
static lv_obj_t *success_bar = nullptr;

// Callbacks for main.cpp to handle
static void (*on_home_clicked)() = nullptr;
static void (*on_start_clicked)() = nullptr;
static void (*on_end_clicked)() = nullptr;

static char s_boot_wifi[64] = "MONGOFLO-LAB-5G";

// Manual mode state
static bool s_manual_mode = false;
static void (*on_manual_mode_toggled)(bool) = nullptr;
static lv_obj_t *boot_manual_value = nullptr;
static lv_obj_t *ready_sub_label = nullptr;

static void clear_refs() {
  status_label = nullptr;
  boot_title_label = nullptr;
  boot_count_label = nullptr;
  boot_bar = nullptr;
  boot_seconds_label = nullptr;
  boot_wifi_value = nullptr;

  weight_label = nullptr;
  time_label = nullptr;
  points_label = nullptr;
  chart = nullptr;
  ser = nullptr;
  end_overlay = nullptr;
  end_count_label = nullptr;

  success_count_label = nullptr;
  success_bar = nullptr;
  boot_manual_value = nullptr;
  ready_sub_label = nullptr;
}

// Animation callback for indeterminate boot progress bar
static void boot_bar_anim_cb(void *bar, int32_t v) {
  lv_bar_set_value((lv_obj_t *)bar, v, LV_ANIM_OFF);
}

static void style_surface(lv_obj_t *obj, lv_color_t bg, lv_color_t border) {
  lv_obj_set_style_bg_color(obj, bg, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(obj, border, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_radius(obj, 4, 0);
  lv_obj_set_style_pad_all(obj, 16, 0);
}

static void apply_mono_style(lv_obj_t *obj, lv_color_t color) {
  lv_obj_set_style_text_font(obj, &jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(obj, color, 0);
  lv_obj_set_style_text_letter_space(obj, 4, 0);
}

static lv_obj_t *create_logo(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             const lv_font_t *font) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 0, 0);

  lv_obj_t *m = lv_label_create(row);
  lv_label_set_text(m, "Mongo");
  lv_obj_set_style_text_color(m, lv_color_hex(UI_COLOR_WHITE), 0);
  lv_obj_set_style_text_font(m, font, 0);

  lv_obj_t *f = lv_label_create(row);
  lv_label_set_text(f, "Flo");
  lv_obj_set_style_text_color(f, lv_color_hex(UI_COLOR_GREEN), 0);
  lv_obj_set_style_text_font(f, font, 0);

  lv_obj_align(row, LV_ALIGN_TOP_LEFT, x, y);
  return row;
}

static void event_handler(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;

  lv_obj_t *obj = lv_event_get_target(e);
  if (on_home_clicked && lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) {
    on_home_clicked();
  } else if (on_start_clicked && lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_2)) {
    on_start_clicked();
  } else if (on_end_clicked && lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_3)) {
    on_end_clicked();
  } else if (lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_4)) {
    // Manual mode toggle
    s_manual_mode = !s_manual_mode;
    if (boot_manual_value) {
      lv_label_set_text(boot_manual_value, s_manual_mode ? "ON" : "OFF");
      lv_obj_set_style_text_color(boot_manual_value,
          lv_color_hex(s_manual_mode ? UI_COLOR_GREEN : UI_COLOR_WHITE), 0);
    }
    if (on_manual_mode_toggled) {
      on_manual_mode_toggled(s_manual_mode);
    }
  }
}

static lv_obj_t *create_metric_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                    const char *title, const char *value,
                                    const char *symbol,
                                    lv_obj_t **out_value_label) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 350, 108);
  style_surface(card, lv_color_hex(UI_COLOR_CARD), lv_color_hex(UI_COLOR_GRAY));
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);

  lv_obj_t *icon_bg = lv_obj_create(card);
  lv_obj_set_size(icon_bg, 40, 40);
  lv_obj_set_style_radius(icon_bg, 4, 0);
  lv_obj_set_style_border_width(icon_bg, 0, 0);
  lv_obj_set_style_bg_color(icon_bg, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(icon_bg, 0, 0);
  lv_obj_clear_flag(icon_bg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(icon_bg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(icon_bg, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *icon = lv_label_create(icon_bg);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_GREEN), 0);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
  lv_obj_center(icon);

  lv_obj_t *t = lv_label_create(card);
  lv_label_set_text(t, title);
  apply_mono_style(t, lv_color_hex(UI_COLOR_MUTED));
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 54, 8);

  lv_obj_t *v = lv_label_create(card);
  lv_label_set_text(v, value);
  lv_obj_set_style_text_color(v, lv_color_hex(UI_COLOR_WHITE), 0);
  lv_obj_set_style_text_font(v, &space_grotesk_24, 0);
  lv_obj_set_width(v, strcmp(title, "CURRENT NETWORK") == 0 ? 120 : 230);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  lv_obj_align(v, LV_ALIGN_TOP_LEFT, 54, 34);
  if (out_value_label) {
    *out_value_label = v;
  }

  // Right-side chevron on export card
  if (strcmp(title, "MEASUREMENT") == 0) {
    lv_obj_t *chev = lv_label_create(card);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(chev, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);
  }

  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, event_handler, LV_EVENT_CLICKED, nullptr);
  return card;
}

void ui_init() {
  main_screen = lv_scr_act();
  lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_BLACK), 0);
  lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, 0);
  ui_set_state(UIState::BOOT);
}

void ui_set_state(UIState state) {
  if (current_ui_state == state)
    return;

  current_ui_state = state;
  lv_obj_clean(main_screen);
  clear_refs();

  switch (state) {
  case UIState::BOOT: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_BLACK), 0);

    create_logo(main_screen, 20, 18, &space_grotesk_48);

    lv_obj_t *tag = lv_label_create(main_screen);
    lv_label_set_text(tag, "FLOW RATE LOGGER V2.1");
    apply_mono_style(tag, lv_color_hex(UI_COLOR_MUTED));
    lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 20, 72);

    lv_obj_t *wifi_card = create_metric_card(main_screen, 20, 116,
                                             "CURRENT NETWORK", s_boot_wifi,
                                             LV_SYMBOL_WIFI, &boot_wifi_value);
    lv_obj_add_flag(wifi_card, LV_OBJ_FLAG_USER_1);

    lv_obj_t *export_card = create_metric_card(main_screen, 20, 238,
                                               "MEASUREMENT",
                                               "Begin New Measurement",
                                               LV_SYMBOL_PLAY, nullptr);
    lv_obj_add_flag(export_card, LV_OBJ_FLAG_USER_2);

    lv_obj_t *manual_card = create_metric_card(main_screen, 20, 360,
                                                "MANUAL MODE",
                                                s_manual_mode ? "ON" : "OFF",
                                                LV_SYMBOL_SETTINGS, &boot_manual_value);
    lv_obj_add_flag(manual_card, LV_OBJ_FLAG_USER_4);
    if (boot_manual_value) {
      lv_obj_set_style_text_color(boot_manual_value,
          lv_color_hex(s_manual_mode ? UI_COLOR_GREEN : UI_COLOR_WHITE), 0);
    }

    lv_obj_t *countdown = lv_obj_create(main_screen);
    lv_obj_set_size(countdown, 390, 320);
    style_surface(countdown, lv_color_hex(UI_COLOR_CARD),
                  lv_color_hex(UI_COLOR_GRAY));
    lv_obj_align(countdown, LV_ALIGN_TOP_RIGHT, -20, 116);

    boot_title_label = lv_label_create(countdown);
    lv_label_set_text(boot_title_label, "SCALE CONNECTION");
    apply_mono_style(boot_title_label, lv_color_hex(UI_COLOR_MUTED));
    lv_obj_align(boot_title_label, LV_ALIGN_TOP_MID, 0, 26);

    // Main status text (replaces countdown number)
    boot_count_label = lv_label_create(countdown);
    lv_label_set_text(boot_count_label, "Scanning for scale...");
    lv_obj_set_style_text_color(boot_count_label, lv_color_hex(UI_COLOR_WHITE),
                                0);
    lv_obj_set_style_text_font(boot_count_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(boot_count_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(boot_count_label, 340);
    lv_obj_align(boot_count_label, LV_ALIGN_CENTER, 0, -16);

    // Hint text (shown during scanning)
    boot_seconds_label = lv_label_create(countdown);
    lv_label_set_text(boot_seconds_label, "Turn on your Acaia Pearl S");
    apply_mono_style(boot_seconds_label, lv_color_hex(UI_COLOR_MUTED));
    lv_obj_set_style_text_align(boot_seconds_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(boot_seconds_label, 340);
    lv_obj_align(boot_seconds_label, LV_ALIGN_CENTER, 0, 40);

    // Indeterminate scanning progress bar
    boot_bar = lv_bar_create(countdown);
    lv_obj_set_size(boot_bar, 300, 6);
    lv_bar_set_range(boot_bar, 0, 100);
    lv_bar_set_value(boot_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(boot_bar, lv_color_hex(UI_COLOR_GRAY),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(boot_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(boot_bar, lv_color_hex(UI_COLOR_GREEN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(boot_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_align(boot_bar, LV_ALIGN_BOTTOM_MID, 0, -30);

    // Start indeterminate sweep animation
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, boot_bar);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 1500);
    lv_anim_set_playback_time(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, boot_bar_anim_cb);
    lv_anim_start(&a);

    lv_obj_t *f1 = lv_label_create(main_screen);
    lv_label_set_text(f1, "ESP32-S3 • LVGL 8.3 • 800x480");
    apply_mono_style(f1, lv_color_hex(0x5A5A5A));
    lv_obj_align(f1, LV_ALIGN_BOTTOM_LEFT, 20, -10);

    lv_obj_t *f2 = lv_label_create(main_screen);
    lv_label_set_text(f2, "© 2026 MongoFlo Instruments");
    apply_mono_style(f2, lv_color_hex(0x5A5A5A));
    lv_obj_align(f2, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    break;
  }

  case UIState::READY: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_GREEN), 0);

    lv_obj_t *tap_layer = lv_btn_create(main_screen);
    lv_obj_set_size(tap_layer, 800, 480);
    lv_obj_set_style_bg_opa(tap_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_layer, 0, 0);
    lv_obj_set_style_shadow_width(tap_layer, 0, 0);
    lv_obj_align(tap_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(tap_layer, LV_OBJ_FLAG_USER_2);
    lv_obj_add_event_cb(tap_layer, event_handler, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *home_btn = lv_btn_create(main_screen);
    lv_obj_set_size(home_btn, 44, 44);
    lv_obj_set_style_radius(home_btn, 4, 0);
    lv_obj_set_style_bg_color(home_btn, lv_color_hex(0x11B851), 0);
    lv_obj_set_style_border_width(home_btn, 0, 0);
    lv_obj_align(home_btn, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_add_flag(home_btn, LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(home_btn, event_handler, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *home = lv_label_create(home_btn);
    lv_label_set_text(home, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_center(home);

    lv_obj_t *outer = lv_obj_create(main_screen);
    lv_obj_set_size(outer, 96, 96);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(outer, lv_color_hex(0x11B851), 0);
    lv_obj_set_style_border_width(outer, 4, 0);
    lv_obj_set_style_shadow_width(outer, 0, 0);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(outer, LV_ALIGN_CENTER, 0, -120);

    lv_obj_t *inner = lv_obj_create(outer);
    lv_obj_set_size(inner, 40, 40);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_border_width(inner, 0, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(inner);

    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "Scale Connected");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);

    ready_sub_label = lv_label_create(main_screen);
    if (s_manual_mode) {
      lv_label_set_text(ready_sub_label,
                        "Manual Mode Active\nTap anywhere to begin measurement");
    } else {
      lv_label_set_text(ready_sub_label,
                        "Add weight to the scale\nto begin measurement automatically");
    }
    lv_obj_set_style_text_font(ready_sub_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ready_sub_label, lv_color_hex(0x13351F), 0);
    lv_obj_set_style_text_align(ready_sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ready_sub_label, LV_ALIGN_CENTER, 0, 48);

    lv_obj_t *tap = lv_label_create(main_screen);
    lv_label_set_text(tap, s_manual_mode ? "Auto-start disabled" : "Tap anywhere to begin manually");
    lv_obj_set_style_text_font(tap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tap, lv_color_hex(0x1D4E2D), 0);
    lv_obj_align(tap, LV_ALIGN_BOTTOM_MID, 0, -62);

    lv_obj_t *f1 = lv_label_create(main_screen);
    lv_label_set_text(f1, "MongoFlo • Ready");
    lv_obj_set_style_text_color(f1, lv_color_hex(0x1D4E2D), 0);
    lv_obj_set_style_text_font(f1, &lv_font_montserrat_14, 0);
    lv_obj_align(f1, LV_ALIGN_BOTTOM_LEFT, 20, -10);

    lv_obj_t *f2 = lv_label_create(main_screen);
    lv_label_set_text(f2, "SCALE ONLINE");
    lv_obj_set_style_text_color(f2, lv_color_hex(0x1D4E2D), 0);
    lv_obj_set_style_text_font(f2, &lv_font_montserrat_14, 0);
    lv_obj_align(f2, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    break;
  }

  case UIState::ACTIVE: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_BLACK), 0);

    lv_obj_t *home_btn = lv_btn_create(main_screen);
    lv_obj_set_size(home_btn, 34, 34);
    lv_obj_set_style_radius(home_btn, 4, 0);
    lv_obj_set_style_bg_color(home_btn, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_border_color(home_btn, lv_color_hex(UI_COLOR_GRAY), 0);
    lv_obj_set_style_border_width(home_btn, 1, 0);
    lv_obj_align(home_btn, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_add_flag(home_btn, LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(home_btn, event_handler, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *home = lv_label_create(home_btn);
    lv_label_set_text(home, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_center(home);

    create_logo(main_screen, 148, 8, &lv_font_montserrat_24);

    lv_obj_t *recording = lv_label_create(main_screen);
    lv_label_set_text(recording, "RECORDING");
    lv_obj_set_style_text_color(recording, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(recording, &lv_font_montserrat_14, 0);
    lv_obj_align(recording, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *elapsed_title = lv_label_create(main_screen);
    lv_label_set_text(elapsed_title, "ELAPSED");
    lv_obj_set_style_text_color(elapsed_title, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(elapsed_title, &lv_font_montserrat_14, 0);
    lv_obj_align(elapsed_title, LV_ALIGN_TOP_RIGHT, -208, 12);

    time_label = lv_label_create(main_screen);
    lv_label_set_text(time_label, "0s");
    lv_obj_set_style_text_color(time_label, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_RIGHT, -206, 28);

    lv_obj_t *current_title = lv_label_create(main_screen);
    lv_label_set_text(current_title, "NET");
    lv_obj_set_style_text_color(current_title, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(current_title, &lv_font_montserrat_14, 0);
    lv_obj_align(current_title, LV_ALIGN_TOP_RIGHT, -76, 12);

    weight_label = lv_label_create(main_screen);
    lv_label_set_text(weight_label, "0.0g");
    lv_obj_set_style_text_color(weight_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(weight_label, &lv_font_montserrat_32, 0);
    lv_obj_align(weight_label, LV_ALIGN_TOP_RIGHT, -72, 28);

    chart = lv_chart_create(main_screen);
    lv_obj_set_size(chart, 776, 362);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 300);
    lv_chart_set_point_count(chart, CHART_BUF_SIZE);
    lv_chart_set_div_line_count(chart, 6, 8);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x050505), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(UI_COLOR_GRAY), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(chart, 4, 0);
    ser = lv_chart_add_series(chart, lv_color_hex(UI_COLOR_GREEN),
                              LV_CHART_AXIS_PRIMARY_Y);
    // Initialize all points to NONE to prevent stale buffer rendering as artifacts
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);

    points_label = lv_label_create(main_screen);
    lv_label_set_text(points_label, "0 data points");
    lv_obj_set_style_text_color(points_label, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_text_font(points_label, &lv_font_montserrat_14, 0);
    lv_obj_align(points_label, LV_ALIGN_BOTTOM_RIGHT, -20, -12);

    lv_obj_t *end_btn = lv_btn_create(main_screen);
    lv_obj_set_size(end_btn, 80, 34);
    lv_obj_set_style_radius(end_btn, 4, 0);
    lv_obj_set_style_bg_color(end_btn, lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_border_width(end_btn, 0, 0);
    lv_obj_align(end_btn, LV_ALIGN_TOP_LEFT, 54, 10);
    lv_obj_add_flag(end_btn, LV_OBJ_FLAG_USER_3);
    lv_obj_add_event_cb(end_btn, event_handler, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *end_lbl = lv_label_create(end_btn);
    lv_label_set_text(end_lbl, "END");
    lv_obj_set_style_text_color(end_lbl, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(end_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(end_lbl);

    end_overlay = lv_obj_create(main_screen);
    lv_obj_set_size(end_overlay, 800, 480);
    lv_obj_align(end_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(end_overlay, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_bg_opa(end_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(end_overlay, 0, 0);
    lv_obj_set_style_pad_all(end_overlay, 0, 0);
    lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *end_a = lv_label_create(end_overlay);
    lv_label_set_text(end_a, "NO WEIGHT CHANGE DETECTED");
    lv_obj_set_style_text_color(end_a, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(end_a, &lv_font_montserrat_14, 0);
    lv_obj_align(end_a, LV_ALIGN_CENTER, 0, -58);

    lv_obj_t *end_b = lv_label_create(end_overlay);
    lv_label_set_text(end_b, "Measurement ending in");
    lv_obj_set_style_text_color(end_b, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(end_b, &lv_font_montserrat_24, 0);
    lv_obj_align(end_b, LV_ALIGN_CENTER, 0, -24);

    end_count_label = lv_label_create(end_overlay);
    lv_label_set_text(end_count_label, "5");
    lv_obj_set_style_text_color(end_count_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(end_count_label, &lv_font_montserrat_48, 0);
    lv_obj_align(end_count_label, LV_ALIGN_CENTER, 0, 24);
    break;
  }

  case UIState::SUCCESS: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_GREEN), 0);

    lv_obj_t *home_btn = lv_btn_create(main_screen);
    lv_obj_set_size(home_btn, 44, 44);
    lv_obj_set_style_radius(home_btn, 4, 0);
    lv_obj_set_style_bg_color(home_btn, lv_color_hex(0x11B851), 0);
    lv_obj_set_style_border_width(home_btn, 0, 0);
    lv_obj_align(home_btn, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_add_flag(home_btn, LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(home_btn, event_handler, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *home = lv_label_create(home_btn);
    lv_label_set_text(home, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_center(home);

    lv_obj_t *ring = lv_obj_create(main_screen);
    lv_obj_set_size(ring, 90, 90);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x143A23), 0);
    lv_obj_set_style_border_width(ring, 4, 0);
    lv_obj_set_style_shadow_width(ring, 0, 0);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -136);
    lv_obj_t *ok = lv_label_create(ring);
    lv_label_set_text(ok, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(ok, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(ok, &lv_font_montserrat_32, 0);
    lv_obj_center(ok);

    lv_obj_t *title = lv_label_create(main_screen);
    lv_label_set_text(title, "Measurement Collected\nSuccessfully");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -28);

    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "Syncing with cloud...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x143A23), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 72);

    success_count_label = lv_label_create(main_screen);
    lv_label_set_text(success_count_label, "");
    lv_obj_set_style_text_color(success_count_label, lv_color_hex(UI_COLOR_BLACK),
                                0);
    lv_obj_set_style_text_font(success_count_label, &lv_font_montserrat_48, 0);
    lv_obj_align(success_count_label, LV_ALIGN_CENTER, -8, 132);
    lv_obj_add_flag(success_count_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *seconds = lv_label_create(main_screen);
    lv_label_set_text(seconds, "");
    lv_obj_set_style_text_color(seconds, lv_color_hex(0x1D4E2D), 0);
    lv_obj_set_style_text_font(seconds, &lv_font_montserrat_14, 0);
    lv_obj_align(seconds, LV_ALIGN_CENTER, 70, 143);
    lv_obj_add_flag(seconds, LV_OBJ_FLAG_HIDDEN);

    success_bar = lv_bar_create(main_screen);
    lv_obj_set_size(success_bar, 260, 6);
    lv_bar_set_range(success_bar, 0, 100);
    lv_bar_set_value(success_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(success_bar, lv_color_hex(0x39A566), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(success_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(success_bar, lv_color_hex(UI_COLOR_BLACK),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(success_bar, LV_OPA_30, LV_PART_INDICATOR);
    lv_obj_align(success_bar, LV_ALIGN_CENTER, 0, 180);

    lv_obj_t *f1 = lv_label_create(main_screen);
    lv_label_set_text(f1, "MongoFlo • Data saved locally");
    lv_obj_set_style_text_color(f1, lv_color_hex(0x1D4E2D), 0);
    lv_obj_set_style_text_font(f1, &lv_font_montserrat_14, 0);
    lv_obj_align(f1, LV_ALIGN_BOTTOM_LEFT, 20, -10);

    lv_obj_t *f2 = lv_label_create(main_screen);
    lv_label_set_text(f2, "© 2026 MongoFlo Instruments");
    lv_obj_set_style_text_color(f2, lv_color_hex(0x1D4E2D), 0);
    lv_obj_set_style_text_font(f2, &lv_font_montserrat_14, 0);
    lv_obj_align(f2, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    break;
  }

  case UIState::SYNCING: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_CARD), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "SYNCING TO CLOUD...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *hint = lv_label_create(main_screen);
    lv_label_set_text(hint, "Please keep device powered");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_24, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 36);
    break;
  }

  case UIState::ERROR: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_DANGER), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "Measurement Failed");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *desc = lv_label_create(main_screen);
    lv_label_set_text(desc,
                      "Data could not be collected.\nCheck scale connection.");
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(desc, lv_color_hex(0xF8D8D9), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_24, 0);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, 44);
    break;
  }
  }

  // Force a full repaint right after structural screen changes.
  lv_obj_invalidate(main_screen);
}

void ui_update_weight(float weight_g, uint32_t elapsed_s, int ending_countdown_s,
                      uint32_t data_points) {
  if (current_ui_state != UIState::ACTIVE)
    return;

  if (weight_label) {
    lv_label_set_text_fmt(weight_label, "%.1fg", weight_g);
  }
  if (time_label) {
    if (elapsed_s < 60) {
      lv_label_set_text_fmt(time_label, "%lus", (unsigned long)elapsed_s);
    } else {
      lv_label_set_text_fmt(time_label, "%lu:%02lu",
                            (unsigned long)(elapsed_s / 60),
                            (unsigned long)(elapsed_s % 60));
    }
  }
  if (points_label) {
    lv_label_set_text_fmt(points_label, "%lu data points",
                          (unsigned long)data_points);
  }
  if (chart && ser) {
    lv_chart_set_next_value(chart, ser, (lv_coord_t)weight_g);
  }

  if (end_overlay && end_count_label) {
    if (ending_countdown_s > 0) {
      lv_obj_clear_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(end_count_label, "%d", ending_countdown_s);
    } else {
      lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void ui_set_boot_status(const char *status, int progress_pct) {
  if (current_ui_state != UIState::BOOT)
    return;

  // Update the main status text
  if (boot_count_label && status) {
    lv_label_set_text(boot_count_label, status);
  }

  if (progress_pct > 0) {
    // Connected state: green text, hide hint, stop animation, fill bar
    if (boot_count_label) {
      lv_obj_set_style_text_color(boot_count_label,
                                  lv_color_hex(UI_COLOR_GREEN), 0);
    }
    if (boot_seconds_label) {
      lv_obj_add_flag(boot_seconds_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (boot_bar) {
      lv_anim_del(boot_bar, boot_bar_anim_cb);
      lv_bar_set_value(boot_bar, 100, LV_ANIM_ON);
    }
  } else {
    // Scanning state: white text, show hint, animation continues
    if (boot_count_label) {
      lv_obj_set_style_text_color(boot_count_label,
                                  lv_color_hex(UI_COLOR_WHITE), 0);
    }
    if (boot_seconds_label) {
      lv_obj_clear_flag(boot_seconds_label, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void ui_set_boot_network(const char *ssid) {
  if (!ssid || strlen(ssid) == 0)
    return;

  strncpy(s_boot_wifi, ssid, sizeof(s_boot_wifi) - 1);
  s_boot_wifi[sizeof(s_boot_wifi) - 1] = '\0';

  if (boot_wifi_value) {
    lv_label_set_text(boot_wifi_value, s_boot_wifi);
  }
}

void ui_set_sync_status(const char *message, bool is_error) {
  if (current_ui_state != UIState::SYNCING &&
      current_ui_state != UIState::SUCCESS) {
    return;
  }

  if (status_label && message) {
    lv_color_t tone = lv_color_hex(UI_COLOR_GREEN);
    if (is_error) {
      tone = lv_color_hex(UI_COLOR_DANGER);
    } else if (current_ui_state == UIState::SUCCESS) {
      tone = lv_color_hex(UI_COLOR_BLACK);
    }
    lv_label_set_text(status_label, message);
    lv_obj_set_style_text_color(status_label, tone, 0);
  }

  if (success_bar && current_ui_state == UIState::SUCCESS) {
    int progress = 60; // active sync by default
    if (is_error) {
      progress = 30;
    } else if (message && strstr(message, "complete")) {
      progress = 100;
    }
    lv_bar_set_value(success_bar, progress, LV_ANIM_OFF);
  }
}

void ui_set_home_cb(void (*cb)()) { on_home_clicked = cb; }
void ui_set_start_cb(void (*cb)()) { on_start_clicked = cb; }
void ui_set_end_cb(void (*cb)()) { on_end_clicked = cb; }
void ui_set_manual_mode_cb(void (*cb)(bool on)) { on_manual_mode_toggled = cb; }
UIState ui_get_state() { return current_ui_state; }

void ui_set_manual_mode(bool on) {
  s_manual_mode = on;
  if (boot_manual_value) {
    lv_label_set_text(boot_manual_value, on ? "ON" : "OFF");
    lv_obj_set_style_text_color(boot_manual_value,
        lv_color_hex(on ? UI_COLOR_GREEN : UI_COLOR_WHITE), 0);
  }
}

bool ui_get_manual_mode() { return s_manual_mode; }
