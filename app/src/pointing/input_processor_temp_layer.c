/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_temp_layer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
#include <zephyr/settings/settings.h>
#include <zmk/pointing/aml.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Constants and Types */
#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

struct temp_layer_config {
    int16_t require_prior_idle_ms;
    const uint16_t *excluded_positions;
    size_t num_positions;
};

struct temp_layer_state {
    uint8_t toggle_layer;
    bool is_active;
    int64_t last_tapped_timestamp;
};

struct temp_layer_data {
    const struct device *dev;
    struct k_mutex lock;
    struct temp_layer_state state;
};

/* Static Work Queue Items */
static struct k_work_delayable layer_disable_works[MAX_LAYERS];

/* ---- AML runtime (RAM-ified config for live Studio RPC tuning) ----------- */
/* Only compiled when the Studio RPC is enabled. The hot-paths always use     */
/* these values so the behaviour is consistent whether RPC is present or not. */

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)

#define AML_SETTINGS_SUBTREE "aml"
#define AML_MAX_EXCLUDED 16

static struct {
    /* Runtime values (live-changeable) */
    uint32_t deactivation_ms;        /* 0 = use binding param2 until first call */
    uint32_t prior_idle_ms;
    uint32_t extend_ms;              /* re-arm amount on excluded-key press (0 = seed from dwell) */
    uint16_t excluded_positions[AML_MAX_EXCLUDED];
    uint8_t  num_excluded;
    /* Devicetree defaults (for reset) */
    uint32_t default_deactivation_ms;  /* captured on first temp_layer_handle_event call */
    uint32_t default_prior_idle_ms;
    uint32_t default_extend_ms;
    uint16_t default_excluded[AML_MAX_EXCLUDED];
    uint8_t  default_num_excluded;
    bool     defaults_captured;
} aml_rt;

/* ---- Public AML API (used by aml_studio.c) ------------------------------- */

int zmk_aml_get(struct zmk_aml_config *out) {
    out->deactivation_ms = aml_rt.deactivation_ms;
    out->prior_idle_ms   = aml_rt.prior_idle_ms;
    out->extend_ms       = aml_rt.extend_ms;
    out->num_excluded    = aml_rt.num_excluded;
    memcpy(out->excluded_positions, aml_rt.excluded_positions,
           aml_rt.num_excluded * sizeof(uint16_t));
    return 0;
}

int zmk_aml_set_deactivation(uint32_t ms) {
    aml_rt.deactivation_ms = ms;
    return 0;
}

int zmk_aml_set_extend(uint32_t ms) {
    aml_rt.extend_ms = ms;
    return 0;
}

int zmk_aml_set_prior_idle(uint32_t ms) {
    aml_rt.prior_idle_ms = ms;
    return 0;
}

int zmk_aml_toggle_excluded(uint32_t position) {
    /* Search for existing entry */
    for (uint8_t i = 0; i < aml_rt.num_excluded; i++) {
        if (aml_rt.excluded_positions[i] == (uint16_t)position) {
            /* Remove: shift left */
            memmove(&aml_rt.excluded_positions[i],
                    &aml_rt.excluded_positions[i + 1],
                    (aml_rt.num_excluded - i - 1) * sizeof(uint16_t));
            aml_rt.num_excluded--;
            return 0;
        }
    }
    /* Not found: add (if there's room) */
    if (aml_rt.num_excluded >= AML_MAX_EXCLUDED) {
        return -ENOMEM;
    }
    aml_rt.excluded_positions[aml_rt.num_excluded++] = (uint16_t)position;
    return 0;
}

int zmk_aml_reset(void) {
    aml_rt.deactivation_ms = aml_rt.default_deactivation_ms;
    aml_rt.prior_idle_ms   = aml_rt.default_prior_idle_ms;
    aml_rt.extend_ms       = aml_rt.default_extend_ms;
    aml_rt.num_excluded    = aml_rt.default_num_excluded;
    memcpy(aml_rt.excluded_positions, aml_rt.default_excluded,
           aml_rt.default_num_excluded * sizeof(uint16_t));
    /* Drop NVS overrides so defaults persist across reboot */
    settings_delete(AML_SETTINGS_SUBTREE "/dec");
    settings_delete(AML_SETTINGS_SUBTREE "/idle");
    settings_delete(AML_SETTINGS_SUBTREE "/ext");
    settings_delete(AML_SETTINGS_SUBTREE "/excl");
    return 0;
}

