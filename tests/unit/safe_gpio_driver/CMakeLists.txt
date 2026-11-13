# Host unit tests for safe_gpio.c (the ESP-BIST backed driver).
#
# The ESP-BIST and ESP-IDF headers are replaced by the stubs in ./stubs, so the
# driver's own decisions are tested without a target:
#
#   cmake -S tests/unit/safe_gpio_driver -B build/safe_gpio_driver_test
#   cmake --build build/safe_gpio_driver_test
#   ctest --test-dir build/safe_gpio_driver_test --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(safe_gpio_driver_test C)

set(SAFE_GPIO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/safe_gpio")

add_executable(safe_gpio_driver_test
    test_safe_gpio_driver.c
    ${SAFE_GPIO_DIR}/safe_gpio.c
    ${SAFE_GPIO_DIR}/safe_gpio_core.c
)

target_include_directories(safe_gpio_driver_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/stubs
    ${SAFE_GPIO_DIR}/include
)

target_compile_options(safe_gpio_driver_test PRIVATE -Wall -Wextra -Werror)

# The driver keeps its state in statics and refuses a second init after a latch,
# so each scenario runs in a fresh process.
set(SCENARIOS
    init_rejects_bad_config
    requires_init
    uses_reciprocal_read_per_pin
    reciprocal_failure_latches
    out_of_range_level_latches
    discrepancy_latches_once
    latched_fault_stops_sampling
    bounce_does_not_latch
    fault_names_are_present
)

enable_testing()
foreach(scenario IN LISTS SCENARIOS)
    add_test(NAME safe_gpio_driver_${scenario}
             COMMAND safe_gpio_driver_test ${scenario})
endforeach()
