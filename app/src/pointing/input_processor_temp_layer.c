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
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG)
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/atomic.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_REBOOT)
#include <zephyr/sys/reboot.h>
#endif
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
#include <stdint.h>
#include <stdlib.h>          /* strtol (excluded-positions seed parser) */
#include <string.h>          /* strcmp / memcpy */
#include <zephyr/settings/settings.h>
#include <zmk/pointing/aml.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Constants and Types */
#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG)
#define TEMP_LAYER_HARD_WDT_NODE DT_NODELABEL(wdt0)

BUILD_ASSERT(DT_NODE_HAS_STATUS(TEMP_LAYER_HARD_WDT_NODE, okay),
             "CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG requires wdt0");
BUILD_ASSERT(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_FEED_MS <
                 CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_RESET_MS,
             "HARD_WATCHDOG_FEED_MS must be lower than HARD_WATCHDOG_RESET_MS");

static const struct device *const temp_layer_hard_wdt = DEVICE_DT_GET(TEMP_LAYER_HARD_WDT_NODE);
static struct k_timer temp_layer_hard_wdt_timer;
static int temp_layer_hard_wdt_channel = -1;
static atomic_t temp_layer_hard_wdt_ready;
static atomic_t temp_layer_hard_wdt_deadline;
static atomic_t temp_layer_hard_wdt_armed_layer;
static atomic_t temp_layer_hard_wdt_feed_blocked;

static void temp_layer_hard_wdt_arm(uint8_t layer) {
    if (!atomic_get(&temp_layer_hard_wdt_ready)) {
        return;
    }

    atomic_set(&temp_layer_hard_wdt_armed_layer, layer + 1);
    atomic_set(&temp_layer_hard_wdt_deadline,
               k_uptime_get_32() +
                   CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_STUCK_TIMEOUT_MS);
    atomic_clear(&temp_layer_hard_wdt_feed_blocked);
}

static void temp_layer_hard_wdt_extend(uint8_t layer) {
    if (atomic_get(&temp_layer_hard_wdt_armed_layer) != layer + 1) {
        return;
    }

    atomic_set(&temp_layer_hard_wdt_deadline,
               k_uptime_get_32() +
                   CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_STUCK_TIMEOUT_MS);
    atomic_clear(&temp_layer_hard_wdt_feed_blocked);
}

static void temp_layer_hard_wdt_disarm(uint8_t layer) {
    if (atomic_get(&temp_layer_hard_wdt_armed_layer) != layer + 1) {
        return;
    }

    atomic_clear(&temp_layer_hard_wdt_armed_layer);
    atomic_clear(&temp_layer_hard_wdt_deadline);
    atomic_clear(&temp_layer_hard_wdt_feed_blocked);
}

static void temp_layer_hard_wdt_timer_cb(struct k_timer *timer) {
    if (!atomic_get(&temp_layer_hard_wdt_ready)) {
        return;
    }

    if (atomic_get(&temp_layer_hard_wdt_feed_blocked)) {
        return;
    }

    uint32_t deadline = (uint32_t)atomic_get(&temp_layer_hard_wdt_deadline);
    if (deadline != 0 && (int32_t)(k_uptime_get_32() - deadline) >= 0) {
        atomic_set(&temp_layer_hard_wdt_feed_blocked, 1);
        LOG_ERR("Temporary layer hard watchdog expired for layer %d; waiting for WDT reset",
                (int)atomic_get(&temp_layer_hard_wdt_armed_layer) - 1);
        return;
    }

    int ret = wdt_feed(temp_layer_hard_wdt, temp_layer_hard_wdt_channel);
    if (ret < 0) {
        LOG_WRN("Temporary layer hard watchdog feed failed (%d)", ret);
    }
}

