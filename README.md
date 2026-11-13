# ESP-BIST Safe GPIO (IEC 60730 Class C, ESP32-P4)

An ESP-BIST standalone application that supervises one mechanical contact wired to two GPIO pins. Each pin level is read by the dual-core reciprocal GPIO input test from ESP-BIST, so both HP cores sample the same pad and cross-check their evidence before the level is used. The two validated levels are then compared inside a bounded skew window; a stuck, shorted, bridged, or broken input latches a fault and drops the actuator.

The application runs bare metal on the ESP-BIST standalone build (no ESP-IDF application build system, no FreeRTOS). ESP-BIST brings up HP core 1 during startup, which is what the reciprocal reads dispatch to.

## Wiring

| Signal          | GPIO | Bias                                                     |
|-----------------|------|----------------------------------------------------------|
| Pin A           | 2    | 10 kOhm pull-up to 3V3; contact closes to GND            |
| Pin B           | 3    | 10 kOhm pull-down to GND; complementary contact or inverter |
| Actuator enable | 4    | Output; high only while the contact is closed             |
| Fault indicator | 5    | Output; high while a fault is latched                     |

Normal levels in complementary mode are `(A, B) = (1, 0)` open and `(0, 1)` closed. Any other pair persisting past the 20 ms discrepancy window latches `SAFE_GPIO_FAULT_DISCREPANCY`.

Pin numbers are CMake cache variables, so a different harness needs no source edit:

```sh
cmake -GNinja -B build -DSOC_TARGET=esp32p4 \
      -DSAFE_GPIO_PIN_A=20 -DSAFE_GPIO_PIN_B=21 \
      -DSAFE_GPIO_ACTUATOR_PIN=22 -DSAFE_GPIO_FAULT_LED_PIN=23 .
```

## Build

Requires an ESP-BIST checkout (`ESP_BIST_PATH`), ESP-IDF (for headers and ROM support files), and `kconfig-frontends`.

```sh
export IDF_PATH=/path/to/esp-idf
export MCUBOOT_PATH=/path/to/mcuboot
export ESP_BIST_PATH=/path/to/esp-bist
export PATH="$IDF_PATH/../.espressif/tools/riscv32-esp-elf/<version>/riscv32-esp-elf/bin:$PATH"

cmake -GNinja -B build -DSOC_TARGET=esp32p4 .
ninja -C build
```

The build produces a signed application image plus `flasher_args.json` in `build/`.

## Flash and run

The application expects an MCUboot bootloader at `0x2000`, which ESP-BIST provides.

```sh
ninja -C build flash_boot      # first time only
ninja -C build flash
ninja -C build monitor
```

Expected output:

```
I (xxx) safe_gpio_example: Post-boot reciprocal self-test passed on GPIO 2 and GPIO 3
I (xxx) safe_gpio: Supervising GPIO 2 and GPIO 3 (complementary), skew window 20 ms
I (xxx) safe_gpio_example: Contact closed, actuator energised
```

## Behaviour

Startup configures the outputs de-energised, initialises the driver, runs a reciprocal read of both pads, then starts a 200 ms watchdog and a 5 ms supervision loop.

Each loop sample calls `bist_gpio_input_reciprocal_test()` for pin A and pin B. The accepted level is taken from the Core 0 evidence slot, so the value the application uses is the value both cores agreed on. A failed reciprocal read latches `SAFE_GPIO_FAULT_BIST_CORE_MISMATCH`.

Faults are latched: the actuator stays low and the indicator stays lit until reset. The callback runs once per latch.

## Layout

```
bist_safe_gpio/
├── CMakeLists.txt          ESP-BIST standalone project definition
├── bist.conf               ESP-BIST Kconfig selection
├── main/main.c             application: outputs, self-test, supervision loop
├── src/safe_gpio/
│   ├── include/safe_gpio.h        driver interface
│   ├── include/safe_gpio_types.h  states, modes, fault codes
│   ├── include/safe_gpio_core.h   skew filter interface
│   ├── safe_gpio.c                reciprocal reads, GPIO setup, fault reporting
│   └── safe_gpio_core.c           framework-free discrepancy filter
└── tests/unit/safe_gpio_core/     host-native filter tests
```

`safe_gpio_core.c` performs no hardware access, so the skew and latch rules run under a host compiler:

```sh
cmake -S tests/unit/safe_gpio_core -B build_unit
cmake --build build_unit
ctest --test-dir build_unit --output-on-failure
```
