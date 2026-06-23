/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom input processor "zip_pyuron_speed" — scales INPUT_REL_X / INPUT_REL_Y
 * by a runtime percentage (100 = x1.0) with per-axis remainder accumulation,
 * so fractional multipliers keep sub-pixel movement instead of dropping it.
 *
 * Placed AFTER zip_keybind_dynamic in the listener pipeline, so it only scales
 * cursor movement on non-gesture layers (gestures/AML see the raw deltas).
 *
 * percent is live-editable via the "pyuron_speed" custom Studio RPC and
 * persisted to NVS. Driver structure follows input_processor_pyuron_scroll_invert.c.
 */

#define DT_DRV_COMPAT zmk_input_processor_pyuron_speed

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#if IS_ENABLED(CONFIG_PYURON_SPEED_STUDIO_RPC)
#include <string.h>
#include <zephyr/settings/settings.h>
#include <zmk/pointing/speed.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PYURON_SPEED_STUDIO_RPC)

#define SPEED_SETTINGS_SUBTREE   "speed"
#define SPEED_PERCENT_DEFAULT    100
#define SPEED_PERCENT_MIN        10
#define SPEED_PERCENT_MAX        1000
#define ACCEL_STRENGTH_DEFAULT   50
#define ACCEL_STRENGTH_MAX       100
#define ACCEL_DIV                400   /* quadratic curve divisor */
#define ACCEL_BOOST_MAX          400   /* cap on the extra percent added by accel */

static struct {
    uint32_t percent;         /* 100 = x1.0 */
    bool     accel_on;        /* mouse acceleration on/off (default false) */
    uint32_t accel_strength;  /* 0..100 (default 50) */
    int32_t  rem_x;           /* remainder accumulators (sub-100 precision) */
    int32_t  rem_y;
    bool     seeded;
} speed_rt = {
    .percent = SPEED_PERCENT_DEFAULT,
    .accel_on = false,
    .accel_strength = ACCEL_STRENGTH_DEFAULT,
};

/* ---- Public API (used by speed_studio.c) --------------------------------- */

int zmk_speed_get(struct zmk_speed_config *out) {
    out->percent = speed_rt.percent ? speed_rt.percent : SPEED_PERCENT_DEFAULT;
    out->accel_on = speed_rt.accel_on;
    out->accel_strength = speed_rt.accel_strength;
    return 0;
}

int zmk_speed_set(uint32_t percent) {
    if (percent < SPEED_PERCENT_MIN) percent = SPEED_PERCENT_MIN;
    if (percent > SPEED_PERCENT_MAX) percent = SPEED_PERCENT_MAX;
    speed_rt.percent = percent;
    return 0;
}

int zmk_speed_set_accel(bool on, uint32_t strength) {
    if (strength > ACCEL_STRENGTH_MAX) strength = ACCEL_STRENGTH_MAX;
    speed_rt.accel_on = on;
    speed_rt.accel_strength = strength;
    return 0;
}

int zmk_speed_save(void) {
    int rc;
    rc = settings_save_one(SPEED_SETTINGS_SUBTREE "/p",
                           &speed_rt.percent, sizeof(speed_rt.percent));
    if (rc) return rc;

    uint8_t accel_on_u8 = speed_rt.accel_on ? 1 : 0;
    rc = settings_save_one(SPEED_SETTINGS_SUBTREE "/a",
                           &accel_on_u8, sizeof(accel_on_u8));
    if (rc) return rc;

    rc = settings_save_one(SPEED_SETTINGS_SUBTREE "/s",
                           &speed_rt.accel_strength, sizeof(speed_rt.accel_strength));
    return rc;
}

/* ---- NVS settings loader ------------------------------------------------- */

static int speed_settings_set(const char *name, size_t len,
                              settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "p") == 0) {
        uint32_t v;
        if (len != sizeof(v)) return -EINVAL;
        ssize_t rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc < 0) return (int)rc;
        if (v < SPEED_PERCENT_MIN) v = SPEED_PERCENT_MIN;
        if (v > SPEED_PERCENT_MAX) v = SPEED_PERCENT_MAX;
        speed_rt.percent = v;
    } else if (strcmp(name, "a") == 0) {
        uint8_t v;
        if (len != sizeof(v)) return -EINVAL;
        ssize_t rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc < 0) return (int)rc;
        speed_rt.accel_on = (v != 0);
    } else if (strcmp(name, "s") == 0) {
        uint32_t v;
        if (len != sizeof(v)) return -EINVAL;
        ssize_t rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc < 0) return (int)rc;
        if (v > ACCEL_STRENGTH_MAX) v = ACCEL_STRENGTH_MAX;
        speed_rt.accel_strength = v;
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pyuron_speed, SPEED_SETTINGS_SUBTREE,
                               NULL, speed_settings_set, NULL, NULL);

#endif /* CONFIG_PYURON_SPEED_STUDIO_RPC */

/* --------------------------------------------------------------------------
 * Input processor driver
 * -------------------------------------------------------------------------- */

static int speed_handle_event(const struct device *dev,
                              struct input_event *event,
                              uint32_t param1, uint32_t param2,
                              struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

#if IS_ENABLED(CONFIG_PYURON_SPEED_STUDIO_RPC)
    if (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) {
        return ZMK_INPUT_PROC_CONTINUE; /* don't scale wheel/other axes */
    }

    uint32_t base = speed_rt.percent ? speed_rt.percent : SPEED_PERCENT_DEFAULT;
    uint32_t eff = base;
    if (speed_rt.accel_on) {
        int32_t v = event->value;
        uint32_t spd = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
        /* quadratic curve: faster movement boosts more; strength(0..100) scales it. */
        uint32_t boost = (spd * spd * speed_rt.accel_strength) / ACCEL_DIV; /* extra percent */
        if (boost > ACCEL_BOOST_MAX) boost = ACCEL_BOOST_MAX;
        eff = base * (100 + boost) / 100;
    }
    if (eff == 100) {
        return ZMK_INPUT_PROC_CONTINUE; /* identity */
    }

    int32_t *rem = (event->code == INPUT_REL_X) ? &speed_rt.rem_x : &speed_rt.rem_y;

    int32_t scaled = event->value * (int32_t)eff + *rem;
    int32_t out = scaled / 100;
    *rem = scaled - out * 100; /* keep fractional remainder */
    event->value = out;
#endif

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api speed_driver_api = {
    .handle_event = speed_handle_event,
};

#define SPEED_INST(n)                                                          \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,             \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                 \
                          &speed_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SPEED_INST)
