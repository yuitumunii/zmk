/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Runtime registry/API for live-editable combo slots. Lets the custom Studio
 * RPC handler (src/studio/combo_handler.c) read and live-edit a fixed set of
 * combo slots (key positions + fire behavior + timeout + layer mask) without
 * reflashing. Implemented in src/combo.c, gated on
 * CONFIG_PYURON_COMBO_STUDIO_RPC.
 */

#pragma once

#include <stdint.h>
#include <zmk/behavior.h>

#define PYURON_COMBO_SLOTS 6
#define PYURON_COMBO_MAX_KEYS 3

// One live combo slot. `valid` means it was configured over RPC / restored from
// NVS. `enabled` means it should fire (set false / empty key set = inactive).
struct pyuron_combo_slot {
    bool valid;
    bool enabled;
    uint8_t key_len; // 0..PYURON_COMBO_MAX_KEYS
    int32_t key_positions[PYURON_COMBO_MAX_KEYS];
    int32_t timeout_ms;
    uint32_t layer_mask; // 0 = all layers
    struct zmk_behavior_binding behavior;
};

size_t pyuron_combo_get_count(void);
int pyuron_combo_get(uint8_t slot, struct pyuron_combo_slot *out);
int pyuron_combo_set(uint8_t slot, const int32_t *keys, uint8_t key_len, uint32_t beh_id, int32_t p1,
                     int32_t p2, uint32_t timeout_ms, uint32_t layer_mask, bool enabled);
int pyuron_combo_clear(uint8_t slot);
