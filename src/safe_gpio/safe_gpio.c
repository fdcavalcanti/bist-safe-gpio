/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file safe_gpio.c
 * @brief Target implementation of the ESP-BIST backed safe digital input
 *
 * Every level that reaches the skew filter has already passed
 * bist_gpio_input_reciprocal_test(): HP core 0 and HP core 1 each sampled the
 * same pad into their own evidence slot and both cores accepted the pair. The
 * accepted level is then taken from Core 0's slot, not from a second
 * single-core gpio_get_level() read, so the value used by the application is
 * the value that was cross-validated.
 */

#include "safe_gpio.h"

#include <inttypes.h>
#include <stddef.h>

#include "bist_esp.h"
#include "bist_log.h"
#include "esp_timer.h"
#include "safe_gpio_core.h"

static const char *TAG = "safe_gpio";

static safe_gpio_config_t s_config;
static safe_gpio_filter_t s_filter;
static bool s_initialized;
static bool s_fault_reported;

static void latch(safe_gpio_fault_t fault, safe_gpio_state_t *out_state)
{
    /* The filter owns the latch state; this only handles the application-facing
     * side effects. */
    safe_gpio_filter_latch(&s_filter, fault, out_state);

    ESP_LOGE(TAG, "Safe state latched: %s", safe_gpio_fault_name(fault));

    /* The application gets exactly one notification per latch, even if it keeps
     * calling safe_gpio_sample(). */
    if (!s_fault_reported && s_config.fault_callback != NULL) {
        s_fault_reported = true;
        s_config.fault_callback(fault);
    }
}

/*
 * One cross-validated level for @p pin. bist_gpio_input_reciprocal_test() only
 * reports pass or fail, so the agreed level is read back from Core 0's evidence
 * slot, which the reciprocal flow leaves untouched after the cross-checks.
 */
static esp_err_t sample_pin(gpio_num_t pin, int *out_level)
{
    bist_esp_err_t ret = bist_gpio_input_reciprocal_test(pin);
    if (ret != BIST_ESP_OK) {
        ESP_LOGE(TAG, "Reciprocal read of GPIO %d failed (%d)", (int)pin, (int)ret);
        return ESP_FAIL;
    }

    uint32_t level = bist_gpio_input_shared_results[0].level;
    if (level > 1U) {
        ESP_LOGE(TAG, "Reciprocal read of GPIO %d returned level %u", (int)pin, (unsigned)level);
        return ESP_FAIL;
    }

    *out_level = (int)level;
    return ESP_OK;
}

const char *safe_gpio_fault_name(safe_gpio_fault_t fault)
{
    switch (fault) {
    case SAFE_GPIO_FAULT_NONE:
        return "none";
    case SAFE_GPIO_FAULT_BIST_CORE_MISMATCH:
        return "reciprocal read mismatch";
    case SAFE_GPIO_FAULT_DISCREPANCY:
        return "pin discrepancy";
    case SAFE_GPIO_FAULT_INVALID_CONFIG:
        return "invalid configuration";
    case SAFE_GPIO_FAULT_INVALID_LEVEL:
        return "invalid sampled level";
    default:
        return "unknown";
    }
}

safe_gpio_fault_t safe_gpio_get_fault(void)
{
    return s_filter.fault;
}

esp_err_t safe_gpio_init(const safe_gpio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->pin_a == config->pin_b) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_GPIO(config->pin_a) || !GPIO_IS_VALID_GPIO(config->pin_b)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->mode != SAFE_GPIO_MODE_COMPLEMENTARY && config->mode != SAFE_GPIO_MODE_EQUAL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->discrepancy_timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_config = *config;

    /* Pin A is always active-low. Pin B follows the mode: active-high in
     * complementary wiring, active-low in equal wiring. Internal pull-ups and
     * pull-downs provide the default bias so post-boot bist_gpio_input_test()
     * can run without external pull-down contention; external pull resistors
     * can also be added when stronger noise immunity is required. */
    gpio_config_t io_conf_a = {
        .pin_bit_mask = BIT64(config->pin_a),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config_t io_conf_b = {
        .pin_bit_mask = BIT64(config->pin_b),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (config->mode == SAFE_GPIO_MODE_EQUAL) ? GPIO_PULLUP_ENABLE
                                                             : GPIO_PULLUP_DISABLE,
        .pull_down_en = (config->mode == SAFE_GPIO_MODE_EQUAL) ? GPIO_PULLDOWN_DISABLE
                                                               : GPIO_PULLDOWN_ENABLE,
    };

    esp_err_t err = gpio_config(&io_conf_a);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pin A GPIO %d rejected (%d)", (int)config->pin_a, (int)err);
        return err;
    }
    err = gpio_config(&io_conf_b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pin B GPIO %d rejected (%d)", (int)config->pin_b, (int)err);
        return err;
    }

    /* The skew filter timestamps samples with esp_timer_get_time(). Init is
     * idempotent, so a caller that already initialised the timer gets
     * ESP_ERR_INVALID_STATE rather than a failure. */
    err = esp_timer_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_timer_init failed (%d)", (int)err);
        return err;
    }

    safe_gpio_filter_init(&s_filter, config->mode, config->discrepancy_timeout_ms);
    s_fault_reported = false;
    s_initialized = true;

    ESP_LOGI(TAG, "Supervising GPIO %d and GPIO %d (%s), skew window %" PRIu32 " ms",
             (int)config->pin_a, (int)config->pin_b,
             (config->mode == SAFE_GPIO_MODE_EQUAL) ? "equal" : "complementary",
             config->discrepancy_timeout_ms);

    return ESP_OK;
}

esp_err_t safe_gpio_sample(safe_gpio_state_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_filter.latched) {
        *out_state = SAFE_GPIO_STATE_FAULT;
        return ESP_FAIL;
    }

    int level_a = 0;
    int level_b = 0;

    if (sample_pin(s_config.pin_a, &level_a) != ESP_OK ||
            sample_pin(s_config.pin_b, &level_b) != ESP_OK) {
        latch(SAFE_GPIO_FAULT_BIST_CORE_MISMATCH, out_state);
        return ESP_FAIL;
    }

    /* The filter latches by itself; this call only mirrors the latch into the
     * application callback and the log. */
    safe_gpio_fault_t fault = safe_gpio_filter_apply(&s_filter, level_a, level_b,
                                                     esp_timer_get_time(), out_state);
    if (fault != SAFE_GPIO_FAULT_NONE) {
        latch(fault, out_state);
        return ESP_FAIL;
    }

    return ESP_OK;
}