int zmk_aml_save(void) {
    int rc;
    rc = settings_save_one(AML_SETTINGS_SUBTREE "/dec",
                           &aml_rt.deactivation_ms, sizeof(aml_rt.deactivation_ms));
    if (rc < 0) return rc;
    rc = settings_save_one(AML_SETTINGS_SUBTREE "/idle",
                           &aml_rt.prior_idle_ms, sizeof(aml_rt.prior_idle_ms));
    if (rc < 0) return rc;
    rc = settings_save_one(AML_SETTINGS_SUBTREE "/ext",
                           &aml_rt.extend_ms, sizeof(aml_rt.extend_ms));
    if (rc < 0) return rc;
    /* Pack excluded: [num_excluded, pos0, pos1, ...] */
    uint8_t excl_buf[1 + AML_MAX_EXCLUDED * 2];
    excl_buf[0] = aml_rt.num_excluded;
    memcpy(excl_buf + 1, aml_rt.excluded_positions,
           aml_rt.num_excluded * sizeof(uint16_t));
    rc = settings_save_one(AML_SETTINGS_SUBTREE "/excl",
                           excl_buf, 1 + aml_rt.num_excluded * 2);
    return rc;
}

/* ---- NVS settings loader ------------------------------------------------- */

static int aml_settings_set(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "dec") == 0) {
        if (len != sizeof(uint32_t)) return -EINVAL;
        uint32_t v;
        ssize_t rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc < 0) return (int)rc;
        aml_rt.deactivation_ms = v;
    } else if (strcmp(name, "idle") == 0) {
        if (len != sizeof(uint32_t)) return -EINVAL;
        uint32_t v;
        ssize_t rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc < 0) return (int)rc;
        aml_rt.prior_idle_ms = v;
    } else if (strcmp(name, "ext") == 0) {
        if (len != sizeof(uint32_t)) return -EINVAL;
        uint32_t v;
        ssize_t rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc < 0) return (int)rc;
        aml_rt.extend_ms = v;
    } else if (strcmp(name, "excl") == 0 && len >= 1) {
        uint8_t excl_buf[1 + AML_MAX_EXCLUDED * 2];
        ssize_t rc = read_cb(cb_arg, excl_buf, MIN(len, sizeof(excl_buf)));
        if (rc < 0) return (int)rc;
        uint8_t n = MIN(excl_buf[0], AML_MAX_EXCLUDED);
        aml_rt.num_excluded = n;
        memcpy(aml_rt.excluded_positions, excl_buf + 1, n * sizeof(uint16_t));
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pyuron_aml, AML_SETTINGS_SUBTREE, NULL,
                               aml_settings_set, NULL, NULL);

#endif /* CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC */

/* ---- Position Search ----------------------------------------------------- */

static bool position_is_excluded(const struct temp_layer_config *config, uint32_t position) {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
    /* Use RAM runtime list (may differ from devicetree config after live edit) */
    if (!aml_rt.num_excluded) return false;
    for (uint8_t i = 0; i < aml_rt.num_excluded; i++) {
        if (aml_rt.excluded_positions[i] == (uint16_t)position) return true;
    }
    return false;
#else
    if (!config->excluded_positions || !config->num_positions) {
        return false;
    }
    const uint16_t *end = config->excluded_positions + config->num_positions;
    for (const uint16_t *pos = config->excluded_positions; pos < end; pos++) {
        if (*pos == position) return true;
    }
    return false;
#endif
}

/* ---- Timing Check -------------------------------------------------------- */

static bool should_quick_tap(const struct temp_layer_config *config, int64_t last_tapped,
                             int64_t current_time) {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
    int64_t idle_ms = (int64_t)aml_rt.prior_idle_ms;
    return (last_tapped + idle_ms) > current_time;
#else
    return (last_tapped + config->require_prior_idle_ms) > current_time;
#endif
}

/* ---- Layer State Management ---------------------------------------------- */

static void update_layer_state(struct temp_layer_state *state, bool activate) {
    /* is_active は handle_event 側で AML 予約時に即セットされることがあるため、
     * 早期 return せず、実レイヤー状態を基準に activate/deactivate を冪等に行う。
     * これにより「AML に入った直後の窓で除外キーを押すと延長(extend)が効かない」
     * race を解消する(is_active を先に立ててもレイヤー有効化が二重/欠落しない)。 */
    state->is_active = activate;
    bool layer_on =
        zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(state->toggle_layer));
    if (activate && !layer_on) {
        zmk_keymap_layer_activate(state->toggle_layer, false);
        LOG_DBG("Layer %d activated", state->toggle_layer);
    } else if (!activate && layer_on) {
        zmk_keymap_layer_deactivate(state->toggle_layer, false);
        LOG_DBG("Layer %d deactivated", state->toggle_layer);
    }
}

struct layer_state_action {
    uint8_t layer;
    bool activate;
};

