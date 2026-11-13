/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for safe_gpio.c with the ESP-BIST and ESP-IDF interfaces stubbed.
 *
 * The stubs make the two facts the driver consumes controllable: whether the
 * reciprocal read passed, and which level Core 0 recorded. The tests cover the
 * driver's own decisions, where a level could be accepted without
 * cross-validation or a fault could fail to latch.
 *
 * The driver keeps its state in statics and refuses a second init after a
 * latch, so each scenario runs in its own process: the binary takes the
 * scenario name as argv[1] and CTest registers one test per scenario.
 */

#include "safe_gpio.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bist_esp.h"
#include "esp_timer.h"
#include "gpio.h"

static int g_failures;

#define EXPECT(cond)                                                                  \
    do {                                                                              \
        if (!(cond)) {                                                                \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                     \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

#define PIN_A 2
#define PIN_B 3
#define TIMEOUT_MS 20

/* ---- Stub control surface ---------------------------------------------- */

volatile bist_gpio_input_result_t bist_gpio_input_shared_results[2];

/* Per-pin scripted reciprocal result and level, indexed by GPIO number. */
static bist_esp_err_t s_reciprocal_result[8];
static uint32_t s_reciprocal_level[8];
static int s_reciprocal_calls[8];
static int64_t s_now_us;

static int g_fault_calls;
static safe_gpio_fault_t g_last_fault;

void test_log_sink(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    (void)gpio_num;
    (void)level;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    return (int)s_reciprocal_level[gpio_num];
}

bist_esp_err_t bist_gpio_input_reciprocal_test(gpio_num_t gpio_num)
{
    s_reciprocal_calls[gpio_num]++;
    if (s_reciprocal_result[gpio_num] != BIST_ESP_OK) {
        return s_reciprocal_result[gpio_num];
    }

    /* Mirror what the real test leaves behind: Core 0's slot holds the agreed
     * level, Core 1's slot holds its own copy. */
    bist_gpio_input_shared_results[0].status = (uint32_t)BIST_ESP_OK;
    bist_gpio_input_shared_results[0].level = s_reciprocal_level[gpio_num];
    bist_gpio_input_shared_results[1].status = (uint32_t)BIST_ESP_OK;
    bist_gpio_input_shared_results[1].level = s_reciprocal_level[gpio_num];
    return BIST_ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

esp_err_t esp_timer_init(void)
{
    return ESP_OK;
}

/* ---- Helpers ----------------------------------------------------------- */

static void on_fault(safe_gpio_fault_t fault)
{
    g_fault_calls++;
    g_last_fault = fault;
}

static void reset_stubs(void)
{
    for (int i = 0; i < 8; i++) {
        s_reciprocal_result[i] = BIST_ESP_OK;
        s_reciprocal_level[i] = 1;
        s_reciprocal_calls[i] = 0;
    }
    s_now_us = 1000;
    g_fault_calls = 0;
    g_last_fault = SAFE_GPIO_FAULT_NONE;
}

static void set_levels(uint32_t level_a, uint32_t level_b)
{
    s_reciprocal_level[PIN_A] = level_a;
    s_reciprocal_level[PIN_B] = level_b;
}

static void init_driver(void)
{
    safe_gpio_config_t config = {
        .pin_a = PIN_A,
        .pin_b = PIN_B,
        .mode = SAFE_GPIO_MODE_COMPLEMENTARY,
        .discrepancy_timeout_ms = TIMEOUT_MS,
        .fault_callback = on_fault,
    };
    EXPECT(safe_gpio_init(&config) == ESP_OK);
}

/* ---- Scenarios --------------------------------------------------------- */

static void scenario_init_rejects_bad_config(void)
{
    safe_gpio_config_t config = {
        .pin_a = PIN_A,
        .pin_b = PIN_A,
        .mode = SAFE_GPIO_MODE_COMPLEMENTARY,
        .discrepancy_timeout_ms = TIMEOUT_MS,
    };
    EXPECT(safe_gpio_init(&config) == ESP_ERR_INVALID_ARG);

    config.pin_b = PIN_B;
    config.discrepancy_timeout_ms = 0;
    EXPECT(safe_gpio_init(&config) == ESP_ERR_INVALID_ARG);

    config.discrepancy_timeout_ms = TIMEOUT_MS;
    config.mode = (safe_gpio_mode_t)7;
    EXPECT(safe_gpio_init(&config) == ESP_ERR_INVALID_ARG);

    EXPECT(safe_gpio_init(NULL) == ESP_ERR_INVALID_ARG);
}

static void scenario_requires_init(void)
{
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    EXPECT(safe_gpio_sample(&state) == ESP_ERR_INVALID_STATE);
    EXPECT(safe_gpio_sample(NULL) == ESP_ERR_INVALID_ARG);
}

/* Every sample must go through the reciprocal read once per pin. A regression
 * that reads the pad directly would show up as a missing call. */
static void scenario_uses_reciprocal_read_per_pin(void)
{
    init_driver();
    set_levels(1, 0);

    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    EXPECT(state == SAFE_GPIO_STATE_OPEN);
    EXPECT(s_reciprocal_calls[PIN_A] == 1);
    EXPECT(s_reciprocal_calls[PIN_B] == 1);

    set_levels(0, 1);
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    EXPECT(state == SAFE_GPIO_STATE_CLOSED);
    EXPECT(s_reciprocal_calls[PIN_A] == 2);
    EXPECT(s_reciprocal_calls[PIN_B] == 2);
    EXPECT(g_fault_calls == 0);
}

/* The driver must report a fault when either reciprocal read fails, and must
 * not fall through to comparing the levels it already holds. */
static void scenario_reciprocal_failure_latches(void)
{
    init_driver();
    set_levels(1, 0);
    s_reciprocal_result[PIN_B] = BIST_ESP_IO_TEST_ERR;

    safe_gpio_state_t state = SAFE_GPIO_STATE_OPEN;
    EXPECT(safe_gpio_sample(&state) == ESP_FAIL);
    EXPECT(state == SAFE_GPIO_STATE_FAULT);
    EXPECT(safe_gpio_get_fault() == SAFE_GPIO_FAULT_BIST_CORE_MISMATCH);
    EXPECT(g_fault_calls == 1);
    EXPECT(g_last_fault == SAFE_GPIO_FAULT_BIST_CORE_MISMATCH);
}

/* A level outside {0,1} in the evidence slot means the read path is corrupt
 * even when the reciprocal test reported success. */
static void scenario_out_of_range_level_latches(void)
{
    init_driver();
    s_reciprocal_level[PIN_A] = 2;

    safe_gpio_state_t state = SAFE_GPIO_STATE_OPEN;
    EXPECT(safe_gpio_sample(&state) == ESP_FAIL);
    EXPECT(safe_gpio_get_fault() == SAFE_GPIO_FAULT_BIST_CORE_MISMATCH);
}

/* Stuck pair: the fault latches once the window expires, the callback runs
 * exactly once, and a later valid pair does not clear the latch. */
static void scenario_discrepancy_latches_once(void)
{
    init_driver();

    set_levels(1, 0);
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    EXPECT(safe_gpio_sample(&state) == ESP_OK);

    /* Both pins low: invalid pair, window opens on this sample. */
    set_levels(0, 0);
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    EXPECT(state == SAFE_GPIO_STATE_OPEN);
    EXPECT(g_fault_calls == 0);

    s_now_us += (int64_t)TIMEOUT_MS * 1000;
    EXPECT(safe_gpio_sample(&state) == ESP_FAIL);
    EXPECT(state == SAFE_GPIO_STATE_FAULT);
    EXPECT(safe_gpio_get_fault() == SAFE_GPIO_FAULT_DISCREPANCY);
    EXPECT(g_fault_calls == 1);

    set_levels(0, 1);
    EXPECT(safe_gpio_sample(&state) == ESP_FAIL);
    EXPECT(state == SAFE_GPIO_STATE_FAULT);
    EXPECT(g_fault_calls == 1);
}

/* A latched fault must stop the reciprocal reads. Continuing to sample a pad
 * the cores cannot trust serves no purpose. */
static void scenario_latched_fault_stops_sampling(void)
{
    init_driver();

    set_levels(0, 0);
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    s_now_us += (int64_t)TIMEOUT_MS * 1000;
    EXPECT(safe_gpio_sample(&state) == ESP_FAIL);

    int calls_before = s_reciprocal_calls[PIN_A];
    EXPECT(safe_gpio_sample(&state) == ESP_FAIL);
    EXPECT(s_reciprocal_calls[PIN_A] == calls_before);
}

/* Contact bounce shorter than the window must not latch; the reported state is
 * the last accepted one. */
static void scenario_bounce_does_not_latch(void)
{
    init_driver();

    set_levels(1, 0);
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    EXPECT(state == SAFE_GPIO_STATE_OPEN);

    /* Contact in flight: both pins low for less than the window. */
    set_levels(0, 0);
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    s_now_us += (int64_t)TIMEOUT_MS * 500;
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    EXPECT(state == SAFE_GPIO_STATE_OPEN);

    set_levels(0, 1);
    EXPECT(safe_gpio_sample(&state) == ESP_OK);
    EXPECT(state == SAFE_GPIO_STATE_CLOSED);
    EXPECT(g_fault_calls == 0);
}

static void scenario_fault_names_are_present(void)
{
    const safe_gpio_fault_t codes[] = {
        SAFE_GPIO_FAULT_NONE,
        SAFE_GPIO_FAULT_BIST_CORE_MISMATCH,
        SAFE_GPIO_FAULT_DISCREPANCY,
        SAFE_GPIO_FAULT_INVALID_CONFIG,
        SAFE_GPIO_FAULT_INVALID_LEVEL,
    };

    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *name = safe_gpio_fault_name(codes[i]);
        EXPECT(name != NULL);
        EXPECT(name[0] != '\0');
    }
}

typedef struct {
    const char *name;
    void (*run)(void);
} scenario_t;

static const scenario_t SCENARIOS[] = {
    {"init_rejects_bad_config", scenario_init_rejects_bad_config},
    {"requires_init", scenario_requires_init},
    {"uses_reciprocal_read_per_pin", scenario_uses_reciprocal_read_per_pin},
    {"reciprocal_failure_latches", scenario_reciprocal_failure_latches},
    {"out_of_range_level_latches", scenario_out_of_range_level_latches},
    {"discrepancy_latches_once", scenario_discrepancy_latches_once},
    {"latched_fault_stops_sampling", scenario_latched_fault_stops_sampling},
    {"bounce_does_not_latch", scenario_bounce_does_not_latch},
    {"fault_names_are_present", scenario_fault_names_are_present},
};

int main(int argc, char **argv)
{
    reset_stubs();

    if (argc != 2) {
        printf("usage: %s <scenario>\navailable:", argv[0]);
        for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); i++) {
            printf(" %s", SCENARIOS[i].name);
        }
        printf("\n");
        return 2;
    }

    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); i++) {
        if (strcmp(argv[1], SCENARIOS[i].name) != 0) {
            continue;
        }

        SCENARIOS[i].run();

        if (g_failures != 0) {
            printf("%s: %d check(s) failed\n", argv[1], g_failures);
            return 1;
        }

        printf("%s: passed\n", argv[1]);
        return 0;
    }

    printf("unknown scenario: %s\n", argv[1]);
    return 2;
}
