/* Host test stand-in for the ESP-BIST aggregate and logging headers. */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "gpio.h"

/* Logging goes through a printf-like sink so the format arguments are still
 * evaluated by the compiler and checked for type mismatches. */
void test_log_sink(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define ESP_LOGE(tag, fmt, ...) ((void)(tag), test_log_sink(fmt, ##__VA_ARGS__))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag), test_log_sink(fmt, ##__VA_ARGS__))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag), test_log_sink(fmt, ##__VA_ARGS__))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag), test_log_sink(fmt, ##__VA_ARGS__))

typedef enum {
    BIST_ESP_OK = 0,
    BIST_ESP_IO_TEST_ERR = 10,
} bist_esp_err_t;

typedef struct {
    uint32_t status;
    uint32_t level;
} bist_gpio_input_result_t;

extern volatile bist_gpio_input_result_t bist_gpio_input_shared_results[2];

bist_esp_err_t bist_gpio_input_reciprocal_test(gpio_num_t gpio_num);
