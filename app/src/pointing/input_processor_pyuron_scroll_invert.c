/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom input processor "zip_pyuron_scroll_invert" — inverts the sign of
 * INPUT_REL_WHEEL (vertical) and/or INPUT_REL_HWHEEL (horizontal) scroll
 * events at runtime, based on two boolean flags (invert_v / invert_h).
 *
 * All other event codes (REL_X, REL_Y, …) pass through unchanged.
 *
 * When CONFIG_PYURON_SCROLL_STUDIO_RPC is enabled the flags are live-editable
 * via the "pyuron_scroll" custom Studio RPC and persisted to NVS so they
 * survive reboot.  The public API is zmk/pointing/scroll_invert.h.
 *
 * Driver structure follows input_processor_scaler.c / input_processor_transform.c.
 * NVS persistence follows input_processor_temp_layer.c (AML section).
 */

#define DT_DRV_COMPAT zmk_input_processor_pyuron_scroll_invert

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#if IS_ENABLED(CONFIG_PYURON_SCROLL_STUDIO_RPC)
#include <zephyr/settings/settings.h>
#include <zmk/pointing/scroll_invert.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* --------------------------------------------------------------------------
 * Runtime state (global, single instance)
 * -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_PYURON_SCROLL_STUDIO_RPC)

#define SCROLL_INVERT_SETTINGS_SUBTREE "scroll_inv"

static struct {
    bool invert_v;
    bool invert_h;
} scroll_inv_rt;

/* ---- Public API (used by scroll_studio.c) -------------------------------- */

int zmk_scroll_invert_get(struct zmk_scroll_invert_config *out) {
    out->invert_v = scroll_inv_rt.invert_v;
    out->invert_h = scroll_inv_rt.invert_h;
    return 0;
}

int zmk_scroll_invert_set(bool invert_v, bool invert_h) {
    scroll_inv_rt.invert_v = invert_v;
    scroll_inv_rt.invert_h = invert_h;
    return 0;
}

int zmk_scroll_invert_save(void) {
    int rc;
    uint8_t v = scroll_inv_rt.invert_v ? 1 : 0;
    uint8_t h = scroll_inv_rt.invert_h ? 1 : 0;
    rc = settings_save_one(SCROLL_INVERT_SETTINGS_SUBTREE "/v", &v, sizeof(v));
    if (rc < 0) {
        return rc;
    }
    rc = settings_save_one(SCROLL_INVERT_SETTINGS_SUBTREE "/h", &h, sizeof(h));
    return rc;
}

/* ---- NVS settings loader ------------------------------------------------- */

static int scroll_inv_settings_set(const char *name, size_t len,
                                   settings_read_cb read_cb, void *cb_arg) {
    uint8_t val;
    if (len != sizeof(uint8_t)) {
        return -EINVAL;
    }
    ssize_t rc = read_cb(cb_arg, &val, sizeof(val));
    if (rc < 0) {
        return (int)rc;
    }
    if (strcmp(name, "v") == 0) {
        scroll_inv_rt.invert_v = (val != 0);
    } else if (strcmp(name, "h") == 0) {
        scroll_inv_rt.invert_h = (val != 0);
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pyuron_scroll_inv, SCROLL_INVERT_SETTINGS_SUBTREE,
                               NULL, scroll_inv_settings_set, NULL, NULL);

#endif /* CONFIG_PYURON_SCROLL_STUDIO_RPC */

/* --------------------------------------------------------------------------
 * Input processor driver
 * -------------------------------------------------------------------------- */

static int scroll_invert_handle_event(const struct device *dev,
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

#if IS_ENABLED(CONFIG_PYURON_SCROLL_STUDIO_RPC)
    bool do_invert = false;
    if (event->code == INPUT_REL_WHEEL) {
        do_invert = scroll_inv_rt.invert_v;
    } else if (event->code == INPUT_REL_HWHEEL) {
        do_invert = scroll_inv_rt.invert_h;
    }
    if (do_invert) {
        LOG_DBG("scroll_invert: code=%u value %d -> %d",
                event->code, event->value, -event->value);
        event->value = -event->value;
    }
#endif

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api scroll_invert_driver_api = {
    .handle_event = scroll_invert_handle_event,
};

/* --------------------------------------------------------------------------
 * Device instantiation (zero-param, no per-instance config needed)
 * -------------------------------------------------------------------------- */

#define SCROLL_INVERT_INST(n)                                                   \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,              \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                  \
                          &scroll_invert_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SCROLL_INVERT_INST)
