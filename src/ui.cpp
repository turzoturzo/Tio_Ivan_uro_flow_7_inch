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
static lv_obj_t *boot_title_label = nullptr;
static lv_obj_t *boot_bar = nullptr;
static lv_obj_t *sync_panel = nullptr;

static UIState current_ui_state = (UIState)-1;

// Callbacks for main.cpp to handle
static void (*on_home_clicked)() = nullptr;
static void (*on_start_clicked)() = nullptr;

static void event_handler(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    lv_obj_t *obj = lv_event_get_target(e);
    if (on_home_clicked && lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) {
      on_home_clicked();
    } else if (on_start_clicked && lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_2)) {
      on_start_clicked();
    }
  }
}

// Helper to create the branded "MongoFlo" logo
static void create_logo(lv_obj_t *parent) {
  lv_obj_t *cont = lv_obj_create(parent);
  lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(cont, 0, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *l1 = lv_label_create(cont);
  lv_label_set_text(l1, "Mongo");
  lv_obj_set_style_text_color(l1, lv_color_hex(UI_COLOR_WHITE), 0);
  lv_obj_set_style_text_font(l1, &lv_font_montserrat_48, 0); // Bolder/Larger

  lv_obj_t *l2 = lv_label_create(cont);
  lv_label_set_text(l2, "Flo");
  lv_obj_set_style_text_color(l2, lv_color_hex(UI_COLOR_GREEN), 0);
  lv_obj_set_style_text_font(l2, &lv_font_montserrat_48, 0);

  lv_obj_t *sub = lv_label_create(parent);
  lv_label_set_text(sub, "FLOW RATE LOGGER V2.1");
  lv_obj_set_style_text_color(sub, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 20, 65);

  lv_obj_align(cont, LV_ALIGN_TOP_LEFT, 20, 20);
}

// Helper to create a card
static lv_obj_t *create_card(lv_obj_t *parent, int w, int h, const char *title,
                             const char *val, const char *symbol) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, w, h);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x111111), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x222222), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 4, 0);
  lv_obj_set_style_pad_all(card, 15, 0);

  if (symbol) {
    lv_obj_t *icon_box = lv_obj_create(card);
    lv_obj_set_size(icon_box, 40, 40);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_radius(icon_box, 4, 0);
    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_center(icon);
  }

  lv_obj_t *t_lbl = lv_label_create(card);
  lv_label_set_text(t_lbl, title);
  lv_obj_set_style_text_color(t_lbl, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(t_lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(t_lbl, LV_ALIGN_TOP_LEFT, 55, 0);

  lv_obj_t *v_lbl = lv_label_create(card);
  lv_label_set_text(v_lbl, val);
  lv_obj_set_style_text_color(v_lbl, lv_color_hex(UI_COLOR_WHITE), 0);
  lv_obj_set_style_text_font(v_lbl, &lv_font_montserrat_24, 0);
  lv_obj_align(v_lbl, LV_ALIGN_TOP_LEFT, 55, 20);

  // Add a clickable surface or button to the card
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, event_handler, LV_EVENT_CLICKED, nullptr);

  return card;
}

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

    create_logo(main_screen);

    lv_obj_t *ready = lv_label_create(main_screen);
    lv_label_set_text(ready, "● SYSTEM READY");
    lv_obj_set_style_text_color(ready, lv_color_hex(0x888888), 0);
    lv_obj_align(ready, LV_ALIGN_TOP_RIGHT, -20, 30);

    // Left Cards
    lv_obj_t *wifi_card = create_card(main_screen, 340, 110, "CURRENT NETWORK",
                                      "MONGOFLO-LAB-5G", LV_SYMBOL_WIFI);
    lv_obj_align(wifi_card, LV_ALIGN_TOP_LEFT, 20, 140);
    lv_obj_add_flag(wifi_card, LV_OBJ_FLAG_USER_1);

    lv_obj_t *export_card =
        create_card(main_screen, 340, 110, "DATA EXPORT",
                    "Export Files Manually", LV_SYMBOL_DOWNLOAD);
    lv_obj_align(export_card, LV_ALIGN_TOP_LEFT, 20, 260);
    lv_obj_add_flag(export_card, LV_OBJ_FLAG_USER_2);

    // Right Connection Box
    lv_obj_t *conn_box = lv_obj_create(main_screen);
    lv_obj_set_size(conn_box, 380, 420);
    lv_obj_set_style_bg_opa(conn_box, 0, 0);
    lv_obj_set_style_border_color(conn_box, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(conn_box, 1, 0);
    lv_obj_align(conn_box, LV_ALIGN_TOP_RIGHT, -20, 140);

    boot_title_label = lv_label_create(conn_box);
    lv_label_set_text(boot_title_label, "CONNECTING TO SCALE");
    lv_obj_set_style_text_color(boot_title_label, lv_color_hex(0x888888), 0);
    lv_obj_align(boot_title_label, LV_ALIGN_TOP_MID, 0, 80);

    status_label = lv_label_create(conn_box);
    lv_label_set_text(status_label, "5");
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48,
                               0); // Need even larger?
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);

    // Progress bar
    lv_obj_t *bar = lv_bar_create(conn_box);
    lv_obj_set_size(bar, 300, 4);
    lv_bar_set_value(bar, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_GREEN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -80);

    // Footer
    lv_obj_t *f1 = lv_label_create(main_screen);
    lv_label_set_text(f1, "ESP32-S3 • LVGL 8.3 • 800x480");
    lv_obj_set_style_text_color(f1, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(f1, &lv_font_montserrat_14, 0);
    lv_obj_align(f1, LV_ALIGN_BOTTOM_LEFT, 20, -10);

    lv_obj_t *f2 = lv_label_create(main_screen);
    lv_label_set_text(f2, "© 2026 MongoFlo Instruments");
    lv_obj_set_style_text_color(f2, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(f2, &lv_font_montserrat_14, 0);
    lv_obj_align(f2, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
  } break;

  case UIState::READY: {
    // Full Screen Green (Cyberpunk)
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x00E660), 0);

    // Home Icon Placeholder (Top Left)
    lv_obj_t *home = lv_label_create(main_screen);
    lv_label_set_text(home, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_align(home, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_add_flag(home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(home, LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(home, event_handler, LV_EVENT_CLICKED, nullptr);

    // Make whole screen clickable to start
    lv_obj_add_flag(main_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(main_screen, LV_OBJ_FLAG_USER_2);
    lv_obj_add_event_cb(main_screen, event_handler, LV_EVENT_CLICKED, nullptr);

    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "Scale Connected");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *sub = lv_label_create(main_screen);
    lv_label_set_text(sub, "Add Weight to Scale to Start");
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_32, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *tap = lv_label_create(main_screen);
    lv_label_set_text(tap, "TAP ANYWHERE TO START");
    lv_obj_set_style_text_color(tap, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_font(tap, &lv_font_montserrat_14, 0);
    lv_obj_align(tap, LV_ALIGN_BOTTOM_MID, 0, -60);
  } break;

  case UIState::ACTIVE: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_BLACK), 0);

    // Header
    create_logo(main_screen);

    header_label = lv_label_create(main_screen);
    lv_label_set_text(header_label, "●  RECORDING");
    lv_obj_set_style_text_color(header_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_align(header_label, LV_ALIGN_TOP_MID, 0, 30);

    // Home button in ACTIVE state too
    lv_obj_t *home = lv_label_create(main_screen);
    lv_label_set_text(home, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home, lv_color_hex(0x444444), 0);
    lv_obj_align(home, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_add_flag(home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(home, LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(home, event_handler, LV_EVENT_CLICKED, nullptr);

    // Metrics Container (Top Right)
    lv_obj_t *metrics_cont = lv_obj_create(main_screen);
    lv_obj_set_size(metrics_cont, 300, 100);
    lv_obj_set_style_bg_opa(metrics_cont, 0, 0);
    lv_obj_set_style_border_width(metrics_cont, 0, 0);
    lv_obj_align(metrics_cont, LV_ALIGN_TOP_RIGHT, -20, 10);

    lv_obj_t *elaps_lbl = lv_label_create(metrics_cont);
    lv_label_set_text(elaps_lbl, "ELAPSED");
    lv_obj_set_style_text_color(elaps_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(elaps_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(elaps_lbl, LV_ALIGN_TOP_LEFT, 50, 0);

    time_label = lv_label_create(metrics_cont);
    lv_label_set_text(time_label, "0s");
    lv_obj_set_style_text_color(time_label, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 80, 20);

    lv_obj_t *curr_lbl = lv_label_create(metrics_cont);
    lv_label_set_text(curr_lbl, "CURRENT");
    lv_obj_set_style_text_color(curr_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(curr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(curr_lbl, LV_ALIGN_TOP_LEFT, 180, 0);

    weight_label = lv_label_create(metrics_cont);
    lv_label_set_text(weight_label, "0.0g");
    lv_obj_set_style_text_color(weight_label, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(weight_label, &lv_font_montserrat_32, 0);
    lv_obj_align(weight_label, LV_ALIGN_TOP_LEFT, 180, 20);

    // Chart
    chart = lv_chart_create(main_screen);
    lv_obj_set_size(chart, 760, 360);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_point_count(chart, CHART_BUF_SIZE);

    lv_obj_set_style_bg_color(chart, lv_color_hex(0x050505), 0);
    lv_obj_set_style_border_width(chart, 0, 0);

    // Dark grid lines
    lv_obj_set_style_line_color(chart, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_chart_set_div_line_count(chart, 5, 10);

    ser = lv_chart_add_series(chart, lv_color_hex(UI_COLOR_GREEN),
                              LV_CHART_AXIS_PRIMARY_Y);
  } break;

  case UIState::SUCCESS: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x00E660), 0);

    lv_obj_t *check_cont = lv_obj_create(main_screen);
    lv_obj_set_size(check_cont, 80, 80);
    lv_obj_set_style_bg_opa(check_cont, 0, 0);
    lv_obj_set_style_border_color(check_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(check_cont, 2, 0);
    lv_obj_set_style_radius(check_cont, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(check_cont);
    lv_obj_set_y(check_cont, -140);

    lv_obj_t *check = lv_label_create(check_cont);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(check, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_32, 0);
    lv_obj_center(check);

    lv_obj_t *msg = lv_label_create(main_screen);
    lv_label_set_text(msg, "Measurement Collected\nSuccessfully");
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_48, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *sync = lv_label_create(main_screen);
    lv_label_set_text(sync, "Syncing with cloud & entering low power mode");
    lv_obj_set_style_text_color(sync, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(sync, &lv_font_montserrat_14, 0);
    lv_obj_align(sync, LV_ALIGN_CENTER, 0, 60);

    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "0 SECONDS");
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_32, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 120);

    // Footer
    lv_obj_t *f1 = lv_label_create(main_screen);
    lv_label_set_text(f1, "MongoFlo • Data saved locally");
    lv_obj_set_style_text_color(f1, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_font(f1, &lv_font_montserrat_14, 0);
    lv_obj_align(f1, LV_ALIGN_BOTTOM_LEFT, 20, -10);

    lv_obj_t *f2 = lv_label_create(main_screen);
    lv_label_set_text(f2, "© 2026 MongoFlo Instruments");
    lv_obj_set_style_text_color(f2, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_font(f2, &lv_font_montserrat_14, 0);
    lv_obj_align(f2, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
  } break;

  case UIState::SYNCING: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(UI_COLOR_GREEN), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text(status_label, "SYNCING TO CLOUD...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_BLACK), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
  } break;

  case UIState::ERROR: {
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x441111), 0);
    status_label = lv_label_create(main_screen);
    lv_label_set_text_fmt(status_label, "SYSTEM ERROR\nPLEASE RESTART");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
  } break;
  }
}

void ui_update_weight(float weight_g, uint32_t elapsed_s) {
  if (current_ui_state != UIState::ACTIVE)
    return;

  if (weight_label) {
    lv_label_set_text_fmt(weight_label, "%.1fg", weight_g);
  }
  if (time_label) {
    lv_label_set_text_fmt(time_label, "%lus", (unsigned long)elapsed_s);
  }
  if (chart && ser) {
    lv_chart_set_next_value(chart, ser, (lv_coord_t)weight_g);
  }
}

void ui_set_boot_status(const char *status, int progress_pct) {
  if (current_ui_state != UIState::BOOT)
    return;
  if (status_label) {
    static char buf[16];
    itoa(progress_pct, buf, 10);
    lv_label_set_text(status_label, buf);
  }
  if (boot_title_label && status) {
    lv_label_set_text(boot_title_label, status);
  }
  if (boot_bar) {
    // If progress_pct is in 0-10 recursive countdown (like setup loops)
    // Map 10-0 to 0-100% or just use as is.
    // For now, let's assume it's a percentage unless it's <= 10.
    if (progress_pct <= 10) {
      lv_bar_set_value(boot_bar, (10 - progress_pct) * 10, LV_ANIM_OFF);
    } else {
      lv_bar_set_value(boot_bar, progress_pct, LV_ANIM_OFF);
    }
  }
}

void ui_set_sync_status(const char *message, bool is_error) {
  if (current_ui_state != UIState::SYNCING &&
      current_ui_state != UIState::SUCCESS)
    return;
  if (status_label) {
    lv_label_set_text(status_label, message);
  }
}

void ui_set_home_cb(void (*cb)()) { on_home_clicked = cb; }
void ui_set_start_cb(void (*cb)()) { on_start_clicked = cb; }
