/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_combos

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>
#if IS_ENABLED(CONFIG_PYURON_COMBO_STUDIO_RPC)
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <zephyr/settings/settings.h>
#include <zmk/combo_tuning.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO > 0

#warning                                                                                           \
    "CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO is deprecated, and is auto-calculated from the devicetree now."

#endif

#if CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY > 0

#warning "CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY is deprecated, and is auto-calculated."

#endif

#define COMBOS_KEYS_BYTE_ARRAY(node_id)                                                            \
    uint8_t _CONCAT(combo_prop_, node_id)[DT_PROP_LEN(node_id, key_positions)];

#define MAX_COMBO_KEYS sizeof(union {DT_INST_FOREACH_CHILD(0, COMBOS_KEYS_BYTE_ARRAY)})

struct combo_cfg {
    int32_t key_positions[MAX_COMBO_KEYS];
    int16_t key_position_len;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
#if IS_ENABLED(CONFIG_PYURON_COMBO_STUDIO_RPC)
    int pyuron_slot; // >=0 => live-editable slot index, -1 => plain DT combo
#endif
};

struct active_combo {
    uint16_t combo_idx;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[MAX_COMBO_KEYS];
};

#define PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))

#define NODE_PROP_BITMASK(n, prop)                                                                 \
    COND_CODE_1(DT_NODE_HAS_PROP(n, prop),                                                         \
                (DT_FOREACH_PROP_ELEM_SEP(n, prop, PROP_BIT_AT_IDX, (|))), (0))

#define GET_KEY_POSITION_MASK_PORTION(idx, n) ((NODE_PROP_BITMASK(n, key_positions) >> idx) & 0xFF)

#define COMBO_INST(n, positions)                                                                   \
    COND_CODE_1(IS_EQ(DT_PROP_LEN(n, key_positions), positions),                                   \
                (                                                                                  \
                    {                                                                              \
                        .timeout_ms = DT_PROP(n, timeout_ms),                                      \
                        .require_prior_idle_ms = DT_PROP(n, require_prior_idle_ms),                \
                        .key_positions = DT_PROP(n, key_positions),                                \
                        .key_position_len = DT_PROP_LEN(n, key_positions),                         \
                        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                              \
                        .slow_release = DT_PROP(n, slow_release),                                  \
                        .layer_mask = NODE_PROP_BITMASK(n, layers),                                \
                        IF_ENABLED(CONFIG_PYURON_COMBO_STUDIO_RPC,                                 \
                                   (.pyuron_slot = DT_PROP_OR(n, pyuron_combo_slot, -1), ))        \
                    }, ),                                                                          \
                ())

#define COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN(positions, _ignore)                              \
    DT_INST_FOREACH_CHILD_VARGS(0, COMBO_INST, positions)

// We do some magic here to generate the `combos` array by "key position length", looping
// by key position length and on each iteration, only include entries where the `key-positions`
// length matches.
// Doing so allows our bitmasks to be "shorted key positions list first" when searching for matches.
// `20` is chosen as a reasonable limit, since the theoretical maximum number of keys you might
// reasonably press simultaneously with 10 fingers is 20 keys, two keys per finger.
static const struct combo_cfg combos[] = {
    LISTIFY(20, COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN, (), 0)};

#if IS_ENABLED(CONFIG_PYURON_COMBO_STUDIO_RPC)
// Mutable working copy of the combo table. Seeded from the DT `combos[]` at
// init; entries tagged with a pyuron slot can have their key set / behavior /
// timeout / layer mask / enabled state rewritten at runtime. All combo reads go
// through pyuron_combo_at() so the live table is the single source of truth.
// Disabled editable slots get key_position_len=0 so they never match.
static struct combo_cfg combo_work[ARRAY_SIZE(combos)];
// slot index -> combo_work index (-1 if that slot has no DT placeholder).
static int pyuron_combo_slot_to_idx[PYURON_COMBO_SLOTS];

static inline const struct combo_cfg *pyuron_combo_at(size_t idx) { return &combo_work[idx]; }

static int pyuron_combo_rebuild_lookup(void);
#else
static inline const struct combo_cfg *pyuron_combo_at(size_t idx) { return &combos[idx]; }
#endif

#define COMBO_ONE(n) +1

#define COMBO_CHILDREN_COUNT (0 DT_INST_FOREACH_CHILD(0, COMBO_ONE))

// We need at least 4 bytes to avoid alignment issues
#define BYTES_FOR_COMBOS_MASK DIV_ROUND_UP(COMBO_CHILDREN_COUNT, 32)