static int temp_layer_hard_wdt_init(void) {
    static bool initialized;

    if (initialized) {
        return 0;
    }

    initialized = true;

    if (!device_is_ready(temp_layer_hard_wdt)) {
        LOG_ERR("Temporary layer hard watchdog device is not ready");
        return -ENODEV;
    }

    struct wdt_timeout_cfg cfg = {
        .flags = WDT_FLAG_RESET_SOC,
        .window = {
            .min = 0,
            .max = CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_RESET_MS,
        },
    };

    temp_layer_hard_wdt_channel = wdt_install_timeout(temp_layer_hard_wdt, &cfg);
    if (temp_layer_hard_wdt_channel < 0) {
        LOG_ERR("Temporary layer hard watchdog install failed (%d)",
                temp_layer_hard_wdt_channel);
        return temp_layer_hard_wdt_channel;
    }

    int ret = wdt_setup(temp_layer_hard_wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        LOG_ERR("Temporary layer hard watchdog setup failed (%d)", ret);
        return ret;
    }

    atomic_set(&temp_layer_hard_wdt_ready, 1);
    k_timer_init(&temp_layer_hard_wdt_timer, temp_layer_hard_wdt_timer_cb, NULL);
    k_timer_start(&temp_layer_hard_wdt_timer,
                  K_MSEC(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_FEED_MS),
                  K_MSEC(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_FEED_MS));

    LOG_INF("Temporary layer hard watchdog started: stuck=%dms reset=%dms",
            CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_STUCK_TIMEOUT_MS,
            CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_HARD_WATCHDOG_RESET_MS);

    return 0;
}
#else
static void temp_layer_hard_wdt_arm(uint8_t layer) {}
static void temp_layer_hard_wdt_extend(uint8_t layer) {}
static void temp_layer_hard_wdt_disarm(uint8_t layer) {}
static int temp_layer_hard_wdt_init(void) { return 0; }
#endif

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
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
static struct k_work_delayable layer_stuck_recovery_works[MAX_LAYERS];
#endif

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

/* ---- Version-gated seed -------------------------------------------------- */
/* NVS から読み込んだ「取り込み済みシードバージョン」。settings_load 中に
 * "aml/seedver" が見つかれば aml_settings_set で更新される。NVS に無ければ
 * 0 のまま(=まだ一度もシードしていない)。 */
static uint32_t aml_seedver_loaded;

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
    } else if (strcmp(name, "seedver") == 0) {
        if (len != sizeof(uint32_t)) return -EINVAL;
        uint32_t v;
        ssize_t rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc < 0) return (int)rc;
        aml_seedver_loaded = v;
    }
    return 0;
}

/* ---- Version-gated seed application -------------------------------------- */
/* 空白区切り文字列("12 34 56")を uint16 配列へパースする。範囲外/上限超過は
 * 無視。strtol を使いオーバーランしない。戻り値 = 取り込んだ件数。 */
static uint8_t aml_parse_excluded(const char *s, uint16_t *out, uint8_t max) {
    uint8_t n = 0;
    if (!s) {
        return 0;
    }
    const char *p = s;
    while (*p && n < max) {
        /* 区切り(空白・タブ等)を読み飛ばす */
        while (*p && !(*p >= '0' && *p <= '9') && *p != '-') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *end = NULL;
        long val = strtol(p, &end, 10);
        if (end == p) {
            /* 数値として進まなかった = 不正文字。1 文字進めて続行 */
            p++;
            continue;
        }
        p = end;
        if (val < 0 || val > UINT16_MAX) {
            /* 範囲外は無視 */
            continue;
        }
        out[n++] = (uint16_t)val;
    }
    return n;
}

/* settings_load() による全 set 呼出しが終わった後に呼ばれる commit。
 * ここで初めて aml_seedver_loaded が確定するので、シード判定は必ずここで行う
 * (set より前に走らせると seedver が常に 0 扱いになり毎回シードしてしまう)。 */
