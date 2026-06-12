/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Forward a CPI/DPI change to a trackball sensor that lives on a split
 * peripheral (e.g. trackball_L on the Pyuron left half).
 *
 * Called by the pmw3610-driver's cpi_handler when the Studio RPC requests a
 * change on a sensor whose device pointer is NULL (i.e. remote, not local).
 */

#pragma once

#include <stdint.h>

/**
 * Send a CPI change to one sensor on a split peripheral over BLE.
 *
 * @param peripheral_idx  ZMK split peripheral index (0-based).
 * @param sensor_id       Sensor index on the *peripheral* side (0-based).
 * @param cpi             New CPI value (e.g. 200–3200 in steps of 200).
 *
 * @return 0 on success.
 *         -ENOTCONN if the peripheral is not connected.
 *         -EAGAIN   if the GATT characteristic handle is not yet discovered.
 *         Other negative errno on BLE write failure.
 */
int zmk_split_bt_cpi_forward(uint8_t peripheral_idx, uint8_t sensor_id, uint16_t cpi);
