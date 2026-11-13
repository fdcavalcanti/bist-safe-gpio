/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-native tests for the safe GPIO discrepancy filter.
 *
 * The filter is the part of the driver that decides when a stuck, bridged, or
 * shorted pin pair becomes a latched fault. These tests pin down that decision:
 * a transition inside the skew window must not trip, a transition that outlasts
 * it must trip, and the latch must survive later valid samples.
 */

#include "safe_gpio_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int g_failures;

#define EXPECT(cond)                                                                  \
    do {                                                                              \
        if (!(cond)) {                                                                \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                     \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

#define TIMEOUT_MS 20
#define TIMEOUT_US ((int64_t)TIMEOUT_MS * 1000)

/* Feeds one sample through a caller-owned state variable, mirroring how the
 * driver holds the last accepted state across calls while a skew window is
 * open. The fault code is dropped; tests that care use safe_gpio_filter_apply()
 * directly. */
static safe_gpio_state_t sample(safe_gpio_filter_t *filter, safe_gpio_state_t *state, int level_a,
                                int level_b, int64_t now_us)
{
    (void)safe_gpio_filter_apply(filter, level_a, level_b, now_us, state);
    return *state;
}

static void test_complementary_decodes_closed(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);

    EXPECT(sample(&filter, &state, 0, 1, 0) == SAFE_GPIO_STATE_CLOSED);
    EXPECT(sample(&filter, &state, 1, 0, 1000) == SAFE_GPIO_STATE_OPEN);
    EXPECT(filter.latched == false);
}

static void test_equal_decodes_closed(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_EQUAL, TIMEOUT_MS);

    EXPECT(sample(&filter, &state, 0, 0, 0) == SAFE_GPIO_STATE_CLOSED);
    EXPECT(sample(&filter, &state, 1, 1, 1000) == SAFE_GPIO_STATE_OPEN);
    EXPECT(filter.latched == false);
}

/*
 * Contact bounce: the pair is invalid for less than the window, then settles.
 * The reported state must stay on the last accepted value and no fault may
 * latch, otherwise every switch press would trip the actuator.
 */
static void test_skew_inside_window_holds_state(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);

    EXPECT(sample(&filter, &state, 1, 0, 0) == SAFE_GPIO_STATE_OPEN);

    /* Both pins low while the contact travels. */
    EXPECT(sample(&filter, &state, 0, 0, 1000) == SAFE_GPIO_STATE_OPEN);
    EXPECT(sample(&filter, &state, 0, 0, 1000 + TIMEOUT_US / 2) == SAFE_GPIO_STATE_OPEN);
    EXPECT(filter.latched == false);

    EXPECT(sample(&filter, &state, 0, 1, 1000 + TIMEOUT_US / 2 + 1) == SAFE_GPIO_STATE_CLOSED);
    EXPECT(filter.latched == false);
}

static void test_skew_past_window_latches_discrepancy(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);

    EXPECT(sample(&filter, &state, 1, 0, 0) == SAFE_GPIO_STATE_OPEN);

    /* Pin A shorted to GND: the pair stays invalid. */
    EXPECT(sample(&filter, &state, 0, 0, 10000) == SAFE_GPIO_STATE_OPEN);
    EXPECT(sample(&filter, &state, 0, 0, 10000 + TIMEOUT_US - 1) == SAFE_GPIO_STATE_OPEN);
    EXPECT(filter.latched == false);

    EXPECT(sample(&filter, &state, 0, 0, 10000 + TIMEOUT_US) == SAFE_GPIO_STATE_FAULT);
    EXPECT(filter.latched == true);
    EXPECT(filter.fault == SAFE_GPIO_FAULT_DISCREPANCY);
}

/* A skew window that opened, recovered, then reopened must be timed from the
 * new opening. A stale start timestamp would trip on the second window. */
static void test_recovered_window_restarts_timer(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);

    EXPECT(sample(&filter, &state, 1, 0, 0) == SAFE_GPIO_STATE_OPEN);

    /* First window opens at t=1000 us and recovers shortly after. */
    EXPECT(sample(&filter, &state, 1, 1, 1000) == SAFE_GPIO_STATE_OPEN);
    EXPECT(sample(&filter, &state, 1, 0, 2000) == SAFE_GPIO_STATE_OPEN);
    EXPECT(filter.latched == false);

    /* Second window opens well past the first opening plus the timeout. */
    EXPECT(sample(&filter, &state, 1, 1, 1000 + TIMEOUT_US * 3) == SAFE_GPIO_STATE_OPEN);
    EXPECT(filter.latched == false);

    EXPECT(sample(&filter, &state, 1, 1, 1000 + TIMEOUT_US * 4) == SAFE_GPIO_STATE_FAULT);
    EXPECT(filter.fault == SAFE_GPIO_FAULT_DISCREPANCY);
}

/* The latch is the safety contract: once tripped, a valid reading must not
 * bring the contact back into service. */
static void test_latch_is_sticky(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);

    EXPECT(sample(&filter, &state, 1, 0, 0) == SAFE_GPIO_STATE_OPEN);
    EXPECT(sample(&filter, &state, 0, 0, 100) == SAFE_GPIO_STATE_OPEN);
    EXPECT(sample(&filter, &state, 0, 0, 100 + TIMEOUT_US) == SAFE_GPIO_STATE_FAULT);

    EXPECT(sample(&filter, &state, 0, 1, 100 + TIMEOUT_US * 2) == SAFE_GPIO_STATE_FAULT);
    EXPECT(safe_gpio_filter_apply(&filter, 0, 1, 100 + TIMEOUT_US * 3, NULL) ==
           SAFE_GPIO_FAULT_DISCREPANCY);
}

/* A level outside {0,1} means the read path itself is broken. There is no
 * mechanical explanation for it, so it trips immediately. */
static void test_invalid_level_latches_without_window(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);

    EXPECT(safe_gpio_filter_apply(&filter, 2, 0, 0, NULL) == SAFE_GPIO_FAULT_INVALID_LEVEL);
    EXPECT(filter.latched == true);
    EXPECT(filter.fault == SAFE_GPIO_FAULT_INVALID_LEVEL);
}

static void test_reinit_clears_latch(void)
{
    safe_gpio_filter_t filter;
    safe_gpio_state_t state = SAFE_GPIO_STATE_FAULT;
    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);

    EXPECT(sample(&filter, &state, 1, 0, 0) == SAFE_GPIO_STATE_OPEN);
    EXPECT(sample(&filter, &state, 0, 0, 100) == SAFE_GPIO_STATE_OPEN);
    EXPECT(sample(&filter, &state, 0, 0, 100 + TIMEOUT_US) == SAFE_GPIO_STATE_FAULT);

    safe_gpio_filter_init(&filter, SAFE_GPIO_MODE_COMPLEMENTARY, TIMEOUT_MS);
    EXPECT(filter.latched == false);
    EXPECT(filter.fault == SAFE_GPIO_FAULT_NONE);
    EXPECT(sample(&filter, &state, 1, 0, 0) == SAFE_GPIO_STATE_OPEN);
}

int main(void)
{
    test_complementary_decodes_closed();
    test_equal_decodes_closed();
    test_skew_inside_window_holds_state();
    test_skew_past_window_latches_discrepancy();
    test_recovered_window_restarts_timer();
    test_latch_is_sticky();
    test_invalid_level_latches_without_window();
    test_reinit_clears_latch();

    if (g_failures != 0) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }

    printf("safe_gpio_core: all checks passed\n");
    return 0;
}