K_MSGQ_DEFINE(temp_layer_action_msgq, sizeof(struct layer_state_action),
              CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_MAX_ACTION_EVENTS, 4);

static void layer_action_work_cb(struct k_work *work) {

    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        LOG_ERR("Error locking for updating %d", ret);
        return;
    }

    struct layer_state_action action;

    while (k_msgq_get(&temp_layer_action_msgq, &action, K_MSEC(10)) >= 0) {
        if (!action.activate) {
            if (zmk_keymap_layer_active(action.layer)) {
                update_layer_state(&data->state, false);
            }
        } else {
            update_layer_state(&data->state, true);
        }
    }

    k_mutex_unlock(&data->lock);
}

static K_WORK_DEFINE(layer_action_work, layer_action_work_cb);

/* ---- Work Queue Callback ------------------------------------------------- */

static void layer_disable_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    int layer_index = ARRAY_INDEX(layer_disable_works, d_work);

    struct layer_state_action action = {.layer = layer_index, .activate = false};

    int ret = k_msgq_put(&temp_layer_action_msgq, &action, K_MSEC(10));
    k_work_submit(&layer_action_work);
}

/* ---- Event Handlers ------------------------------------------------------ */

static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    if (!zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(data->state.toggle_layer))) {
        LOG_DBG("Deactivating layer that was activated by this processor");
        data->state.is_active = false;
        k_work_cancel_delayable(&layer_disable_works[data->state.toggle_layer]);
    }
    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    const struct temp_layer_config *cfg = dev->config;

    if (data->state.is_active) {
        if (!position_is_excluded(cfg, ev->position)) {
            LOG_DBG("Position not excluded, deactivating layer");
            update_layer_state(&data->state, false);
        } else {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
            /* Excluded key pressed while AML is active: extend the dwell timer
             * from now, so actively clicking (e.g. K / left-click) keeps the
             * mouse layer alive just like trackball motion does. The amount is
             * a separate, app-adjustable value (defaults to the dwell). */
            uint32_t timeout_ms = aml_rt.extend_ms;
            if (timeout_ms > 0) {
                k_work_reschedule(&layer_disable_works[data->state.toggle_layer],
                                  K_MSEC(timeout_ms));
                LOG_DBG("Excluded position, extending AML by %u ms", timeout_ms);
            }
#else
            LOG_DBG("Position excluded, continuing");
#endif
        }
    }

    k_mutex_unlock(&data->lock);

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    LOG_DBG("Setting last_tapped_timestamp to: %lld", ev->timestamp);
    data->state.last_tapped_timestamp = ev->timestamp;

    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_state_changed_dispatcher(const struct device *dev, const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh) != NULL) {
        LOG_DBG("Dispatching handle_layer_state_changed");
        return handle_layer_state_changed(dev, eh);
    } else if (as_zmk_position_state_changed(eh) != NULL) {
        LOG_DBG("Dispatching handle_position_state_changed");
        return handle_position_state_changed(dev, eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        LOG_DBG("Dispatching handle_keycode_state_changed");
        return handle_keycode_state_changed(dev, eh);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#define DISPATCH_EVENT(inst)                                                                       \
    {                                                                                              \
        int err = handle_state_changed_dispatcher(DEVICE_DT_INST_GET(inst), eh);                   \
        if (err < 0) {                                                                             \
            return err;                                                                            \
        }                                                                                          \
    }

static int handle_event_dispatcher(const zmk_event_t *eh) {
    DT_INST_FOREACH_STATUS_OKAY(DISPATCH_EVENT)

    return 0;
}

/* ---- Driver Implementation ----------------------------------------------- */

static int temp_layer_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    if (param1 >= MAX_LAYERS) {
        LOG_ERR("Invalid layer index: %d", param1);
        return -EINVAL;
    }

    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    const struct temp_layer_config *cfg = dev->config;

    data->state.toggle_layer = param1;

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
    /* Capture the binding's param2 as the deactivation default on first call.
     * This is the only place we see param2, so do it here before any NVS
     * override may already be in aml_rt.deactivation_ms. */
    if (!aml_rt.defaults_captured && param2 > 0) {
        aml_rt.default_deactivation_ms = param2;
        aml_rt.default_extend_ms       = param2;
        if (aml_rt.deactivation_ms == 0) {
            /* No NVS override loaded yet — seed with the devicetree value */
            aml_rt.deactivation_ms = param2;
        }
        if (aml_rt.extend_ms == 0) {
            /* Default the excluded-key extension to the dwell value; the user
             * can then adjust it independently via the app slider. */
            aml_rt.extend_ms = param2;
        }
        aml_rt.defaults_captured = true;
    }
    uint32_t timeout_ms = aml_rt.deactivation_ms;
#else
    uint32_t timeout_ms = param2;
#endif

    if (!data->state.is_active &&
        !should_quick_tap(cfg, data->state.last_tapped_timestamp, k_uptime_get())) {
        /* is_active を予約時に即セットする。こうしないと activate が work queue
         * 経由で遅れて反映されるまでの窓で除外キーを押しても is_active==false で
         * 延長(extend)分岐に入れず、AML 延長が効かない。実際のレイヤー有効化は
         * 従来どおり work queue 経由(update_layer_state を冪等化済み)。 */
        data->state.is_active = true;
        struct layer_state_action action = {.layer = param1, .activate = true};

        int ret = k_msgq_put(&temp_layer_action_msgq, &action, K_MSEC(10));
        if (ret < 0) {
            LOG_ERR("Failed to enqueue action to enable layer %d (%d)", param1, ret);
            data->state.is_active = false;
        } else {
            k_work_submit(&layer_action_work);
        }
    }

    if (timeout_ms > 0) {
        k_work_reschedule(&layer_disable_works[param1], K_MSEC(timeout_ms));
    }

    k_mutex_unlock(&data->lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int temp_layer_init(const struct device *dev) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    k_mutex_init(&data->lock);

    for (int i = 0; i < MAX_LAYERS; i++) {
        k_work_init_delayable(&layer_disable_works[i], layer_disable_callback);
    }

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
    /* Seed RAM config from devicetree (before NVS settings_load() runs) */
    const struct temp_layer_config *cfg = dev->config;
    aml_rt.prior_idle_ms         = (uint32_t)cfg->require_prior_idle_ms;
    aml_rt.default_prior_idle_ms = (uint32_t)cfg->require_prior_idle_ms;
    uint8_t n = (uint8_t)MIN(cfg->num_positions, AML_MAX_EXCLUDED);
    aml_rt.num_excluded         = n;
    aml_rt.default_num_excluded = n;
    for (uint8_t i = 0; i < n; i++) {
        aml_rt.excluded_positions[i] = cfg->excluded_positions[i];
        aml_rt.default_excluded[i]   = cfg->excluded_positions[i];
    }
    /* deactivation_ms defaults captured on first handle_event (see param2 comment) */
#endif

    return 0;
}

/* ---- Driver API ---------------------------------------------------------- */

static const struct zmk_input_processor_driver_api temp_layer_driver_api = {
    .handle_event = temp_layer_handle_event,
};

/* ---- Event Listeners Conditions ------------------------------------------ */

#define NEEDS_POSITION_HANDLERS(n, ...) DT_INST_PROP_HAS_IDX(n, excluded_positions, 0)
#define NEEDS_KEYCODE_HANDLERS(n, ...) (DT_INST_PROP_OR(n, require_prior_idle_ms, 0) > 0)

/* Always register position + keycode handlers when RPC is enabled,
 * because runtime config may add excluded positions or a prior-idle guard
 * even if the devicetree node doesn't have them. */
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
#define NEEDS_POSITION_HANDLERS_OVERRIDE 1
#define NEEDS_KEYCODE_HANDLERS_OVERRIDE  1
#else
#define NEEDS_POSITION_HANDLERS_OVERRIDE 0
#define NEEDS_KEYCODE_HANDLERS_OVERRIDE  0
#endif

/* ---- Event Handlers Registration ----------------------------------------- */

ZMK_LISTENER(processor_temp_layer, handle_event_dispatcher);
ZMK_SUBSCRIPTION(processor_temp_layer, zmk_layer_state_changed);

#if NEEDS_POSITION_HANDLERS_OVERRIDE || DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_POSITION_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_temp_layer, zmk_position_state_changed);
#endif

#if NEEDS_KEYCODE_HANDLERS_OVERRIDE || DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_KEYCODE_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_temp_layer, zmk_keycode_state_changed);
#endif

/* ---- Device Instantiation ------------------------------------------------ */

#define TEMP_LAYER_INST(n)                                                                         \
    static struct temp_layer_data processor_temp_layer_data_##n = {};                              \
    static const uint16_t excluded_positions_##n[] = DT_INST_PROP(n, excluded_positions);          \
    static const struct temp_layer_config processor_temp_layer_config_##n = {                      \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                     \
        .excluded_positions = excluded_positions_##n,                                              \
        .num_positions = DT_INST_PROP_LEN(n, excluded_positions),                                  \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, temp_layer_init, NULL, &processor_temp_layer_data_##n,                \
                          &processor_temp_layer_config_##n, POST_KERNEL,                           \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &temp_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TEMP_LAYER_INST)
