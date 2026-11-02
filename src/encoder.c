/*
 * Drop-in replacement for ZMK's alps,ec11 driver that decodes every A/B edge directly in the GPIO
 * interrupt and keeps interrupts enabled. ZMK's driver turns interrupts off and decodes later on
 * the work queue, so fast turns lose edges and whole detents.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT alps_ec11

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(radiance_encoder, CONFIG_SENSOR_LOG_LEVEL);

#define FULL_ROTATION 360

struct encoder_config {
    struct gpio_dt_spec a;
    struct gpio_dt_spec b;
    uint16_t steps;
};

struct encoder_data {
    const struct device *dev;
    struct gpio_callback a_cb;
    struct gpio_callback b_cb;
    struct k_spinlock lock;
    uint8_t ab_state;
    int8_t last_dir;
    int32_t pulses;  // decoded in the interrupt, not yet fetched
    int32_t fetched; // taken by sample_fetch, reported by channel_get
    sensor_trigger_handler_t handler;
    const struct sensor_trigger *trigger;
    struct k_work work;
};

// Indexed by (previous AB << 2) | new AB, same direction convention as ZMK's driver.
// 2 marks a jump over one state (both pins changed between interrupts).
static const int8_t transitions[16] = {
    0, 1, -1, 2, -1, 0, 2, 1, 1, 2, 0, -1, 2, -1, 1, 0,
};

static uint8_t read_ab(const struct encoder_config *cfg) {
    return (gpio_pin_get_dt(&cfg->a) << 1) | gpio_pin_get_dt(&cfg->b);
}

static void encoder_isr(struct encoder_data *data) {
    const struct encoder_config *cfg = data->dev->config;
    k_spinlock_key_t key = k_spin_lock(&data->lock);

    uint8_t ab = read_ab(cfg);
    int8_t delta = transitions[(data->ab_state << 2) | ab];
    if (delta == 2) {
        // Missed the state in between; it was most likely moving the same way as before.
        delta = 2 * data->last_dir;
    } else if (delta != 0) {
        data->last_dir = delta;
    }
    data->ab_state = ab;
    data->pulses += delta;

    k_spin_unlock(&data->lock, key);

    if (delta != 0 && data->handler) {
        k_work_submit(&data->work);
    }
}

static void a_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    encoder_isr(CONTAINER_OF(cb, struct encoder_data, a_cb));
}

static void b_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    encoder_isr(CONTAINER_OF(cb, struct encoder_data, b_cb));
}

static void encoder_work(struct k_work *work) {
    struct encoder_data *data = CONTAINER_OF(work, struct encoder_data, work);
    sensor_trigger_handler_t handler = data->handler;

    if (handler) {
        handler(data->dev, data->trigger);
    }
}

static int encoder_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
                               sensor_trigger_handler_t handler) {
    struct encoder_data *data = dev->data;

    data->trigger = trig;
    data->handler = handler;
    return 0;
}

static int encoder_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    struct encoder_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);

    data->fetched += data->pulses;
    data->pulses = 0;

    k_spin_unlock(&data->lock, key);
    return 0;
}

static int encoder_channel_get(const struct device *dev, enum sensor_channel chan,
                               struct sensor_value *val) {
    struct encoder_data *data = dev->data;
    const struct encoder_config *cfg = dev->config;

    if (chan != SENSOR_CHAN_ROTATION) {
        return -ENOTSUP;
    }

    int32_t pulses = data->fetched;
    data->fetched = 0;

    val->val1 = (pulses * FULL_ROTATION) / cfg->steps;
    val->val2 = ((pulses * FULL_ROTATION) % cfg->steps) * 1000000 / cfg->steps;
    return 0;
}

static const struct sensor_driver_api encoder_api = {
    .trigger_set = encoder_trigger_set,
    .sample_fetch = encoder_sample_fetch,
    .channel_get = encoder_channel_get,
};

static int setup_pin(const struct gpio_dt_spec *pin, struct gpio_callback *cb,
                     gpio_callback_handler_t handler) {
    if (!gpio_is_ready_dt(pin)) {
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(pin, GPIO_INPUT);
    if (!err) {
        gpio_init_callback(cb, handler, BIT(pin->pin));
        err = gpio_add_callback(pin->port, cb);
    }
    return err;
}

static int encoder_init(const struct device *dev) {
    struct encoder_data *data = dev->data;
    const struct encoder_config *cfg = dev->config;

    data->dev = dev;
    data->last_dir = 1;
    k_work_init(&data->work, encoder_work);

    int err = setup_pin(&cfg->a, &data->a_cb, a_callback);
    if (!err) {
        err = setup_pin(&cfg->b, &data->b_cb, b_callback);
    }
    if (err) {
        LOG_ERR("Failed to set up encoder pins (%d)", err);
        return err;
    }

    data->ab_state = read_ab(cfg);

    gpio_pin_interrupt_configure_dt(&cfg->a, GPIO_INT_EDGE_BOTH);
    gpio_pin_interrupt_configure_dt(&cfg->b, GPIO_INT_EDGE_BOTH);
    return 0;
}

#define ENCODER_INST(n)                                                                            \
    BUILD_ASSERT(DT_INST_PROP_OR(n, steps, 0) > 0, "encoder needs a steps property");              \
    static struct encoder_data encoder_data_##n;                                                   \
    static const struct encoder_config encoder_cfg_##n = {                                         \
        .a = GPIO_DT_SPEC_INST_GET(n, a_gpios),                                                    \
        .b = GPIO_DT_SPEC_INST_GET(n, b_gpios),                                                    \
        .steps = DT_INST_PROP(n, steps),                                                           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, encoder_init, NULL, &encoder_data_##n, &encoder_cfg_##n,              \
                          POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &encoder_api);

DT_INST_FOREACH_STATUS_OKAY(ENCODER_INST)
