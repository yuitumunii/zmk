/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Runtime registry/API for live-editable macro slots. Lets the custom Studio
 * RPC handler (src/studio/macro_handler.c) read and live-edit the contents of
 * each fixed macro slot (&pmac0..&pmac5) without reflashing.
 * Implemented in src/behaviors/behavior_macro.c, gated on
 * CONFIG_PYURON_MACRO_STUDIO_RPC.
 */

#pragma once

#include <stdint.h>
#include <zmk/behavior.h>

#define PYURON_MACRO_SLOTS 6
#define PYURON_MACRO_MAX_STEPS 16

// One live macro slot. `valid` means the slot was configured over RPC (or
// restored from NVS); otherwise the DT default (empty / &none) is used.
struct pyuron_macro_slot {
    bool valid;
    uint8_t step_count; // valid steps 0..PYURON_MACRO_MAX_STEPS
    struct zmk_behavior_binding steps[PYURON_MACRO_MAX_STEPS];
};

// Number of editable macro slots (= PYURON_MACRO_SLOTS).
size_t pyuron_macro_get_count(void);

// Copy slot state into *out. Returns 0, or -EINVAL on bad slot/out.
int pyuron_macro_get(uint8_t slot, struct pyuron_macro_slot *out);

// Set one step. beh_id is a behavior local id (0 = &none). Persists to NVS.
int pyuron_macro_set_step(uint8_t slot, uint8_t idx, uint32_t beh_id, int32_t p1, int32_t p2);

// Set the valid step count. Persists to NVS.
int pyuron_macro_set_len(uint8_t slot, uint8_t len);

// Clear the slot (len=0, all steps none) and drop NVS overrides.
int pyuron_macro_clear(uint8_t slot);
