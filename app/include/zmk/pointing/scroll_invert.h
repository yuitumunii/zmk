/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Public API for runtime scroll invert config.
 * Implemented in input_processor_pyuron_scroll_invert.c when
 * CONFIG_PYURON_SCROLL_STUDIO_RPC is enabled.
 */
#pragma once

#include <stdbool.h>

struct zmk_scroll_invert_config {
    bool invert_v; /* invert INPUT_REL_WHEEL  (vertical scroll) */
    bool invert_h; /* invert INPUT_REL_HWHEEL (horizontal scroll) */
};

int zmk_scroll_invert_get(struct zmk_scroll_invert_config *out);
int zmk_scroll_invert_set(bool invert_v, bool invert_h);
int zmk_scroll_invert_save(void);
