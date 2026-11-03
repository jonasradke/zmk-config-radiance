/*
 * Custom 128x32 status screen for the Radiance keyboards.
 *
 *   +--------------------------------------------+
 *   | BT 1 o     [C][S]              87% [####]  |
 *   | . . . . . . . . . . . . . . . . . . . . .  |
 *   | Default                       72 /\/\_/\   |
 *   +--------------------------------------------+
 *
 * Top: output (USB, or Bluetooth profile with a dot: filled = connected, hollow = not
 * connected, blinking = ready to pair), held modifiers, battery (fill animates while charging).
 * Battery detection: without USB the keyboard must be running on a battery, so the level is
 * shown. On USB a level below FULL_LEVEL means a battery is charging, so the level is shown with
 * the charging sweep. On USB at FULL_LEVEL or above there is either no battery (the charger then
 * reads as full) or a full one; in both cases a bolt "on USB power" icon replaces the gauge.
 * Bottom, by priority:
 *   1. volume bar for 1.5 s after a volume/mute key (estimated, see below)
 *   2. short message for 1.3 s (profile, USB/Bluetooth, Caps Lock)
 *   3. scrolling Fn cheat sheet while a non-default layer is active
 *   4. layer name (inverted while Caps Lock is on), WPM and a WPM graph
 * A splash with the keyboard name is shown for 1.8 s at startup.
 *
 * The host never reports its volume to the keyboard, so the volume is an estimate: every
 * volume key moves it by VOLUME_STEP (one key press on Windows). Turning to 0 % or 100 %
 * brings it back in sync, since both sides stop there.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/wpm.h>

#define SHOW_BATTERY IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#if SHOW_BATTERY
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

#define HID_INDICATOR_CAPS_LOCK BIT(1)

#define TICK_MS 50
#define SPLASH_MS 1800
#define TOAST_MS 1300
#define VOLUME_SHOW_MS 1500
#define STARTUP_QUIET_MS 3000 // no messages for the connection events while booting
#define VOLUME_STEP 2
#define WPM_POINTS 30
#define WPM_TEXT_W 24 // "199" in Montserrat 12 plus a gap
#define FULL_LEVEL 99

#if IS_ENABLED(CONFIG_SHIELD_RADIANCE_V2)
#define FN_HINTS                                                                                   \
    LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT " profile    " LV_SYMBOL_UP " " LV_SYMBOL_DOWN              \
                   " page    Bksp clear    Del USB/BT    Ins boot    Home Studio    End reset    " \
                   "Y < > |"
#else
#define FN_HINTS                                                                                   \
    LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT " profile    Bksp clear    Del USB/BT    Ins boot    "      \
                   "Home Studio    End reset"
#endif

/* ---------------------------------------------------------------- shared state */

struct status_state {
    struct zmk_endpoint_instance endpoint;
    bool ble_connected;
    bool ble_open;
    uint8_t battery;
    bool usb_powered;
    zmk_keymap_layer_index_t layer_index;
    const char *layer_name;
    bool caps_lock;
    uint8_t mods;
    int volume;
    bool muted;
    int64_t volume_until;
    int64_t toast_until;
    char toast[16];
};

// Written by the event listener (system work queue), read by the LVGL timer (display queue).
static struct status_state state = {.volume = 50};
K_MUTEX_DEFINE(state_mutex);

static void show_toast(const char *text) {
    if (k_uptime_get() < STARTUP_QUIET_MS) {
        return;
    }
    strncpy(state.toast, text, sizeof(state.toast) - 1);
    state.toast_until = k_uptime_get() + TOAST_MS;
    state.volume_until = 0;
}

static void refresh_state(void) {
    struct zmk_endpoint_instance endpoint = zmk_endpoints_selected();
    zmk_keymap_layer_index_t layer = zmk_keymap_highest_layer_active();

    if (endpoint.transport != state.endpoint.transport) {
        show_toast(endpoint.transport == ZMK_TRANSPORT_USB ? "USB" : "Bluetooth");
    } else if (endpoint.transport == ZMK_TRANSPORT_BLE &&
               endpoint.ble.profile_index != state.endpoint.ble.profile_index) {
        char text[sizeof(state.toast)];
        snprintf(text, sizeof(text), "Profile %u", (uint8_t)(endpoint.ble.profile_index + 1));
        show_toast(text);
    }

    state.endpoint = endpoint;
    state.ble_connected = zmk_ble_active_profile_is_connected();
    state.ble_open = zmk_ble_active_profile_is_open();
#if SHOW_BATTERY
    state.battery = zmk_battery_state_of_charge();
#endif
    state.usb_powered = zmk_usb_is_powered();
    state.layer_index = layer;
    state.layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(layer));
}

