/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_tap_dance

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <zephyr/sys/util.h>
#include <zephyr/settings/settings.h>
#include <zmk/behaviors/tapdance_tuning.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_TAP_DANCE_MAX_HELD CONFIG_ZMK_BEHAVIOR_TAP_DANCE_MAX_HELD

#define ZMK_BHV_TAP_DANCE_POSITION_FREE UINT32_MAX

struct behavior_tap_dance_config {
    uint32_t tapping_term_ms;
    size_t behavior_count;
    struct zmk_behavior_binding *behaviors;
#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)
    int pyuron_slot; // >=0 => live-editable slot index, -1 => plain DT tap-dance
#endif
};

struct active_tap_dance {
    // Tap Dance Data
    int counter;
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    uint32_t param1;
    uint32_t param2;
    bool is_pressed;
    const struct behavior_tap_dance_config *config;

    // Timer Data
    bool timer_started;
    bool timer_cancelled;
    bool tap_dance_decided;
    int64_t release_at;
    struct k_work_delayable release_timer;
};

struct active_tap_dance active_tap_dances[ZMK_BHV_TAP_DANCE_MAX_HELD] = {};

#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)
// RAM mirror: contents of each editable tap-dance slot. Empty (valid=false)
// until populated by RPC / NVS restore, in which case the DT defaults are used
// (identical to upstream).
static struct pyuron_td_slot pyuron_td_slots[PYURON_TD_SLOTS];

size_t pyuron_td_get_count(void) { return PYURON_TD_SLOTS; }

int pyuron_td_get(uint8_t slot, struct pyuron_td_slot *out) {
    if (slot >= PYURON_TD_SLOTS || out == NULL) {
        return -EINVAL;
    }
    *out = pyuron_td_slots[slot];
    return 0;
}

static void pyuron_td_fill_binding(struct zmk_behavior_binding *b, uint32_t beh_id, int32_t p1,
                                   int32_t p2) {
    *b = (struct zmk_behavior_binding){0};
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    b->local_id = (zmk_behavior_local_id_t)beh_id;
#endif
    b->behavior_dev =
        beh_id ? zmk_behavior_find_behavior_name_from_local_id((zmk_behavior_local_id_t)beh_id)
               : NULL;
    b->param1 = (uint32_t)p1;
    b->param2 = (uint32_t)p2;
}

// Resolve the active config view for a tap-dance instance: RAM slot when valid,
// otherwise the DT defaults.
static inline bool td_slot_valid(const struct behavior_tap_dance_config *cfg) {
    return cfg->pyuron_slot >= 0 && pyuron_td_slots[cfg->pyuron_slot].valid;
}
static inline uint32_t td_effective_term(const struct behavior_tap_dance_config *cfg) {
    return td_slot_valid(cfg) ? pyuron_td_slots[cfg->pyuron_slot].tapping_term_ms
                              : cfg->tapping_term_ms;
}
static inline size_t td_effective_count(const struct behavior_tap_dance_config *cfg) {
    return td_slot_valid(cfg) ? pyuron_td_slots[cfg->pyuron_slot].count_len : cfg->behavior_count;
}
static inline struct zmk_behavior_binding td_effective_binding(
    const struct behavior_tap_dance_config *cfg, int idx) {
    if (td_slot_valid(cfg)) {
        return pyuron_td_slots[cfg->pyuron_slot].steps[idx];
    }
    return cfg->behaviors[idx];
}

// --- NVS persistence -----------------------------------------------------
// One blob per slot at "ptd/<slot>": { count_len; term; {local_id,p1,p2}[4] }.
#define PYURON_TD_SETTINGS_SUBTREE "ptd"

struct pyuron_td_nvs_step {
    uint16_t local_id;
    int32_t param1;
    int32_t param2;
} __packed;

struct pyuron_td_nvs_blob {
    uint8_t count_len;
    uint32_t tapping_term_ms;
    struct pyuron_td_nvs_step steps[PYURON_TD_MAX_COUNT];
} __packed;

static void pyuron_td_settings_key(char *buf, size_t len, uint8_t slot) {
    snprintf(buf, len, PYURON_TD_SETTINGS_SUBTREE "/%u", slot);
}

