/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file safe_gpio_core.h
 * @brief Discrepancy filter for the dual-pin digital input
 *
 * Pure decision logic: it takes the two already-validated levels and a
 * monotonic timestamp, and produces the contact state. It performs no
 * hardware access and no logging, so the same code runs on the target and in
 * host unit tests.
 */

#pragma once

#include "safe_gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Skew filter state. Owned by safe_gpio.c, never by the caller. */
typedef struct {
    safe_gpio_mode_t mode;
    uint32_t discrepancy_timeout_ms;
    bool latched;         /**< A fault has been raised; every later call fails */
    safe_gpio_fault_t fault; /**< Cause latched by the last failing call */
    bool skew_active;     /**< Currently inside an invalid-pair window */
    int64_t skew_start_us; /**< Timestamp of the first invalid sample in this window */
} safe_gpio_filter_t;

/**
 * @brief Reset the filter to its unlatched state
 *
 * @param filter Filter to reset; must not be NULL
 * @param mode Expected relationship between the pins
 * @param discrepancy_timeout_ms Maximum time an invalid pair may persist
 */
void safe_gpio_filter_init(safe_gpio_filter_t *filter, safe_gpio_mode_t mode,
                           uint32_t discrepancy_timeout_ms);

/**
 * @brief Latch a fault and force the reported state to FAULT
 *
 * Used by the driver for faults the filter cannot see, such as a failed
 * reciprocal read. Idempotent: a second call keeps the first fault cause.
 *
 * @param filter Filter state; must not be NULL
 * @param fault Fault cause to latch
 * @param out_state Receives SAFE_GPIO_STATE_FAULT; may be NULL
 */
void safe_gpio_filter_latch(safe_gpio_filter_t *filter, safe_gpio_fault_t fault,
                            safe_gpio_state_t *out_state);

/**
 * @brief Apply one validated sample to the filter
 *
 * A level pair that violates the mode opens a skew window. While the window is
 * open the reported state is left untouched, so callers keep the last accepted
 * state and short mechanical skew does not trip the fault. If the pair is still
 * invalid @p discrepancy_timeout_ms after the window opened, the filter latches
 * SAFE_GPIO_FAULT_DISCREPANCY and reports SAFE_GPIO_STATE_FAULT.
 *
 * @param filter Filter state; must not be NULL
 * @param level_a Level of pin A, 0 or 1
 * @param level_b Level of pin B, 0 or 1
 * @param now_us Monotonic timestamp in microseconds
 * @param out_state Receives the decoded state; may be NULL on a fault-only call
 *
 * @return SAFE_GPIO_FAULT_NONE when the sample was accepted, otherwise the
 *         fault that was latched
 */
safe_gpio_fault_t safe_gpio_filter_apply(safe_gpio_filter_t *filter, int level_a, int level_b,
                                         int64_t now_us, safe_gpio_state_t *out_state);

#ifdef __cplusplus
}
#endif