static void set_caps_lock(bool on) {
    if (on != state.caps_lock) {
        show_toast(on ? "CAPS ON" : "caps off");
    }
    state.caps_lock = on;
}

static void handle_keycode(const struct zmk_keycode_state_changed *ev) {
    if (ev->usage_page == HID_USAGE_KEY && ev->keycode >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
        ev->keycode <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI) {
        uint8_t bit = BIT(ev->keycode - HID_USAGE_KEY_KEYBOARD_LEFTCONTROL);
        state.mods = ev->state ? (state.mods | bit) : (state.mods & ~bit);
        return;
    }

    if (ev->usage_page != HID_USAGE_CONSUMER || !ev->state) {
        return;
    }

    switch (ev->keycode) {
    case HID_USAGE_CONSUMER_VOLUME_INCREMENT:
        state.volume = MIN(100, state.volume + VOLUME_STEP);
        state.muted = false;
        break;
    case HID_USAGE_CONSUMER_VOLUME_DECREMENT:
        state.volume = MAX(0, state.volume - VOLUME_STEP);
        state.muted = false;
        break;
    case HID_USAGE_CONSUMER_MUTE:
        state.muted = !state.muted;
        break;
    default:
        return;
    }
    state.volume_until = k_uptime_get() + VOLUME_SHOW_MS;
    state.toast_until = 0;
}

static int status_listener(const zmk_event_t *eh) {
    k_mutex_lock(&state_mutex, K_FOREVER);

    const struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);
    const struct zmk_hid_indicators_changed *ind = as_zmk_hid_indicators_changed(eh);

    if (kc != NULL) {
        handle_keycode(kc);
    } else if (ind != NULL) {
        set_caps_lock((ind->indicators & HID_INDICATOR_CAPS_LOCK) != 0);
    } else {
        refresh_state();
        set_caps_lock((zmk_hid_indicators_get_current_profile() & HID_INDICATOR_CAPS_LOCK) != 0);
    }

    k_mutex_unlock(&state_mutex);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(radiance_status, status_listener);
ZMK_SUBSCRIPTION(radiance_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_ble_active_profile_changed);
#if SHOW_BATTERY
ZMK_SUBSCRIPTION(radiance_status, zmk_battery_state_changed);
#endif
ZMK_SUBSCRIPTION(radiance_status, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_keycode_state_changed);

/* ---------------------------------------------------------------- LVGL objects */

enum bottom_mode { MODE_NONE, MODE_NORMAL, MODE_FN, MODE_TOAST, MODE_VOLUME };

static lv_color_t fg, bg;

static lv_obj_t *output_label, *status_dot, *mod_labels[4];
#if SHOW_BATTERY
static lv_obj_t *battery_box, *battery_label, *battery_fill, *usb_power_icon;
#endif
static lv_obj_t *normal_box, *layer_label, *wpm_label, *wpm_chart;
static lv_chart_series_t *wpm_series;
static lv_obj_t *fn_label, *toast_label;
static lv_obj_t *volume_box, *volume_icon, *volume_bar, *volume_label;
static lv_obj_t *splash;

static enum bottom_mode shown_mode = MODE_NONE;
static int wpm_history[WPM_POINTS];
static uint32_t ticks;

static void set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static lv_obj_t *plain_obj(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, fg, LV_PART_MAIN);
    return label;
}

