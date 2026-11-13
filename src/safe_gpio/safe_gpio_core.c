/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "safe_gpio_core.h"

#include <stddef.h>

static bool pair_is_valid(safe_gpio_mode_t mode, int level_a, int level_b)
{
    if (level_a != 0 && level_a != 1) {
        return false;
    }
    if (level_b != 0 && level_b != 1) {
        return false;
    }

    return (mode == SAFE_GPIO_MODE_EQUAL) ? (level_a == level_b) : (level_a != level_b);
}

static safe_gpio_state_t decode_state(safe_gpio_mode_t mode, int level_a, int level_b)
{
    /* Equal mode: both pins low is the actuated (closed) position.
     * Complementary mode: pin A active-low, pin B active-high. */
    if (mode == SAFE_GPIO_MODE_EQUAL) {
        return (level_a == 0) ? SAFE_GPIO_STATE_CLOSED : SAFE_GPIO_STATE_OPEN;
    }

    return (level_a == 0 && level_b == 1) ? SAFE_GPIO_STATE_CLOSED : SAFE_GPIO_STATE_OPEN;
}

static safe_gpio_fault_t latch(safe_gpio_filter_t *filter, safe_gpio_fault_t fault,
                               safe_gpio_state_t *out_state)
{
    filter->latched = true;
    filter->fault = fault;
    filter->skew_active = false;

    if (out_state != NULL) {
        *out_state = SAFE_GPIO_STATE_FAULT;
    }

    return fault;
}

void safe_gpio_filter_init(safe_gpio_filter_t *filter, safe_gpio_mode_t mode,
                           uint32_t discrepancy_timeout_ms)
{
    if (filter == NULL) {
        return;
    }

    filter->mode = mode;
    filter->discrepancy_timeout_ms = discrepancy_timeout_ms;
    filter->latched = false;
    filter->fault = SAFE_GPIO_FAULT_NONE;
    filter->skew_active = false;
    filter->skew_start_us = 0;
}

void safe_gpio_filter_latch(safe_gpio_filter_t *filter, safe_gpio_fault_t fault,
                            safe_gpio_state_t *out_state)
{
    if (filter == NULL) {
        return;
    }

    if (!filter->latched) {
        filter->latched = true;
        filter->fault = fault;
    }
    filter->skew_active = false;

    if (out_state != NULL) {
        *out_state = SAFE_GPIO_STATE_FAULT;
    }
}

safe_gpio_fault_t safe_gpio_filter_apply(safe_gpio_filter_t *filter, int level_a, int level_b,
                                         int64_t now_us, safe_gpio_state_t *out_state)
{
    if (filter == NULL) {
        return SAFE_GPIO_FAULT_INVALID_CONFIG;
    }

    if (filter->latched) {
        if (out_state != NULL) {
            *out_state = SAFE_GPIO_STATE_FAULT;
        }
        return filter->fault;
    }

    /* A level outside {0,1} can only come from a broken read path; there is no
     * skew window for it because it is not a mechanical transition. */
    if ((level_a != 0 && level_a != 1) || (level_b != 0 && level_b != 1)) {
        return latch(filter, SAFE_GPIO_FAULT_INVALID_LEVEL, out_state);
    }

    if (pair_is_valid(filter->mode, level_a, level_b)) {
        filter->skew_active = false;
        filter->skew_start_us = 0;
        if (out_state != NULL) {
            *out_state = decode_state(filter->mode, level_a, level_b);
        }
        return SAFE_GPIO_FAULT_NONE;
    }

    if (!filter->skew_active) {
        filter->skew_active = true;
        filter->skew_start_us = now_us;
        /* Contact is in flight: hold the previous state for now. */
        return SAFE_GPIO_FAULT_NONE;
    }

    int64_t elapsed_us = now_us - filter->skew_start_us;
    if (elapsed_us >= (int64_t)filter->discrepancy_timeout_ms * 1000) {
        return latch(filter, SAFE_GPIO_FAULT_DISCREPANCY, out_state);
    }

    return SAFE_GPIO_FAULT_NONE;
}
