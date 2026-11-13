# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Host-native unit tests for the safe GPIO discrepancy filter. The filter has no
# hardware dependency, so the skew-window and latch behaviour is checked without
# a board:
#
#   cmake -S tests/unit/safe_gpio_core -B build/safe_gpio_core_test
#   cmake --build build/safe_gpio_core_test
#   ctest --test-dir build/safe_gpio_core_test --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(safe_gpio_core_test C)

set(SAFE_GPIO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/safe_gpio")

add_executable(safe_gpio_core_test
    test_safe_gpio_core.c
    ${SAFE_GPIO_DIR}/safe_gpio_core.c
)

target_include_directories(safe_gpio_core_test PRIVATE
    "${SAFE_GPIO_DIR}/include"
)

target_compile_options(safe_gpio_core_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME safe_gpio_core_test COMMAND safe_gpio_core_test)
