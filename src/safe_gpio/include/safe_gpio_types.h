/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file safe_gpio_types.h
 * @brief Framework-free types shared by the safe GPIO driver and its core
 *
 * Kept free of ESP-IDF headers so the discrepancy state machine in
 * safe_gpio_core.c builds and runs on a host compiler for unit testing.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Validated state of the supervised contact. */
typedef enum {
    SAFE_GPIO_STATE_OPEN = 0,
    SAFE_GPIO_STATE_CLOSED = 1,
    SAFE_GPIO_STATE_FAULT = 2,
} safe_gpio_state_t;

/** Expected relationship between the two input pins. */
typedef enum {
    SAFE_GPIO_MODE_COMPLEMENTARY = 0, /**< Valid pairs are (1,0) and (0,1) */
    SAFE_GPIO_MODE_EQUAL = 1,         /**< Valid pairs are (0,0) and (1,1) */
} safe_gpio_mode_t;

/** Latched fault cause reported to the application. */
typedef enum {
    SAFE_GPIO_FAULT_NONE = 0,
    SAFE_GPIO_FAULT_BIST_CORE_MISMATCH, /**< Reciprocal read failed or the cores disagreed */
    SAFE_GPIO_FAULT_DISCREPANCY,        /**< Pin pair stayed invalid past the skew window */
    SAFE_GPIO_FAULT_INVALID_CONFIG,     /**< Rejected pin, mode, or timeout at init */
    SAFE_GPIO_FAULT_INVALID_LEVEL,      /**< A sampled level was neither 0 nor 1 */
} safe_gpio_fault_t;

#ifdef __cplusplus
}
#endif
