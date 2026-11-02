/*
 * Custom 128x32 status screen for the Radiance.
 *
 *   +----------------------------------------+
 *   | BT 2 ok                    [chg] 87 %  |
 *   | Default                     CAPS / wpm |
 *   +----------------------------------------+
 *
 * Top: active output (USB or Bluetooth profile and its state), battery.
 * Bottom: active layer, Caps Lock indicator (inverted box) or typing speed.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/wpm.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/wpm_state_changed.h>

#define HID_INDICATOR_CAPS_LOCK BIT(1)

struct status_state {
    struct zmk_endpoint_instance endpoint;
    bool ble_connected;
    bool ble_bonded;
    uint8_t battery;
    bool usb_powered;
    const char *layer_name;
    zmk_keymap_layer_index_t layer_index;
    bool caps_lock;
    int wpm;
};

static lv_obj_t *output_label;
static lv_obj_t *battery_label;
static lv_obj_t *layer_label;
static lv_obj_t *caps_label;
static lv_obj_t *wpm_label;

static const char *battery_symbol(uint8_t level) {
    if (level > 90) {
        return LV_SYMBOL_BATTERY_FULL;
    } else if (level > 65) {
        return LV_SYMBOL_BATTERY_3;
    } else if (level > 35) {
        return LV_SYMBOL_BATTERY_2;
    } else if (level > 10) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void status_update_cb(struct status_state state) {
    char text[24];

    if (state.endpoint.transport == ZMK_TRANSPORT_USB) {
        lv_label_set_text(output_label, LV_SYMBOL_USB " USB");
    } else {
        const char *ble_state = !state.ble_bonded     ? LV_SYMBOL_SETTINGS
                                : state.ble_connected ? LV_SYMBOL_OK
                                                      : LV_SYMBOL_CLOSE;
        snprintf(text, sizeof(text), LV_SYMBOL_BLUETOOTH " %i %s",
                 state.endpoint.ble.profile_index + 1, ble_state);
        lv_label_set_text(output_label, text);
    }

    snprintf(text, sizeof(text), "%s%s %u%%", state.usb_powered ? LV_SYMBOL_CHARGE " " : "",
             battery_symbol(state.battery), state.battery);
    lv_label_set_text(battery_label, text);

    if (state.layer_name != NULL && strlen(state.layer_name) > 0) {
        lv_label_set_text(layer_label, state.layer_name);
    } else {
        snprintf(text, sizeof(text), "Layer %i", state.layer_index);
        lv_label_set_text(layer_label, text);
    }

    if (state.caps_lock) {
        lv_obj_clear_flag(caps_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(caps_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
        snprintf(text, sizeof(text), "%i wpm", state.wpm);
        lv_label_set_text(wpm_label, text);
    }
}

static struct status_state status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();

    return (struct status_state){
        .endpoint = zmk_endpoints_selected(),
        .ble_connected = zmk_ble_active_profile_is_connected(),
        .ble_bonded = !zmk_ble_active_profile_is_open(),
        .battery = zmk_battery_state_of_charge(),
        .usb_powered = zmk_usb_is_powered(),
        .layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
        .layer_index = index,
        .caps_lock = (zmk_hid_indicators_get_current_profile() & HID_INDICATOR_CAPS_LOCK) != 0,
        .wpm = zmk_wpm_get_state(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(radiance_status, struct status_state, status_update_cb,
                            status_get_state)
ZMK_SUBSCRIPTION(radiance_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(radiance_status, zmk_wpm_state_changed);

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

    output_label = lv_label_create(screen);
    lv_obj_set_style_text_font(output_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(output_label, LV_ALIGN_TOP_LEFT, 0, 0);

    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    layer_label = lv_label_create(screen);
    lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(layer_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    wpm_label = lv_label_create(screen);
    lv_obj_set_style_text_font(wpm_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(wpm_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // Caps Lock: text in the background color on a box in the text color, so it stands out
    // whatever the display inversion setting is.
    caps_label = lv_label_create(screen);
    lv_label_set_text(caps_label, "CAPS");
    lv_obj_set_style_text_font(caps_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(caps_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(caps_label, lv_obj_get_style_text_color(screen, LV_PART_MAIN),
                              LV_PART_MAIN);
    lv_obj_set_style_text_color(caps_label, lv_obj_get_style_bg_color(screen, LV_PART_MAIN),
                                LV_PART_MAIN);
    lv_obj_set_style_pad_hor(caps_label, 2, LV_PART_MAIN);
    lv_obj_align(caps_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(caps_label, LV_OBJ_FLAG_HIDDEN);

    radiance_status_init();
    return screen;
}
