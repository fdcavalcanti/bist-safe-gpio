/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file safe_gpio.h
 * @brief IEC 60730 Class C safe digital input built on ESP-BIST
 *
 * Supervises one mechanical contact wired to two GPIO pins:
 *
 * - Each level is read by a dual-core reciprocal ESP-BIST test, so both HP
 *   cores sample the same pad and compare their evidence before the level is
 *   used. That catches pad, GPIO-matrix, and core sampling faults.
 * - The two resulting levels are checked against the configured relationship
 *   inside a bounded skew window, so wire breaks, shorts, and inter-pin bridges
 *   trip a latched fault that the application turns into a safe state.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "gpio.h"
#include "safe_gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from safe_gpio_sample() when a fault is latched. */
typedef void (*safe_gpio_fault_cb_t)(safe_gpio_fault_t fault);

typedef struct {
    gpio_num_t pin_a;                    /**< Active-low input, pulled up */
    gpio_num_t pin_b;                    /**< Complementary input, pulled down */
    safe_gpio_mode_t mode;               /**< Expected pin relationship */
    uint32_t discrepancy_timeout_ms;     /**< Max skew before a fault latches */
    safe_gpio_fault_cb_t fault_callback; /**< Invoked once, on fault latch */
} safe_gpio_config_t;

/**
 * @brief Configure both input pads and reset the supervision state
 *
 * Enables the internal pull resistors that match @p mode and leaves the pins as
 * inputs. The pads are not touched by later samples: the reciprocal BIST test
 * only reads them.
 *
 * @param config Pin, mode, and timing configuration; copied by value
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG on a NULL config, equal pins, an unusable GPIO
 *         number, an unknown mode, or a zero timeout
 * @return ESP_ERR_INVALID_STATE if called twice without a reset
 */
esp_err_t safe_gpio_init(const safe_gpio_config_t *config);

/**
 * @brief Take one supervised sample of the contact
 *
 * Reads pin A and pin B through the reciprocal BIST test, applies the skew
 * filter, and reports the resulting state. Sampling is non-blocking with
 * respect to hardware: it never drives a pin.
 *
 * Once a fault latches, every later call returns ESP_FAIL with
 * SAFE_GPIO_STATE_FAULT and the callback is not invoked again. Recovery
 * requires a reset; safe_gpio_init() rejects a second call so a latched fault
 * cannot be cleared by re-initialisation.
 *
 * @param out_state Receives the decoded state; set to FAULT when a fault latches
 *
 * @return ESP_OK when the sample was accepted, including while the contact is
 *         inside the skew window (the previous state is retained)
 * @return ESP_FAIL when a fault was latched, reported, or is already latched
 * @return ESP_ERR_INVALID_ARG on a NULL out_state
 * @return ESP_ERR_INVALID_STATE before safe_gpio_init()
 */
esp_err_t safe_gpio_sample(safe_gpio_state_t *out_state);

/**
 * @brief Report the fault latched by the most recent sample
 *
 * @return SAFE_GPIO_FAULT_NONE while no fault is latched
 */
safe_gpio_fault_t safe_gpio_get_fault(void);

/**
 * @brief Convert a fault code to its log-friendly name
 */
const char *safe_gpio_fault_name(safe_gpio_fault_t fault);

#ifdef __cplusplus
}
#endif
