/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Public API for runtime AML (AutoMouse Layer) config.
 * Implemented in input_processor_temp_layer.c when
 * CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC is enabled.
 */
#pragma once

#include <stdint.h>

#define ZMK_AML_MAX_EXCLUDED 16

struct zmk_aml_config {
    uint32_t deactivation_ms;
    uint32_t prior_idle_ms;
    uint32_t extend_ms;       /* re-arm amount when an excluded key is pressed in AML */
    uint16_t excluded_positions[ZMK_AML_MAX_EXCLUDED];
    uint8_t  num_excluded;
};

int zmk_aml_get(struct zmk_aml_config *out);
int zmk_aml_set_deactivation(uint32_t ms);
int zmk_aml_set_prior_idle(uint32_t ms);
int zmk_aml_set_extend(uint32_t ms);
int zmk_aml_toggle_excluded(uint32_t position);
int zmk_aml_reset(void);
int zmk_aml_save(void);