static int aml_settings_commit(void) {
    if (CONFIG_PYURON_AML_SEED_VERSION <= 0) {
        /* version=0(既定) = シードしない。現状の NVS 値は一切触らない。 */
        return 0;
    }
    if ((uint32_t)CONFIG_PYURON_AML_SEED_VERSION <= aml_seedver_loaded) {
        LOG_DBG("AML seed v%d already applied (loaded v%u), skipping",
                CONFIG_PYURON_AML_SEED_VERSION, aml_seedver_loaded);
        return 0;
    }

    /* バージョンが上がった回だけ 1 度取り込む。
     * 印 -1(または空文字)は「未指定」とみなし、その項目は据え置く。
     * 0 以上は明示値として焼く。これにより prior_idle_ms=0(ガードなし)や
     * extend_ms=0(延長なし)という有効値も正しくシードできる。未指定(-1)の
     * deactivation は 0 のまま残り、初回 handle_event での param2 由来の
     * 既定シードを壊さない。 */
    if (CONFIG_PYURON_AML_SEED_DEACTIVATION_MS >= 0) {
        aml_rt.deactivation_ms = (uint32_t)CONFIG_PYURON_AML_SEED_DEACTIVATION_MS;
    }
    if (CONFIG_PYURON_AML_SEED_PRIOR_IDLE_MS >= 0) {
        aml_rt.prior_idle_ms = (uint32_t)CONFIG_PYURON_AML_SEED_PRIOR_IDLE_MS;
    }
    if (CONFIG_PYURON_AML_SEED_EXTEND_MS >= 0) {
        aml_rt.extend_ms = (uint32_t)CONFIG_PYURON_AML_SEED_EXTEND_MS;
    }
    {
        const char *excl_str = CONFIG_PYURON_AML_SEED_EXCLUDED;
        if (excl_str && excl_str[0] != '\0') {
            uint16_t parsed[AML_MAX_EXCLUDED];
            uint8_t n = aml_parse_excluded(excl_str, parsed, AML_MAX_EXCLUDED);
            aml_rt.num_excluded = n;
            memcpy(aml_rt.excluded_positions, parsed, n * sizeof(uint16_t));
        }
    }

    /* AML のキー(dec/idle/ext/excl)だけを永続化。BLE ボンド等の他 NVS は触らない。 */
    int rc = zmk_aml_save();
    if (rc < 0) {
        LOG_ERR("AML seed save failed (%d)", rc);
        return rc;
    }
    /* 取り込み済みバージョンを更新(以後 同バージョンでは再シードしない)。 */
    uint32_t ver = (uint32_t)CONFIG_PYURON_AML_SEED_VERSION;
    rc = settings_save_one(AML_SETTINGS_SUBTREE "/seedver", &ver, sizeof(ver));
    if (rc < 0) {
        LOG_ERR("AML seedver save failed (%d)", rc);
        return rc;
    }
    aml_seedver_loaded = ver;
    LOG_INF("seeded AML config v%u", ver);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pyuron_aml, AML_SETTINGS_SUBTREE, NULL,
                               aml_settings_set, aml_settings_commit, NULL);

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

static bool set_layer_state_locked(struct temp_layer_state *state, uint8_t layer, bool activate) {
    /* Only mutate temp-layer state here. zmk_keymap_layer_activate/deactivate raises
     * layer_state_changed synchronously, so calling it while holding data->lock can
     * re-enter this listener and deadlock the input path. */
    state->toggle_layer = layer;
    state->is_active = activate;

    bool layer_on = zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(layer));
    return activate ? !layer_on : layer_on;
}

static void apply_layer_state_unlocked(uint8_t layer, bool activate) {
    bool layer_on = zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(layer));
    if (activate && !layer_on) {
        zmk_keymap_layer_activate(layer, false);
        LOG_DBG("Layer %d activated", layer);
    } else if (!activate && layer_on) {
        zmk_keymap_layer_deactivate(layer, false);
        LOG_DBG("Layer %d deactivated", layer);
    }
}

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
static void schedule_stuck_recovery(uint8_t layer) {
    k_work_reschedule(&layer_stuck_recovery_works[layer],
                      K_MSEC(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_TIMEOUT_MS));
}

static void cancel_stuck_recovery(uint8_t layer) {
    k_work_cancel_delayable(&layer_stuck_recovery_works[layer]);
}
#endif

struct layer_state_action {
    uint8_t layer;
    bool activate;
};

K_MSGQ_DEFINE(temp_layer_action_msgq, sizeof(struct layer_state_action),
              CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_MAX_ACTION_EVENTS, 4);

static void layer_action_work_cb(struct k_work *work) {

    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    struct layer_state_action action;

    while (k_msgq_get(&temp_layer_action_msgq, &action, K_MSEC(10)) >= 0) {
        bool apply_layer_change = false;

        int ret = k_mutex_lock(&data->lock, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("Error locking for updating %d", ret);
            continue;
        }

        if (!action.activate) {
            apply_layer_change = set_layer_state_locked(&data->state, action.layer, false);
            k_work_cancel_delayable(&layer_disable_works[action.layer]);
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
            cancel_stuck_recovery(action.layer);
#endif
        } else {
            apply_layer_change = set_layer_state_locked(&data->state, action.layer, true);
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
            schedule_stuck_recovery(action.layer);
#endif
        }

        k_mutex_unlock(&data->lock);

        if (apply_layer_change) {
            apply_layer_state_unlocked(action.layer, action.activate);
        }

        if (!action.activate) {
            temp_layer_hard_wdt_disarm(action.layer);
        } else {
            temp_layer_hard_wdt_arm(action.layer);
        }
    }
}