static void set_inverted(lv_obj_t *label, bool inverted, int pad) {
    lv_obj_set_style_bg_opa(label, inverted ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(label, fg, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, inverted ? bg : fg, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(label, inverted ? pad : 0, LV_PART_MAIN);
}

/* ---------------------------------------------------------------- periodic update */

static void update_top(const struct status_state *s) {
    char text[16];

    if (s->endpoint.transport == ZMK_TRANSPORT_USB) {
        set_text(output_label, "USB");
        set_hidden(status_dot, true);
    } else {
        snprintf(text, sizeof(text), "BT %d", s->endpoint.ble.profile_index + 1);
        set_text(output_label, text);
        lv_obj_update_layout(output_label);
        lv_obj_set_pos(status_dot, lv_obj_get_width(output_label) + 3, 4);
        // filled = connected, hollow = paired but not connected, blinking = ready to pair
        lv_obj_set_style_bg_opa(status_dot, s->ble_connected ? LV_OPA_COVER : LV_OPA_TRANSP,
                                LV_PART_MAIN);
        set_hidden(status_dot, s->ble_open && (ticks / (400 / TICK_MS)) % 2);
    }

    // Ctrl, Alt, Shift, Win: left and right variants share one badge
    static const uint8_t mod_masks[4] = {0x11, 0x44, 0x22, 0x88};
    int x = 46;
    for (int i = 0; i < 4; i++) {
        bool held = (s->mods & mod_masks[i]) != 0;
        set_hidden(mod_labels[i], !held);
        if (held) {
            lv_obj_set_pos(mod_labels[i], x, 1);
            x += 11;
        }
    }

#if SHOW_BATTERY
    bool gauge = !s->usb_powered || s->battery < FULL_LEVEL;
    set_hidden(battery_box, !gauge);
    set_hidden(usb_power_icon, gauge);
    if (!gauge) {
        return;
    }

    snprintf(text, sizeof(text), "%d%%", s->battery);
    set_text(battery_label, text);
    int level = s->battery;
    if (s->usb_powered && s->battery < 100) {
        // sweep the fill from the current level to full while charging
        int span = 100 - s->battery + 20;
        level = MIN(100, s->battery + (int)((ticks * 4) % span));
    }
    lv_obj_set_width(battery_fill, 11 * level / 100);
#endif
}

static void update_normal(const struct status_state *s) {
    if (s->layer_name != NULL && s->layer_name[0] != '\0') {
        set_text(layer_label, s->layer_name);
    } else {
        char text[12];
        snprintf(text, sizeof(text), "Layer %d", s->layer_index);
        set_text(layer_label, text);
    }
    set_inverted(layer_label, s->caps_lock, 3);
}

static void update_volume(const struct status_state *s) {
    char text[8];
    set_text(volume_icon, s->muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_bar_set_value(volume_bar, s->muted ? 0 : s->volume, LV_ANIM_OFF);
    if (s->muted) {
        set_text(volume_label, "Mute");
    } else {
        snprintf(text, sizeof(text), "%d%%", s->volume);
        set_text(volume_label, text);
    }
}

static void sample_wpm(void) {
    int wpm = zmk_wpm_get_state();
    int max = 60;
    char text[8];

    memmove(wpm_history, wpm_history + 1, sizeof(wpm_history) - sizeof(wpm_history[0]));
    wpm_history[WPM_POINTS - 1] = wpm;
    for (int i = 0; i < WPM_POINTS; i++) {
        max = MAX(max, wpm_history[i]);
    }
    lv_chart_set_range(wpm_chart, LV_CHART_AXIS_PRIMARY_Y, 0, max);
    lv_chart_set_next_value(wpm_chart, wpm_series, wpm);

    snprintf(text, sizeof(text), "%d", wpm);
    set_text(wpm_label, text);
}

static void status_tick(lv_timer_t *timer) {
    struct status_state s;
    k_mutex_lock(&state_mutex, K_FOREVER);
    s = state;
    k_mutex_unlock(&state_mutex);

    ticks++;
    int64_t now = k_uptime_get();

    update_top(&s);

    enum bottom_mode mode = now < s.volume_until  ? MODE_VOLUME
                            : now < s.toast_until ? MODE_TOAST
                            : s.layer_index > 0   ? MODE_FN
                                                  : MODE_NORMAL;
    if (mode == MODE_VOLUME) {
        update_volume(&s);
    } else if (mode == MODE_TOAST) {
        set_text(toast_label, s.toast);
    } else if (mode == MODE_NORMAL) {
        update_normal(&s);
    }
    if (mode != shown_mode) {
        set_hidden(volume_box, mode != MODE_VOLUME);
        set_hidden(toast_label, mode != MODE_TOAST);
        set_hidden(fn_label, mode != MODE_FN);
        set_hidden(normal_box, mode != MODE_NORMAL);
        shown_mode = mode;
    }

    if (ticks % (1000 / TICK_MS) == 0) {
        sample_wpm();
    }
}

/* ---------------------------------------------------------------- startup splash */

static void splash_line_cb(void *obj, int32_t width) { lv_obj_set_width(obj, width); }

static void splash_done(lv_timer_t *timer) { lv_obj_add_flag(splash, LV_OBJ_FLAG_HIDDEN); }

static void create_splash(lv_obj_t *screen) {
    static char name[sizeof(CONFIG_ZMK_KEYBOARD_NAME)];
    strcpy(name, CONFIG_ZMK_KEYBOARD_NAME);

    splash = plain_obj(screen);
    lv_obj_set_size(splash, 128, 32);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(splash, bg, LV_PART_MAIN);

    // "Hannes' Radiance" -> small "Hannes'" above a large "Radiance"
    char *last_word = strrchr(name, ' ');
    if (last_word != NULL) {
        *last_word++ = '\0';
        lv_obj_t *first = make_label(splash, &lv_font_montserrat_12);
        lv_label_set_text_static(first, name);
        lv_obj_align(first, LV_ALIGN_TOP_MID, 0, -1);
        lv_obj_t *second = make_label(splash, &lv_font_montserrat_16);
        lv_label_set_text_static(second, last_word);
        lv_obj_align(second, LV_ALIGN_TOP_MID, 0, 12);
    } else {
        lv_obj_t *only = make_label(splash, &lv_font_montserrat_16);
        lv_label_set_text_static(only, name);
        lv_obj_align(only, LV_ALIGN_CENTER, 0, -2);
    }

    lv_obj_t *line = plain_obj(splash);
    lv_obj_set_size(line, 0, 1);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(line, fg, LV_PART_MAIN);
    lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, line);
    lv_anim_set_values(&a, 0, 110);
    lv_anim_set_time(&a, SPLASH_MS * 6 / 10);
    lv_anim_set_exec_cb(&a, splash_line_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_timer_t *timer = lv_timer_create(splash_done, SPLASH_MS, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

/* ---------------------------------------------------------------- screen */

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    fg = lv_obj_get_style_text_color(screen, LV_PART_MAIN);
    bg = lv_obj_get_style_bg_color(screen, LV_PART_MAIN);

    // top row
    output_label = make_label(screen, &lv_font_montserrat_12);
    lv_obj_set_pos(output_label, 0, -1);

    status_dot = plain_obj(screen);
    lv_obj_set_size(status_dot, 4, 4);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_dot, fg, LV_PART_MAIN);
    lv_obj_set_style_border_color(status_dot, fg, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_dot, 1, LV_PART_MAIN);

    static const char *mod_names[4] = {"C", "A", "S", "W"};
    for (int i = 0; i < 4; i++) {
        mod_labels[i] = make_label(screen, &lv_font_montserrat_10);
        lv_label_set_text_static(mod_labels[i], mod_names[i]);
        set_inverted(mod_labels[i], true, 2);
        lv_obj_add_flag(mod_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

#if SHOW_BATTERY
    battery_box = plain_obj(screen);
    lv_obj_set_size(battery_box, 70, 13);
    lv_obj_align(battery_box, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *battery_outline = plain_obj(battery_box);
    lv_obj_set_size(battery_outline, 15, 8);
    lv_obj_set_pos(battery_outline, 52, 2);
    lv_obj_set_style_border_color(battery_outline, fg, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_outline, 1, LV_PART_MAIN);

    lv_obj_t *battery_nub = plain_obj(battery_box);
    lv_obj_set_size(battery_nub, 2, 4);
    lv_obj_set_pos(battery_nub, 67, 4);
    lv_obj_set_style_bg_opa(battery_nub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_nub, fg, LV_PART_MAIN);

    battery_fill = plain_obj(battery_box);
    lv_obj_set_size(battery_fill, 0, 4);
    lv_obj_set_pos(battery_fill, 54, 4);
    lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_fill, fg, LV_PART_MAIN);

    battery_label = make_label(battery_box, &lv_font_montserrat_12);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -21, -1);

    usb_power_icon = make_label(screen, &lv_font_montserrat_12);
    lv_label_set_text_static(usb_power_icon, LV_SYMBOL_CHARGE);
    lv_obj_align(usb_power_icon, LV_ALIGN_TOP_RIGHT, 0, -1);
    lv_obj_add_flag(usb_power_icon, LV_OBJ_FLAG_HIDDEN);
#endif

    // dotted divider
    static lv_point_t divider_points[] = {{0, 0}, {127, 0}};
    lv_obj_t *divider = lv_line_create(screen);
    lv_line_set_points(divider, divider_points, 2);
    lv_obj_set_pos(divider, 0, 13);
    lv_obj_set_style_line_color(divider, fg, LV_PART_MAIN);
    lv_obj_set_style_line_width(divider, 1, LV_PART_MAIN);
    lv_obj_set_style_line_dash_width(divider, 1, LV_PART_MAIN);
    lv_obj_set_style_line_dash_gap(divider, 1, LV_PART_MAIN);

    // bottom: normal view
    normal_box = plain_obj(screen);
    lv_obj_set_size(normal_box, 128, 17);
    lv_obj_set_pos(normal_box, 0, 15);

    layer_label = make_label(normal_box, &lv_font_montserrat_14);
    lv_obj_align(layer_label, LV_ALIGN_LEFT_MID, 0, 0);

    wpm_chart = lv_chart_create(normal_box);
    lv_obj_remove_style_all(wpm_chart);
    lv_obj_set_size(wpm_chart, WPM_POINTS, 13);
    lv_obj_align(wpm_chart, LV_ALIGN_RIGHT_MID, -WPM_TEXT_W, 0); // room for up to 3 digits
    lv_chart_set_type(wpm_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(wpm_chart, WPM_POINTS);
    lv_chart_set_div_line_count(wpm_chart, 0, 0);
    lv_chart_set_range(wpm_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 60);
    lv_obj_set_style_line_width(wpm_chart, 1, LV_PART_ITEMS);
    lv_obj_set_style_size(wpm_chart, 0, LV_PART_INDICATOR);
    wpm_series = lv_chart_add_series(wpm_chart, fg, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(wpm_chart, wpm_series, 0);

    wpm_label = make_label(normal_box, &lv_font_montserrat_12);
    lv_label_set_text_static(wpm_label, "0");
    lv_obj_align(wpm_label, LV_ALIGN_RIGHT_MID, 0, 0);

    // bottom: Fn cheat sheet
    fn_label = make_label(screen, &lv_font_montserrat_12);
    lv_label_set_long_mode(fn_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(fn_label, 128);
    lv_obj_set_style_anim_speed(fn_label, 30, LV_PART_MAIN);
    lv_label_set_text_static(fn_label, FN_HINTS);
    lv_obj_set_pos(fn_label, 0, 17);
    lv_obj_add_flag(fn_label, LV_OBJ_FLAG_HIDDEN);

    // bottom: short messages
    toast_label = make_label(screen, &lv_font_montserrat_14);
    lv_obj_set_width(toast_label, 128);
    lv_obj_set_style_text_align(toast_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text_static(toast_label, "");
    lv_obj_set_pos(toast_label, 0, 16);
    lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);

    // bottom: volume
    volume_box = plain_obj(screen);
    lv_obj_set_size(volume_box, 128, 17);
    lv_obj_set_pos(volume_box, 0, 15);
    lv_obj_add_flag(volume_box, LV_OBJ_FLAG_HIDDEN);

    volume_icon = make_label(volume_box, &lv_font_montserrat_12);
    lv_label_set_text_static(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(volume_icon, LV_ALIGN_LEFT_MID, 0, 0);

    volume_bar = lv_bar_create(volume_box);
    lv_obj_remove_style_all(volume_bar);
    lv_obj_set_size(volume_bar, 74, 8);
    lv_obj_align(volume_bar, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_border_color(volume_bar, fg, LV_PART_MAIN);
    lv_obj_set_style_border_width(volume_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(volume_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volume_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_bar, fg, LV_PART_INDICATOR);
    lv_bar_set_range(volume_bar, 0, 100);

    volume_label = make_label(volume_box, &lv_font_montserrat_12);
    lv_obj_align(volume_label, LV_ALIGN_RIGHT_MID, 0, 0);

    k_mutex_lock(&state_mutex, K_FOREVER);
    refresh_state();
    state.caps_lock = (zmk_hid_indicators_get_current_profile() & HID_INDICATOR_CAPS_LOCK) != 0;
    k_mutex_unlock(&state_mutex);

    create_splash(screen);
    lv_timer_create(status_tick, TICK_MS, NULL);
    status_tick(NULL);

    return screen;
}
