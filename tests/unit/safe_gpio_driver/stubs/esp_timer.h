/* Host test stand-in for the ESP-BIST timer. The test advances the clock. */
#pragma once

#include <stdint.h>

#include "esp_err.h"

int64_t esp_timer_get_time(void);
esp_err_t esp_timer_init(void);