static K_WORK_DEFINE(layer_action_work, layer_action_work_cb);

/* ---- Work Queue Callback ------------------------------------------------- */

static void layer_disable_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    int layer_index = ARRAY_INDEX(layer_disable_works, d_work);

    struct layer_state_action action = {.layer = layer_index, .activate = false};

    k_msgq_put(&temp_layer_action_msgq, &action, K_MSEC(10));
    k_work_submit(&layer_action_work);
}

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
static void layer_stuck_recovery_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    int layer_index = ARRAY_INDEX(layer_stuck_recovery_works, d_work);
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);

    if (!zmk_keymap_layer_active(layer_id)) {
        return;
    }

    LOG_WRN("Temporary layer %d remained active for %d ms; forcing recovery", layer_index,
            CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_TIMEOUT_MS);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_MSEC(100));
    if (ret == 0) {
        if (data->state.toggle_layer == layer_index) {
            data->state.is_active = false;
        }
        k_work_cancel_delayable(&layer_disable_works[layer_index]);
        k_mutex_unlock(&data->lock);
    } else {
        LOG_WRN("Could not lock temp layer state for stuck recovery (%d)", ret);
    }

    zmk_keymap_layer_deactivate(layer_id, false);
    temp_layer_hard_wdt_disarm(layer_index);

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_REBOOT)
    LOG_WRN("Rebooting after temporary layer stuck recovery");
    sys_reboot(SYS_REBOOT_WARM);
#endif
}
#endif

/* ---- Event Handlers ------------------------------------------------------ */

static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_NO_WAIT);
    if (ret < 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(data->state.toggle_layer))) {
        LOG_DBG("Deactivating layer that was activated by this processor");
        data->state.is_active = false;
        k_work_cancel_delayable(&layer_disable_works[data->state.toggle_layer]);
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
        cancel_stuck_recovery(data->state.toggle_layer);
#endif
        temp_layer_hard_wdt_disarm(data->state.toggle_layer);
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
    bool apply_layer_change = false;
    uint8_t layer = data->state.toggle_layer;
    bool deactivate_layer = false;
    bool disarm_watchdog = false;

    if (data->state.is_active) {
        if (!position_is_excluded(cfg, ev->position)) {
            LOG_DBG("Position not excluded, deactivating layer");
            layer = data->state.toggle_layer;
            apply_layer_change = set_layer_state_locked(&data->state, layer, false);
            k_work_cancel_delayable(&layer_disable_works[layer]);
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
            cancel_stuck_recovery(layer);
#endif
            deactivate_layer = true;
            disarm_watchdog = true;
        } else {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
            /* Excluded key pressed while AML is active: extend the dwell timer
             * from now, so actively clicking (e.g. K / left-click) keeps the
             * mouse layer alive just like trackball motion does. The amount is
             * a separate, app-adjustable value (defaults to the dwell). */
            layer = data->state.toggle_layer;
            uint32_t timeout_ms = aml_rt.extend_ms;
            if (timeout_ms > 0) {
                k_work_reschedule(&layer_disable_works[layer], K_MSEC(timeout_ms));
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
                schedule_stuck_recovery(layer);
#endif
                temp_layer_hard_wdt_extend(layer);
                LOG_DBG("Excluded position, extending AML by %u ms", timeout_ms);
            }
#else
            LOG_DBG("Position excluded, continuing");
#endif
        }
    }

    k_mutex_unlock(&data->lock);

    if (deactivate_layer && apply_layer_change) {
        apply_layer_state_unlocked(layer, false);
    }
    if (disarm_watchdog) {
        temp_layer_hard_wdt_disarm(layer);
    }

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
         * lock を解放した後の work queue 経由で行う。 */
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
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
        schedule_stuck_recovery(param1);
#endif
        temp_layer_hard_wdt_extend(param1);
    }

    k_mutex_unlock(&data->lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int temp_layer_init(const struct device *dev) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    k_mutex_init(&data->lock);

    for (int i = 0; i < MAX_LAYERS; i++) {
        k_work_init_delayable(&layer_disable_works[i], layer_disable_callback);
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUCK_RECOVERY)
        k_work_init_delayable(&layer_stuck_recovery_works[i], layer_stuck_recovery_callback);
#endif
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

    int err = temp_layer_hard_wdt_init();
    if (err < 0) {
        LOG_WRN("Temporary layer hard watchdog disabled after init error (%d)", err);
    }

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
