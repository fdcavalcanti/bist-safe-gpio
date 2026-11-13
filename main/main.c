/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief ESP-BIST powered IEC 60730 Class C safe GPIO example (ESP32-P4)
 *
 * Wiring, all biases external to the SoC:
 *
 * | Signal          | GPIO | Bias                                        |
 * |-----------------|------|---------------------------------------------|
 * | Pin A           | 2    | 10 kOhm pull-up to 3V3, contact to GND      |
 * | Pin B           | 3    | 10 kOhm pull-down to GND, complementary     |
 * | Actuator enable | 4    | Output, high only while the contact is closed |
 * | Fault indicator | 5    | Output, high while a fault is latched       |
 *
 * Flow: post-boot reciprocal self-test of both input pads, then a 5 ms
 * supervision loop that samples through ESP-BIST, filters skew, drives the
 * actuator, and feeds the watchdog. Any fault drops the actuator and latches.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bist_esp.h"
#include "bist_conf.h"
#include "bist_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "gpio.h"
#include "hal/gpio_ll.h"
#include "rom/ets_sys.h"
#include "safe_gpio.h"
#include "wdt.h"

/* Pins match the article's breadboard harness. Override with -D on the
 * compiler command line if your board wires them differently. */
#ifndef SAFE_GPIO_PIN_A
#define SAFE_GPIO_PIN_A 2
#endif
#ifndef SAFE_GPIO_PIN_B
#define SAFE_GPIO_PIN_B 3
#endif
#ifndef SAFE_GPIO_ACTUATOR_PIN
#define SAFE_GPIO_ACTUATOR_PIN 4
#endif
#ifndef SAFE_GPIO_FAULT_LED_PIN
#define SAFE_GPIO_FAULT_LED_PIN 5
#endif

/* One full sample of both pins takes a few microseconds of dispatch. At 5 ms
 * the worst-case discrepancy reaction is the 20 ms skew window plus one
 * period, inside the article's 25 ms budget. */
#define SUPERVISION_PERIOD_US 5000

/* Mechanical contacts need a window that covers bounce and contact transit
 * skew; 20 ms is the value validated in the article's bench matrix. */
#define DISCREPANCY_TIMEOUT_MS 20

static const char *TAG = "safe_gpio_example";

static volatile bool s_wdt_expired;

/* Register-level writes so this is safe from the watchdog interrupt, where the
 * driver functions must not be called. */
static void IRAM_ATTR drive_fail_safe(void)
{
    gpio_dev_t *hw = GPIO_LL_GET_HW(0);
    gpio_ll_set_level(hw, SAFE_GPIO_ACTUATOR_PIN, 0);
    gpio_ll_set_level(hw, SAFE_GPIO_FAULT_LED_PIN, 1);
}

/*
 * Watchdog reaction. Runs in interrupt context, so it only touches registers
 * and a volatile flag.
 */
static void IRAM_ATTR on_wdt_timeout(void *arg)
{
    (void)arg;
    drive_fail_safe();
    s_wdt_expired = true;
}

static void fail_safe_exit(const char *reason)
{
    drive_fail_safe();
    ESP_LOGE(TAG, "Fail-safe stop: %s", reason);
    while (1) {
        __asm__ __volatile__("wfi");
    }
}

static void on_safe_gpio_fault(safe_gpio_fault_t fault)
{
    (void)fault;
    drive_fail_safe();
}

static esp_err_t configure_outputs(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << SAFE_GPIO_ACTUATOR_PIN) | (1ULL << SAFE_GPIO_FAULT_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    esp_err_t err = gpio_config(&out_conf);
    if (err != ESP_OK) {
        return err;
    }

    /* De-energised and unlit before anything else can be trusted. */
    gpio_set_level(SAFE_GPIO_ACTUATOR_PIN, 0);
    gpio_set_level(SAFE_GPIO_FAULT_LED_PIN, 0);
    return ESP_OK;
}

/*
 * Post-boot check: both pads must agree across the two HP cores before the
 * application starts acting on them. A pin that is stuck, floating, or not
 * readable fails here and the system never energises the actuator.
 */
static void post_boot_self_test(void)
{
    bist_esp_err_t ret = bist_gpio_input_reciprocal_test(SAFE_GPIO_PIN_A);
    if (ret != BIST_ESP_OK) {
        fail_safe_exit("post-boot reciprocal read of pin A failed");
    }

    ret = bist_gpio_input_reciprocal_test(SAFE_GPIO_PIN_B);
    if (ret != BIST_ESP_OK) {
        fail_safe_exit("post-boot reciprocal read of pin B failed");
    }

    ESP_LOGI(TAG, "Post-boot reciprocal self-test passed on GPIO %d and GPIO %d",
             SAFE_GPIO_PIN_A, SAFE_GPIO_PIN_B);
}

static void wait_until(int64_t deadline_us)
{
    int64_t remaining = deadline_us - esp_timer_get_time();
    while (remaining > 0) {
        ets_delay_us((uint32_t)remaining);
        remaining = deadline_us - esp_timer_get_time();
    }
}

int main(void)
{
    ESP_LOGI(TAG, "IEC 60730 Class C safe GPIO example (ESP-BIST, ESP32-P4)");

    esp_err_t err = configure_outputs();
    if (err != ESP_OK) {
        fail_safe_exit("output configuration failed");
    }

    safe_gpio_config_t safety_conf = {
        .pin_a = SAFE_GPIO_PIN_A,
        .pin_b = SAFE_GPIO_PIN_B,
        .mode = SAFE_GPIO_MODE_COMPLEMENTARY,
        .discrepancy_timeout_ms = DISCREPANCY_TIMEOUT_MS,
        .fault_callback = on_safe_gpio_fault,
    };
    err = safe_gpio_init(&safety_conf);
    if (err != ESP_OK) {
        fail_safe_exit("safe GPIO initialisation failed");
    }

    post_boot_self_test();

    if (wdt_init(CONFIG_ESP_BIST_WDT_TIMEOUT_US) != 0) {
        fail_safe_exit("watchdog initialisation failed");
    }
    wdt_register_callback(on_wdt_timeout, NULL);

    safe_gpio_state_t state = SAFE_GPIO_STATE_OPEN;
    bool last_closed = false;
    int64_t next_sample_us = esp_timer_get_time();

    while (1) {
        next_sample_us += SUPERVISION_PERIOD_US;

        if (safe_gpio_sample(&state) != ESP_OK) {
            /* The fault is latched and the callback already dropped the
             * actuator. Hold the lockout, keep the indicator lit, and stay in
             * a fed loop so the bench operator can observe the fault. */
            break;
        }

        bool closed = (state == SAFE_GPIO_STATE_CLOSED);
        if (closed != last_closed) {
            last_closed = closed;
            gpio_set_level(SAFE_GPIO_ACTUATOR_PIN, closed ? 1 : 0);
            ESP_LOGI(TAG, "Contact %s, actuator %s", closed ? "closed" : "open",
                     closed ? "energised" : "de-energised");
        }

        wdt_feed();

        if (s_wdt_expired) {
            fail_safe_exit("watchdog callback fired");
        }

        wait_until(next_sample_us);
    }

    ESP_LOGE(TAG, "Lockout: actuator de-energised, reset the board to clear");
    while (1) {
        wdt_feed();
        ets_delay_us(1000);
    }
}
