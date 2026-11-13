# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# ESP-BIST standalone application (bare metal, no ESP-IDF build system).
#
# Point ESP_BIST_PATH at an ESP-BIST checkout, then:
#   cmake -GNinja -B build -DSOC_TARGET=esp32p4 -DCMAKE_BUILD_TYPE=Release .
#   ninja -C build

cmake_minimum_required(VERSION 3.22)

set(BIST_ROOT_DIR "$ENV{ESP_BIST_PATH}" CACHE PATH "Path to the ESP-BIST checkout")
if(NOT BIST_ROOT_DIR)
    message(FATAL_ERROR "ESP_BIST_PATH is not set. Point it at an ESP-BIST checkout.")
endif()
if(NOT EXISTS "${BIST_ROOT_DIR}/cmake/project.cmake")
    message(FATAL_ERROR "ESP_BIST_PATH (${BIST_ROOT_DIR}) does not contain cmake/project.cmake")
endif()

# The reciprocal GPIO input test dispatches to a second HP core and is built for
# ESP32-P4 only.
if(NOT "${SOC_TARGET}" STREQUAL "esp32p4")
    message(FATAL_ERROR "This example targets esp32p4; got SOC_TARGET='${SOC_TARGET}'")
endif()

set(APP_NAME safe_gpio_${SOC_TARGET})

set(APP_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/main)
set(APP_DRIVER_DIR ${CMAKE_CURRENT_LIST_DIR}/src/safe_gpio)

set(APP_SOURCES
    ${APP_MAIN_DIR}/main.c
    ${APP_DRIVER_DIR}/safe_gpio.c
    ${APP_DRIVER_DIR}/safe_gpio_core.c
)

include(${BIST_ROOT_DIR}/cmake/project.cmake)

project(safe_gpio LANGUAGES C ASM)

# Pin assignment from the article's breadboard harness. Override at configure
# time, e.g. -DSAFE_GPIO_PIN_A=20.
set(SAFE_GPIO_PIN_A 2 CACHE STRING "Active-low input pin, externally pulled up")
set(SAFE_GPIO_PIN_B 3 CACHE STRING "Complementary input pin, externally pulled down")
set(SAFE_GPIO_ACTUATOR_PIN 4 CACHE STRING "Actuator enable output")
set(SAFE_GPIO_FAULT_LED_PIN 5 CACHE STRING "Fault indicator output")

target_include_directories(${APP_EXECUTABLE} PRIVATE
    ${APP_MAIN_DIR}
    ${APP_DRIVER_DIR}/include
)

target_compile_definitions(${APP_EXECUTABLE} PRIVATE
    SAFE_GPIO_PIN_A=${SAFE_GPIO_PIN_A}
    SAFE_GPIO_PIN_B=${SAFE_GPIO_PIN_B}
    SAFE_GPIO_ACTUATOR_PIN=${SAFE_GPIO_ACTUATOR_PIN}
    SAFE_GPIO_FAULT_LED_PIN=${SAFE_GPIO_FAULT_LED_PIN}
)
