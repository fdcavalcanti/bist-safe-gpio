/* Host test stand-in for the ESP-BIST GPIO driver interface.
 *
 * Only the declarations safe_gpio.c depends on are present. The test controls
 * the reciprocal read result and the level Core 0 records, which are the two
 * facts the driver consumes. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;

typedef enum {
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE = 1,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE = 1,
} gpio_pulldown_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
} gpio_config_t;

#define BIT64(n) (1ULL << (n))
#define GPIO_IS_VALID_GPIO(num) ((num) >= 0 && (num) < 55)
#define GPIO_IS_VALID_OUTPUT_GPIO(num) GPIO_IS_VALID_GPIO(num)

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int gpio_get_level(gpio_num_t gpio_num);
