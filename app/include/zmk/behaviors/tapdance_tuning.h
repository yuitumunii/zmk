/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Runtime registry/API for live-editable tap-dance slots. Lets the custom
 * Studio RPC handler (src/studio/tapdance_handler.c) read and live-edit the
 * contents of each fixed tap-dance slot (&ptd0..&ptd3) without reflashing.
 * Implemented in src/behaviors/behavior_tap_dance.c, gated on
 * CONFIG_PYURON_TAPDANCE_STUDIO_RPC.
 */

#pragma once

#include <stdint.h>
#include <zmk/behavior.h>

#define PYURON_TD_SLOTS 4
#define PYURON_TD_MAX_COUNT 4

// One live tap-dance slot. `valid` means it was configured over RPC / restored
// from NVS; otherwise the DT default is used.
struct pyuron_td_slot {
    bool valid;
    uint8_t count_len;        // valid counts 1..PYURON_TD_MAX_COUNT
    uint32_t tapping_term_ms; // shared across counts
    struct zmk_behavior_binding steps[PYURON_TD_MAX_COUNT];
};

size_t pyuron_td_get_count(void);
int pyuron_td_get(uint8_t slot, struct pyuron_td_slot *out);
int pyuron_td_set_step(uint8_t slot, uint8_t idx, uint32_t beh_id, int32_t p1, int32_t p2);
int pyuron_td_set_term(uint8_t slot, uint32_t tapping_term_ms);
int pyuron_td_set_len(uint8_t slot, uint8_t len);
int pyuron_td_clear(uint8_t slot);
