/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Public API for the runtime trackball speed multiplier.
 * Implemented in input_processor_pyuron_speed.c when
 * CONFIG_PYURON_SPEED_STUDIO_RPC is enabled.
 *
 * The multiplier is applied AFTER gesture detection (zip_keybind_dynamic) in
 * the listener pipeline, so it only scales cursor movement — gestures and AML
 * see the raw deltas. percent=100 is identity (no change).
 */
#pragma once

#include <stdint.h>

#include <stdbool.h>

struct zmk_speed_config {
    uint32_t percent;        /* cursor speed multiplier in percent (100 = x1.0) */
    bool     accel_on;       /* mouse acceleration on/off */
    uint32_t accel_strength; /* acceleration strength 0..100 (only when accel_on) */
};

int zmk_speed_get(struct zmk_speed_config *out);
int zmk_speed_set(uint32_t percent);              /* unchanged: set percent only */
int zmk_speed_set_accel(bool on, uint32_t strength); /* set acceleration on/off + strength */
int zmk_speed_save(void);                          /* persist percent + accel_on + accel_strength */