uint8_t pressed_keys_count = 0;
// set of keys pressed
struct zmk_position_state_changed_event pressed_keys[MAX_COMBO_KEYS] = {};
// the set of candidate combos based on the currently pressed_keys
uint32_t candidates[BYTES_FOR_COMBOS_MASK];
// the last candidate that was completely pressed
int16_t fully_pressed_combo = INT16_MAX;
// a lookup dict that maps a key position to all combos on that position
uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_COMBOS_MASK] = {};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
uint8_t active_combo_count = 0;

struct k_work_delayable timeout_task;
int64_t timeout_task_timeout_at;

// this keeps track of the last non-combo, non-mod key tap
int64_t last_tapped_timestamp = INT32_MIN;
// this keeps track of the last time a combo was pressed
int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

// Store the combo key pointer in the combos array, one pointer for each key position
// The combos are sorted shortest-first, then by virtual-key-position.
static int initialize_combo(size_t index) {
    const struct combo_cfg *new_combo = pyuron_combo_at(index);

    for (size_t kp = 0; kp < new_combo->key_position_len; kp++) {
        sys_bitfield_set_bit((mem_addr_t)&combo_lookup[new_combo->key_positions[kp]], index);
    }

    return 0;
}

static bool combo_active_on_layer(const struct combo_cfg *combo, uint8_t layer) {
    if (!combo->layer_mask) {
        return true;
    }

    return combo->layer_mask & BIT(layer);
}