static int pyuron_td_save(uint8_t slot) {
    if (slot >= PYURON_TD_SLOTS) {
        return -EINVAL;
    }
    struct pyuron_td_slot *s = &pyuron_td_slots[slot];
    struct pyuron_td_nvs_blob blob = {0};
    blob.count_len = s->count_len;
    blob.tapping_term_ms = s->tapping_term_ms;
    for (int i = 0; i < PYURON_TD_MAX_COUNT; i++) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        blob.steps[i].local_id = s->steps[i].local_id;
#else
        blob.steps[i].local_id = 0;
#endif
        blob.steps[i].param1 = (int32_t)s->steps[i].param1;
        blob.steps[i].param2 = (int32_t)s->steps[i].param2;
    }
    char key[16];
    pyuron_td_settings_key(key, sizeof(key), slot);
    return settings_save_one(key, &blob, sizeof(blob));
}

// Ensure a slot is "valid" by seeding its RAM mirror from the matching DT
// instance the first time it is edited, so unset counts keep flashed defaults.
static void pyuron_td_ensure_seeded(uint8_t slot);

int pyuron_td_set_step(uint8_t slot, uint8_t idx, uint32_t beh_id, int32_t p1, int32_t p2) {
    if (slot >= PYURON_TD_SLOTS || idx >= PYURON_TD_MAX_COUNT) {
        return -EINVAL;
    }
    pyuron_td_ensure_seeded(slot);
    struct pyuron_td_slot *s = &pyuron_td_slots[slot];
    pyuron_td_fill_binding(&s->steps[idx], beh_id, p1, p2);
    if (idx >= s->count_len) {
        s->count_len = idx + 1;
    }
    return pyuron_td_save(slot);
}

int pyuron_td_set_term(uint8_t slot, uint32_t tapping_term_ms) {
    if (slot >= PYURON_TD_SLOTS) {
        return -EINVAL;
    }
    pyuron_td_ensure_seeded(slot);
    pyuron_td_slots[slot].tapping_term_ms = MAX(tapping_term_ms, 1);
    return pyuron_td_save(slot);
}

int pyuron_td_set_len(uint8_t slot, uint8_t len) {
    if (slot >= PYURON_TD_SLOTS || len < 1 || len > PYURON_TD_MAX_COUNT) {
        return -EINVAL;
    }
    pyuron_td_ensure_seeded(slot);
    pyuron_td_slots[slot].count_len = len;
    return pyuron_td_save(slot);
}

int pyuron_td_clear(uint8_t slot) {
    if (slot >= PYURON_TD_SLOTS) {
        return -EINVAL;
    }
    pyuron_td_slots[slot] = (struct pyuron_td_slot){0};
    char key[16];
    pyuron_td_settings_key(key, sizeof(key), slot);
    settings_delete(key);
    return 0;
}

static int pyuron_td_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    uint8_t slot = (uint8_t)strtoul(name, NULL, 10);
    if (slot >= PYURON_TD_SLOTS) {
        return -EINVAL;
    }
    struct pyuron_td_nvs_blob blob;
    if (len != sizeof(blob)) {
        return -EINVAL;
    }
    ssize_t rc = read_cb(cb_arg, &blob, sizeof(blob));
    if (rc < 0) {
        return (int)rc;
    }
    struct pyuron_td_slot *s = &pyuron_td_slots[slot];
    *s = (struct pyuron_td_slot){0};
    s->valid = true;
    s->count_len = CLAMP(blob.count_len, 1, PYURON_TD_MAX_COUNT);
    s->tapping_term_ms = blob.tapping_term_ms ? blob.tapping_term_ms : 200;
    for (int i = 0; i < PYURON_TD_MAX_COUNT; i++) {
        pyuron_td_fill_binding(&s->steps[i], blob.steps[i].local_id, blob.steps[i].param1,
                               blob.steps[i].param2);
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pyuron_td, PYURON_TD_SETTINGS_SUBTREE, NULL, pyuron_td_settings_set,
                               NULL, NULL);
#endif /* CONFIG_PYURON_TAPDANCE_STUDIO_RPC */

static struct active_tap_dance *find_tap_dance(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
        if (active_tap_dances[i].position == position && !active_tap_dances[i].timer_cancelled) {
            return &active_tap_dances[i];
        }
    }
    return NULL;
}

static int new_tap_dance(struct zmk_behavior_binding_event *event,
                         const struct behavior_tap_dance_config *config,
                         struct active_tap_dance **tap_dance) {
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
        struct active_tap_dance *const ref_dance = &active_tap_dances[i];
        if (ref_dance->position == ZMK_BHV_TAP_DANCE_POSITION_FREE) {
            ref_dance->counter = 0;
            ref_dance->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            ref_dance->source = event->source;
#endif
            ref_dance->config = config;
            ref_dance->release_at = 0;
            ref_dance->is_pressed = true;
            ref_dance->timer_started = true;
            ref_dance->timer_cancelled = false;
            ref_dance->tap_dance_decided = false;
            *tap_dance = ref_dance;
            return 0;
        }
    }
    return -ENOMEM;
}

