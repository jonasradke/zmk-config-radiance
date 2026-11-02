/*
 * Replacement for CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE: the display is blanked when the keyboard
 * goes idle, but only while it runs on battery. With USB power present it stays on.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

// LVGL tick timer, defined in zmk/app/src/display/main.c
extern struct k_timer display_timer;

static bool blanked;

static void blank_display_cb(struct k_work *work) {
    if (blanked) {
        return;
    }
    k_timer_stop(&display_timer);
    display_blanking_on(display);
    blanked = true;
}

static void unblank_display_cb(struct k_work *work) {
    if (!blanked) {
        return;
    }
    display_blanking_off(display);
    k_timer_start(&display_timer, K_MSEC(CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS),
                  K_MSEC(CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS));
    blanked = false;
}

K_WORK_DEFINE(blank_display_work, blank_display_cb);
K_WORK_DEFINE(unblank_display_work, unblank_display_cb);

static int display_usb_awake_listener(const zmk_event_t *eh) {
    if (!zmk_display_is_initialized()) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool idle = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE;
    bool blank = idle && !zmk_usb_is_powered();

    k_work_submit_to_queue(zmk_display_work_q(), blank ? &blank_display_work : &unblank_display_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(display_usb_awake, display_usb_awake_listener);
ZMK_SUBSCRIPTION(display_usb_awake, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(display_usb_awake, zmk_usb_conn_state_changed);
