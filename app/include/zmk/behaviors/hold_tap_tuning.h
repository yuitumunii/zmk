/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Runtime registry/API for hold-tap (&mt / &lt) behavior instances. Lets the
 * custom Studio RPC handler (src/studio/timing_handler.c) read and live-change
 * each instance's tapping-term-ms / quick-tap-ms / flavor without reflashing.
 * Implemented in src/behaviors/behavior_hold_tap.c, gated on
 * CONFIG_PYURON_TIMING_STUDIO_RPC.
 */

#pragma once

#include <stdint.h>

// Tunable parameters. Values MUST match enum Param in
// proto/pyuron/timing/timing.proto.
enum hold_tap_tuning_param {
    HOLD_TAP_TUNING_PARAM_TAPPING_TERM_MS = 0,
    HOLD_TAP_TUNING_PARAM_QUICK_TAP_MS = 1,
    HOLD_TAP_TUNING_PARAM_FLAVOR = 2,
};

struct hold_tap_tuning_info {
    const char *name; // devicetree node name (static storage), e.g. "mt" / "lt"
    uint32_t tapping_term_ms;
    int32_t quick_tap_ms; // signed: -1 means "disabled"
    uint32_t flavor;      // 0..3
};

// Number of hold-tap instances present on this build.
int hold_tap_tuning_get_count(void);

// Fill *out with the current (live) state of instance `id`.
// Returns 0 on success, -EINVAL if id is out of range.
int hold_tap_tuning_get_info(uint32_t id, struct hold_tap_tuning_info *out);

// Live-change one parameter of instance `id`. Takes effect immediately.
// Returns 0 on success, -EINVAL on bad id/param.
int hold_tap_tuning_set_param(uint32_t id, enum hold_tap_tuning_param param, int32_t value);

// Persist one parameter to NVS so it survives reboot. Returns 0 on success.
int hold_tap_tuning_save_param(uint32_t id, enum hold_tap_tuning_param param, int32_t value);

// Restore instance `id` to its devicetree (flashed) defaults.
// Returns 0 on success, -EINVAL if id is out of range.
int hold_tap_tuning_reset(uint32_t id);