static void clear_tap_dance(struct active_tap_dance *tap_dance) {
    tap_dance->position = ZMK_BHV_TAP_DANCE_POSITION_FREE;
}

static int stop_timer(struct active_tap_dance *tap_dance) {
    int timer_cancel_result = k_work_cancel_delayable(&tap_dance->release_timer);
    if (timer_cancel_result == -EINPROGRESS) {
        // too late to cancel, we'll let the timer handler clear up.
        tap_dance->timer_cancelled = true;
    }
    return timer_cancel_result;
}

static void reset_timer(struct active_tap_dance *tap_dance,
                        struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)
    tap_dance->release_at = event.timestamp + td_effective_term(tap_dance->config);
#else
    tap_dance->release_at = event.timestamp + tap_dance->config->tapping_term_ms;
#endif
    int32_t ms_left = tap_dance->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_work_schedule(&tap_dance->release_timer, K_MSEC(ms_left));
        LOG_DBG("Successfully reset timer at position %d", tap_dance->position);
    }
}

static inline int press_tap_dance_behavior(struct active_tap_dance *tap_dance, int64_t timestamp) {
    tap_dance->tap_dance_decided = true;
#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)
    struct zmk_behavior_binding binding =
        td_effective_binding(tap_dance->config, tap_dance->counter - 1);
#else
    struct zmk_behavior_binding binding = tap_dance->config->behaviors[tap_dance->counter - 1];
#endif
    struct zmk_behavior_binding_event event = {
        .position = tap_dance->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tap_dance->source,
#endif
    };
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static inline int release_tap_dance_behavior(struct active_tap_dance *tap_dance,
                                             int64_t timestamp) {
#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)
    struct zmk_behavior_binding binding =
        td_effective_binding(tap_dance->config, tap_dance->counter - 1);
#else
    struct zmk_behavior_binding binding = tap_dance->config->behaviors[tap_dance->counter - 1];
#endif
    struct zmk_behavior_binding_event event = {
        .position = tap_dance->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tap_dance->source,
#endif
    };
    clear_tap_dance(tap_dance);
    return zmk_behavior_invoke_binding(&binding, event, false);
}

static int on_tap_dance_binding_pressed(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_tap_dance_config *cfg = dev->config;
    struct active_tap_dance *tap_dance;
    tap_dance = find_tap_dance(event.position);
    if (tap_dance == NULL) {
        if (new_tap_dance(&event, cfg, &tap_dance) == -ENOMEM) {
            LOG_ERR("Unable to create new tap dance. Insufficient space in active_tap_dances[].");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("%d created new tap dance", event.position);
    }
    tap_dance->is_pressed = true;
    LOG_DBG("%d tap dance pressed", event.position);
    stop_timer(tap_dance);
    // Increment the counter on keypress. If the counter has reached its maximum
    // value, invoke the last binding available.
#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)
    size_t behavior_count = td_effective_count(cfg);
#else
    size_t behavior_count = cfg->behavior_count;
#endif
    if (tap_dance->counter < behavior_count) {
        tap_dance->counter++;
    }
    if (tap_dance->counter == behavior_count) {
        // LOG_DBG("Tap dance has been decided via maximum counter value");
        press_tap_dance_behavior(tap_dance, event.timestamp);
        return ZMK_EV_EVENT_BUBBLE;
    }
    reset_timer(tap_dance, event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_tap_dance_binding_released(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    LOG_DBG("%d tap dance keybind released", event.position);
    struct active_tap_dance *tap_dance = find_tap_dance(event.position);
    if (tap_dance == NULL) {
        LOG_ERR("ACTIVE TAP DANCE CLEARED TOO EARLY");
        return ZMK_BEHAVIOR_OPAQUE;
    }
    tap_dance->is_pressed = false;
    if (tap_dance->tap_dance_decided) {
        release_tap_dance_behavior(tap_dance, event.timestamp);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

void behavior_tap_dance_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_tap_dance *tap_dance =
        CONTAINER_OF(d_work, struct active_tap_dance, release_timer);
    if (tap_dance->position == ZMK_BHV_TAP_DANCE_POSITION_FREE) {
        return;
    }
    if (tap_dance->timer_cancelled) {
        return;
    }
    LOG_DBG("Tap dance has been decided via timer. Counter reached: %d", tap_dance->counter);
    press_tap_dance_behavior(tap_dance, tap_dance->release_at);
    if (tap_dance->is_pressed) {
        return;
    }
    release_tap_dance_behavior(tap_dance, tap_dance->release_at);
}

static const struct behavior_driver_api behavior_tap_dance_driver_api = {
    .binding_pressed = on_tap_dance_binding_pressed,
    .binding_released = on_tap_dance_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int tap_dance_position_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_tap_dance, tap_dance_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_tap_dance, zmk_position_state_changed);

static int tap_dance_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!ev->state) {
        LOG_DBG("Ignore upstroke at position %d.", ev->position);
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
        struct active_tap_dance *tap_dance = &active_tap_dances[i];
        if (tap_dance->position == ZMK_BHV_TAP_DANCE_POSITION_FREE) {
            continue;
        }
        if (tap_dance->position == ev->position) {
            continue;
        }
        stop_timer(tap_dance);
        LOG_DBG("Tap dance interrupted, activating tap-dance at %d", tap_dance->position);
        if (!tap_dance->tap_dance_decided) {
            press_tap_dance_behavior(tap_dance, ev->timestamp);
            if (!tap_dance->is_pressed) {
                release_tap_dance_behavior(tap_dance, ev->timestamp);
            }
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int behavior_tap_dance_init(const struct device *dev) {
    static bool init_first_run = true;
    if (init_first_run) {
        for (int i = 0; i < ZMK_BHV_TAP_DANCE_MAX_HELD; i++) {
            k_work_init_delayable(&active_tap_dances[i].release_timer,
                                  behavior_tap_dance_timer_handler);
            clear_tap_dance(&active_tap_dances[i]);
        }
    }
    init_first_run = false;
    return 0;
}

#define _TRANSFORM_ENTRY(idx, node) ZMK_KEYMAP_EXTRACT_BINDING(idx, node)

#define TRANSFORMED_BINDINGS(node)                                                                 \
    {LISTIFY(DT_INST_PROP_LEN(node, bindings), _TRANSFORM_ENTRY, (, ), DT_DRV_INST(node))}

#define KP_INST(n)                                                                                 \
    static struct zmk_behavior_binding                                                             \
        behavior_tap_dance_config_##n##_bindings[DT_INST_PROP_LEN(n, bindings)] =                  \
            TRANSFORMED_BINDINGS(n);                                                               \
    static struct behavior_tap_dance_config behavior_tap_dance_config_##n = {                      \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                       \
        .behaviors = behavior_tap_dance_config_##n##_bindings,                                     \
        IF_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC,                                              \
                   (.pyuron_slot = DT_INST_PROP_OR(n, pyuron_td_slot, -1), ))                      \
        .behavior_count = DT_INST_PROP_LEN(n, bindings)};                                          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_tap_dance_init, NULL, NULL,                                \
                            &behavior_tap_dance_config_##n, POST_KERNEL,                           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_tap_dance_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#if IS_ENABLED(CONFIG_PYURON_TAPDANCE_STUDIO_RPC)

// Map editable slot index -> matching DT tap-dance config, so an edited slot can
// be seeded from its flashed bindings the first time it is touched.
#define PYURON_TD_CFG_ENTRY(n)                                                                     \
    {.slot = DT_INST_PROP_OR(n, pyuron_td_slot, -1), .cfg = &behavior_tap_dance_config_##n},

static const struct {
    int slot;
    const struct behavior_tap_dance_config *cfg;
} pyuron_td_cfg_map[] = {DT_INST_FOREACH_STATUS_OKAY(PYURON_TD_CFG_ENTRY)};

static void pyuron_td_ensure_seeded(uint8_t slot) {
    struct pyuron_td_slot *s = &pyuron_td_slots[slot];
    if (s->valid) {
        return;
    }
    *s = (struct pyuron_td_slot){0};
    s->valid = true;
    s->count_len = 1;
    s->tapping_term_ms = 200;
    for (size_t i = 0; i < ARRAY_SIZE(pyuron_td_cfg_map); i++) {
        if (pyuron_td_cfg_map[i].slot != (int)slot) {
            continue;
        }
        const struct behavior_tap_dance_config *cfg = pyuron_td_cfg_map[i].cfg;
        s->tapping_term_ms = cfg->tapping_term_ms;
        s->count_len = (uint8_t)CLAMP(cfg->behavior_count, 1, PYURON_TD_MAX_COUNT);
        for (size_t k = 0; k < cfg->behavior_count && k < PYURON_TD_MAX_COUNT; k++) {
            s->steps[k] = cfg->behaviors[k];
        }
        break;
    }
}

#endif /* CONFIG_PYURON_TAPDANCE_STUDIO_RPC */

#endif