static bool is_quick_tap(const struct combo_cfg *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            const struct combo_cfg *combo = pyuron_combo_at(i);
            if (combo_active_on_layer(combo, highest_active_layer) &&
                !is_quick_tap(combo, timestamp)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
            // LOG_DBG("combo timeout %d %d %d", position, i, candidates[i].timeout_at);
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    for (int i = 0; i < BYTES_FOR_COMBOS_MASK; i++) {
        candidates[i] &= combo_lookup[position][i];
        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    if (pressed_keys_count == 0) {
        return LONG_MAX;
    }

    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, pyuron_combo_at(i)->timeout_ms);
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct combo_cfg *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    return candidate->key_position_len == pressed_keys_count;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for a candidate timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {

            if (pressed_keys[0].data.timestamp + pyuron_combo_at(i)->timeout_ms > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == MAX_COMBO_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

const struct zmk_listener zmk_listener_combo;

static int release_pressed_keys() {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is needed)
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length =
        MIN(pressed_keys_count, pyuron_combo_at(active_combo->combo_idx)->key_position_len);
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    // move any other pressed keys up
    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, pyuron_combo_at(combo_idx),
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count] = (struct active_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = active_combo->key_positions_pressed_count ==
                                pyuron_combo_at(active_combo->combo_idx)->key_position_len;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else { // position matches
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            const struct combo_cfg *c = pyuron_combo_at(active_combo->combo_idx);
            if ((c->slow_release && all_keys_released) || (!c->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, c, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    memset(candidates, 0, BYTES_FOR_COMBOS_MASK * sizeof(uint32_t));
    if (fully_pressed_combo != INT16_MAX) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = INT16_MAX;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
        for (int i = 0; i < ARRAY_SIZE(combos); i++) {
            if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
                const struct combo_cfg *candidate_combo = pyuron_combo_at(i);
                if (candidate_is_completely_pressed(candidate_combo)) {
                    fully_pressed_combo = i;
                    if (num_candidates == 1) {
                        cleanup();
                    }
                }

                return ret;
            }
        }
    } else {
        cleanup();
        return ret;
    }

    return -EINVAL;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        // timer was cancelled or rescheduled.
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        LOG_DBG("CLEANUP!");
        cleanup();
    }

    LOG_DBG("ABOUT TO UPDATE IN TIMEOUT");
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

int behavior_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(combo, behavior_combo_listener);
ZMK_SUBSCRIPTION(combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(combo, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_PYURON_COMBO_STUDIO_RPC)

// --- live combo editing (RAM mirror + NVS) -------------------------------
// Each editable slot's full state persists at "pcmb/<slot>". On boot the
// settings handler stashes the blobs; combo_init() seeds combo_work[] from DT,
// applies any restored slots, then builds the lookup once.

#define PYURON_COMBO_SETTINGS_SUBTREE "pcmb"

struct pyuron_combo_nvs_blob {
    bool enabled;
    uint8_t key_len;
    int32_t key_positions[PYURON_COMBO_MAX_KEYS];
    int32_t timeout_ms;
    uint32_t layer_mask;
    uint16_t behavior_local_id;
    int32_t param1;
    int32_t param2;
} __packed;

// Restored-from-NVS slot blobs, kept until combo_init() applies them.
static struct pyuron_combo_nvs_blob pyuron_combo_restore[PYURON_COMBO_SLOTS];
static bool pyuron_combo_restore_valid[PYURON_COMBO_SLOTS];

static void pyuron_combo_settings_key(char *buf, size_t len, uint8_t slot) {
    snprintf(buf, len, PYURON_COMBO_SETTINGS_SUBTREE "/%u", slot);
}

// Write a slot's live combo_work[] entry into a packed NVS blob.
static void pyuron_combo_pack(uint8_t slot, struct pyuron_combo_nvs_blob *blob) {
    int idx = pyuron_combo_slot_to_idx[slot];
    *blob = (struct pyuron_combo_nvs_blob){0};
    if (idx < 0) {
        return;
    }
    const struct combo_cfg *c = &combo_work[idx];
    blob->enabled = (c->key_position_len > 0);
    blob->key_len = (uint8_t)c->key_position_len;
    for (int i = 0; i < PYURON_COMBO_MAX_KEYS && i < c->key_position_len; i++) {
        blob->key_positions[i] = c->key_positions[i];
    }
    blob->timeout_ms = c->timeout_ms;
    blob->layer_mask = c->layer_mask;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    blob->behavior_local_id = c->behavior.local_id;
#endif
    blob->param1 = (int32_t)c->behavior.param1;
    blob->param2 = (int32_t)c->behavior.param2;
}

// Apply a packed blob into the live combo_work[] entry for `slot`.
// Disabled / empty key sets collapse to key_position_len=0 (never matches).
static void pyuron_combo_apply_blob(uint8_t slot, const struct pyuron_combo_nvs_blob *blob) {
    int idx = pyuron_combo_slot_to_idx[slot];
    if (idx < 0) {
        return;
    }
    struct combo_cfg *c = &combo_work[idx];
    uint8_t key_len = MIN(blob->key_len, (uint8_t)PYURON_COMBO_MAX_KEYS);
    if (!blob->enabled || key_len == 0) {
        c->key_position_len = 0;
        return;
    }
    c->key_position_len = key_len;
    for (int i = 0; i < PYURON_COMBO_MAX_KEYS; i++) {
        c->key_positions[i] = (i < key_len) ? blob->key_positions[i] : 0;
    }
    c->timeout_ms = blob->timeout_ms > 0 ? blob->timeout_ms : 50;
    c->layer_mask = blob->layer_mask;
    c->behavior = (struct zmk_behavior_binding){0};
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    c->behavior.local_id = blob->behavior_local_id;
#endif
    c->behavior.behavior_dev =
        blob->behavior_local_id
            ? zmk_behavior_find_behavior_name_from_local_id(blob->behavior_local_id)
            : NULL;
    c->behavior.param1 = (uint32_t)blob->param1;
    c->behavior.param2 = (uint32_t)blob->param2;
}

// Clear all candidate/active tracking then rebuild combo_lookup from scratch.
// Must run whenever any combo's key set changes.
static int pyuron_combo_rebuild_lookup(void) {
    memset(combo_lookup, 0, sizeof(combo_lookup));
    memset(candidates, 0, sizeof(candidates));
    fully_pressed_combo = INT16_MAX;
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i] = (struct active_combo){0};
        active_combos[i].combo_idx = UINT16_MAX;
    }
    active_combo_count = 0;
    pressed_keys_count = 0;
    for (int i = 0; i < ARRAY_SIZE(combo_work); i++) {
        if (combo_work[i].key_position_len > 0) {
            initialize_combo(i);
        }
    }
    return 0;
}

size_t pyuron_combo_get_count(void) { return PYURON_COMBO_SLOTS; }

int pyuron_combo_get(uint8_t slot, struct pyuron_combo_slot *out) {
    if (slot >= PYURON_COMBO_SLOTS || out == NULL) {
        return -EINVAL;
    }
    *out = (struct pyuron_combo_slot){0};
    int idx = pyuron_combo_slot_to_idx[slot];
    if (idx < 0) {
        return 0; // no DT placeholder -> empty/disabled view
    }
    const struct combo_cfg *c = &combo_work[idx];
    out->enabled = (c->key_position_len > 0);
    out->valid = out->enabled;
    out->key_len = (uint8_t)c->key_position_len;
    for (int i = 0; i < PYURON_COMBO_MAX_KEYS && i < c->key_position_len; i++) {
        out->key_positions[i] = c->key_positions[i];
    }
    out->timeout_ms = c->timeout_ms;
    out->layer_mask = c->layer_mask;
    out->behavior = c->behavior;
    return 0;
}

int pyuron_combo_set(uint8_t slot, const int32_t *keys, uint8_t key_len, uint32_t beh_id,
                     int32_t p1, int32_t p2, uint32_t timeout_ms, uint32_t layer_mask,
                     bool enabled) {
    if (slot >= PYURON_COMBO_SLOTS || key_len > PYURON_COMBO_MAX_KEYS) {
        return -EINVAL;
    }
    if (pyuron_combo_slot_to_idx[slot] < 0) {
        return -ENODEV; // no DT placeholder reserved for this slot
    }
    struct pyuron_combo_nvs_blob blob = {0};
    blob.enabled = enabled;
    blob.key_len = key_len;
    for (int i = 0; i < key_len; i++) {
        blob.key_positions[i] = keys[i];
    }
    blob.timeout_ms = timeout_ms > 0 ? (int32_t)timeout_ms : 50;
    blob.layer_mask = layer_mask;
    blob.behavior_local_id = (uint16_t)beh_id;
    blob.param1 = p1;
    blob.param2 = p2;

    pyuron_combo_apply_blob(slot, &blob);
    pyuron_combo_rebuild_lookup();

    char key[16];
    pyuron_combo_settings_key(key, sizeof(key), slot);
    return settings_save_one(key, &blob, sizeof(blob));
}

int pyuron_combo_clear(uint8_t slot) {
    if (slot >= PYURON_COMBO_SLOTS) {
        return -EINVAL;
    }
    int idx = pyuron_combo_slot_to_idx[slot];
    if (idx >= 0) {
        // Restore the DT default for this slot, then disable it.
        combo_work[idx] = combos[idx];
        combo_work[idx].key_position_len = 0;
    }
    pyuron_combo_rebuild_lookup();
    char key[16];
    pyuron_combo_settings_key(key, sizeof(key), slot);
    settings_delete(key);
    return 0;
}

static int pyuron_combo_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    uint8_t slot = (uint8_t)strtoul(name, NULL, 10);
    if (slot >= PYURON_COMBO_SLOTS) {
        return -EINVAL;
    }
    struct pyuron_combo_nvs_blob blob;
    if (len != sizeof(blob)) {
        return -EINVAL;
    }
    ssize_t rc = read_cb(cb_arg, &blob, sizeof(blob));
    if (rc < 0) {
        return (int)rc;
    }
    // Stash; combo_init() applies after combo_work[] is seeded from DT.
    pyuron_combo_restore[slot] = blob;
    pyuron_combo_restore_valid[slot] = true;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pyuron_combo, PYURON_COMBO_SETTINGS_SUBTREE, NULL,
                               pyuron_combo_settings_set, NULL, NULL);

#endif /* CONFIG_PYURON_COMBO_STUDIO_RPC */

static int combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    k_work_init_delayable(&timeout_task, combo_timeout_handler);
    LOG_WRN("Have %d combos!", ARRAY_SIZE(combos));

#if IS_ENABLED(CONFIG_PYURON_COMBO_STUDIO_RPC)
    // Seed the mutable working copy from the flashed DT combos.
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        combo_work[i] = combos[i];
    }
    // Build slot index -> combo_work index map.
    for (int s = 0; s < PYURON_COMBO_SLOTS; s++) {
        pyuron_combo_slot_to_idx[s] = -1;
    }
    for (int i = 0; i < ARRAY_SIZE(combo_work); i++) {
        int slot = combo_work[i].pyuron_slot;
        if (slot >= 0 && slot < PYURON_COMBO_SLOTS) {
            pyuron_combo_slot_to_idx[slot] = i;
            // Placeholder slots start disabled until configured over RPC / NVS.
            combo_work[i].key_position_len = 0;
        }
    }
    // Apply any NVS-restored slot overrides captured by the settings handler.
    for (int s = 0; s < PYURON_COMBO_SLOTS; s++) {
        if (pyuron_combo_restore_valid[s]) {
            pyuron_combo_apply_blob((uint8_t)s, &pyuron_combo_restore[s]);
        }
    }
    for (int i = 0; i < ARRAY_SIZE(combo_work); i++) {
        if (combo_work[i].key_position_len > 0) {
            initialize_combo(i);
        }
    }
#else
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        initialize_combo(i);
    }
#endif
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif
